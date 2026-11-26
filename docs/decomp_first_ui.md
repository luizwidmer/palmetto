# The first-run, UI and movie path: the boot-media gate, the frame handshake and the calls behind the first visible frame

**Status: sections 1-10 are static decompilation, no boot; section 11 is measured.** This page maps the title's
own first-run/UI/movie path from the image alone: the boot-media gate the stage-2 boot sequence waits on, the frame loop that gate releases, the first-run,
accessibility, text-entry, profile, settings/save and launcher code with the runtime calls each one makes, and the
separate movie/Bink module that produces the launch-video frames. Every entry is an image RVA (image base
`0x140000000`) and a call site, a call graph edge, a structure offset or a count; no strings, asset contents or hashes
of game data appear here. **M** marks a fact measured statically in this slice (decompiled or scanned from the image);
**I** marks an inference from the call shape that the page does not resolve further. Runtime pages are cited only as
context and are never used as the evidence for a static claim.

## 1. What was measured, and with what

| item | value |
|---|---|
| image | the retail package's title executable, imported once with `-noanalysis`; 299,257 `.pdata` functions |
| decompiler | Ghidra 12.1.4 PUBLIC headless, project `D:\leonida-work\classa\ghidra-proj`, opened `-readOnly`; scripts under `D:\leonida-work\rea-tooling\ghidra_scripts` |
| functions decompiled | 88 function ranges over six headless batches; 74 with real bodies (the rest are two-instruction stage thunks or ranges without a `.pdata` start); per-function files under `D:\leonida-work\decomp\out-*` |
| static scans | RIP-relative references and immediate stores to every watched word, direct `E8` caller graphs from 299k function starts, the whole import table, the 161 GDK-runtime thunks and their 348 direct call sites |
| runtime answers seen statically | 17 GDK runtime classes (the thunk census from `tooling/gdk_runtime_calls.py`), the `Bink2Scarlett.dll` 23-import surface, the host kernel/PLM objects |
| not used | no boot, no game-data strings or bytes, no shim behaviour as evidence, no repository code changed, no git write |
| work notes | `D:\leonida-work\decomp\first-ui.md` (raw RVA list, scan outputs, next steps) |

The decompile covers the two producers the peer ports report as their first visible content: the title's **2D/UI
layer** ([first_visible.md](first_visible.md) section 2) and its **launch movie**. The frame loop below is the one the
existing tree already measures as a stationary black loop; this page does not run it.

## 2. The gate chain into the boot-media frame loop

The title's stage machine ([state_machine.md](state_machine.md) sections 2-3) enters stage 2, runs
`FUN_1421B8960` (`0x21B8960`), and blocks in a loop at `0x21B8A3E`. That loop is where this page starts: it is not a
sleep, it is the boot-media module's **frame tick**.

| gate | RVA | written at (site) | read by | condition that releases it | class |
|---|---|---|---|---|---|
| game/stage word | `0x7446510` | `0x53EC0C0`, called from executor `0x53EBFE0` | stage pump `0x53EBE50` | stage 2 requested at `0x81B25A`, stage 3 requested at `0x81B2EF` | M |
| boot-media gate byte | `0x71B9048` | `0x5BF9CE0` in ctor `0x5BF9A40` | predicate `0x5EA6930`, called at `0x21B8A43` | byte equals 2; the ctor runs during module init `0x220FCA0` at `0x220FEBF` | M |
| frame-request flag | `0x71B90AD` | set 1 at `0x22184AB`, cleared at `0x2218689` | frame body `0x22185A0` compare at `0x22185A5` | a queued media item with state 2 is polled by `0x2218410` | M |
| present handshake flag | `0x71B90AC` | set 1 by the tiny setter at `0x2218900`/`0x2218910` | frame body compare at `0x22185B2` | cleared at `0x22188D5` by `0x22188C0` after it signals the event at `0x1471B9078` | M |
| platform gate byte | `0xA210D81` | (state machine's platform gate) | four gated loops in `0x21B8960` | equals 1 | M, cited from [state_machine.md](state_machine.md) |
| scene/movie player state | `player+0x1C4` | set by the movie module | movie frame wrapper `0x2EF0E0` | equals 9 before any Bink decode is advanced | M |
| movie frame-ready byte | `player+0x1C7` | set by the movie module | movie draw `0x2349D80` | non-zero allows the decoded frame to be drawn | M |

The measured call graph of the loop, all RVAs:

`0x81B270` stage-2 entry -> `0x21B8960` boot sequence -> loop at `0x21B8A3E`:
`0x313550` (thunk) -> `0x313560` which is `call 0x2218410` followed by `jmp 0x22185A0`;
`0x21B8A43` calls `0x5EA6930` and breaks out at `0x21B8A58` when `0x71B9048 == 2`.

`0x2218410` is the **request poll**: it reads a 32-slot ring of 16-byte entries at `0x1471B8DE8` (index `0x71B8FFC`,
pending `0x71B9000`, counter word `0x71B9008`) and dispatches on the entry's state field (high dword, low dword 0):

| item state | action | site | effect |
|---|---|---|---|
| 2 | set frame-request flag | `0x22184AB` | `0x71B90AD = 1`, which lets the frame body run on this tick |
| 3 | `0x2218890` | `0x221849D` | also stores `0x149AEA45C = 1` at `0x2218496` |
| 4 | `0x2218870` | `0x2218476` | per-state handler |
| 5 | `0x2218880` | `0x2218486` | per-state handler |

After the state action the poll drains the subscriber list rooted at `0x149AEA338`; the poll stops as soon as
`0x71B90AD` is non-zero (`0x2218458`), so one tick consumes at most one state item. The state encoding and the
per-state handlers are M at the instruction level; the meaning of states 3-5 is **I** (they are three different
media requests, named only by their dispatch sites).

## 3. The frame body: how a frame is composed and handed over

`0x22185A0` is the engine's frame body. It runs only when both flags are set, and it calls, in this measured order:

| # | call | RVA | what it does (measured shape) |
|---|---|---|---|
| 1 | `0x140B5B6B0` | `0x140B5B6B0` | frame-begin helper (no `.pdata` entry) |
| 2 | begin-phase subscribers | `0x22177F0` | walks list `0x149AEA488` with the argument pair `(1, item)` and posts `0x149AEA498` worker permits (I for "permits") |
| 3 | phase dispatch | `0x22186B0` | calls `0x2217460`, which walks the 24-byte handler table at `0x1471B7450` (count `0x1471B744C`) and then calls the optional callback at the frame object's `+0x60` |
| 4 | worker semaphore | `0x22186F0` | `ReleaseSemaphore(0x1471B9070, 1, 0)` |
| 5 | signal event | `0x1402A1BE0` | `(0x1471B9078, 1)` |
| 6 | phase mask 0x20 | `0x2218710` | tail-calls `0x2217F50` with phase bit `0x20` |
| 7 | callback 1 | `[0x71B8CD0]` | if installed, called with `0x1471B8C80` |
| 8 | phase mask 0x40 | `0x2218720` | tail-calls `0x2217DE0` with phase bit `0x40` |
| 9 | callback 2 | `[0x71B8D38]` | if installed, called with `0x1471B8CF0`; the frame-handshake helper `0x22188C0` has this shape (the install sites of both callbacks are not statically located in this slice, I) |
| 10 | signal events | `0x144DAA220`, `0x1402A1BE0` | `(0x1471B9080, 1)` and `(0x1471B9088, 1)` |
| 11 | post item | `0x2218080` -> `0x2218090` | pushes an 8-byte payload into the same 32-slot ring and increments `0x71B9000` |
| 12 | phase dispatch | `0x2218730` | calls `0x2217460` again (second phase group) |
| 13 | end-phase subscribers | `0x2217A50` | walks `0x149AEA488` with `(0, item)` |
| 14 | dispatch item | `0x2217B80` | walks the subscriber list `0x149AEA338` with the item |
| 15 | frame end | `0x2218689`, `0x140B5B6F0`, `0x2218410` | clears `0x71B90AD`, then re-polls |

The kernel and platform objects behind this loop are created by `0x5A91C40`: six event handles
(`0x1471B9050`, `0x1471B9058`, `0x1471B9070`, `0x1471B9078`, `0x1471B9080`, `0x1471B9088`), one engine async/queue
object `0x1471B9090` (I), and the two **PLM notifications** `RegisterAppStateChangeNotification` (handle `0x71B9060`)
and `RegisterAppConstrainedChangeNotification` (handle `0x71B9068`). Teardown `0x2218220` closes the handles and
`0x2218290` unregisters the notifications. Both imports exist in the image's import table
(`api-ms-win-core-psm-appnotify-*`).

The loop is not only entered from the boot sequence. Its tick is called from eleven measured call sites, and the
last one is the title's own task-queue pump:

| caller | call site | context |
|---|---|---|
| boot sequence `0x21B8960` | `0x21B8A3E` | the stage-2 boot wait |
| boot wrappers | `0x21B7970`, `0x21B79F0` | small wrappers around the boot state (one tick and one poll call each) |
| task-queue pump | `0x63AD70` | `if (cl) call tick`, then `0x63AD90` drains two GDK task queues with `XTaskQueueDispatch` (thunk `0x55DD9B0`, slot 16) at `0x63ADA9` and `0x63ADCA` |
| async pump | `0x1E405F0` | three tick sites around an async wait |
| engine/task paths | `0x2225870`, `0x23CF6A0` (2), `0x275A470`, `0x10D6460` | other engine pumps |

That is the **first runtime-answer dependency** this page names (section 7, H1): the frame tick is interleaved with
`XTaskQueueDispatch`, so the title's own frame cadence depends on the GDK task-queue surface our shim serves.

## 4. The first-run path: screens, their owners, and the runtime calls they make

The boot-media module initialiser `0x220FCA0` registers twelve submodule descriptors (the first at `0x142210150`,
one per registered engine module) and calls the boot-media ctor at `0x220FEBF`. The same module owns the GDK
service wrappers below (`0x221xxxx`/`0x222xxxx`), which are the title-side functions that call the runtime thunks
directly. The screen-level rows are the decompiled wrappers; each row's runtime calls are measured, and the call
site is given where the scan resolved it.

| screen / step | owner (RVA) | calls | caller | class |
|---|---|---|---|---|
| accessibility boot step | `0x5A75F80` | `XSpeechSynthesizerCreate` thunk `0x55DCE7C`, then enumerate `0x55DCEF0` at `0x5A75FB1` | engine-side caller (not traced in this slice) | M (function), I (step name) |
| accessibility narration | `0x3E40E80` | set default voice `0x3E41A50` at `0x3E41097`, custom voice `0x3E41A30` at `0x3E4106A`, stream from SSML `0x3E41A70` at `0x3E410B3`, stream data `0x3E41AB0` at `0x3E410CC`, close `0x3E40C60` | narration request path | M |
| user sign-in request | `0x5BB5390` | `XUserAddAsync(1)` thunk `0x55DE140` when the signed-in flag `0x149AB3680` is clear | first-run user step | M (call), I (screen order) |
| user state poll | `0x539470` | wrapper `0x2211400` -> `XUserGetState` thunk `0x55DE3A8` at `0x5394AC`; copies four 0x20-0x28-byte blocks at `+0xC4`, `+0x164`, `+0x204`, `+0x2A4` (I: settings/profile snapshots) | per-frame settings/profile update | M (calls), I (block meaning) |
| user lookup / result | `0x2211390`, `0x2212B00`, `0x2219490` | `XUserFindUserById` `0x55DE230`, `XUserFindUserByLocalId` `0x55DE2B0`, `XUserAddResult` `0x55DE1B8` | user service layer | M |
| profile-name text entry | `0x2234A40` -> `0x43A2730` | `0x2234A40` builds a 6-qword request (`0x147452810` name, max length `0x1474523D0`) and calls the entry start `0x43A2730` at `0x2234ABD`; the start calls `0x43A2800`, which calls `XGameUiShowTextEntryAsync` thunk `0x55DFDF0`; result path `0x43A2A80` calls `0x55DFEA8` + `0x55DFF28` | state machine `0x22364A0` at `0x2236676`; also `0x27D2500` and `0x27D4D10` | M |
| text-entry adapter | `0x3DAEAA0`, result `0x3DAE860` | same `0x55DFDF0` async thunk with a `0x400` max length; result size/result thunks `0x55DFEA8`/`0x55DFF28` at `0x3DAE87B`/`0x3DAE8B3` | UI request layer | M |
| profile card | `0x3DAF310`, result `0x3DAF210` | `XGameUiShowPlayerProfileCardAsync` thunk `0x55DFCF4` at `0x3DAF3C1`; result `0x55DFD80` at `0x3DAF218` | UI request layer | M |
| launcher action | `0x3DAED40` | `XLaunchUri` thunk `0x55DF190` at `0x3DAED25` | UI request layer | M |
| save container enumerate/read | `0x28D12B0`, `0x28D12E0`, `0x28D1480` | `XGameSaveEnumerateContainerInfo` `0x55DF62C` at `0x28D12FB`, enumerate blobs `0x55DF7BC` at `0x28D152E`, read blob `0x55DF864` at `0x28D15EF` | save/settings load path | M |
| persistent settings load | `0x28D1A30`, callback `0x28D1BB0` | `0x28D1A30` calls the persistent-storage wrapper `0x28D0E00` at `0x28D1A5F`; that wrapper calls the `XPersistentLocalStorageGetPath` thunk `0x55E09A0` at `0x28D0E1E`; async start `0x142213A50`/`0x142213B10` | settings load | M (calls), I (what the blob holds) |
| display capability | `0x2481CA0` | `XDisplayTryEnableHdrMode` thunk `0x55DCDFC` at `0x2481D68` | display service layer | M |
| screenshot capture | `0x223C9B0` | `XAppCaptureTakeScreenshot` thunk `0x55DE988` at `0x223C9D9` | capture service layer | M |
| store/licence/connectivity | `0x1D82FE0`, `0x1D5D1A0`, `0x9DDDD0`, `0x9DDE30`, `0xF9D80`, `0x1CF8F00` | store context/query/licence and connectivity-hint thunks | service layer | M |

The **gate that orders these screens** is the same chain as section 2: stage 1 start `0x81B200`, stage 2
`0x81B270`, boot sequence `0x21B8960`, gate byte `0x71B9048`, stage-3 request `0x81B2EF`. In the image the
accessibility create is reachable only from `0x5A75F80`, and the measured boot reaches exactly that one create
([first_run_path.md](first_run_path.md) section 2.2) - so the accessibility step is the first of these screen
owners that actually executes. The legal, settings and start-screen **pages** themselves are data-driven front-end
pages; no static code path to them was found beyond the wrappers above, and this page does not claim one (I).

## 5. The movie/Bink path

The movie code is a separate module from the frame loop. Its initialiser `0x442E350` sets up **eight player slots**
(eight identical 0x148-byte blocks initialised in the function), initialises a **66-entry command ring of
`0x3E0`-byte items** at `0x149DC6EA0` (producer index `0x149DD6E78`, pending count `0x149DD6E80`, lock
`0x149DD6E60`, signal word `0x149DD6E88`), registers the module callback `0x14442E7B0`, and the ring post is
`0x20EEBC0`. A worker loop `0x442FA80` consumes it:

| command id | handled by | effect (measured) |
|---|---|---|
| 0 | `0x199EF10` | player prepare/attach (I) |
| 1 | `0x442E8B0` in a `Sleep(1)` retry loop | wait for the player |
| 2 | `0x442EF20` | open for playback |
| 3 | default branch | calls the registered callback `[0x149DD6EC0]` with the item fields |
| 4 | `0x442EA70` | probe open the movie (metadata only) |
| 5 | return | worker exit |

The title-side **API** functions that post commands are `0x442F260` (command 0), `0x442EDC0` (command 2),
`0x442F170` (command 3), `0x442EE50` (command 4), `0x442FD10` (command 5 close, which also calls
`BinkRequestStopAsyncThreadsMulti`, `BinkWaitStopAsyncThreadsMulti` and `BinkFreeGlobals`) and `0x442F0B0`
(player configure, which calls `0x20EED90` to install the frame-buffer lists). The requesters are the movie
component `0x3A18620` (calls `0x442EDC0`, `0x442EE50`, `0x442F260`, `0x442F0B0` at `0x3A191F8`, `0x3A18BD0`,
`0x3A19243`, `0x3A19285`/`0x3A192E6`/`0x3A19392`), started by `0x3A1AC50` <- `0x3A1AEB0` <- `0x22577C9` /
`0x26110C9`; command 3 is posted from `0x1850B00` (callers `0x57CB570`, `0x59EF2C0`); close from `0x3A1A860`.

The **open chain** is the one the public reference measured at the same RVAs:

| step | RVA | call |
|---|---|---|
| metadata/probe open | `0x442EA70` (function `0x442EA70..0x442ED35`) | `BinkOpenWithOptions` at `0x442EBE6`, `BinkGetFrameBuffersInfo`, `BinkClose` - the open exists only to read the frame-buffer info and the aspect ratio, then the handle is released |
| playback open | `0x199EF20` -> `0x442EFE0` (function `0x442EFE0..0x442F09B`) | `BinkOpenWithOptions` at `0x442F056` retains the handle at `player+0x50`, `BinkGetFrameBuffersInfo`, then `0x442F0A0` |
| frame-buffer registration | `0x442F760` (function `0x442F760..0x442F9D9`) | for each of the `player+0x58` frame buffers, maps the buffer through `0x145121690` and calls `BinkRegisterFrameBuffers` at `0x442F9B8` |
| per-frame decode | `0x2EF0E0` (function `0x2EF0E0..0x2EF3E8`) | `BinkSetWillLoop`, `BinkDoFrameAsyncMulti` + `BinkDoFrameAsyncWait`, `BinkNextFrame`, `BinkGoto`; frame number from `handle+0x10` into `player+0x1D0`; the async frame list is `0x149DD6E90`, count `0x149DD6EA4` |
| draw | `0x23495A0` -> `0x2349D80` | see below |

**How a decoded frame reaches the presented surface** is the measured 2D draw path: `0x2349D80` checks
`player+0x1C7` (frame ready), writes the movie's render state into the device object (`+0x12310`, `+0x1236C`,
`+0x1236E`, `+0x12368`, `0x12520`/`0x12522` state bits, `0x12530`), calls the frame wrapper `0x2EF0E0` at
`0x234A117`, reads the destination matrix with `0x1458C8950` into a 16-float block, then builds a three-entry
`0x18`-byte vertex batch with `0x14517A290`, submits it with `0x1402AACF0`, and flushes/presents with
`0x140BB7B00` (the submit and flush names are I from their position in the sequence). `0x23495A0` is the layer
entry: it reads the visibility flag `+0x90 & 1`, calls the player's draw, and, when the layer has a non-zero
height, draws a filled rectangle with `0x143A1B860` from the same matrix (I: a letterbox/fill layer). A second,
independent per-frame advance is `0x51B45B0`, which calls `0x2EF0E0` at `0x51B464A` when the player state is 9.
No direct caller of the draw functions exists in the image: `0x23495A0` is called only indirectly, through the
engine's registered callbacks (I, consistent with the frame phases in section 3).

**Shims in this chain** ([movie_path.md](movie_path.md) section 2.2 gives our side): `Bink2Scarlett.dll` is the
only decoder; its 23 imports bind to `port/shims/xmem.cpp` (`XMemAlloc`/`XMemFree`), `port/shims/file_system.cpp`
(`CreateFile2`), `port/shims/com_shim.cpp` (audio device only) and the host's file APIs for the handle it is given.
The frame-buffer mapping (`0x145121690`) and the draw batch/submit/flush are title code that reaches the D3D12
device layer, so `port/shims/d3d12_xs_device.cpp`, `port/shims/d3d12_vulkan_device.cpp` and `port/src/vulkan_replay.h`
sit under the draw (that the mapping is a device-layer call is **I** from the call shape; the reference names the
same frame wrapper and the slot-133 uploader on its side).

**Static negative that matters:** nothing in the boot-media module calls the movie API. The movie requests come from
the component functions `0x3A18620`/`0x3A1AC50`/`0x3A1AEB0` and from `0x1850B00`'s callers, which are not in the
frame loop's call graph; the launch movie therefore runs only if some higher state starts that component. In the
measured tree no movie entry executes ([movie_path.md](movie_path.md) sections 2.3-2.5), which matches this static
split: the producer is in the state machine above the frame loop, not in the frame loop.

## 6. The UI path

The 2D/UI surface is drawn through the engine's batch API, not through a middleware library the image imports.

| UI mechanism | measured RVA | what the code does |
|---|---|---|
| batch begin/build | `0x14517A290` | writes `0x18`-byte entries into the draw batch; called with the movie's 3 entries |
| rectangle/fill | `0x143A1B860` | draws a positioned/coloured rectangle from a 16-float matrix |
| matrix read | `0x1458C8950` | copies the 16-float transform used by both draws |
| state block | device object `+0x12310..0x12530` | blend/depth/scissor/descriptor fields written before the draw |
| submit / flush | `0x1402AACF0`, `0x140BB7B00` | submit the batch and flush/present (I for the submit/flush names) |
| UI class calls | `XGameUiImpl` slots 7, 8, 17, 18, 19 | profile card and text entry, table in section 4 |
| text | wide-char strings handed to the text-entry request (`0x43A2800`: `wcslen`/`wcsncpy` of the title, description and initial value) | the screen owns its text; the UI layer reads it from the request |

Text and font **resources** are not resolved by this page: the UI page data is in the package's RBF0
UI/Scaleform definitions ([rbf0.md](rbf0.md), [design.md](design.md) line 135), and the image contains no Scaleform
import or Scaleform-named API. The measured interface between page data and the renderer is the batch API above (I).
The runtime answers the UI path consumes are exactly the wrappers in section 4: user (`0x2211400`), UI dialogs
(`0x43A2800`, `0x3DAEAA0`, `0x3DAF310`, `0x3DAED40`), speech (`0x5A75F80`, `0x3E40E80`), saves/persistent storage
(`0x28D12B0`, `0x28D1480`, `0x28D1A30`), display (`0x2481CA0`), capture (`0x223C9B0`), store and connectivity.

## 7. Ranked runtime-answer hypotheses

Ranked by how directly each answer gates the first visible first-run/UI/movie frame in this static map. Each row
names the title-side evidence, the shim/file where our runtime answers it, and the falsifier that retires it.

| rank | hypothesis | static evidence (RVA) | where we answer it | falsifier |
|---|---|---|---|---|
| H1 | the frame tick is driven by GDK task-queue dispatch, and our dispatch semantics/cadence decide whether the boot-media frame body runs at all | `0x63AD70` calls the tick, then `0x63AD90` drains two queues with `XTaskQueueDispatch` slot 16 at `0x63ADA9`/`0x63ADCA`; nine tick call sites, the boot loop only one of them | `port/shims/xgameruntime.cpp` and the task-queue layer the shim serves (thread/slot 16); trace via the existing platform trace | a boot where `XTaskQueueDispatch` returns success but the tick entry `0x313560` or its call site `0x63AD78` never executes: then the tick is not reached through dispatch and H1 is retired |
| H2 | the PLM app-state/constrained notifications gate the present handshake: the frame body needs `0x71B90AC`, which is set by the present side only when the frame is consumed | event objects and notifications created at `0x5A91C40`; `0x71B90AC` set at `0x2218910`, cleared at `0x22188D5`; notification handles `0x71B9060`/`0x71B9068` | host `api-ms-win-core-psm-appnotify` imports; no shim answers them today, so this is a host/PLM condition to measure | sample `0x71B90AD`, `0x71B90AC` and the six handles: if both flags cycle while the body runs, the handshake is healthy and H2 is not the gate |
| H3 | the first-run user step gates the pages: `XUserAddAsync` completes and user state must become signed-in before the user-facing screen code proceeds | sign-in request `0x5BB5390` -> `0x55DE140`; state poll `0x539470` -> wrapper `0x2211400` -> `0x55DE3A8` | `port/shims/xgameruntime_user.cpp` (offline signed-in user and event), with the existing user-event slice | a boot where the user async completes signed-in and `XUserGetState` returns a state, yet the first-run call count does not advance: H3 is retired as the gate (the existing persistent/user slices already measured no stage move) |
| H4 | the accessibility step gates the first-run flow: the boot asks for the speech synthesizer, and a refused create can hold the accessibility page | create `0x5A75F80` -> `0x55DCE7C` at `0x5A75F99`; enumerate `0x55DCEF0` at `0x5A75FB1`; narration `0x3E40E80` | `port/shims/xgameruntime_offline.cpp` (`PALMETTO_SILENT_SPEECH=1`), tested in the existing offline test target | the A/B already run in [first_run_path.md](first_run_path.md) section 5: with the silent synthesizer the create and enumerate advance one call and the stage does not move, so H4 is not the gate by itself |
| H5 | the settings/save load gates the first-run pages being skipped or shown: the title reads save containers and the persistent-storage path before its screens | `0x28D12B0`, `0x28D1480`, `0x28D1A30` -> `0x55DF62C`, `0x55DF7BC`, `0x55DF864`, `0x55E09A0` | `port/shims/xgameruntime_gamesave.cpp`, `port/shims/persistent_store.h` | a boot where every save/storage wrapper is entered and no save/first-run call follows; or the measured zero saves in 130 s ([first_run_path.md](first_run_path.md) section 2.2) with the flow unchanged - either way H5 is retired as the visible-frame gate |
| H6 | the movie trigger and its frame-buffer mapping gate the launch video: the movie module must receive a command and map each Bink buffer to a GPU address before a frame can be decoded and drawn | module init `0x442E350`, ring post `0x20EEBC0`, probe open `0x442EA70`, playback open `0x199EF20`/`0x442EFE0`, mapping `0x442F760` -> `0x145121690`, registration at `0x442F9B8`, draw `0x2349D80` | Bink's xmem/file/com imports in `port/shims/xmem.cpp`, `port/shims/file_system.cpp`, `port/shims/com_shim.cpp`; the mapping/draw go through the D3D12 layer (`port/shims/d3d12_xs_device.cpp`, `port/shims/d3d12_vulkan_device.cpp`) | one boot in which the player state byte `player+0x1C4` reaches 9 or `0x442EA70` executes; if the module is reached and still no frame is drawn, the gap is the mapping/draw (H6) |
| H7 | the display capability answer (HDR) gates how the first content frame is configured, not whether it is drawn | `0x2481CA0` -> `0x55DCDFC` at `0x2481D68` | `port/shims/xgameruntime_offline.cpp` / the display shim path; the measured boot answers HDR disabled | the display query already answers `S_OK` with HDR disabled and the boot continues; if the same answer holds while a content frame is drawn, H7 is retired |
| H8 | input-device presence gates the first-run/UI phase, which is documented to read a paired pad before any key/poll gate | the title's input surface is one `GameInputCreate` + one gamepad device callback; no read is made without a pad ([input_gate.md](input_gate.md), [input_backend.md](input_backend.md)) | `port/shims/gameinput.cpp`, `PALMETTO_FAKE_INPUT` | the existing measurement: with a pad present the first-run/UI code reads it at `+25.44 s` and still no save/stage follows; if a pad is present and no read appears, H8 is retired as the gate |

## 8. Trigger table, read as one statement

The first visible first-run/UI/movie frame in this title has **four measurable gates in series**, and each is a
different kind of answer:

| order | gate | owner | answer that moves it |
|---|---|---|---|
| 1 | stage 2 -> stage 3 request `0x81B2EF` executes | stage pump `0x53EBE50` / executor `0x53EBFE0` | the stage-2 boot sequence returns; the frame loop is already running inside that wait, so this is the *state* gate |
| 2 | boot-media gate `0x71B9048 == 2` and the frame-body pair `0x71B90AD`/`0x71B90AC` | boot-media ctor `0x5BF9A40`, poll `0x2218410`, frame body `0x22185A0` | the request ring gets a state-2 item and the present side completes a frame |
| 3 | a screen owner executes (accessibility `0x5A75F80`, user `0x5BB5390`, text entry `0x2234A40`, profile `0x3DAF310`, saves `0x28D12B0`/`0x28D1A30`) | the first-run module `0x220FCA0` | the runtime answers that screen's calls (section 4) |
| 4 | the movie component `0x3A18620` starts and the frame-buffer mapping `0x442F760` succeeds | the movie module `0x442E350` | the launch video is requested and its buffers map to GPU addresses |

Steps 1 and 2 are title-side state that the runtime does not answer (I for step 2's present side). Steps 3 and 4 are
where the runtime-answer hypotheses H1-H8 sit, ranked in section 7. The tree's own measured boot stops before step 3
([first_run_path.md](first_run_path.md)) and never enters step 4 ([movie_path.md](movie_path.md)); this page adds the
exact functions and gates to probe next, not a new measurement of them.

## 9. Limits, and what this page does not claim

- **Static only.** Nothing here was run. The frame loop, the screens and the movie module are named by decompiled
  call graphs and call sites; the page does not claim any of them executes in the current boot.
- **Decompiler and caller limits.** Function starts come from `.pdata` (a leaf without unwind info, such as
  `0x5EA6930` or `0x145121690`, has no entry and was handled by its call site). Ghidra's C for the merged frame
  helpers is partially reconstructed (`0x2218870`/`0x2218890` decompile as a shared body); the RVAs and call sites
  are still exact, the register-level meaning is not.
- **State encodings are inferred.** The ring payload's state field (2-5) and the meaning of states 3-5 are read from
  the dispatch comparisons (`0x22184AB`, `0x221849D`, `0x2218476`, `0x2218486`), not from a documented structure.
- **Page names are inferred.** The rows in section 4 are named by the runtime call they make ("accessibility",
  "text entry", "profile card"); the title's own page names are in data, not in the image's code, and this page does
  not read game data.
- **The decompiler project is shared.** The Ghidra project was locked by another session during the last batch (the
  frame-flow ranges were re-run after it released); a later pass that needs the project must take it read-only
  (`-noanalysis -readOnly`) and not save.
- **No peer code or text is copied.** The peer-port sentences and the public-reference RVAs cited above are other
  projects' published observations, used as cross-checks only.

## 10. Files, evidence, uncommitted

- **This page**: `docs/decomp_first_ui.md`, the only repository file this slice writes. No runtime, build, test or
  data file changed; no boot was run; no git write.
- **Work notes and raw evidence (outside the repository)**: `D:\leonida-work\decomp\first-ui.md` (RVA list, scan
  outputs, next steps); `D:\leonida-work\decomp\scan.py` and `run-ghidra.ps1` (the two local drivers);
  `D:\leonida-work\decomp\out-*` (per-function decompile logs); `D:\leonida-work\decomp\gdk\` (the 161-thunk census);
  `D:\leonida-work\decomp\scan-*.txt` (caller/ref/import scans).
- **Ghidra project**: `D:\leonida-work\classa\ghidra-proj`, opened read-only from the copy the classa slice made;
  this slice saved nothing into it.
- **Index**: this page needs one row in `docs/README.md` next to the picture-frontier pages; that file is outside
  this slice's write scope, so the lead adds it.
- **Uncommitted files**: `docs/decomp_first_ui.md` (new, this slice). No other repository change.

## 11. Measured: the AC chain in a boot (bounded probes, negative)

**Status: measured, diagnostic only, negative.** Sections 1-10 are the static slice. This section reports what two
130 s probe boots and one clean 300 s boot showed about the chain in section 2. The media module is armed - its kernel
objects and both present semaphores exist early, the event handler is registered once, the per-frame media tick runs
and the frame body is ticked 4556 times in 130 s - but the handler `0x247B500`, the AC opener `0x2484A90` and the AC
setter `0x2218910` never execute, and both present-pass keys (`0x71B90AD`, `0x71B90AC`) read 0 in every sample of all
three boots. The only in-image producer of the type-`0x24` set event, `0x2218180`, has no reference in the image and
never executes (11.2). So **no shim answer is the missing side**: the AC handshake cannot be opened by title code in
this build, and this slice changes no shim behaviour beyond the two diagnostic fields in 11.1.

### 11.1 The probe and the two 130 s boots

The probe is the tree's existing `PALMETTO_POKE_LOG` (up to four int3 entry probes; a hit logs the registers and the
title stack words and then re-arms) plus two fields added to the existing `PALMETTO_STATE_TRACE` sampler in
`port/src/main.cpp`: the two media/UI present semaphores `0x71B9078`/`0x71B9088`, which section 3 names as the body's
release sites. Both switches are opt-in; the added fields are diagnostic-only and the default path is unchanged. The
boots ran one at a time under `_setup/boot_lock.ps1` with `tooling/boot_report.py`, the standard Vulkan environment
(`PALMETTO_D3D12=vulkan`, `PALMETTO_SHADERS`, `PALMETTO_SKIP_INT3=1`), logs outside the repository under
`D:\leonida-work\boot-mediaac-probe`, `-producer` and `-300`. `PALMETTO_STATE_TRACE` reads the byte flags as a dword
at the byte address, so the sample prints a key of 0 as `0xff000000` when the neighbouring byte is 0xff; the byte is
the low one.

| probe site | role (section 2/3) | hits in 130 s boot A |
|---|---|---|
| `0x247B500` | event handler that sets `0x75DF0D8` on event type `0x24` and clears it on `0x25` | **0** |
| `0x2484A90` | AC opener: tail jump to `0x2218900` -> `0x2218910` | **0** |
| `0x2218910` | the only instruction that sets `0x71B90AC = 1` | **0** |
| `0x22185A0` | the boot-media/UI present body | **4556** |

The 4556 body entries name the pumps that drive the tick, and the body exits at its first two compares every time:

| body caller (return address after the tick call) | hits | what it is |
|---|---|---|
| `0x63AD7D` (tick call at `0x63AD78`) | 4529 | the title's task-queue pump, which drains two GDK task queues afterwards; supports H1 in section 7 |
| `0x1E40BF3` | 6 | the async pump around an async wait |
| `0x21B8A43` (tick call at `0x21B8A3E`) | 1 | the stage-2 boot-wait loop; it exits on its first iteration because the gate `0x71B9048` is already 2 |

Boot A's chain words: `0x71B9048` 0 -> **2 at 10 s**; `0x71B90AD` 0 and `0x71B90AC` 0 in every sample; the semaphores
`0x71B9078`/`0x71B9088` were **0x714/0x71c** from the 10 s sample; the state machine stayed current 2, next 2 (reached
at 25 s); session 0; loading 1.

Boot B re-armed the probe on the producer side: `PALMETTO_POKE_LOG=24846D0,2478440,2218180,2217C70`. It shows the media
module is open and ticking while the one producer of the set event does not run:

| probe site | role | hits in 130 s boot B |
|---|---|---|
| `0x24846D0` | the title's media/frame tick; it calls `0x2484A90` when bit 0 of `0x75DF0D8` is set | **5214** (caller return `0xC9E603`, the per-frame entry `0xC9E5E0`) |
| `0x2478440` | media module open: creates the device-side objects and calls the registration thunk `0x5CABBB0` at `0x2479675` | **1** (caller return `0x19F4F54`) |
| `0x2218180` | the only in-image producer of the type-`0x24` set event (11.2) | **0** |
| `0x2217C70` | the selector thunk that tail-jumps to `0x2218180` | **0** |

Boot B's chain words are the same negative (gate 2 at 20 s, the two keys 0, semaphores 0x714/0x71c, machine 2/2,
session 0), with 0 exceptions and 249,337 D3D12 calls recorded.

### 11.2 The static cross-check that names the missing side

The probe is the measurement; the image's own reference graph explains why it reads 0, and it corrects one claim in
sections 2-3:

- **The flag has one writer and one reader.** `0x75DF0D8` is stored only at `0x247B527` (inside `0x247B500`) and
  loaded only at `0x2484823` (inside the media tick `0x24846D0`). A RIP-relative scan of the whole image finds no
  reference to it in `0x2490080`, so the earlier "cleared by `0x2490080`" is not what this image shows; the clear is
  the frame body's own end-of-frame dispatch of the type-`0x25` item at `0x2218673` (section 3, step 15).
- **The handler is an intrusive-list node.** The registration thunk `0x5CABBB0` stores the handler address in the
  node at `0x75DF0B0` (`node+8 = 0x247B500`, `node+0 = 0`) and tail-jumps to the generic insert `0x1B0BC20`, which
  links the node into the subscriber list rooted at `image+0x9AEA338` (the same list section 2 calls the subscriber
  list). The module open `0x2478440` is the only caller of that thunk, and boot B measured it running once.
- **That list has two dispatchers only:** the request poll `0x2218410` and the item dispatch `0x2217B80`. Both call
  each subscriber with a 16-byte item whose type is the high dword of the item's `+8` qword. The queued items the
  poll sees carry types 2-5/8/9/0x20; the directly dispatched items carry `0x24` (only from `0x2218180`, which also
  queues the state-2 item `0x200000000` the poll turns into `0x71B90AD = 1`), `0x25` (only from the body at
  `0x2218673`), `0x26` (`0x2217CC0`), `0x27` (`0x2217D50`) and `0x10` (`0x59DCBD0`). So the two present-pass keys have
  exactly one common producer, `0x2218180`.
- **That producer is unreferenced code in this image.** `0x2218180` has no reference anywhere in the image except
  its own selector thunk `0x2217C70`; the two selector thunks `0x2217C70`/`0x2217C90` themselves have no call, jump,
  conditional jump or address-taken pointer (8-byte VA or 32-bit RVA) anywhere, and `0x2217CC0`/`0x2217D50` are each
  referenced only by their selector thunk. The constant `0x2400000000` that builds the type-`0x24` item appears twice
  in `.text`: at `0x22181C3` inside `0x2218180`, and at `0x526D3EA` in the GDK task-queue port-message builder
  `0x526D3E0`, which posts into its own queue (`0x5220050`) and never dispatches into the boot-media list. A linear
  scan also finds one other `[reg+0xC] = 0x24` builder at `0x3C2DC60`; it builds a different 16-byte event object for
  its own consumer (`0x3C0C970`) and never reaches either dispatcher of the boot-media list.

### 11.3 The clean 300 s boot

One boot with no int3 probe: standard environment, `PALMETTO_STATE_TRACE=10`, presenter captures armed
(`PALMETTO_CAPTURE`, every 600 presented frames, at most 16), 300 s timeout. Logs, trace and captures are under
`D:\leonida-work\boot-mediaac-300` and `D:\leonida-work\capture-mediaac-300`.

| measure | value |
|---|---|
| verdict | ALIVE 300.3 s, killed by the harness timeout; stopped nowhere, no crash, 0 exceptions |
| D3D12 calls / trace events | 435,282 calls recorded; 52,124 trace events |
| presenter captures | 16, each 2564x1421, mean rgb **0.00 0.00 0.00** (black); at least 9,001 frames presented by the kill (~30/s) |
| device-lost | no device-lost line anywhere in the capture |
| other device answers | 113 slot-67 calls failed with `0x80070057` (a pre-existing stand-in answer in this tree, none fatal) |
| stage/session words | init 2 (last change 20 s), loading 1 (last change 20 s), session 0 in every sample; machine current 2, next 2 |
| boot-media gate | `0x71B9048` 0 -> 2 at 20 s, 2 to the kill |
| present-pass keys | `0x71B90AD = 0`, `0x71B90AC = 0` in every sample (0 s and 20-290 s every 10 s) |
| media semaphores | `0x71B9078`/`0x71B9088` = 0x714/0x71c from 20 s to the kill |
| platform / loading-active | platform gate `0xA210D81` 0x1000001 at 20 s then 0x101; loading-active 0x7453EE0 = 1 from 30 s |

Before/after in one sentence: at 0 s every chain word is zero; from 20 s to the kill the gate, the state machine and
the semaphores are set exactly as in section 2's static model, and the two keys that would let the body run stay 0,
which is why the 16 captures are black.

### 11.4 Verdict, limits and the next falsifier

The verdict is a negative, and it is not a shim gap. The media module is fully armed in the measured boots - the
kernel objects and both semaphores exist by 10-20 s, the event handler is registered once, the per-frame media tick
runs 5214 times in 130 s and the frame body is ticked 4556 times - but the request/set side of the handshake never
executes, and the image's own reference graph shows why: the only producer of the type-`0x24` event (and of the state-2
item that sets the other key) is unreferenced code in this build. A runtime that synthesized that event or those ring
items would be inventing title state, not answering a runtime call, so this slice implements no producer fix. The
picture-roadmap consequence is that the boot-media/UI present pass is not the missing first visible producer on this
build; the stage-2 -> 3 gate (roadmap row 1) and the first-run/UI and movie producers (rows 4/5) remain the candidates.

- **Next falsifier:** one boot in which `0x247B500` is entered, `0x71B90AD` reads 1, or `0x2218180`/`0x2217C70`
  executes. Any of them means the producer is reachable after all (through a pointer form these scans do not see) and
  the chain trace restarts from that site with the same four probes.
- **Static limit.** The "unreferenced" result comes from RIP-relative, direct `E8`/`E9`/`jcc` and 8-byte-VA/32-bit-RVA
  data scans of the image as it sits in the package. A pointer computed at run time from a table outside those forms
  would not be seen; the probe boot is what makes the negative a measurement (the function did not execute).
- **Probe limit.** Function entries only. `0x2218180` is a `.pdata` function start and the selector thunks tail-jump to
  it, so the entry is the site a caller would hit; an interior entry is not plausible for the measured call shapes.
- **Host-side noise.** Boot A recorded 8 exceptions, all host-side C++ exceptions (`0xe06d7363`) raised and caught
  inside the host's UI Automation DLL during the boot; they are host-only and did not stop the boot. Boot B and the
  300 s boot recorded none.

### 11.5 Files and gates

- **Source change**: `port/src/main.cpp` only - the `PALMETTO_STATE_TRACE` boot-media line now also prints the two
  present semaphores `0x71B9078`/`0x71B9088` (`kTitleBootMediaSemaphoreARva`/`B`). Diagnostic only, off unless the
  sampler switch is set; no shim, no default path and no test target changed.
- **This page**: the measured section above and the status line in the header.
- **Evidence (outside the repository)**: `D:\leonida-work\boot-mediaac-probe`, `boot-mediaac-producer`,
  `boot-mediaac-300` (stderr, trace, summary) and `D:\leonida-work\capture-mediaac-300` (captures + `index.txt`);
  driver scripts `D:\leonida-work\boot_mediaac_probe.ps1`, `boot_mediaac_producer.ps1`, `boot_mediaac_final.ps1`;
  static scan helpers under `D:\leonida-work\mediaac`.
- **No game data**: RVAs, counts, handles and outcomes only; no string, asset, package path or hash is quoted here.
- **No git write**: the lead commits the two uncommitted files (`port/src/main.cpp`, `docs/decomp_first_ui.md`).

## 12. Measured: the first-run service answers, their defaults, and the first save-provider start

**Status: measured; two boots plus the existing A/B pages.** Sections 1-10 are the static slice and section 11 is the
boot-media negative. This section answers the picture roadmap's row 4 ([decomp_campaign.md](decomp_campaign.md)): it
measures the first-run services the boot consumes, makes the measured console answers the defaults, and reports whether
a screen owner executes or the presented frame changes. Three defaults change (the speech surface, the user change event
and the input device); the offline sign-in and the save/settings reads do not, because the tree already serves the
measured console answer for them (a signed-in user and the file-backed offline store).

### 12.1 Measuring the answers and what follows them (200 s boot, pre-change binary)

One 200 s boot of the pre-change binary (SHA-256 `7D2DFEB28E346EE45E4CAFBFF4F7D00B60528F3EB91492CECA423ADB29E23F3B`,
built 09:50) with the tree's existing opt-in answers on at once: `PALMETTO_SILENT_SPEECH=1`,
`PALMETTO_USER_CHANGE_EVENT=1`, `PALMETTO_FAKE_INPUT=3`, `PALMETTO_KEYBOARD_PAD=always`, plus the standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) and the
measurement layer `PALMETTO_USER_TRACE`, `PALMETTO_INPUT_TRACE`, `PALMETTO_STATE_TRACE=10` and presenter captures.
The arm measures the answers and everything the title does after them; it cannot separate which answer causes which
follow-up, which is why 12.3 repeats it with the defaults alone.

| measure | value |
|---|---|
| verdict | ALIVE 200.4 s, killed by the harness timeout; 0 exceptions, no stop |
| D3D12 calls / startup-trace events | 326,245 / 33,770 |
| presenter captures | 3 (frames 1, 3,001, 6,001), every one mean rgb 0.00 0.00 0.00 |
| stage words | init 2, loading 1 (both last changed 20.0 s), session 0; machine current 2 next 2 |
| boot-media gate / semaphores | `0x71B9048` 0 -> 2 by 20 s; `0x71B9078`/`0x71B9088` = 0x714/0x71c |
| last platform call | +104.33 s (`XNetworkingGetConnectivityHint`, bounded trace #10000) |

The sequence the answered services release (times are the runtime's process clock; the caller is the title's thunk or
its own call site):

| when (s) | call | answer | what the title does next |
|---|---|---|---|
| +1.82 | `XUserRegisterForChangeEvent` (queue, context, callback `game+0x2211C80`) | `S_OK` | one `SignedInAgain` (0) delivered; `XUserFindUserByLocalId`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle`, `XUserCloseHandle` once each |
| +1.84 | `XPersistentLocalStorageGetPath` | `S_OK` | the overlay path; the persistent profile record is created here |
| +2.x | `GameInputCreate` (`game+0x59258C8`) and `RegisterDeviceCallback` (`game+0x592586E`) | `S_OK` | the callback enumerates the two paired devices (keyboard slot 4, scripted slot 5) and calls `GetDeviceInfo` |
| +20.27 | `XSpeechSynthesizerCreate` (`game+0x55DCED0`) | `S_OK`, handle handed out | `XSpeechSynthesizerEnumerateInstalledVoices` `S_OK` with one silent voice; no stream request (slots 13 and 21 are not called) |
| +20.55 | `IGameInput::GetCurrentReading(Gamepad)` (`game+0x2211BA3`) and `GetGamepadState` (`game+0x6AEDAA`) | `S_OK` / `true` | the first-run/UI read that no-device boots never make |
| +20.55 | `XGameSaveImpl` class request x2, then `XGameSaveInitializeProvider` | `S_OK` | the offline file store opens as the save provider: its first measured slot call |
| +20.55 | user-ready refresh: 9x `XUserGetId`/`GetLocalId`/`DuplicateHandle`, 9x `XUserGetGamertag`, `XUserGetIsGuest`, `XUserGetAgeGroup`, 9x `XUserCheckPrivilege` | all `S_OK` | the title's own user refresh, the same shape the delivered event measured in [user_event_probe.md](user_event_probe.md) |
| +21.35 .. +104.33 | `XNetworkingGetConnectivityHint` (bounded trace) | `S_OK` | the connectivity poll; nothing after it |

**What the arm does not show.** `XUserAddAsync`/`XUserAddResult` (the sign-in owner `0x5BB5390`) is not called in
200 s; `XGameUiImpl`, `XLauncherImpl` and the movie API are not called; the narration stream slots are not called, so
the speech surface this boot consumes is one create and one voice enumeration, not a narration. The accessibility owner
`0x5A75F80` executes - section 4's static map makes it the only caller of that create, and here the create succeeds - and
the save store starts, but no presented frame changes: the stage words stay at 2/2 and every capture is black.

### 12.2 The defaults after the measurement

| service | before this slice | after this slice | fallback | why the measured answer is the default |
|---|---|---|---|---|
| speech surface | create `E_FAIL` with a null handle, no voice enumerated | create `S_OK` with a synthesizer handle; one silent voice enumerated; text/SSML streams are a 44-byte WAVE container with no samples | `PALMETTO_SILENT_SPEECH=0` or `off` | a console has a synthesizer; the switch-on A/B in [first_run_path.md](first_run_path.md) section 5 advanced one call and did not regress anything, and the 12.1 arm's +20.27 s is that advance reaching the first-run phase |
| user change event | registered, never delivered | one `SignedInAgain` per registration, on the title's own queue, once | `PALMETTO_USER_CHANGE_EVENT=0` or `off` | [user_events.md](user_events.md) and [user_event_probe.md](user_event_probe.md) measured the delivery consumed to the user-ready refresh and the first `XGameSave` class request, with the stage words unchanged |
| input device | no device on a host with no pad; the title reads nothing | one paired neutral pad from the always-built backend, so the first-run read succeeds | `PALMETTO_PAD_PRESENT=0` or `off` | a console has a pad; [input_backend.md](input_backend.md) section 5 measured that presence causes the first-run read and absence never does, and the default boot's +24.75 s read has no presses |
| offline sign-in | signed in; `XUserAddAsync`/`AddResult` answer `S_OK` with the offline user | unchanged | - | already the measured console answer; the sign-in owner is not reached in either boot |
| save/settings reads | the offline file store is the default provider | unchanged | - | [persistent_store.md](persistent_store.md) and the gamesave store; this slice measures the first `XGameSaveInitializeProvider` |

**What stays a stand-in, named.** The audio output itself: the WAVE container has a valid header and no samples, so a
title that plays the stream hears silence. The press is still a stimulus and not a runtime answer: the default pad is
present and neutral, and `PALMETTO_FAKE_INPUT` keeps the scripted press schedule opt-in. The change event is the
registration's initial state, not a transition the offline user took: the user is signed in when the title registers.
The input default changes the always-built no-SDL backend only; a build with SDL3 keeps its previous input defaults. On
a host with a physical controller the presence pad is a second paired device (the measured host has no controller, so
this boot's enumeration is the presence pad alone) - the switch is the fallback for a caller that wants device count to
match the host exactly.

### 12.3 The clean default 300 s boot

One 300 s boot of the post-change binary (SHA-256
`F0A45CD2C146BAE5061A3688563061A2F88E30C65378171090B5100BE1AFC048`, built 09:59:25) with no opt-in answer switches:
the standard environment plus `PALMETTO_USER_TRACE`, `PALMETTO_INPUT_TRACE`, `PALMETTO_STATE_TRACE=10` and presenter
captures every 3,000 frames up to 16.

| measure | value |
|---|---|
| verdict | ALIVE 300.2 s, killed by the harness timeout; 0 exceptions, no stop, no device-lost line |
| D3D12 calls / startup-trace events | 461,752 / 51,717 |
| presenter captures | 4 (frames 1, 3,001, 6,001, 9,001), every one mean rgb 0.00 0.00 0.00 |
| stage words | init 2 (last changed 30 s), loading 1 (last changed 30 s), session 0; machine current 2 next 2 |
| boot-media gate / platform gate / loading-active | `0x71B9048` = 2; `0xA210D81` = 0x101; `0x7453EE0` = 1 |
| media semaphores | `0x71B9078`/`0x71B9088` = 0x714/0x71c |
| slot-67 refusals | 113 calls with `0x80070057` (the pre-existing stand-in answer, none fatal) |
| last platform call | +108.68 s (`XNetworkingGetConnectivityHint`, bounded trace #10000) |

The three changed defaults are consumed by the boot itself:

| answer | clean default boot |
|---|---|
| speech surface | `XSpeechSynthesizerCreate` + `XSpeechSynthesizerEnumerateInstalledVoices` at +24.46 s, both `S_OK` |
| user change event | `SignedInAgain` delivered at +1.90 s to `game+0x2211C80`; the five follow-up user calls at +1.90 s |
| input device | one neutral pad enumerated by the title's device callback; `GetCurrentReading` + `GetGamepadState` at +24.75 s, `S_OK`/`true` (no scripted press is set) |
| offline save provider | `XGameSaveInitializeProvider` at +24.75 s, `S_OK`, 0 bytes |
| user-ready refresh | +24.75 s, the same call shape as 12.1 |
| sign-in owner / UI dialogs / movie | not called in 300 s |

**Reading.** The defaults reproduce the sequence of 12.1 within boot-speed variance (+20.3/+20.6 s there,
+24.5/+24.8 s here): the accessibility screen owner executes against a synthesizer that exists, the first-run gamepad
read happens with only the neutral pad present, and the save provider starts. The presented frame still does not
change: the four captures are black and the stage words are 2/2, as in the clean boot of section 11.3.

The boot's binary is the one above. After the boot this slice edited two comment blocks in the shims and nothing else,
so the gate build's SHA-256 differs (`2E992BB7D91502C974D4D0CAB896FE49D46273CBFC9D08FC37B188A509E283BC`, built
10:09:41; the parallel task-149's `port/src/main.cpp` is also in that build) and the boot was not repeated - the same
comment-only-difference convention [first_run_path.md](first_run_path.md) section 9 records.

### 12.4 Verdict and the next wall

The slice's first half is answered yes: the first screen owner the boot reaches now answers its runtime services. The
accessibility create `0x5A75F80` succeeds by default, its narration enumeration runs, the first-run read succeeds against
the default pad, the offline user event is consumed and the save store opens. This is the first measurement on this tree
where the save store is initialized by the title at all, and it follows the answered services directly.

The second half is answered no, honestly: no presented frame changes and no stage word moves. The four captures are
black, `init` stays 2, `loading` stays 1 and `session` stays 0 for 300 s. The remaining wall is the title-side
stage/present producer of section 11.4, not another first-run service answer.

- **Next falsifier.** A boot in which a presenting screen owner executes - `XGameUiImpl` text entry or profile card, the
  `XUserAddAsync` sign-in owner `0x5BB5390`, or the movie open `0x442EA70` - or in which any presenter capture is
  non-black. Either would mean the first-run flow moved beyond the service answers and the remaining gate is named by
  the new call.
- **The cause split.** One boot per fallback (`PALMETTO_PAD_PRESENT=0`, `PALMETTO_USER_CHANGE_EVENT=0`,
  `PALMETTO_SILENT_SPEECH=0`) attributes the +24.75 s read and the provider start at the same clock to presence, event
  or speech. The three A/B pages already separate the answers pairwise on older tree states; this tree's split is not
  measured.
- **The boot is dormant after +108.68 s**: no platform call, no file open and no stage change follows in the 300 s
  window, so a later window cannot be ruled out by this boot alone (the 600 s boot of [long_boot.md](long_boot.md) is
  the argument against it).

### 12.5 Files, tests and evidence

- **Source changed**: `port/shims/xgameruntime_offline.cpp` (speech default), `port/shims/xgameruntime_user.cpp`
  (change-event default), `port/shims/gameinput_nosdl.h` (default pad presence in the always-built backend). No build
  file, no `port/shims/persistent_store.h`, no D3D12, DirectStorage or xmem change; `port/src/main.cpp` is not touched
  by this slice (the parallel task-149 edits it).
- **Tests**: `port/test/xgameruntime_offline_test.cpp` (79 checks, 0 failures; default surface, the 0/off fallback,
  and the surface back after clearing), `port/test/xgameruntime_user_test.cpp` (96 checks, 0 failures; the default
  delivery, the 0 fallback, the dropped delivery and the per-process cap), `port/test/gameinput_nosdl_test.cpp`
  (9 of 9 cases; a new `default pad presence` case covers the neutral default, a refused press schedule and the
  `PALMETTO_PAD_PRESENT=0` fallback). `xgameruntime_gamesave_test` is unchanged and still passes.
- **Gates**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the three
  targets above pass, and the docs checks pass.
- **Evidence (outside the repository)**: `D:\leonida-work\firstrunowners\boot-measure1` and `boot-default300` (stderr,
  summary, startup trace), `capture-measure1` and `capture-default300` (captures + `index.txt` with the frame means),
  and the drivers `boot_arm.ps1`, `analyze_boot.py`, `build_tests_targets.cmd`. No game data: API names, slot numbers,
  times, counts, HRESULTs, the title's own RVAs and image means only.
- **Uncommitted**: the three shim files and three test files above, plus this page. `port/src/main.cpp` is the parallel
  slice's uncommitted change, not this one's.

