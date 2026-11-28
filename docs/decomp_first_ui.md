# The first-run, UI and movie path: the boot-media gate, the frame handshake and the calls behind the first visible frame

**Status: sections 1-10 are static decompilation; sections 11-19 add measured boot evidence.** This page maps the title's
own first-run/UI/movie path from the image and a bounded series of boots: the boot-media gate the stage-2 boot sequence
waits on, the frame loop that gate releases, the first-run, accessibility, text-entry, profile, settings/save and launcher
code with the runtime calls each one makes, and the separate movie/Bink module that produces the launch-video frames.
Every entry is an image RVA (image base `0x140000000`) and a call site, a call graph edge, a structure offset or a count;
no strings, asset contents or hashes of game data appear here. **M** marks a fact measured statically in this slice
(decompiled or scanned from the image); **I** marks an inference from the call shape that the page does not resolve
further. Runtime pages are cited only as context and are never used as the evidence for a static claim.

## 1. What was measured, and with what

| item | value |
|---|---|
| image | the retail package's title executable, imported once with `-noanalysis`; 299,257 `.pdata` functions |
| decompiler | Ghidra 12.1.4 PUBLIC headless, project `D:\leonida-work\classa\ghidra-proj`, opened `-readOnly`; scripts under `D:\leonida-work\rea-tooling\ghidra_scripts` |
| functions decompiled | 88 function ranges over six headless batches; 74 with real bodies (the rest are two-instruction stage thunks or ranges without a `.pdata` start); per-function files under `D:\leonida-work\decomp\out-*` |
| static scans | RIP-relative references and immediate stores to every watched word, direct `E8` caller graphs from 299k function starts, the whole import table, the 161 GDK-runtime thunks and their 348 direct call sites |
| runtime answers seen statically | 17 GDK runtime classes (the thunk census from `tooling/gdk_runtime_calls.py`), the `Bink2Scarlett.dll` 23-import surface, the host kernel/PLM objects |
| evidence boundary | static sections 1-10 use no boot or shim behaviour as evidence; later measured sections cite their own bounded runs; no game-data strings or bytes are included |
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

**Status: measured diagnostic negative for the sampled boots; static callback path found.** Sections 1-10 are the static slice. This section reports what two
130 s probe boots and one clean 300 s boot showed about the chain in section 2. The media module is armed - its kernel
objects and both present semaphores exist early, the event handler is registered once, the per-frame media tick runs
and the frame body is ticked 4556 times in 130 s - but the handler `0x247B500`, the AC opener `0x2484A90` and the AC
setter `0x2218910` record 0 hits in boot A, and both present-pass keys (`0x71B90AD`, `0x71B90AC`) read 0 in every sample of all
three boots. Boot B's producer-side probe also records 0 hits at the callback selector `0x2217C70` and its target
`0x2218180`. Static cross-checking finds a conditional registered app-state callback path to that producer (11.2), so the
runtime negative is bounded to these arms: it does not prove the producer unreachable or that a shim answer is missing.
This documentation correction changes no runtime or shim behavior; no title event or ring item is synthesized.

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

The 4556 body entries name the pumps that drive the tick, not successful passes past the guards. The disassembly exits
at the first compare if `0x71B90AD != 1`, and at the second if `0x71B90AC != 1`:

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

Boot B's sampled chain words are also negative (gate 2 at 20 s, both keys read 0 in every sample, semaphores
0x714/0x71c, machine 2/2, session 0), with 0 exceptions and 249,337 D3D12 calls recorded.

### 11.2 Static cross-check: registered callback route and both gates

The runtime probes are the measured negative; the code graph is a conditional route, not proof that the callback fired.
It corrects the earlier "unreferenced producer" conclusion: the decisive edge is a code pointer passed to an imported
notification API, not an in-image direct `call` to `0x2218180`. The separate correction about the flag's writer and
clear remains part of the model, as recorded in the first bullet below.

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
- **The producer is reached through a registered app-state callback.** `0x2218180` has no direct `E8` caller; its
  code edge is the `jmp 0x2218180` at `0x2217C7C`. Selector `0x2217C70` requires non-null `RDX`; if `CL != 0`, it
  copies `RDX` to `RCX` and jumps to that producer, while `CL == 0` takes the alternate path to `0x22181F0`.
  Registration function `0x1D10A30` forms the callback address with `lea` at `0x1D10A34` (target `0x2217C70`) and
  calls `RegisterAppStateChangeNotification` via IAT slot `0x6793B38` at `0x1D10A49`, passing context `0x71B8C78`
  and output handle `0x71B9060`. It also passes selector `0x2217C90` (LEA at `0x1D10A53`) to
  `RegisterAppConstrainedChangeNotification` via IAT slot `0x6793B50` at `0x1D10A68` (output `0x71B9068`); that
  constrained-change selector does not branch to `0x2218180`.
- **The in-image registration path is concrete.** `0x56A1E55` calls `0x5A91BD0`; on its non-negative status path,
  `0x5A91BD0` calls thunk `0x5A91C30` at `0x5A91BE1`, `0x5A91C30` jumps to `0x5A91C40`, and `0x5A91C40`
  tail-jumps to `0x1D10A30` at `0x5A91CB4`. This establishes a static path to the registration calls. These arms did
  not record the import return values or prove that the OS delivered a notification; a callback address reference is not
  callback execution.
- **The two key effects follow from that one conditional producer.** `0x2218180` enqueues the high-dword state-2 item
  (`0x200000000`) through `0x2218080` at `0x22181B7`; the request poll `0x2218410` sets `0x71B90AD = 1` at
  `0x22184AB`. It also builds the high-dword type-`0x24` item (`0x2400000000`) and directly dispatches it through
  `0x2217B80` at `0x22181D2`; the registered handler `0x247B500` sets `0x75DF0D8`, then media tick `0x24846D0`
  reads its bit at `0x2484823` and calls opener `0x2484A90` -> `0x2218900`/setter `0x2218910`, which writes
  `0x71B90AC = 1`. Frame body `0x22185A0` checks `0x71B90AD` first at `0x22185A5`, then `0x71B90AC` at
  `0x22185B2`; either non-1 value branches to the common exit at `0x221869A`.
- **Other type-`0x24` builders are not this dispatch.** `0x526D3E0` writes the same qword type code at `0x526D3EA`
  but posts a port message to its own queue (`0x5220050`), not the boot-media subscriber list. A full `.text` scalar-store
  scan found two separate `mov [reg+0xC], 0x24` sites: `0x33DE67A` and `0x3C2DC92`. The first initializes a 0x68-byte
  notification record in `0x33DE660`; callback adapter `0x33DE650` is passed to the object constructor `0x3AA8F0` at
  `0x5D55D4F`, not to the boot-media dispatcher. The second belongs to builder `0x3C2DC60` and its different consumer
  `0x3C0C970`. These known alternate record/queue paths do not call `0x2217B80` or `0x2218080`; the boot-media
  dispatch-list producer remains unique in the identified paths, but it is not unreferenced.

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
the semaphores are set exactly as in section 2's static model, and every periodic sample reads both keys as 0, consistent
with the black captures. The sampler is not a watch on every byte write and does not exclude a brief key pulse between samples.

### 11.4 Verdict, limits and concrete next-arm recipe

**Measured verdict:** negative for the sampled boots, not a proof of static unreachability. The media module is armed -
its kernel objects and both semaphores exist by 10-20 s; the event handler is registered once in boot B, whose media
tick runs 5214 times, while boot A ticks the frame body 4556 times in 130 s. The probed handler/opener/setter in boot A
and callback selector/producer in boot B have 0 hits; periodic state-trace samples read both keys as 0 in the two probe boots plus the clean 300 s arm.
The code does contain a route from the successful registration path to the state-2 and type-`0x24` producer, guarded by
an OS callback and its `CL` value (11.2). These runs did not record registration return values or a callback delivery, so
they do not distinguish registration failure, no notification, or a notification taking the alternate `CL == 0` path.
No shim gap is established. Synthesizing a callback, event, or ring item would still invent title state, so no producer fix
is made. The boot-media/UI pass is not yet a measured first-visible producer; the stage-2 -> 3 gate (roadmap row 1) and
the first-run/UI and movie producers (rows 4/5) remain candidates.

- **Next arm (one light boot, no binary change):** under the boot lock, use the existing standard environment with
  `PALMETTO_STATE_TRACE=5` and `PALMETTO_POKE_LOG=5A91BD0,1D10A30,2217C70,2218180`. After the title reaches its
  normal window, record one ordinary focus loss/restore (do not inject an event). Keep the trace outside the repo. The
  expected static sequence is `0x5A91BD0` success path -> `0x1D10A30` registration -> callback `0x2217C70`; at that
  callback record `CL`/`RDX`. If `CL != 0`, the disassembly predicts the tail jump to `0x2218180`; a callback hit
  followed by that producer-entry hit is the reachability falsifier. If registration entry is seen but the callback is not
  delivered during that real transition, keep the result bounded; it does not refute the static route. `STATE_TRACE` is
  periodic and may miss brief key pulses. If the producer hits but its downstream effects are unclear, a follow-up should
  probe `0x247B500`, `0x24846D0`, `0x2484A90`, and `0x2218910`, with a write watch on the two key bytes.
- **Static limit.** The helper sweeps this mapped image's `.text` for direct-branch and RIP-displacement candidates, then
  decodes the relevant instruction/function ranges; `ptr_scan.py` searches literal VA/RVA forms. This confirms the callback-
  address path in this image, not a successful API return, OS notification, or callback execution. Pointer construction outside
  those forms and cross-image callback wiring remain outside this bounded static contract.
- **Probe limit.** `PALMETTO_POKE_LOG` observes the named entry sites only. Boot B covered both `0x2217C70` and
  `0x2218180`, but not the registration return codes; its zero hits mean those entries were not reached in that window.
- **Host-side noise.** Boot A recorded 8 exceptions, all host-side C++ exceptions (`0xe06d7363`) raised and caught
  inside the host's UI Automation DLL during the boot; they were host-only and did not stop the boot. Boot B and the
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
- **Static method (read-only)**: `scan.py` `fn`/`dis`/`callers`/`refs` cross-checks the producer, selector, registration
  routine and upstream branch; `jmp_scan.py` checks direct branch sites; `ptr_scan.py imm64 2400000000` finds the two
  `.text` qword builders (`0x22181C3`, `0x526D3EA`). `type_scan.py` decodes `.text` for scalar `[reg+0xC] = 0x24`
  stores and found `0x33DE67A`/`0x3C2DC92`; `iat.py` resolves slots `0x6793B38`/`0x6793B50`, and `pdata_list.py`
  checks function ranges. The decisive edge is the decoded RIP-relative callback LEA plus imported API call, not an
  absolute data-pointer hit. No instruction bytes or executable data are copied here.
- **No game data**: RVAs, counts, handles and outcomes only; no string, asset, package path or hash is quoted here.
- **Round-21 delta**: this section's static correction only; no runtime code, test target or CMake stanza changed.

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

## 13. Measured: the movie/Bink trigger, decoded statically and probed in one bounded boot (negative)

**Status: measured; static decode plus one bounded probe arm, negative.** This section answers the picture roadmap's
row 5 ([decomp_campaign.md](decomp_campaign.md)): **what would have to happen for the movie/Bink path to run**, and
whether the measured boot reaches it. The static decode below names the module init, the per-frame component update,
the two slot gates and every play step behind them, and the game-side request that would set those gates. The probe
arm shows the module is armed and the component update is enabled, but no request ever binds a player slot: the four
play-step probes read 0 hits and the slot-0 words that a request would set read 0 in every sample. The Bink decoder is
mapped, bound and never executed. The arm was cut at 131.4 s by the host GPU losing the device (the lead terminated the
post-loss spin; not a harness timeout), but every movie-chain word had been stable since 30 s, so the negative is the
measured state of the full boot window rather than of its first half.

### 13.1 The trigger decode (static)

The path is a chain of four layers: the middleware module's init, the engine frame's component update, a per-player
slot gate, and the play steps behind that gate. The names below are the title's own RVAs (image base `0x140000000`).

| step | RVA | measured shape |
|---|---|---|
| module init dispatcher | `0x577F500` | param 2 = process init: sets the player-slot count `0x9187418 = 0x3a` (**58**) at `0x577F630`, runs the slot constructor `0x347140` for all 58 (`0x149165B98 + i*0x940`), registers the two async workers `0x3A1A180`/`0x3A1A500`, and sets the module-ready byte `0x91874B2 = 1` at `0x577F9A7`; param 8 = the `BinkGetPlatformInfo`/thread teardown branch |
| boot call into the init | `0x21B8960` | the stage-2 boot sequence calls `0x21B8B20` at `0x21B8A64` (immediately after its boot-media wait loop breaks at `0x21B8A58`); `0x21B8B20` calls `0x577F500(2)` at `0x21B8BA4`. The second caller is the middleware module dispatcher `0x57780E0` at `0x5778504` |
| per-frame component update | `0x11F740` -> `0x2257700` -> `0x3A1AEB0` | the engine frame `0x2257700` calls the movie component update at `0x22577C9` only while the movie-active guard byte `0x91AC9FA` reads 0 (`0x22571E0` clears it at `0x225723A`; `0x1E77BA2` and `0x3A19AF8` set it 1 as a re-entry guard) |
| the ready gate | `0x3A1AEB0` | `if (0x91874B2 != 1) return;`; then calls `0x3A1AC50`, then rebuilds an 8-entry active-handle list from the slot array's `+0x4c8`/`+0x4f0` words |
| the per-frame slot loop | `0x3A1AC50` | calls `0x3A13C80`, then `0x3A18620(slot)` for each of the 58 slots (`0x149165B98`, stride `0x940`); afterwards, for eight per-slot handle pairs starting at `0x14916592C`, looks the entity up (`0x51B60`) and, when `entity+0x1c4 == 9` and `entity+0x1c7 == 1` and the frame is inside a per-slot window, calls `0x140C06970` - the frame-ready/present scheduling already modelled in section 2 |
| the two slot gates | `0x3A18620` | returns at `+0` if `slot+8` (the entity) is null, and at `+0x10` if `slot[0x694] == 0`; otherwise the armed flag `slot[0x5f2]` is refreshed each frame as `(slot[0x898] != 0) | slot[0x8ad]` |
| the request that sets them | `0x3A16260` -> `0x3A167E0` | allocates a free slot (`0x3A16770`: first slot whose `+8` is null; creates the entity with `0x1416136B0` and binds it at `+8`), copies the play descriptor into `+0x4c8`/`+0x4cc`/`+0x4d0`/`+0x50c`/`+0x50d`/`+0x50e`/`+0x510`/`+0x520`/`+0x574`, then `0x3A167E0` copies the `0x2a8`-byte Bink options block to `+0x698` and sets `slot[0x694] = 1` |
| the descriptor helper that calls the request | `0x18501E0` | if the descriptor's first dword equals `0xB7003DFF` it starts no player; otherwise calls `0x3A16260` (its only caller, at `0x185020A`) and attaches the entity via `0x3A16650` |
| the stop path | `0x3358080` -> `0x3A1A860` | code 1 or 8: releases the bound entities, posts worker command 5 (`0x442FD10`, close) and clears the ready byte `0x91874B2 = 0` at `0x3A1AAA5` |

The play steps inside `0x3A18620`, each with the decompiled condition that decides it (all are below the two slot
gates; `armed` is `slot[0x5f2]`, `entity` is `slot+8`):

| play step | entry RVA | posted/called from | condition that enters it |
|---|---|---|---|
| probe/metadata open (worker command 4) | `0x442EA70` | `0x442EE50` at `0x3A18BD0`; worker `0x442FA80` dispatches command 4 at `0x442FC31` | `armed && entity+0x1c4 == 3`, or `!armed && entity+0x1c4 == 1` |
| player prepare (worker command 0) | `0x199EF10` | `0x442F260` at `0x3A19243`; the packet-0 body `0x199EF10` at `0x3A19222`, and worker command 0 dispatches `0x199EF10` at `0x442FC01` | `entity+0x1c4 != 0xb`, `slot[0x5f0] == 0`, `!armed`, and `entity+0x1c4 == 6` or `slot+0x598` non-empty; `slot[0x5c0]` selects `0x442FE20` instead |
| playback open (worker command 2) | `0x199EF20` -> `0x442EFE0` (`BinkOpenWithOptions` at `0x442F056`) | `0x442EDC0` at `0x3A191F8`; player wrapper `0x18AA500` at `0x3A19382` -> `0x199EF20` at `0x18AA50C` | `entity+0x1c4 != 0xb`, `slot[0x5f0] == 0`, the slot's `+0x598` buffer record ready or `entity+0x1c4 == 6`, armed and `slot[0x5f1] == 0` (runs once: `[0x5f1] = 1`); the `entity+0x1c4 == 6` branch also enters `0x199EF10` when `slot[0x8ae] == 0` |
| frame-buffer configure/registration | `0x442F0B0` -> `0x442F760` (`BinkRegisterFrameBuffers` at `0x442F9B8`) | `0x442F0B0` at `0x3A19285`/`0x3A192E6`/`0x3A19392`; `0x442F760` from the decode wrapper `0x2EF0E0` at `0x2EF130`/`0x2EF3AD` | `entity+0x1c4 == 7` (line `0x3A1924F`), or `slot[0x5fb] == 1` once the buffer record is ready, or after `0x18AA500` succeeds |
| player open gate | `0x199EF20` | `0x18AA500` at `0x3A19382` | armed, `slot[0x515] == 1`, and `entity+0x1c5 == 3` |
| per-frame decode | `0x2EF0E0` | called by the draw `0x2349D80` at `0x234A117` and the second advance `0x51B45B0` at `0x51B464A`; calls `0x442F760` and the `BinkDoFrameAsync*`/`BinkNextFrame` set | only once a playback handle exists (`player+0x50`) and `player+0x1c4 == 9` |

**Who would issue the request.** `0x18501E0` is the common helper, and its callers are the game/UI owners of video
playback: the UI video-widget constructor `0x184F100` (from its update `0x184EEC0` at `0x184EED8`) and `0x184FC00`
(from `0x184F100` at `0x184F1D1` and `0x4C6D420` at `0x4C6D6F2`); the front-end widget helpers `0x3A15F00`/`0x3A16090`
(from `0x3A15DB0` and `0x3C284A0`); the game video manager `0x335AB60` at `0x335B02C` (keyed by a hash; its sibling
`0x3358080` is the stop path); `0x3A1B4F0` (from `0x3A26B30`); `0x2261790`; `0x1B39130`; and `0x5BAC680`. The UI widget's
per-frame present/notification path is separate: `0x57CB570` -> `0x1850B00` -> `0x442F170` (worker command 3), reached
from `0x184F100` at `0x184F335`/`0x184F3D7`/`0x184F63A`. So the request is an event the front-end/page layer issues
when it owns a video widget; it is not part of the boot-media frame loop decoded in sections 2-3.

### 13.2 Shims on the path

No shim answer gates the entry, and none was asked. `Bink2Scarlett.dll` maps at +0.92 s and its `DllMain` returns 1
(section 2.2 of [movie_path.md](movie_path.md) records the same binding); its allocator/file imports remain
`port/shims/xmem.cpp` and `port/shims/file_system.cpp`, and the boot's reach signals show the module making none of
those calls. The play steps are title-side worker state posted by the component, so the missing piece is a game-side
request, not a runtime refusal; this slice changes no shim.

### 13.3 The probe and the boot

`port/src/main.cpp` gains two opt-in diagnostics, both off unless their switch is set:

- `PALMETTO_MOVIE_PROBE=player` installs four int3 entry probes through the existing poke-log machinery on the four
  play steps above: `0x2EF0E0` (per-frame decode), `0x442EA70` (probe open), `0x199EF20` (playback open) and
  `0x442F760` (frame-buffer registration). `PALMETTO_MOVIE_PROBE=trigger` is the game-side set for a later arm:
  `0x577F500` (module init), `0x3A16260` (play request), `0x18501E0` (descriptor helper) and `0x3A1A860` (stop).
- The existing `PALMETTO_STATE_TRACE` sampler gains a movie-chain line: the ready byte `0x91874B2`, the update guard
  `0x91AC9FA`, the slot count `0x9187418`, and the slot-0 word pair/triple a request would set (`+8` entity `0x9165BA0`,
  `+0x694` active `0x916622C`, `+0x5f2` armed `0x916618A`, `+0x4c8` media id `0x9166060`).

One arm ran under `_setup/boot_lock.ps1` with `tooling/boot_report.py`, the standard Vulkan environment
(`PALMETTO_D3D12=vulkan`, `PALMETTO_SHADERS`, `PALMETTO_SKIP_INT3=1`) plus `PALMETTO_MOVIE_PROBE=player`,
`PALMETTO_STATE_TRACE=10`, the Bink reach switches `PALMETTO_XMEM_CALLERS=1` and `PALMETTO_FILE_TRACE=1`, and presenter
captures every 600 frames (at most 16). Logs, trace and captures are outside the repository under
`D:\leonida-work\boot-movie-probe` and `capture-movie-probe`. The four probes installed (each byte `0x41` -> `0xCC`).

| probe site | role (13.1) | hits |
|---|---|---|
| `0x2EF0E0` | per-frame decode wrapper | **0** |
| `0x442EA70` | probe/metadata open | **0** |
| `0x199EF20` | playback open | **0** |
| `0x442F760` | frame-buffer registration | **0** |

| measure | value |
|---|---|
| verdict | terminated at 131.4 s by the host GPU losing the device (lead terminated the post-loss spin; not a harness timeout), 0 exceptions in the 19,768-event trace |
| D3D12 calls / trace events | 236,913 / 19,768 |
| presenter captures | 9 (frames 1, 601, 1201, ..., 4801), each 2564x1421, **mean rgb 0.00 0.00 0.00** |
| stage words | init 0 -> 1 at 20 s, 1 -> 2 at 30 s; loading 1 at 30 s; session 0; machine current 2, next 2 |
| boot-media gate / platform gate / semaphores | `0x71B9048` = 2; `0xA210D81` = 0x101; `0x71B9078`/`0x71B9088` = 0x714/0x71c |
| Bink module | mapped at +0.92 s, `DllMain` = 1; **no** `xmem_caller` and **no** `file_open` event names the module (3 xmem callers and 121 opens in the whole boot, all title or host) |

The movie-chain line over the arm (samples every 10 s):

| time | ready `0x91874B2` | update guard `0x91AC9FA` | slot count | slot0 entity | slot0 active `+0x694` | slot0 armed `+0x5f2` | slot0 media id |
|---|---|---|---|---|---|---|---|
| 0 s, 20 s | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 30 s .. 120 s (every sample) | **1** | **0** | **58** | **0** | **0** | **0** | **0** |

Reading: the module init ran between the 20 s and 30 s samples (ready byte 0 -> 1 and slot count 0 -> 58 are written
only by `0x577F500(2)`), and the engine frame's update guard reads 0 from the start, so the component update
`0x3A1AEB0` is called on the per-frame path from then on. The two slot gates are never passed: no entity is bound and
no slot is active in any sample, which is exactly the state the request path (13.1) leaves until `0x3A16260` runs.

### 13.4 Verdict, limits and the next falsifier

**Verdict: negative, and not a shim gap.** The movie middleware is armed in the measured boot - its init ran, the slot
array exists (58 slots), the ready byte is 1 and the component update is enabled - but nothing ever binds a playback
request to a slot: the slot-0 entity/active/armed words are 0 in every sample, the four play-step probes have 0 hits,
and the Bink decoder, though mapped and bound at +0.92 s, never executes (no allocator or file call names it). So the
launch-movie producer is above this module: a game/front-end request through `0x18501E0 -> 0x3A16260`, not a runtime
answer this tree can serve differently.

- **Next falsifier.** One boot in which `0x18501E0` or `0x3A16260` executes (`PALMETTO_MOVIE_PROBE=trigger`), or the
  slot-0 entity/active words read non-zero, or any of the four `player` probes fires. Any of them means the request is
  reachable and the chain trace restarts one level down; then the existing `PALMETTO_XMEM_CALLERS`/`PALMETTO_FILE_TRACE`
  switches on the same boot say whether the decoder follows.
- **The named event.** The request is issued by a front-end/page owner of video playback (13.1): the UI video widget
  `0x184F100`/`0x184EEC0`/`0x184FC00`, the front-end helpers `0x3A15F00`/`0x3A16090`, the game video manager
  `0x335AB60`, or `0x3A1B4F0`. The measured boot never runs one of those owners - the stage words are frozen at
  init 2 / loading 1 / session 0 - so the next slice's question is which front-end page or story-load step creates the
  first video widget, not another shim answer.
- **Arm limit.** The arm was cut at 131.4 s by the host device loss, so a request that would have arrived after 131 s
  is not ruled out by this run alone; the chain words are stable from 30 s to 120 s and the earlier 300 s clean boot of
  section 12.3 (same tree family) and the 600 s boot of [long_boot.md](long_boot.md) show no later stage change, which
  is the argument this negative does not rely on a truncated window.
- **Probe limit.** Entry only, and four sites; an interior branch or the request path itself is not separated. The
  probes are exactly the four play steps the section asks about; `PALMETTO_MOVIE_PROBE=trigger` covers the request side
  in a later arm.
- **Data limit.** The play descriptor is game data (a front-end/RBF0 page resource); this page reads none of it and
  names only RVAs, counts and outcomes. The requester list is static address-taken/call-site evidence, not a measured
  call order.

### 13.5 Files, tests, evidence and gates

- **Source change**: `port/src/main.cpp` only - the `PALMETTO_MOVIE_PROBE` switch (two four-site groups through the
  existing poke-log machinery) and four movie-chain fields added to the existing `PALMETTO_STATE_TRACE` line. Both
  opt-in; the default path, every shim and every build target are unchanged.
- **This page**: the section above. `docs/README.md` needs one index row; that file is outside this slice's write
  scope, so the lead adds it.
- **Tests and gates**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`;
  the GPU-free gate targets pass (the listed checks: `unimplemented_policy_test` 98, `graphics_record_test` 180,
  `graphics_pso_test` 350, `xbox_present_test` all cases, `dstorage_test` 234, `xapu_chad_test` 87, `deferred_compile_test`
  18, plus `frame_dump_test`, `xs_proxy_mock_test` and `buffer_va_index_test`); the pre-existing quarantined
  `d3d12_fence_timing_test` crash is unchanged ([testing.md](testing.md) line 95 - not a gate check); and the docs checks
  (`tests/test_docs_*.py`) pass (43 passed, 218 subtests). No shim, test source or build target changed in this slice.
- **Evidence (outside the repository)**: `D:\leonida-work\boot-movie-probe` (stderr, stdout, `summary.txt`,
  `boot.jsonl`) and `D:\leonida-work\capture-movie-probe` (9 captures + `index.txt`); driver
  `D:\leonida-work\boot_movie_probe.ps1`; the static work notes and decompiles under `D:\leonida-work\decomp\out-movietrigger*`
  and `.md` notes. No game data: API names, RVAs, counts, times, HRESULTs and image means only.
- **No git write**: the lead commits the two uncommitted files of this slice (`port/src/main.cpp`,
  `docs/decomp_first_ui.md`). The tree also carries other slices' uncommitted files, which are not this slice's.


## 14. Measured: the movie slot's stop at game+0x3DB49C is the front-end render's seventh service getter, and with the device alive the play steps run

**Status: measured, attributed statically and with in-process probes; no runtime-answer fix, and the picture frontier moved one layer in.** The picture task asked what stops the boot at `game+0x3DB49C` right after the movie request binds a slot, and whether a runtime answer of this tree is on that path. The stop site is attributed exactly: it is the tail virtual call of the title's own front-end manager render `FUN_1403DB3F0`, reached from the front-end module's async worker thread, and the object it calls through had its first qword (the vtable word) overwritten with a small negative value (`0xFFFFFFFFFFFFFFE7` = -25) - a stray write/lifetime artefact in the title's heap, not a layer refusal. The probes measure the manager created once, its seven service holders live and the render called 1,786 times with a valid service holder and vtable, and the manager's teardown and member-destroy functions never called in the window. The stop did not reproduce on the three light arms run here; on the one light arm that did not lose the Vulkan device the movie chain advanced past the request: the metadata/probe open `0x442EA70` fired three times, the playback open `0x199EF20` fired once, the Bink 2 library's async threads appear in the stage-3 thread census, and the boot stopped later at the pre-existing renderer-executor fault `game+0x1335DCB`. Frame-buffer registration `0x442F760` and per-frame decode `0x2EF0E0` did not run and every presenter capture is black, so no movie frame is claimed. No shim, resource, view or presenter answer is changed; the default path is byte-identical.

### 14.1 The static attribution of game+0x3DB49C

`0x3DB49C` is the last instruction of `FUN_1403DB3F0` (`0x3DB3F0..0x3DB4A0`): `jmp qword ptr [rdx+0x18]`, after `mov rdx,[rax]`/`mov rcx,rax`/`add rsp,0x28`. The function is the front-end manager's per-frame render:

| step | RVA | measured shape |
|---|---|---|
| render function | `0x3DB3F0` | loads the image global `0x98C0F20` (`_DAT_1498c0f20`), calls the manager vtable slot `+0x10`, then for each vtable slot `+0x28`, `+0x30`, `+0x38`, `+0x40`, `+0x48`, `+0x50`, `+0x58` calls the getter and then calls the returned object's vtable slot `+0x18`; the last getter is the tail-called one at the stop |
| the manager single ton | `0x98C0F20` qword | written only by `FUN_143DA1A80` at `0x3DA1B1C`, from `FUN_1419DB790()` - the factory cache global `0x98C1040`, a 0x98-byte object constructed by `FUN_145800410` with vtable `0x654CF30` |
| manager create/init | `0x3DA1A80` | `(**vtable)(manager, context)` = slot 0 `0x5AF53B0`, which allocates the seven service objects with `operator new` (`0x4F97130`, 8 bytes each) and stores them at manager `+0x60,+0x68,+0x70,+0x78,+0x80,+0x88,+0x90`, then checks each service's vtable slot `+8` and sets the ready byte `0x98C0E98 = 1`; bails with a logged line (0x4F..0x64) on the first service whose start returns 0 |
| the seven getters | vtable slots `+0x28..+0x58` | `mov rax,[rcx+0x60]` ... `mov rax,[rcx+0x90]` (service 6 is the `+0x58` getter `0x4DEE990` returning `[manager+0x90]`); measured service vtables: `0x654D830`, `0x654D870`, `0x654D920`, `0x654D200`, `0x654D2C0`, `0x654D960`, `0x654D350` |
| service 6's class | vtable `0x654D350` | slot 0 destructor `0x3DADEC0`, slot `+8` start `0x5DB1D80` (sets `0x98C10A0 = 1`, registers the front-end callback `0x3DAF4D0`), slot `+0x10` stop `0x3DADEF0`, slot `+0x18` **`0x4380` = bare `ret`**, slot `+0x20` `0x4380` |
| manager destroy | `0x3DADD00` (vtable slot `+8`) | for each of the seven fields: call service vtable slot 0 (destructor, `edx=1` frees the 8-byte holder), then zero the field; then destroy the manager itself and clear `0x98C1040`; it does **not** clear `0x98C0F20` |
| manager teardown caller | `0x3DA2FB0` | calls each service's slot `+0x10`, then the manager's slot `+8`; called from `FUN_143DA2880` (front-end shutdown) <- `FUN_14220F3B0(1)` (module callback) |
| front-end module init | `0x22082D0` (`FUN_1422082D0(2)`) | sets up the front-end config (including the code pointer `FUN_142208B90` and the async worker's argument), then calls `FUN_143DA1560` -> `FUN_143DA1A80`; sets the front-end running byte `0x7453DB8 = 1` |
| per-frame caller | `0x2208B90` -> `0x267FD0` | the front-end async worker's registered callback `FUN_142208B90` calls the frame routine `FUN_140267FD0`, which (when the bit test `FUN_14512A480(0)` is set) calls `FUN_1403DB3F0` |
| the worker thread | `0x3DA3D60` <- `0x2215EA0` | a worker loop that, while a byte flag reads 1, calls the registered callback with its argument, then sleeps; `FUN_142215EA0` is the thread entry |

The exception's own stack in the measured stop names exactly that chain, innermost first: `game+0x3DB49C`, `+0x268021` (return address after the call to `FUN_1403DB3F0` in `FUN_140267FD0`), `+0x2208BC1` (return address after the call to `FUN_140267FD0` in `FUN_142208B90`), `+0x3DA3D9D` (return address after the indirect callback call in the worker loop `FUN_143DA3D60`) and `+0x2215ECA` (the worker thread entry `FUN_142215EA0`). So the call sequence from the movie slot arming to the stop is: game side arms the movie slot -> the front-end UI/video-widget request runs on the front-end page path -> the front-end async worker's frame renders the seven services -> the seventh service getter returns an object whose vtable word is garbage -> the tail call faults.

### 14.2 What the faulting word is

`jmp qword ptr [rdx+0x18]` faults at address `0xFFFFFFFFFFFFFFFF` (= -1) when `rdx = 0xFFFFFFFFFFFFFFE7` (= -25): `mov rdx,[rax]` loaded the service-6 holder's first qword as -25, and -25 + 0x18 = -1. So the object at `[manager+0x90]` had its 8-byte vtable word overwritten by a small negative value. With the measured class that word must be `0x14654D350`; the render call on a live holder is the bare `ret` at `0x4380`, i.e. a no-op. The value is not the manager's teardown (which zeroes the field, giving a `[0x18]` fault) and not a failed allocation (which would be a null getter result); it is a stray 8-byte write or a freed-block first word. The same signature appears in this session's other fast arms: `game+0x1335DCB` is `call [rax+0x10]` with the object's first qword `0xFFFFFFFFFFFFFFEF` (-17; -17 + 0x10 = -1) - the renderer executor `0x1335A20..0x13361ED`, the family [3d_textures.md](3d_textures.md) already records stopping at `0x13360A3` after three `ExecuteCommandLists`.

### 14.3 The in-process probes and the measured state

`port/src/main.cpp` gains two diagnostics, both off unless set:

- `PALMETTO_MOVIE_PROBE=frontend`: a third four-site group through the existing one-shot poke-log machinery (each site's byte becomes `0xCC`, registers and the six register windows are logged, the instruction is restored and single-stepped, and the probe re-arms): `0x3DA1A80` (manager create/init), `0x3DA2FB0` (manager teardown), `0x3DADD00` (member destroy) and `0x3DB492` (the render's last getter, the instruction whose operand is the faulted object). The `player` and `trigger` groups are unchanged.
- The existing `PALMETTO_STATE_TRACE` line gains a front-end line: the manager pointer `0x98C0F20`, the ready byte `0x98C0E98`, the context qword `0x98C0EA0`, the running byte `0x7453DB8`, and for each of the seven service fields `+0x60..+0x90` the holder pointer and its first qword (vtable word).

One arm ran `PALMETTO_MOVIE_PROBE=frontend` (`moviefe1`), the section-12 content environment plus presenter captures; three further arms ran `PALMETTO_MOVIE_PROBE=player` (`movieplay1` before this slice's diagnostics, `movierepro1`, `movieplay2`). All arms are light (no `PALMETTO_DUMP_REGS`/`PALMETTO_FAULT_MEM`), one at a time under `_setup/boot_lock.ps1`, 120-130 s timeout, logs and captures outside the repository.

| measure (moviefe1) | value |
|---|---|
| manager created | between the 20.0 s and 22.0 s state-trace samples (first non-zero line at 22.0 s): pointer, ready 1, context, running 1 |
| seven services at the last samples | `0x60/0x654D830`, `0x68/0x654D870`, `0x70/0x654D920`, `0x78/0x654D200`, `0x80/0x654D2C0`, `0x88/0x654D960`, `0x90/0x654D350` - all constant through the arm |
| init probe `0x3DA1A80` | **1** hit, rax=0x2 (the process-init parameter), rcx=the front-end context |
| render last-getter probe `0x3DB492` | **1,786** hits, every one `rax` = the service-6 holder (`...1A270`) and `rdx` = the service-5 vtable (`0x14654D960`, the previous iteration's value), i.e. the render ran with a live holder and a valid vtable |
| teardown probe `0x3DA2FB0` / member-destroy probe `0x3DADD00` | **0** hits |
| movie slot | armed at the 58-60 s samples (slot0 entity non-null, active 1, armed 1, media-id `0x540714f3`) |
| stop | `game+0x43AE303` read `0x10000007E`, trace t 62.39 s, wall 63.7 s |

The render ran from the manager's creation (~20-22 s) to the stop, so the 3DB49C manifestation is not a first-call race on the manager's construction. The teardown and member-destroy sites were never reached, so the manager and its services were still live when the arm stopped; the corrupted word must therefore have been written by something else in the title's heap (or be a freed block's first word in a heap whose reuse the probes do not see).

### 14.4 The reproduction and the play-step measurement

The 3DB49C stop did not reproduce: `movieplay1` (before this slice) stopped there, and three light arms with the same content environment plus `PALMETTO_MOVIE_PROBE=player` stopped elsewhere: `movierepro1` at `game+0x1335DCB` read -1 (wall 96.9 s, trace t 95.17 s), `movieplay2` at `game+0x43AE303` read -1 (wall 62.2 s, trace t 60.91 s) and a first reproduction attempt at `game+0x1335DCB` read -1. In every arm that stopped at `0x3DB49C`/`0x43AE303` the log also contains `[rt] replay: the Vulkan device was lost` shortly before the stop (stderr line 216,987 of 221,613 in `movieplay1`, 222,625 of 227,244 in `movieplay2`, 239,933 of 244,426 in `moviefe1`); the one arm without a device loss, `movierepro1`, did not stop there. The device loss is the host GPU condition the FSR pages already record; this page does not claim it as a shim answer, and after the second device loss the arms were stopped per the task's rule.

The positive measurement is `movierepro1`, the same light environment and `PALMETTO_MOVIE_PROBE=player`, with no device loss:

| measure (movierepro1) | value |
|---|---|
| movie slot | armed by the 54.0 s sample: slot0 entity non-null, active 1, armed 1, media-id `0x540714f3` |
| probe `0x442EA70` (metadata/probe open) | **3** hits; each with rcx/rdi = the bound entity and the player-slot pointers |
| probe `0x199EF20` (playback open) | **1** hit; rcx = the player slot (entity pointer), i.e. the playback open step ran |
| probes `0x442F760` (frame-buffer registration) and `0x2EF0E0` (per-frame decode) | **0** hits |
| Bink 2 library at stage 3 | `Bink2Scarlett.dll` mapped and `DllMain` = 1; the stage-3 thread census at stage3+2.0 s lists 4 threads waiting in `NtWaitForAlertByThreadId` with `Bink2Scarlett.dll+0x237D8`/`+0xA505`/`+0x9CAC`/`+0xAAEE`/`+0xA609` frames and 1 with `+0x23878`/`+0x5D56` - the decoder's async thread surface is up |
| stage/session words | init 3 at 56.0 s (last change), loading 2 at 54.0 s, session 3 at 56.0 s, machine current 3 next 3 |
| xmem/file reach switches | no `xmem_caller` or `file_open` event names the library in `movieplay2` (the only xmem callers are the two title sites section 13 records, and the archive opens are the boot sets) |
| stop | `game+0x1335DCB` read -1, trace t 95.17 s, wall 96.9 s |
| presenter captures | 11 captures (`movierepro1`, frames 1..2,001 at every 200), each 2564x1421, **mean rgb 0.00 0.00 0.00** (black) |

So the request path now reaches two of the four middleware play steps and starts the decoder's thread surface - the first time this tree measures any play step - and the first visible frame is still not reached: the boot stops at the renderer executor before frame-buffer registration and decode. That executor fault `0x1335DCB` takes the same garbage-vtable-word shape as the 3DB49C site (`-17` at slot `+0x10`), so it is a separate title-side lifetime/stray-write symptom, not the movie layer refusing a call.

### 14.5 Verdict, fix, limits and the next falsifier

**Verdict: attributed, and not a runtime answer on the measured path.** The 3DB49C stop is the front-end render's seventh service getter returning an object whose vtable word has been replaced by a small negative value; the call sequence from the front-end worker to the site is measured in the exception stack, the manager and its seven services are measured live by the state trace, the render site is measured called with a valid holder and vtable 1,786 times, and its two teardown sites are measured never called. No shim, resource, view, display or file answer sits between the holder's creation and the fault. **No shim change is made and the default path is unchanged**; the only source change is the two diagnostics above.

- **Limits.** The stop is intermittent and did not reproduce in three light arms; the probe arm measured the render valid, not the writer. The corrupted word is an 8-byte heap cell; the layer has no write watch on a dynamically allocated title heap cell, so this page names the corruption's shape and victim, not the storing instruction. The device-loss correlation is measured (every 3DB49C/43AE303 stop follows `[rt] replay: the Vulkan device was lost`) but not causal here. The one play-step arm is a single boot; two of four play steps ran, and the stop that follows is the executor fault below, not this slice's site. The captures are black in every arm (11 in `movierepro1`, 12 in the other two arms, at every 200 presented frames); they cover only the loading frames.
- **The next wall** is `game+0x1335DCB`: `call qword ptr [rax+0x10]` in the renderer executor `0x1335A20..0x13361ED`, with the command object's first qword `0xFFFFFFFFFFFFFFEF`, reached from the UI/video-widget submit chain `0x186424` <- `0x18573B` <- `0x184F42` <- `0x1847D8` <- `0x1837A2` ... (the stack the exception reports) - i.e. the movie display job. Its shape (`-17 + 0x10 = -1`) is the same stray-word symptom as this stop, and the docs already record its family at `0x13360A3`.
- **Next falsifier**: a boot in which the render's last getter returns a holder whose first qword is a measured service vtable and the boot still stops (would move the attribution off the holder), or a write watch on the 8-byte cell `[manager+0x90]` / the service-6 holder naming the storing instruction (would make it a named title-side stray write or a layer write). For the picture, the falsifier is a boot that passes `0x1335DCB` and fires `0x442F760` or `0x2EF0E0`: the moment the decode step runs, `PALMETTO_XMEM_CALLERS`/`PALMETTO_FILE_TRACE` on the same boot say whether the decoder follows and the presenter capture answers the frame.

### 14.6 Files, tests, evidence and gates

- **Source change**: `port/src/main.cpp` only - the front-end manager constants and the state-trace front-end line, and `PALMETTO_MOVIE_PROBE=frontend` (a third four-site group in the existing poke-log switch). Both opt-in; the default path, every shim and every build target are unchanged.
- **This page**: the section above. `docs/README.md` needs one index row; that file is outside this slice's write scope, so the lead adds it.
- **Tests and gates**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`; the GPU-free gate subset exits 0 (`unimplemented_policy_test` 98, `graphics_record_test` 180, `graphics_pso_test` 350, `xbox_present_test` all cases, `dstorage_test` 234, `xapu_chad_test` 87, `deferred_compile_test` 18, `frame_dump_test`, `xs_proxy_mock_test`, `buffer_va_index_test`); the pre-existing quarantined `d3d12_fence_timing_test` crash is unchanged ([testing.md](testing.md) line 95 - not a gate check); the docs checks (`tests/test_docs_*.py`) pass. No shim, test source or build target changed in this slice.
- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-movieplay1` (the stop), `boot-moviefe1` (the front-end probes and state trace), `boot-movierepro1` and `boot-movieplay2` (the play-step arms) with their `stderr.log`/`stdout.log`/`summary.txt`/`boot.jsonl`, `capture-movierepro1` (11 captures + `index.txt`), and the static work under `D:\leonida-work\moviecrash` (decompiles of the manager, its vtable slots and the call-chain functions; `.pdata` function map; the exception stacks). No game data: RVAs, runtime pointers, vtable words, counts, times and image means only.
- **No git write**: the lead commits this slice's two uncommitted files (`port/src/main.cpp`, `docs/decomp_first_ui.md`). The tree otherwise has no uncommitted files from this slice.

## 15. Measured: the movie player opens and reaches the playing state, and the first display job is the class-0xa constant upload that kills the renderer executor

**Status: measured, negative on the picture, with the movie arm's stop attributed and the player state machine decoded.** Section 14
left the movie chain at two of the four play steps with the boot stopping later in the renderer executor, and named one
falsifier: a boot that passes the executor stop and fires frame-buffer registration `0x442F760` and per-frame decode
`0x2EF0E0`. This section measures what happens between the play steps and that stop. The player state machine is decoded
statically (every state byte writer), two new diagnostics read it at run time, and three light arms run it. In the arm that
reaches the full chain the Bink player **opens** (`BinkOpenWithOptions` returns a non-null handle, state byte `0x9`) and
about 1.3 s later the renderer submits the first display job; that job builds the title's class-0xa 256x1 constant upload,
the block is allocated with a declared size of zero and the 0x1000-byte copy runs into the live command object, and the
executor faults through the overwritten job header 0.1 s into the pass. Frame-buffer registration and per-frame decode
still have **0 hits** and every capture is black, so no movie pixel is claimed. The writer/mechanism is the same title-side
zero-size overrun the content arms name ([vertex_buffer_va.md](vertex_buffer_va.md) sections 14-16); no layer answer or
layer-owned write is in the chain.

### 15.1 The player state machine (static)

`entity+0x1c4` is the movie middleware's player state byte; every one of its writers is a small lock-free setter (an
`xchg`/`mov` of the byte, functions in the `0x442E..`/`0x442F..` range) and the two higher-level ones live in the engine
frame and the playback-open wrapper. The component `0x3A18620` calls the setters and the play steps in a fixed order.

| state | setter RVA | in the chain it is |
|---|---|---|
| `0x1` | `0x16136E0` | entity created / request binds the slot |
| `0x2` | `0x442EE40` | the playback-open work item is built (`0x442EDC0` -> `0x442EE40`) |
| `0x3` | `0x442EFD0` | the playback-open worker asks for the metadata probe again |
| `0x4` | `0x442EF10` | the probe work item is built (`0x442EE50`) |
| `0x5` | `0x442FD00` | worker command 4 (metadata/probe open `0x442EA70`) has run |
| `0x6` | `0x442FE60` | component `0x3A1916F`: the next step (prepare/playback open) may run |
| `0x7` | `0x442F0A0` | `BinkOpenWithOptions` succeeded in `0x199EF20` and `BinkGetFrameBuffersInfo` ran |
| `0x8` | `0x442F160` | frame-buffer configure `0x442F0B0` ran |
| `0x9` | inline in `0x5536E00` | finalise: `BinkPause`/`BinkSetWillLoop`, the playing state the decoder gates on |
| `0xa` | `0x442E8A0` | transfer/pause helper |
| `0xb` | `0x19463D0` | error; the code is stored at `entity+0x240` (metadata probe or playback-open failure) |

The component-side order the arms measure (decompiled at `0x3A18620`): it posts the metadata/probe command and calls the
`0x442FE60` setter at `0x3A1916F`; the armed branch at `0x3A19174` posts the playback open (`0x442EDC0` at `0x3A191F8`, once,
`slot[0x5f1] = 1`) and then calls player prepare `0x199EF10` while the state is still `0x6`; at state `0x7` it calls
frame-buffer configure `0x442F0B0` (`0x3A1924F`/`0x3A19285`/`0x3A192E6`/`0x3A19392`) which sets `0x8`; the per-frame call to
`0x5536E00` at `0x3A19B5F` turns `0x8` into `0x9`. The player wrapper `0x18AA500` and the `slot[0x515]` gate
(`entity+0x1c5 == 3`) are a separate one-shot open path the measured arms did not take.

**Frame-buffer registration has one caller.** `0x442F760` (`BinkRegisterFrameBuffers`) is called only from inside the
per-frame decode wrapper `0x2EF0E0` (`0x2EF130` and `0x2EF3AD`), so it can fire only once the renderer's draw calls the
decode. The player must first be in state `0x9` and the draw must execute, which is why this section measures the display
job and not a shim answer.

### 15.2 The diagnostics this slice adds

`port/src/main.cpp` only, both opt-in and off by default; no shim, layer answer or default path changes:

- the `PALMETTO_STATE_TRACE` movie block gains two lines: the player state byte `entity+0x1c4`, the byte after it, the Bink
  handle at `entity+0x50`, the `+0x1b8` decode gate, the probe and open results (`+0x23d`, `+0x23e`) and the error code
  (`+0x240`); and the player-slot gates the component reads (`slot+0x5f0`, `+0x5f1`, `+0x5fb`, `+0x515`, `+0x8ae`,
  `+0x5c0`; slot 0 base RVA `0x9165B98`). A small `title_read_u32_at` helper is added for the entity reads.
- `PALMETTO_MOVIE_PROBE=player2` is a fourth four-site group through the existing one-shot poke-log machinery: player
  prepare `0x199EF10`, player wrapper `0x18AA500`, frame-buffer configure `0x442F0B0` and the finalise step `0x5536E00`.
  `player`, `trigger` and `frontend` are unchanged.

### 15.3 The measured arms

One boot per arm under `_setup/boot_lock.ps1`, the standard content environment from `boot_arm.ps1`, `-Light`, one at a
time; logs, captures and readbacks outside the repository. `moviebink1` uses the section-14 binary; `moviepix2` and
`moviepix3` use this slice's build.

| arm | binary sha256 (first 16) | wall | stage 3 | movie arm | probes and state (measured) | stop / trace t | device lost | captures |
|---|---|---:|---:|---|---|---|---:|---:|
| `moviebink1` | `859699C2B5B11BEF` | 62.3 s | **no** | slot armed at the 58.0 s sample; metadata probe `0x442EA70` x1 | `0x28CD3CC` read `0x833DC0F950`, 60.76 s | 1 | 11, all mean rgb 0.00 |
| `moviepix2` | `4238F141ED2FBD52` | 55.9 s | 51.4 s | 50.0 s state `0x3` (probe 1 open 1); 52.0 s state `0x5`; prepare `0x199EF10` x1, configure `0x442F0B0` x1, finalise `0x5536E00` x144 (per-frame), wrapper `0x18AA500` 0 | `0x1335DCB` read -1, 54.36 s | 0 | 9, all mean rgb 0.00 |
| `moviepix3` | `4238F141ED2FBD52` | 58.8 s | 54.3 s | 52.0 s and 54.0 s state `0x1`; metadata probe x3 and playback open `0x199EF20` x1 between the 54.0 s and 56.0 s samples; **56.0 s state `0x9`, handle non-null, open 1** | `0x13360A3` read -1, 57.38 s | 0 | 9, all mean rgb 0.00 |

Reading: the component update only runs after stage 3 (the state stayed `0x1` through the 54.0 s sample and the whole
chain advanced inside the next two seconds). In `moviepix3` the chain is complete at the 56.0 s sample: state `0x9`, a
non-null Bink handle (`0x29F0752DA00`), the probe and open result bytes both 1. `moviebink1` is the stage-3 race's losing
draw (it also carries one `replay: the Vulkan device was lost` line); it is arm selection, not evidence against the chain.

### 15.4 The stop in the movie arm: the first display job is the class-0xa constant upload

`moviepix3` ran `PALMETTO_ALLOC_WATCH=1`, so the same boot that read state `0x9` also measured the executor's first
stage-3 entry and the constant upload it runs. In chronological order, with the steady clock the runtime prints:

| t (steady) | event |
|---|---|
| 9669.668 | FORMAT KEY hit: key `0xa`, command object vtable `+0x20` = `0x23E550`, dims 256x1, caller `0x1837A2` (the command-graph opcode dispatch) |
| 9669.669 | STAGE 3 ENTRY: first executor entry, job from `0x186424`, tid 43588 |
| 9669.692 | HELPER ENTRY: class `0xa`, plane count 1, dims 256x1, allocator arg 0 |
| 9669.702 | BLOCK ALLOC: requested **0x0** bytes, width 256, height 1, caller `0x1335D35`; **job - block = 0xEF0**, "overlap YES" |
| 9669.742 | ENTRY WRITTEN: stride 0x0, size 0x0, block recorded in the entry |
| 57.38 s (trace) | first exception: `game+0x13360A3` (`MOV RAX,[RSI]; CALL [RAX+0xB0]`), read at `0xFFFFFFFFFFFFFFFF`, trace t 57.376716 |

The geometry decides the stop: the block is 0xEF0 bytes **before** the command object and the constant upload copies
0x1000 bytes from there, so its last 0x110 bytes land on the object's header - the vtable word at `job+0`, the state dword
and the fields the executor calls through - and the executor's next call through that header faults. The value it reads is
a small negative word (the fault address is -1), so the copied table word at that offset is not a pointer. In the earlier
section-14 arm (`movierepro1`) the same zero-size block landed 0xA0 bytes *after* the job and the overrun covered the
descriptor object instead, which is why that boot stopped at `0x1335DCB` in the descriptor release; both arms are one
producer with two geometries. The 0x1000-byte copy itself is the host-CRT store section 14 measures from the executor
thread's stack; this arm reproduces its allocation geometry in the movie display job and the crash 0.1 s later.

### 15.5 The writer/mechanism verdict, and the reach switches

- **Title-side, not a layer write.** The class `0xa` is the title's own compile-time format-table entry, the row-stride
  switch `0x1555C10` has no case for it, and the zero-byte request, the block and the 0x1000-byte copy are all title code
  over title-private memory ([vertex_buffer_va.md](vertex_buffer_va.md) sections 14-16). The layer's only participation is
  that the arena pages were served by the `XMemVirtualAlloc` pass-through; the in-arena size class, block address and copy
  length are the title's own. No layer call is between the executor entry and the fault.
- **The Bink memory/file reach switches are a measured negative.** `PALMETTO_XMEM_CALLERS=1` and `PALMETTO_FILE_TRACE=1`
  ran on `moviepix2` and `moviepix3`; in both, no `xmem_caller` and no `file_open` event names the Bink library, even in
  `moviepix3` where `BinkOpenWithOptions` returned a non-null handle and the player reached state `0x9`. The movie's
  allocator/file path is therefore title-callback side on this boot (the title imports `BinkSetMemory` and
  `BinkSetOSFileCallbacks`, and the module init `0x577F500` builds the callback descriptors `0x141502D10`,
  `0x1404F9E90`, `0x14169C4E0`), not a call into our shims. The falsifier for that reading is a probe on those setters;
  the negative itself is measured.
- **The executor stop is not passed.** `0x442F760` and `0x2EF0E0` stay at 0 hits in every arm, and the display job's pass
  dies before the draw that would call the decode wrapper. Passing it needs the zero-size overrun avoided, which is the
  title-side branch the class-origin slice (`docs/vertex_buffer_va.md` section 16 and the open class-branch work) names;
  the layer has no measured input to that branch.

### 15.6 Verdict, limits and the next falsifier

- **Verdict: the movie chain now reaches the playing state and its first display job; the picture is still unreachable in
  this tree.** The advance over section 14 is measured: not just two play-step entries, but the completed open
  (`BinkOpenWithOptions` handle non-null), the configure step and the finalise to state `0x9`; and the first renderer job
  after that state is the class-0xa constant upload whose zero-size block overruns into the live command object.
- **Limits.** The state trace samples every 2 s, so the exact transition times between `0x1` and `0x9` are bounded by the
  sample gap, not measured per step; the same progression in `moviepix2` is read from its probe hits, not from a state
  sample. The copied table word at the job offset is not re-read (`PALMETTO_WRITER_WATCH` was off in these arms); the
  evidence for the stop is the measured allocation geometry plus the crash 0.1 s later in the executor call that reads the
  job's first qword, in the family the content arms attribute. The movie-display-job attribution rests on the first
  executor entry of the boot coinciding with the class-0xa allocation in the same pass and with state `0x9` 1.3 s earlier;
  the content arms show the same executor and the same family for the renderer's own draws. `moviebink1` lost the device
  once (one arm, not two in a row) and is not used as evidence.
- **Next falsifier.** A boot in which the class-0xa constant upload is not created for this job (a runtime answer or title
  branch that avoids the class, or a block allocated with a non-zero size) should let the executor pass run and fire
  `0x442F760` and `0x2EF0E0`; the moment `0x2EF0E0` is entered, the same boot's `PALMETTO_XMEM_CALLERS`/`FILE_TRACE`
  answer whether the decoder allocates or opens anything through the shims, and the presenter capture answers the frame.
  A boot that fires `0x2EF0E0` while the executor still crashes on the class-0xa word would move the attribution off this
  overrun.

### 15.7 Tests and gates

- **No test target changed.** The slice adds diagnostics behind their switches (the state-trace fields and the `player2`
  probe group); the default path, every shim and every build target are unchanged, and no pure helper was added to test.
- **Gates on the changed tree**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints
  `TESTS_BUILT`; the GPU-free gate subset exits 0 (`unimplemented_policy_test` 98, `graphics_record_test` 180,
  `graphics_pso_test` 350, `xbox_present_test` all cases, `dstorage_test` 234, `xapu_chad_test` 87,
  `deferred_compile_test` 18, `frame_dump_test`, `xs_proxy_mock_test`, `buffer_va_index_test`, `xmem_test` 305,
  `arena_size_test`); the pre-existing quarantined `d3d12_fence_timing_test` crash is unchanged
  ([testing.md](testing.md) line 95 - not a gate check); the docs checks (`tests/test_docs_*.py`, run with the session's
  broken `GIT_CONFIG_*` variables cleared) pass (33 passed, 205 subtests).
- **Attribution.** This section was implemented from this project's own measurements: the decompiled state setters and the
  component's call order, the three probe arms, the state trace, the allocation watch payload and the exception stacks.
  The title functions were decompiled with the project's Ghidra tooling from the user's own image; the host CRT is the
  system's own module. No peer code was used and no game data is added: RVAs, VAs, field offsets, register values, counts,
  times and image means only.

### 15.8 Evidence and uncommitted files

- **Source change**: `port/src/main.cpp` only (the `title_read_u32_at` helper, the two movie state-trace lines and the
  `PALMETTO_MOVIE_PROBE=player2` group). Opt-in; the default path is unchanged.
- **This page**: the section above. `docs/README.md` needs one index row; that file is outside this slice's write scope,
  so the lead adds it.
- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-moviebink1`, `boot-moviepix2` and
  `boot-moviepix3` with `stderr.log`/`stdout.log`/`summary.txt`/`boot.jsonl`, `capture-moviebink1`,
  `capture-moviepix2`, `capture-moviepix3` (all-mean-0.00 captures + `index.txt`); the static decompiles under
  `D:\leonida-work\decomp\out-moviepix-steps`, `out-moviepix-trans`, `out-moviepix-trans2` and the earlier movie work
  under `D:\leonida-work\moviecrash` and `D:\leonida-work\decomp\out-movieui`/`out-movietrigger*`. No game data: RVAs,
  VAs, runtime pointers, register values, counts, times and image means only.
- **No git write**: the lead commits this slice's two uncommitted files (`port/src/main.cpp`,
  `docs/decomp_first_ui.md`). The tree otherwise has no uncommitted files from this slice.

## 16. Measured: a title-side row-stride patch makes the class-0xa upload allocate its 0x1000 bytes, the executor pass survives the window, and the movie decode is still not reached

**Status: measured with the user-approved opt-in title-side mitigation. The zero-byte request sections 14-15 named is
replaced by the 0x1000 the copy writes: an in-memory trampoline answers the title's row-stride switch `width * 0x10`
for class 0xa only, so the constant-upload helper asks its allocator for 0x1000 and writes
`{256, 1, 0x1000, 0x1000, block}`. With the switch on the block no longer touches the live command object and the
executor pass runs on for the rest of the window - two boots, 90.6 s and 175.0 s past the entry, killed by the harness
timeout, zero device losses - where the same binary with the switch off faults about a second after the same entry. The
movie decode is still not reached: frame-buffer registration `0x442F760` and per-frame decode `0x2EF0E0` have 0 hits in
both arms, every presenter capture is mean rgb 0.00, and the first visible frame is not reached. The mitigation is off
by default (`PALMETTO_TITLE_CLASS0A_SIZE`), installed after the image is mapped, and its scope and risks are
section 16.5.**

### 16.1 The ABI and the patch point: image+0x1555C10

The zero-size request's producer was already named statically and in a boot ([vertex_buffer_va.md](vertex_buffer_va.md)
sections 14-16, [decomp_memory.md](decomp_memory.md) section 9); this slice decodes the switch's exact ABI from the
image and patches its callers, not the title's data. The function is a leaf with no `.pdata` entry:

| item | measured |
|---|---|
| function | image+0x1555C10, `uint32_t row_stride(uint32_t class_and_flags /*ecx*/, uint32_t width /*edx*/)` |
| class | `class = ecx & 0xfff` (the first three instructions are `mov eax, ecx` / `and eax, 0xfff` / the compare ladder) |
| answer | bytes per row, by class: 1,2 -> `width * 4`; 3,4 -> `width * 3` rounded up to 4; 9 -> `width`; 0x15 -> `width * 2`; the 0x32-0x42 and 100/101/200/201 families -> their untiled/tiled answers; every other class, 0xa included -> 0 |
| shape | reads only ecx and edx, writes eax, touches no stack, branches only within its own body |
| references | three direct branches in the image: `call` at image+0x1555B85 inside the per-plane size helper image+0x1555B60, `call` at image+0x455965C (the per-plane stride write of the constant helper image+0x45594F0) and `call` at image+0x455AD4B inside a second per-plane helper image+0x455ACE0 that stores the row stride at `+8` of a descriptor; no rip-relative `lea`/`mov` of its address and no relocation entry pointing at it |
| class-0xa inputs | the class is the image's format-table record 8 (`0xa`), the row width is 0x100, and the copy writes `width << 4` = 0x1000 bytes |

The size the helper requests is `image+0x455A610`'s per-plane sum of `height * row_stride`; at class 0xa,
`height * row_stride(0xa, 0x100) = 1 * 0 = 0`, which is the measured `size 0x0` request. What the mitigation changes
is that one answer, `width * 0x10` for class 0xa: the class's format record says R32G32B32A32_FLOAT (16 bytes per
element) and the copy's own step is 16 bytes per element, so `height * width * 0x10 = 0x1000` at the measured width.
At any other width the same rule keeps the size and the copy's per-element step consistent; a fixed 0x1000 would only
fit the measured table. Every other class returns through the original bytes, so the other 15 format-table classes and
every other caller keep byte-identical answers.

### 16.2 The implementation and the switch

| change | what it is |
|---|---|
| new include-only header under port/src (`title_class0a.h`) | the measured constants (RVA, class mask, class, element size), the pure switch parser and class decision, the executable trampoline emitter, and install/uninstall with a scope guard |
| `port/src/main.cpp` | the opt-in install after `loader.load()` (image mapped, before any image code), the log line naming the RVA and the redirected call-site count, and a `ScopedPatch` that restores the call sites when the run's C++ scope exits |
| `port/test/code_hooks_test.cpp` | the pure switch/class checks, the trampoline on a synthetic original, and install/call-through/uninstall on a synthetic mapped image |

The trampoline is 30 bytes of x64: read `class = ecx & 0xfff`; for class 0xa return `edx << 4` in eax; for every other
class load the original function's address and `jmp` to it with ecx, edx and the stack untouched. It has no prolog, so
the Windows unwinder treats it as a leaf, which is correct on both paths. It uses no int3, no VEH and no debug register,
so it cannot collide with the `PALMETTO_SKIP_INT3` handler, the movie probes or any other switch hook; it is not on the
per-frame path.

The install walks the mapped image's committed executable regions (the same effective span rule the loader uses) and
rewrites only `E8`/`E9` rel32 sites whose target is exactly image+0x1555C10, recording each site's original displacement.
Uninstall writes those displacements back and flushes the instruction cache, so the image returns byte for byte; the
trampoline's page is left mapped by default because a thread could be inside its few instructions when a shutdown-time
uninstall runs (the test frees it explicitly once no site points at it). A title-initiated exit ends the process with the
patch in place, which is process-local: the mapped image is this process's private copy and nothing on disk changes. The
switch is off unless `PALMETTO_TITLE_CLASS0A_SIZE` is exactly `1` (the registry's `exact_one` contract); unset, empty,
and every other value leave the title's original bytes byte-identical. This is a title-side workaround for the experiment,
not a console-faithful emulation, and section 16.5 is part of the change.

### 16.3 The A/B

One boot per arm under the shared boot lock, the section-12 content environment from the arm script, `-Light`,
`PALMETTO_MOVIE_PROBE=player`, `PALMETTO_ALLOC_WATCH=1`, 120-240 s. A and B are one binary: sha256 first 16
`F7AC0564C163D5A7`. B3 repeated the on arm on the tree's gate build after the measurement (sha256 first 16
`2120E306743D2DBD`), which differs from the measured arms only in the install-failure cleanup: the measured path and
the switch are identical. The A baseline from the previous commit (`4238F141ED2FBD52`) is the section-15.4 boot,
quoted for the class-0xa record.

| arm | binary sha256 (first 16) | wall | stage 3 (run time) | BLOCK ALLOC request | descriptor entry | executor pass | play-step probes | stop / trace t | device lost | presenter captures |
|---|---|---:|---:|---|---|---|---|---:|---:|---|
| A: `boot-mitig-off4`, switch off | `F7AC0564C163D5A7` | 66.7 s | 64.2 s | **0x0** | stride 0, size 0 | faults about 1 s after the entry | - | `0x538D68E` read -1, t 65.14 | 0 | 12, all mean rgb 0.00 |
| A: `boot-mitig-off1..3`, switch off | `F7AC0564C163D5A7` | 63.7 / 63.2 / 54.2 s | none | - | - | - | - | `0x28CD3CC`, `VCRUNTIME140.dll+0x1CCAB`, `0x4D9E79C` | 1 each | - |
| B: `boot-mitig-on1`, switch on | `F7AC0564C163D5A7` | 150.2 s (timeout) | 58.6 s | **0x1000** | stride 0x1000, size 0x1000 | survives to the 150 s kill: 90.6 s past the entry, no fault | open 3 / playback 1 / decode 0 / registration 0 | none (killed) | 0 | 12, all mean rgb 0.00 |
| B: `boot-mitig-on2`, switch on | `F7AC0564C163D5A7` | 240.2 s (timeout) | 64.3 s | **0x1000** | stride 0x1000, size 0x1000 | survives to the 240 s kill: 175.0 s past the entry, no fault | open 3 / playback 1 / decode 0 / registration 0 | none (killed) | 0 | 12, all mean rgb 0.00 |
| B: `boot-mitig-on3`, gate build, switch on | `2120E306743D2DBD` | 120.3 s (timeout) | 54.6 s | **0x1000** | stride 0x1000, size 0x1000 | survives to the 120 s kill: 64.6 s past the entry, no fault | open 3 / playback 1 / decode 0 / registration 0 | none (killed) | 0 | 12, all mean rgb 0.00 |

The A baseline of the previous commit, same environment: `BLOCK ALLOC` request `0x0`, job-block `0xef0` (the block
held the live job), `ENTRY WRITTEN` stride 0 size 0, stop `0x13360A3` 0.1 s after the pass (section 15.4). So the
measured step is the request itself: 0x0 on the off path, 0x1000 on the on path, and the job no longer sits in the
copy's range. In `boot-mitig-off4` the same binary even produced the other placement (`job-block 0x0`, the zero-size
block 0xA0 after the job) and still faulted, at `0x538D68E` in the allocator's classed tree - the overrun of a 0x1000
copy into allocator metadata, the section-15 third stop family. The on arms' block sits after the job's 0xA0 bytes in
both boots (0xC0 and 0x120 bytes after its base), so the copy lands entirely inside the 0x1000-size chunk the helper
allocated and the executor's next call through the job header sees an intact object.

The on arms keep submitting: 13,437 `ExecuteCommandLists` in B1's window and 23,480 in B2's, i.e. the renderer loop
runs for the whole window after the pass that used to kill it; the probes' per-frame decode still sees nothing. The
three failed off attempts are the stage-3 race sections 10-15 record (and each lost the Vulkan device once); the clean
off arm for comparison is `boot-mitig-off4`.

### 16.4 The state and the pixels

The on arms' last state sample (B2, 238 s; B1 reads the same at 148 s): init 3 (last change 64/58 s), loading 2,
session 3, machine current 3 next 3; the movie slot is armed (entity non-null, active 1, armed 1, media id 0x540714f3)
and the player state byte is **0x9** with a non-null Bink handle and `open 1` - the playing state section 15 measured -
but `buffers 0`. The middleware play steps that do fire are the same two section 15 measured: metadata/probe open
`0x442EA70` 3 times and playback open `0x199EF20` once. Per-frame decode `0x2EF0E0` and frame-buffer registration
`0x442F760` (called only from inside the decode wrapper) have 0 hits, so the draw that would decode a movie frame is
never issued in the window; the executor pass that used to die completes, and the boot then keeps rendering the
loading frames.

Pixels: no title pixel is measured. The 12 presenter captures per on arm are 2564x1421 and every one has mean rgb
`0.00 0.00 0.00` (black) - the visible frame is a black target, unchanged from section 15. The 128x128 centre readback
on the content target reads the layer's own write-control colour (`51 102 204`, 16,384/16,384 non-black) on the
write-control draws and `0 0 0` on the others; that colour is the replay's control, not title content. So the first
visible frame milestone is not reached: the overrun is no longer what stops the boot, but the decode that would put a
frame on screen is still not called.

### 16.5 Risks and limits

- **Title-side, narrow, and named.** The patch changes one answer of one title function for one class. It is not a
  console-faithful emulation: no console evidence says the console's switch answers width*16 for 0xa; the inference is
  the image's own format record (16 bytes per element), this title's own 16-byte copy step, and every other case of the
  function being `width * bytes-per-element`. If the console's answer is different, the overrun would be the console's
  own bug and the mitigation is only a way to keep this experiment moving.
- **Image-specific RVA and shape.** The install trusts image+0x1555C10 to be the measured leaf and only redirects direct
  branches to that address. A different image build needs the function and its callers re-measured; with the switch off
  nothing changes. The third reference (image+0x455AD4B, inside the second per-plane helper image+0x455ACE0) is
  redirected too; whether that helper ever sees class 0xa is not measured, and no other consumer was measured.
- **The clean A arm is one boot.** Three further off attempts did not reach stage 3 (the section 10-15 race; each lost
  the device once), and the clean off arm did. The on arms reached stage 3 on both attempts and neither lost the device;
  that correlation with the device loss is measured but not claimed as causal (n=5, and the device loss is the host-GPU
  family the FSR pages record). A falsifier: an off boot that reaches stage 3 without a device loss, or a count of
  class-0xa stride queries before stage 3 (the third call site is the named candidate for a pre-stage-3 consumer).
- **The executor pass survives; the next wall is the decode gating, not this overrun.** The player stays at state 0x9
  with `buffers 0` and the draw that calls `0x2EF0E0` is never issued; whether that is the movie middleware's own
  gating or the loading state (loading is still 2 in this window) is not answered here.
- **The switch does not synthesise anything.** It changes no shim answer, allocates nothing itself and writes no title
  data; it only changes what the title's own switch returns for one class, in memory, for this process.

### 16.6 Tests and gates

- The extended `port/test/code_hooks_test.cpp` covers the switch parser, the class mask/decision and the stride
  (`211 checks, 0 failures`, run as the `code_hooks` ctest target), the trampoline on a synthetic original function (class
  0xa answered, every other class reaching the original unchanged, class bits masked, scaling with width), and
  install/call-through/uninstall on a synthetic executable image: both an E8 call and an E9 tail jump redirected, a
  decoy branch to another address left byte for byte, the original function's bytes untouched, byte-for-byte restore on
  uninstall, the default answer coming back after uninstall, re-install, a refusing install on a null image / an
  out-of-image RVA / a non-executable image, and the explicit free of the trampoline page.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the GPU-free gate subset exits 0 (`code_hooks`, `frame_dump`, `unimplemented_policy`,
  `graphics_record`, `graphics_pso`, `buffer_va_index`, `dstorage`, `xapu_chad`, `deferred_compile`, `arena_size`,
  `xbox_present`, `xs_proxy_mock`, `xmem` - 13 of 13), and the docs checks pass (this tracked section names
  `PALMETTO_TITLE_CLASS0A_SIZE` for the environment-variable check).

### 16.7 Evidence, uncommitted files, attribution

- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-mitig-off1`, `boot-mitig-off2`,
  `boot-mitig-off3`, `boot-mitig-off4`, `boot-mitig-on1`, `boot-mitig-on2` and `boot-mitig-on3` with
  `stderr.log`/`stdout.log`/`summary.txt`/`boot.jsonl`, and `capture-mitig-*`/`readback-mitig-*` for the arms that
  reached a capture. No game data: RVAs, sizes, register/pointer values, counts, times and image means only.
- **Uncommitted files**: the new include-only header under port/src (`title_class0a.h`), `port/src/main.cpp`,
  `port/test/code_hooks_test.cpp` and this section of `docs/decomp_first_ui.md`. No git write: the lead commits them.
- **Attribution.** The ABI decode, the trampoline, the install/uninstall, the tests and the six boots are this project's
  own work on the user's image and the measured chain of [vertex_buffer_va.md](vertex_buffer_va.md) sections 14-16 and
  [decomp_memory.md](decomp_memory.md) section 9. No peer code was used and no game data is added.

## 17. Measured: with the class-0xa mitigation the boot lives 300 s past stage 3, the session-3 loading record still never completes, and the presented frame stays black

**Status: measured with the approved opt-in mitigation, negative on the picture, with the new wall named.** Section 16
left the mitigation at two points: the executor pass survives and the picture is still a black target. This section
runs the long window the mitigation makes reachable: one clean 300.2 s light arm held stage 3 for 185 s with zero
exceptions and zero device losses, seven thousand command lists after the stage-3 entry, six full-4K content-target
readbacks, and thirty presenter captures. The state words never move past `init 3 / loading 2 / session 3`; the session-3
loading record's completion dword is still never observed; the movie player reaches state `0x9` with a non-null Bink
handle and `buffers 0`, while per-frame decode `0x2EF0E0` and frame-buffer registration `0x442F760` still have **0 hits**;
every capture and every full-target readback is mean `0 0 0`. The new wall is title-side and named: the frame thread
parks in the session-3 poll and the loading task blocks on a title semaphore, so the frame pump that would issue the
movie draw stops, while the loader keeps updating. Two later arms lost the host GPU device before stage 3
(`nvlddmkm` event 153), so boots stopped after the second loss per the task rule and the input A/B has no post-stage-3
arm; that is the honest state of the input question.

### 17.1 The arms and the environment

One binary for all three arms: SHA-256 `2120E306743D2DBD...`, the section-16 gate build. Each boot is one at a time under
`_setup/boot_lock.ps1` (`-Who aftermitig`), `-Light`, via `tooling/boot_report.py`; logs, captures and readbacks are
outside the repository. `long1` is the task's required movie arm; `long2` and `input1` add the session-record and
screen-owner diagnostics (and `input1` the scripted pad); `long2` also carries the hardware completion watch.

| arm | binary SHA-256 (first 16) | wall | stage 3 (state clock) | verdict / stop | device lost | presenter captures |
|---|---|---|---|---|---|---|
| `long1` | `2120E306743D2DBD` | 300.2 s (harness timeout) | 114.4 s (alloc watch read; state words 116.0 s) | ALIVE, 0 exceptions | 0 | 30 (frames 1-2901), every one mean rgb `0.00 0.00 0.00` |
| `long2` | `2120E306743D2DBD` | 81.6 s | not reached (last sample: init 2, loading 2, session 1 at 78 s) | STOP `0x0` execute (0xC0000005) | 1 | 21, every one mean rgb `0.00 0.00 0.00` |
| `input1` | `2120E306743D2DBD` | 58.1 s | not reached (last sample: init 2, loading 2 at 54 s) | STOP `game+0x86f981` read `0x962a` (0xC0000005) | 1 | 16, every one mean rgb `0.00 0.00 0.00` |

The environment is the section-12/20 content environment from the boot template (deferred compile, stage-3 pump, 2 s
state trace, alloc watch, console graphics root feed `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, slot-70 publication
`PALMETTO_SLOT70_PUBLISH=1`, content probe `PALMETTO_CONTENT_PROBE=1`, full-target readback `PALMETTO_READBACK_FULL=1`)
with these additions: the mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1` in every arm; `PALMETTO_MOVIE_PROBE=player` in
`long1`; `PALMETTO_SLOT_WATCH=1`, `PALMETTO_DS_RECORD=500,300` and
`PALMETTO_POKE_LOG=5A75F80,5BB5390,2234A40,3DAF310` in `long2`; the same poke log and record sampler plus
`PALMETTO_FAKE_INPUT=70` in `input1`; captures every 100 presented frames to a limit of 30 (the template's 200 and 12
are overridden by the later value); stage-3 pump offsets overridden to `5,20,60,120,180,240` in `long2`/`input1`;
`PALMETTO_WAIT_TRACE=120,200,280` in `long2`/`input1`. The template carries the content environment's
`PALMETTO_SILENT_SPEECH=0`, `PALMETTO_USER_CHANGE_EVENT=0` and `PALMETTO_PAD_PRESENT=0`, so this page's speech create
answers `E_FAIL` and the neutral pad is absent; the scripted pad is `input1`'s only pad.

The host's GPU dropped the device in the two later arms: the System log records `nvlddmkm` event 153 errors at the two
boots' windows, and each `stderr.log` carries one `replay: the Vulkan device was lost` line before its stop. The
`long1` window (16:07-16:12 host time) has no such record. Two device losses in a row ended the boots per the task's
rule, so `long3` (the input A/B baseline) and a movie-draw arm were not run.

### 17.2 The long-run state table

`long1`'s state words, selected samples of the 149 the 2 s trace took (the state trace and the layer timeline clocks
agree to 0.4 s or better; the table's `t` is the state trace's process clock). `tick` is the loading worker's own
progress dword.

| t (s) | init | loading | session | machine current/next | boot-media gate | loading tick | movie player state / buffers |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 0.0 | 0 | 0 | 0 | 0/0 | 0x0 | 0 | 0x0 / 0 |
| 4.0 | 1 | 0 | 0 | 0/0 | 0x2 | 0 | 0x0 / 0 |
| 22.0 | 2 | 1 | 0 | 2/2 | 0x2 | 14404147 | 0x0 / 0 |
| 60.0 | 2 | 1 | 0 | 2/2 | 0x2 | 14441877 | 0x0 / 0 |
| 100.0 | 2 | 1 | 0 | 2/2 | 0x2 | 14482443 | 0x0 / 0 |
| 112.0 | 2 | 2 | 1 | 2/2 | 0x2 | 14494463 | 0x3 / 0 |
| 116.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14497733 | 0x5 / 0 |
| 118.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14499919 | 0x9 / 0 |
| 140.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14520158 | 0x9 / 0 |
| 200.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14582434 | 0x9 / 0 |
| 260.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14642433 | 0x9 / 0 |
| 298.0 | 3 | 2 | 3 | 3/3 | 0x2 | 14680460 | 0x9 / 0 |

The measured changes are exactly four: `init 0 -> 1` and `boot-media gate 0x0 -> 0x2` at 4.0 s, `init 1 -> 2` and
`loading 0 -> 1` at 22.0 s, `loading 1 -> 2` and `session 0 -> 1` at 112.0 s, and `init 2 -> 3`, `session 1 -> 3` and the
machine's pending stage `2 -> 3` at 116.0 s (the alloc watch reads the machine's stage 3 at 114.4 s). Nothing moves in
the 180 s after that. The loading tick advances about 1,000 counts a second for the whole window, so the loading update
keeps being driven; the loader is running, not hung. The front-end manager the movie/UI request path uses is non-null,
ready and running from the 26.0 s sample with its seven service pointers and vtables unchanged to 298.0 s.

The stage-3 pump samples the other threads after the stage-3 read. At every offset (`+2`, `+5`, `+10`, `+20`, `+40` s)
the frame thread is in the session-3 poll chain `[session3-poll session-dispatch stage3-post transition pump-loop
machine-wrapper tick-dispatch frame-pump frame-update]` waiting on `NtDelayExecution` with a 33 ms timeout - the measured
`Sleep(33)` of the poll at `game+0x21B7070`. The loading task is in
`[frame-tick load-monitor-tick loading-update loading-task task-executor loading-worker]`, and from `+5` s it waits with
`NtWaitForSingleObject`, infinite timeout, on a title semaphore (handle `0x950`). The pump's heartbeat advance count is
16 at `+5` s and 17 by `+40` s (its last change 16.8 s before that sample), i.e. the per-frame tick that pets the
heartbeat stops advancing while the renderer queue keeps being submitted: the layer's timeline records 12,913
`ExecuteCommandLists` entries, 7,309 of them after the stage-3 read, the last at 299.2 s. So the title keeps rendering
while the frame thread that drives the loading/movie path sits in the poll. The loading task's wait chain is title
frames only (`loading-update` -> `loading-task` -> `task-executor` -> `loading-worker`); no layer call is between the
task and the wait.

### 17.3 The session-3 loading record: the completion never arrives

The session-3 poll `0x21B7070` reads the loading record's slot-4 completion dword `image+0x78ACB20` and, when the
predicate `0x41E8C0` is true, consumes it in one pass and stores session 4 (section 10.4). In `long1` the session word
reads 3 from 116.0 s to the last sample at 298.0 s, so the title never observed the predicate true in that window. The
two shorter arms add direct dword samples before their stops:

| witness | arm | window | measurement |
|---|---|---|---|
| session word (the predicate's consumer) | `long1` | 116.0-298.0 s | 3 at every sample; a true predicate would be consumed in one 33 ms pass and store 4, so no true read |
| `PALMETTO_DS_RECORD` slot 0..4 tick/completion/state | `long2` | +0.0 to +73.3 s, 146 samples | every completion dword 0; no `SLOT n COMPLETION` line; slot 0 tick 14,766,572 at the last sample |
| the same sampler | `input1` | +0.0 to +51.0 s, 102 samples | every completion dword 0; slot 0 tick 14,858,270 at the last sample |
| hardware completion watch (DR0 on slot 4 `+0x18`) | `long2` | 1.1-81.6 s, 104 threads armed | completion hits 0 at every 10 s counter; method control 3 hits at 1.1 s; live control 4 hits at 75.9-78.4 s (`game+0x41E8FD`, the loading pass's slot-0 tick writes) |

The watch's live control firing is the method proof: the debug registers are honoured and the watch catches title stores
into this record, yet no store into the completion field arrives in its window. `long2` stopped before stage 3, so the
post-stage-3 window's direct dword watch is not measured; the session word is its witness, and the poll's own store is
what a true predicate would consume. This reproduces and extends sections 10-11: no shim answer or shim-held completion
is in this path, and the same semaphore wait the loading task is parked on is the next object to name.

### 17.4 The movie chain after the mitigation

The movie module is ready from 26.0 s (`movie ready 1`, its player-count word reads 58), and the game-side request binds
slot 0 at the stage-3 transition: at 112.0 s the slot shows entity non-null, `active 1`, `armed 1`, media id
`0x540714f3`. The player state then advances and holds:

| probe / field | measured in `long1` |
|---|---|
| player state `entity+0x1c4` | 0x3 at 112.0 s (metadata-probe result 1, playback open 1), 0x5 at 116.0 s, **0x9 from 118.0 s to 298.0 s** |
| Bink handle `entity+0x50` | non-null from 118.0 s, unchanged |
| decode gate `+0x1b8` (`buffers`), probe/open/error bytes | `buffers 0`, `probe 0`, `open 1`, `error 0` at every sample from 120.0 s to 298.0 s |
| play-step probe `0x442EA70` (metadata/probe open) | 3 hits (before the final open) |
| play-step probe `0x199EF20` (playback open) | 1 hit |
| play-step probe `0x2EF0E0` (per-frame decode) | **0 hits** |
| play-step probe `0x442F760` (frame-buffer registration) | **0 hits** (its only caller is inside the decode wrapper) |
| class-0xa constant upload (alloc watch) | entry `{width 256, height 1, stride 0x1000, size 0x1000}`, block size 0x1000, `overlap no`, executor entry at 155.5 s (timeline clock) and no fault after it; mitigation reports 3 redirected call sites |

So the mitigation does what section 16 measured - the executor pass that used to die completes and the boot keeps
submitting - and the movie open completes even though it happens after stage 3 (the player reached 0x9 at 118 s, around
the stage-3 read at 114.4 s). The decode is still not reached: the movie's own draw path, which is what calls the decode
wrapper, is not issued while the frame thread is in the session-3 poll. The decode gate is consistent with that: number
of frame buffers 0, and the player is at the playing state waiting for the first draw.

### 17.5 The content window and the pixels

After stage 3 `long1` records two content windows of three draws each (draws 5630-5632 at about 116 s and 5633-5635 at
about 136 s, state clock). The content probe prints each window draw's pipeline, its PS presence (`0 image(s)` against
77/157/213 loads for the first window), its 3840x2160 area and its viewport. The full-target readback (armed at those six
draws) reads each target completely:

| readback | draws | measurement |
|---|---|---|
| render-target (full 4K, 4-byte texel) | 5630, 5631 | mean `0 0 0`, non-black `0/8294400`, peak 0, alpha mean 0 |
| render-target (full 4K, 4-byte texel) | 5632 | mean `0 0 0`, non-black `0/8294400`, peak 0, alpha mean 0 |
| render-target (full 4K, 4-byte texel) | 5633, 5634, 5635 | mean `0 0 0`, non-black `0/8294400`, peak 0, alpha mean 0 |
| 8x8 tile map | all six | all-zero |
| presenter captures | 30, frames 1-2901 | every one 2564x1421, mean rgb `0.00 0.00 0.00` (black) |

The readback wrote 4 PPM files (the template's PPM limit) under the arm's readback directory. The layer's write-control
draw was skipped in this arm ("the image is an armed sampled input"), so no control colour is quoted here; section 16's
on arms measured the `51 102 204` write control on the same content targets. The matrix-block readbacks the same arm
prints are descriptor-table contents, not pixels: 96 and 64 entries read all-zero, and one 5,768-entry block reads mean
4 with 214 non-black entries and peak 255 - a table, not a target. So no title pixel is measured and the first visible
frame is not reached: the visible target is black, as it was in section 16.

### 17.6 The UI path and the screen owners

The four screen-owner entry probes (accessibility `0x5A75F80`, sign-in `0x5BB5390`, profile-name text entry `0x2234A40`,
profile card `0x3DAF310`) run in `long2` and `input1`; the title-side user trace runs in all three arms.

| screen / step | `long2` (hit, state clock) | `input1` (hit, state clock) | the runtime call it makes |
|---|---|---|---|
| accessibility boot step `0x5A75F80` | 1 hit at about 20.0 s | 1 hit at about 16.0 s | `XSpeechSynthesizerCreate` answers `E_FAIL` under this environment (`PALMETTO_SILENT_SPEECH=0`) |
| user sign-in request `0x5BB5390` | 1 hit at about 22.0 s | 1 hit at about 18.0 s | `XUserAddAsync`/`XUserAddResult` once: +24.57 s / +20.24 s (user-trace clock) |
| profile-name text entry `0x2234A40` | 0 hits | 0 hits | `XGameUiShowTextEntryAsync` never called |
| profile card `0x3DAF310` | 0 hits | 0 hits | `XGameUiShowPlayerProfileCardAsync` never called |

The long arm's user trace (no poke log in that arm) repeats the same shape: `XUserAddAsync`/`XUserAddResult` at
+27.00 s and the 9x user-ready refresh, plus one persistent-storage path and the one speech create. The front-end
manager is non-null, ready and running from 26.0 s with its seven services. So a UI service path executes and the
sign-in screen owner runs, but neither changes a stage word nor a pixel: the presented frames before and after it are
black. That is the same negative section 12 measured, now with the sign-in owner observed in a boot that reaches stage 3
rather than one that stays at stage 2.

### 17.7 The scripted-input arm

`input1` is the always-built input backend's scripted arm: `PALMETTO_FAKE_INPUT=70` attaches one scripted pad (30
presses, 1 s apart, 150 ms each, first at +70.000 s; confirm on even presses, start on odd ones) and the boot log shows
the pad connected on slot 5, the device callback enumerating it, and two first-run reads (`IGameInput::GetCurrentReading`
and `IGameInputReading::GetGamepadState`). The arm then stopped at 58.1 s, before the first press: the host device loss
happened first and the stop is `game+0x86f981` reading `0x962a`. So no confirm/start press reached the title, no
post-stage-3 input effect is measured, and no A/B input result is claimed. The stream to compare is named for the next
arm: the same environment with the presses at +20 s (the first-run window) and at +70 s (the post-stage-3 window), and
the state words, screen-owner hits, movie player state and presenter captures compared with `long2` and `long1`.

### 17.8 Verdict, the new wall, and the next falsifier

- **The mitigation's long-run verdict is measured.** The class-0xa overrun no longer stops the boot: `long1` held stage
  3 for 185 s, the layer received 7,309 command lists after the entry, and 0 exceptions and 0 device losses were
  recorded. The step over section 16 is time and state: 300 s of boot, 149 state samples and thirty captures instead of
  the 90-175 s windows that stopped at the harness kill.
- **The wall is now the session-3 loading record's completion, and it is title-side.** The frame thread is in the
  session-3 poll at every stage-3 pump sample, the loading task is parked on a title semaphore, the loading tick keeps
  advancing, and the completion dword is never observed non-zero. No layer answer is in the chain (the storage path was
  measured complete in section 10.5 and the executor allocation is intact in section 16), so no shim producer is
  measured to fix and the default path is unchanged.
- **The picture is still not reached.** The movie open completes to state `0x9`, but its draw path never issues the
  decode (`0x2EF0E0` and `0x442F760` 0 hits, `buffers 0`), the content window's six full-4K targets read black, and all
  30 presenter captures are black. The first visible frame remains the milestone.
- **Next falsifiers, in the order they would decide the wall.** (a) The hardware completion watch (DR0 on
  `image+0x78ACB20`) through a post-stage-3 long window on a stable host - the arm that would have carried it (`long2`)
  lost the device before stage 3, so raise the survival odds by running it as the only heavy probe in that boot. (b) An
  execution probe on the streaming subscriber start `0x2355DD0` and the record reset `0x22310D0`, printing the event
  payload the reset consumed (section 11.6's named falsifier), to date the job whose completion the poll is waiting for.
  (c) A probe of the movie's own draw path - layer entry `0x23495A0`, player draw `0x2349D80`, second per-frame advance
  `0x51B45B0` - which separates "draw never issued" from "draw issued and skipped by the frame-ready flag". (d) A wait
  trace of the loading task's semaphore `0x950` to name its signaller.
- **Limits.** One clean long arm; stage 3 arrived at 114.4 s in it (the stage-3 reach is the environmental race sections
  10-16 record), and the two other arms are losses, not measurements of their switch. The completion-dword witness after
  stage 3 is the session word, not a direct dword sample; the direct sample is the named next arm. The boot template's
  content environment disables the section-12 speech and pad defaults, so the UI sequence measured here is that
  environment's, not the clean-default one. The host GPU's event 153 losses are the FSR pages' host condition, not a
  property of this tree.

### 17.9 Tests and gates

- **No code changed.** This slice is measurement plus this page: no shim, no probe, no build file and no test target was
  edited, so the default path and every switch's default are byte-identical. The binary measured is the section-16 gate
  build `2120E306743D2DBD...`, the same one the clean-tree gate builds after the measurement.
- **Gates on this tree**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` (`ninja: no work to do`, so the binary is the
  tree's); `_setup\build_tests.cmd` prints `TESTS_BUILT`; the GPU-free gate subset exits 0 - `code_hooks`, `frame_dump`,
  `unimplemented_policy`, `graphics_record`, `graphics_pso`, `buffer_va_index`, `dstorage`, `xapu_chad`,
  `deferred_compile`, `arena_size`, `xbox_present`, `xs_proxy_mock`, `xmem` (13 of 13) - and the docs checks
  (`tests/test_docs_*.py`, run with the session's broken `GIT_CONFIG_*` variables cleared) pass (12 of 12 files). The
  pre-existing quarantined `d3d12_fence_timing_test` is not part of the gate subset ([testing.md](testing.md) line 95).

### 17.10 Evidence, uncommitted files, attribution

- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-long1` (stderr 45 MB, stdout, summary, boot
  trace, meta with the exact environment), `capture-long1` (30 presenter PPMs + `index.txt`),
  `readback-long1` (4 full-target PPMs), `boot-long2`, `capture-long2`, `boot-input1`, `capture-input1`, and the
  analysis scripts under `D:\leonida-work\aftermitig` (`run_arm.ps1`, `analyze.py`, `details*.py`). The host's
  `nvlddmkm` event 153 records are quoted by time only. No game data: RVAs, sizes, pointer/register values, counts,
  times and image means only.
- **Uncommitted files**: this section of `docs/decomp_first_ui.md` only. `docs/README.md` needs one index row for the
  page's new section; that file is outside this slice's write scope, so the lead adds it. No git write.
- **Attribution.** The arms, the state tables, the record samples, the completion watch, the screen-owner probes and the
  captures are this project's own measurements on the user's own image and the chain of
  [decomp_stage.md](decomp_stage.md) sections 10-11 and [vertex_buffer_va.md](vertex_buffer_va.md) sections 14-16. No
  peer code was used and no game data is added.

## 18. Measured: the decoder's async-service field HBINK+0x1b8 is its open block allocator's answer; the device-loss arm's console-range pointer is not a shim answer and does not reproduce on a clean arm

**Status: measured, negative for a runtime-answer fix, with the fault site attributed statically and its field origin
measured on a clean arm.** Section 17 left the movie player at state `0x9` with `buffers 0`, the per-frame decode wrapper
`0x2EF0E0` and frame-buffer registration `0x442F760` at **0 hits**, and the decode not issued. The open task was the one
movie arm that did enter the decoder and fault: `boot-open3` stopped inside `Bink2Scarlett.dll+0x5de9` reading
`0x1660000005c` after a Vulkan device loss ([decomp_stage.md](decomp_stage.md) section 15.5). This section attributes the
read statically (the decoder's async-service worker walking its open-context list), measures where the field comes from
with a new opt-in probe in a clean 300.4 s arm, and reports the negative: on the clean arm the field is a valid pointer
into the block the decoder's own open allocated through the title's memory callbacks, the service thread pumps it without
a fault, no shim answer produces the console-range value, and no shim is changed. Decode and registration still have
**0 hits**, `buffers` stays 0 and the presented captures stay black, so no movie pixel is claimed.

### 18.1 The fault site, statically

`Bink2Scarlett.dll` is the package's own Bink 2 decoder (`SizeOfImage` `0x44000`), mapped by the loader at
`0x15a76800000` in `boot-open3`. The exception the boot reported has four frames only, the decoder's own service thread:
`+0x5de9` <- `+0x23ece` <- `host:KERNEL32.DLL+0x2dda7` (BaseThreadInitThunk) <- `host:ntdll.dll+0xc9c5c`
(RtlUserThreadStart), i.e. a thread the decoder created itself, not a title frame and not a shim call.

| item | measured |
|---|---|
| containing function | RVA `0x5d30..0x5eed` (exception-directory range), the decoder's async-service worker: it takes its global lock at `0x237a0`, walks the decoder's open-context list (global list head RVA `0x3e8c8`, next node at context `+0xb8`) and, for a node with `+0x4c == 0` and `+0x12c != 0`, reads the node status dword and runs the pump `0x8570` |
| faulting instruction | `0x5de9` = `mov ecx,[rax+0x5c]`, preceded by `0x5de2` = `mov rax,[rbx+0x1b8]`; `rbx` is the list node (the decoder's `HBINK` context), so the reported read address `0x1660000005c` means `[HBINK+0x1b8]` = `0x16600000000` |
| the field | `HBINK+0x1b8` is the base of a `0x1c0`-byte-stride array whose count is `HBINK+0x12c`; entries carry status words at `+0x4c`/`+0x5c` and callbacks at `+0x170`/`+0x178`/`+0x180` (decompiled teardown `0x2418`, pump `0x8570`/`0x85f6`/`0x873a`). So the fault is on the decoder's per-track structure array, not on compressed video and not on a frame buffer; `+0x5c` is a track status dword |
| worker caller | thread trampoline RVA `0x23ec8` (`mov rcx,[rdi+8]` / `call [rdi]`), which is the `+0x23ece` return address in the exception stack |

### 18.2 Where HBINK+0x1b8 comes from (static)

`BinkOpenWithOptions` is export RVA `0x4710` and tail-jumps to the open `0x67c0..0x8357`. The open memsets a `0x66c`-byte
stack template (`0x6868`), registers the track-array slot `lea rdx,[rbp+0x178]` (template `+0x1b8`) with size
`max(count,1) * 0x1c0` through the block builder `0x25b20` (`0x6e31`), allocates the whole context block at `0x6f0c` with
`0x25a40`, fills the template slot from that allocation, and copies the template over the context at `0x6f83`
(context = `(block+0x100) & ~0xff` at `0x6f6e`). The value is therefore **the open block allocator's answer for the
track-array slot**: `block + slot offset`.

Two measurements make that attribution complete:

- **No decoder instruction writes `[reg+0x1b8]`.** An exhaustive capstone scan of every `.pdata` function plus a raw
  scan for the disp32 bytes `b8 01 00 00` found only loads and `add` uses of the field (the one dword store, RVA
  `0x18767`, is a stack local in another function; the two `lea rbp+0x1b8`, RVAs `0x19ffd`/`0x20d8e`, are other frames).
  The only writer is the block builder's slot table, writing through the registered stack-slot address during `0x25a40`.
- **The allocation is title memory, not a shim answer.** The title installs the decoder's callbacks at its movie module
  init RVA `0x577f500`: `BinkSetOSFileCallbacks` with `0x1946220`/`0x92ebc0`/`0x442e890`/`0x1a400f0`, and
  `BinkSetMemory(&UNK_143a1a170,&UNK_14169c4d0)`. The malloc wrapper RVA `0x3a1a170` is `mov edx,0x20; jmp 0x4f9e90`,
  and `0x4f9e90` allocates through the title's own allocator (small path) or its async/virtual path (>= `0x20000`). The
  decoder's XMem/File imports are not called in any measured boot (`PALMETTO_XMEM_CALLERS`/`FILE_TRACE` negative,
  section 15.5), and no shim sits between the allocation and this field.

### 18.3 The diagnostic this slice adds

`PALMETTO_BINK_PROBE=1` (opt-in, off by default; the default path is unchanged) puts four `int3` points in the mapped
decoder after the loader maps it and uses the same restore / single-step / re-poke discipline as the movie probes. The
base is read from the decoder's own export table (`BinkOpenWithOptions`, export RVA `0x4710`) through the loader's
bundled-module lookup, so every printed address is `bink+<RVA>`. A handler prints the registers, the containing site,
the caller and the decoded fields; the worker site is disarmed after 64 hits and the log is capped at 24 lines.

| site RVA | where it fires | what it prints |
|---|---|---|
| `0x6c33` | first open path, right after its context block is allocated | `rax` block, `rbx` context |
| `0x6f11` | second open path, right after its context block is allocated | `rax` block; the template's track slot at `[rbp+0x178]` (the value the copy will put at `HBINK+0x1b8`) |
| `0x6f88` | second open path, right after the template copy | `rdi` context and its `+0x0/+0x4c/+0x12c/+0x1b8`, plus `[+0x1b8]+0x5c` when readable |
| `0x5de2` | the service worker's load of `HBINK+0x1b8` | `rbx` node and its `+0x0/+0x4c/+0x12c/+0x1b8/+0xb8`, plus `[+0x1b8]+0x5c` |

### 18.4 The clean arm

One arm, `binkprobe1`, one boot under `_setup/boot_lock.ps1`, the section-12 content environment from the template,
`-Light`, 300 s, plus `PALMETTO_TITLE_CLASS0A_SIZE=1`, `PALMETTO_MOVIE_PROBE=player` and `PALMETTO_BINK_PROBE=1`.
Binary sha256 first 16 `57EFCB2E729B6622` (this slice's probe build). Logs and captures outside the repository.

| measure | value |
|---|---|
| wall / verdict | 300.4 s, **ALIVE** (harness timeout), 0 exceptions, **0 device losses** |
| stage 3 | init 3 / session 3 last change 98.0 s, loading 2 |
| movie player | 0x3 at 96.0/98.0 s (probe 1, open 1, handle 0), 0x6 at 100.0 s, **0x9 from 102.0 s to 298.0 s**, handle `0x1805acdb600`, `buffers 0`, probe 0, open 1, error 0 |
| play-step probes | metadata/probe open `0x442EA70` **3**, playback open `0x199EF20` **1**, per-frame decode `0x2EF0E0` **0**, frame-buffer registration `0x442F760` **0** |
| presenter captures | 12 (frames 1..2201), every one 2564x1421 mean rgb `0.00 0.00 0.00` (black) |

The new probe measured the field's origin and the service thread's use of it:

| site | measurement |
|---|---|
| `0x6f11` / `0x6f88` | 4 hits each: the metadata/probe open (`block 0x1806fa4dee0`, template slot `0x1806fa502c0`) and the playback open (`block 0x1805acdb560`, context `0x1805acdb600`, template slot and `context+0x1b8` = `0x1805acdd940`); the slot is **block + 0x23e0** in both opens; `+0x12c` reads 0 at the copy point (the count is stored later by `0x791c`) |
| `0x5de2` | 16 logged hits starting 0.1 s after the playback open: the node **is the playing handle** `0x1805acdb600`, `+0x12c` = **1**, `+0x1b8` = `0x1805acdd940` (readable), `[+0x1b8]+0x5c` = **0**, next node 0; the worker pumps the track and loops, no fault |

So on the clean arm `HBINK+0x1b8` is a valid pointer into the very block the decoder's open allocated through the title's
memory callbacks, at a constant `+0x23e0` slot offset for this open's layout, with one track and status 0. The service
thread runs, and the whole boot holds the playing state for 196 s with no decoder exception.

### 18.5 The device-loss arms and the pointer's shape

- **`boot-open3`** (section 15.5's arm; pre-slice binary `2A553C49...`): 97.8 s, one device loss, movie player state `0x9`
  next `0x3` handle `0x16681428100` at the 90.0/92.0/94.0 s samples. `stderr` carries `[rt] replay: the Vulkan device was
  lost` after the 94.0 s sample (the sampler's own fine line reads 92.7 s) and the exception is trace t 96.25 s, so the
  fault is inside the device-loss window. The crashed thread is the decoder's own service thread (stack above), and the
  movie player was still holding a live handle at state `0x9`.
- **`boot-binkprobe2`** (this slice's probe build; the section-17 content environment plus the stream-open probe and the
  Bink probe, 182.5 s, one device loss): the decoder open ran and the probe measured the field valid there too
  (`block 0x2efb306d3a0`, template slot `0x2efb306f780` = block + `0x23e0`, context `0x2efb306d400`, count 0 at the copy
  point). The movie player sat at state `0x3` next `0x3` handle 0 with probe 1/open 1, so the service worker never
  reached a nonzero-count node, and the boot died in the host compute driver, `host:nvgpucomp64.dll+0x3eac474` reading
  `0x48`, after the device loss. That arm did not reach the Bink fault.
- **The crash value's shape.** In `boot-open3` every title-side address reads `0x166...` (slot0 entity `0x1667e4ef350`,
  stream `0x1667e248520`, handle `0x16681428100`), and `0x16600000000` is exactly that range with the low 32 bits zero. In
  the clean arm the same class of value would be `0x18000000000` (the title's ranges read `0x1805...`/`0x1806...`). The
  crash value is therefore the high half of that boot's title VirtualAlloc range, **not** a value any measured allocator
  answer produces: the XMem layout arm (`boot-xmemlayout1`) shows the shim's answers are ordinary host VirtualAlloc bases
  (`0x1da37ce0000`, `0x1e037ce0000`, `0x1e6...`, `0x1e8...` - none 4 GiB-aligned), and the decoder's allocations never
  enter the shims at all (title callbacks, 18.2).

**Negative: the writer of the crash-time value is not measured.** The clean arm reads the field valid at the open and at
the worker; no decoder instruction stores it; the open's allocator answer is `block+0x23e0`; and the crash happened only
in a device-loss window on a boot whose player still held state `0x9` and a live handle. The two mechanisms left are
(a) a torn-down/reused context: after the device loss the title's teardown or another allocation rewrites the context
memory while the decoder's service list still holds the node (a reused `+0x12c` would make the worker take the branch),
or (b) a second decoder context from a different allocation pool whose field already holds that shape. Neither is
measured, so no shim is changed.

### 18.6 Verdict, limits and the next falsifier

- **Verdict: the fault site is attributed and the field's origin is measured on a clean arm; the crash value is not a
  runtime answer of this tree and does not reproduce without a device loss.** No shim, resource, view, presenter or file
  answer is on the decoder's allocation path (the title supplies the allocator itself), so there is nothing for this
  slice to fix; the default path is byte-identical and the only source change is the opt-in probe.
- **Limits.** One clean arm. The one crash boot (`open3`) predates the probe, so it carries no in-decoder evidence; the
  second probe arm (`binkprobe2`) carried the probe but did not reach the same site. The value's shape is an
  observation (that boot's title-range high half), not a named writer. No decoder close/teardown probe and no service-list
  membership read ran; the worker-node site logs 16 lines before it is disarmed.
- **Next falsifier, in the order it would decide the reading.** (a) A boot with `PALMETTO_BINK_PROBE=1` that reproduces
  `Bink2Scarlett.dll+0x5de9` in a device-loss window: its `0x5de2` line names the node and shows `+0x12c`/`+0x1b8` at the
  fault, which separates "the playing handle's own context" from "a stale/reused node". (b) A hardware write watch on
  `HBINK+0x1b8` (and `+0x12c`) armed at `0x6f88` and left through the device-loss window, with the clean arm's 0x5de2
  hits as the live control: a named store is the producer, no store at all says the node is stale. (c) A decoder
  close-path probe (the title's teardown calling `BinkClose`, export RVA `0x2400`) reading the service-list membership
  word `+0x1d4` before the free. A clean arm that ever shows this fault would falsify the device-loss reading.
- **The picture is unchanged.** The playing state, `buffers 0` and the 0 hits on `0x2EF0E0`/`0x442F760` are the same
  result section 17 measured in a longer window; the decode wrapper is what would call `BinkRegisterFrameBuffers`, and it
  is never issued while the movie draw path is parked. Every capture is black, so the first visible frame is not reached.

### 18.7 Tests and gates

- **No test target changed.** The slice adds one opt-in diagnostic probe in `port/src/main.cpp` (no shim, no pure helper,
  no build target); the default path and every switch's default are unchanged.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints
  `TESTS_BUILT`; the GPU-free gate subset exits 0, 13 of 13 (`code_hooks`, `frame_dump`, `unimplemented_policy`,
  `graphics_record`, `graphics_pso`, `buffer_va_index`, `dstorage`, `xapu_chad`, `deferred_compile`, `arena_size`,
  `xbox_present`, `xs_proxy_mock`, `xmem`); the docs checks (`tests/test_docs_*.py`, run with the session's broken
  `GIT_CONFIG_*` variables cleared) pass, and this section names `PALMETTO_BINK_PROBE` for the environment-variable check.

### 18.8 Evidence, uncommitted files, attribution

- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-binkprobe1` and `boot-binkprobe2` with
  `stderr.log`/`stdout.log`/`summary.txt`/`meta.txt`/`boot.jsonl`; `capture-binkprobe1` (12 presenter PPMs +
  `index.txt`); `boot-xmemlayout1` (the XMem served-range layout arm); `boot-open3` (the crash arm, pre-slice binary).
  The static work is under `D:\leonida-work\binkdecode`: the local PE parse and export/function map of the decoder, the
  capstone scans of the `0x1b8` field and the open's slot registrations, the Ghidra decompiles of the decoder's worker,
  open, block builder and close/pump family, and of the title's Bink memory/file callbacks (`0x577f500`, `0x3a1a170`,
  `0x4f9e90`, `0x169c4d0`). No game data: RVAs, field offsets, register/pointer values, counts and image means only.
- **Uncommitted files**: `port/src/main.cpp` (the `PALMETTO_BINK_PROBE` diagnostic) and this section of
  `docs/decomp_first_ui.md`. `docs/README.md` needs one index row for the page's new section; that file is outside this
  slice's write scope, so the lead adds it. No git write.
- **Attribution.** The arms, the probe, the field measurements, the allocations and the captures are this project's own
  measurements; the title functions and the package decoder were decompiled locally from the user's own image with the
  project's Ghidra tooling. No peer code was used and no game data is added.

## 19. Measured: the UI 2D-batch route and the media/UI present body

### 19.1 The arm and probe

This is the one authorized light arm that fills the 2D-batch gap in section 14: it uses the class-0xa mitigation and the
same content environment, then adds the bounded `PALMETTO_UI_PROBE=draw` counters and `PALMETTO_PRESENT_ATTEST=1`.
There was no scripted input and no movie probe. The four measured instruction prefixes were validated before any trap;
all four sites armed and none was refused. The stage split reads the title's game/stage word at `0x7446510`; each site
stops counting and restores its instruction at 4,096 hits. The probe records entry counts only, not caller, arguments,
stack or drawn content, so it cannot attribute these calls to a particular screen owner.

The UI draw reporter's `t` field is absolute `steady_clock` seconds. I normalized its samples against the run's
`timeline origin`; the `<3`/`>=3` buckets are read from the live title word and do not depend on this clock conversion.

### 19.2 Timeline and site results

| run-relative time | observation |
|---:|---|
| 0 s | `PALMETTO_UI_PROBE=draw` armed batch-build `0x517A290`, rectangle-fill `0x3A1B860`, transform-copy `0x58C8950` and batch-submit `0x2AACF0`: 4/4, 0 refused. |
| 2 s | Present attestation: `body-entries 166`, `presents 0`; gate `0x2`, both present-pass keys `0x0`, state `body-exits-on-keys`. |
| about 10/20 s | All four UI draw counters remain 0 in the snapshots. The game/stage word advances through stages 1 and 2. |
| about 30 s | `batch-build=404` and `batch-submit=404`; rectangle-fill and transform-copy remain 0. |
| 32.1 s | Present attestation: `presents 273`, `submits 547`, `frame-callbacks 274`, `body-entries 1,632`; both keys remain 0 and the body still exits on keys. |
| 92.1 s | Present attestation: `presents 2,083`, `submits 4,168`, `frame-callbacks 2,085`, `body-entries 1,632`; state remains `body-exits-on-keys`. |
| about 100 s | Batch-build and batch-submit each reach their 4,096-hit cap, all in the `<3` bucket; rectangle-fill and transform-copy remain at 0. |
| 112 s | State trace has init/stage word 2, loading 2, session 1, machine current 2 / next 2. Stage 3 is not reached. |
| about 113.0 s | One `[rt] replay: the Vulkan device was lost` event. |
| 114.363 s | Startup trace records an access-violation exception at `game+0x4D9F6C4`, read from `0xffffffffffffffff`; the arm exits at 116.1 s, not by timeout. |

| site | `<3` | `>=3` | unknown | observed result |
|---|---:|---:|---:|---|
| batch-build | 4,096 (capped) | 0 | 0 | reached before stage 3; the true total after the cap is unobserved |
| rectangle-fill | 0 | 0 | 0 | no entry during this arm |
| transform-copy | 0 | 0 | 0 | no entry during this arm |
| batch-submit | 4,096 (capped) | 0 | 0 | reached before stage 3; the true total after the cap is unobserved |

The batch-build and batch-submit counters matched at every snapshot. The title remained at machine stage 2 through the
last state sample and crashed before stage 3, so the zero `>=3` columns are **not** evidence that these sites do not run
after stage 3. The device-loss line preceded the exception; this single arm does not establish that the loss caused the
exception. No second arm was run.

The present-body result is a separate measurement from the 2D-batch sites. The existing attestation counts entries into
the measured media/UI body at `0x22185A0`; it observed at least 1,632 entries by 32.1 s, with the last logged sample at
92.1 s still at 1,632. At every emitted sample the body's two present-pass keys were 0 and its state was
`body-exits-on-keys`. Renderer `PresentX`/queue activity was also non-zero (2,083 presents and 4,168 submits at 92.1 s),
but the capture index contains 28 frames (1 through 2,701), all with mean RGB `0.00 0.00 0.00`. Thus code reaches the
media/UI body and the renderer's present path, but this arm does not reach the body's present pass or a visible frame.

### 19.3 Verdict, limits and next falsifier

- **The measured 2D batch path is reached before stage 3.** Batch-build and batch-submit each hit 4,096 and capped;
  rectangle-fill and transform-copy did not fire. These are function-entry counts, not proof that the calls belong to
  the first-run screen owner or that they draw visible UI.
- **The present body is reached but exits on its keys.** At least 1,632 body entries were sampled; the pass keys stayed
  zero. `PresentX` activity and black captures are not contradictory: the former names renderer submission/presentation,
  while `0x22185A0` is the distinct media/UI body.
- **Post-stage-3 behavior remains unmeasured.** The one arm lost the Vulkan device at about 113 s and stopped on an
  access violation at 114.363 s, before stage 3; it was not a 300 s timeout. There was one device-loss event, below the
  two-loss ceiling, but no further boot is authorized or claimed here.
- **Next falsifier, only if separately authorized:** run one stable-host arm through a confirmed stage-3 transition with
  these same bounded counters and a bounded caller/return-site sample for batch-build and batch-submit. That decides
  whether the batch path continues after stage 3 and whether the pre-stage-3 hits belong to a UI screen owner. Do not
  infer a negative post-stage-3 result from this stopped arm.

### 19.4 Tests, gates, evidence and files

- The lead's pre-arm `PALMETTO_OK` and `TESTS_BUILT` gates passed. After this section was appended, the documentation
  suite passed 38/38 tests. The GPU-free `ui_draw_probe_test` CTest ran without rebuilding: 1/1 passed; its executable
  reported 80 checks and 0 failures. No Palmetto build or relink was done after the arm.
- **Evidence (outside the repository; local lab):** `D:\leonida-work\fsrstable\boot-ui2d-draw-one` contains the
  original `meta.txt`, `boot.jsonl`, `stderr.log`, `stdout.log` and `summary.txt`; captures and `index.txt` are in
  `D:\leonida-work\fsrstable\capture-ui2d-draw-one`. Actor: `luna3-ui2d`; target: the local Palmetto process running
  the supplied title image under the class-0xa/content environment. Capture window: 2026-11-27 09:46:30–09:48:26
  America/Sao_Paulo. The used `palmetto.exe` SHA-256 starts `1DD3A3E84789BDB0`. Raw proof SHA-256:
  `stderr.log` `772B5E6938624444531060EF8550936C2848226CB6ABFD1C7037E50666B99EA3`,
  `boot.jsonl` `F6709FC60F5B3586E9981C10C80B31C9F3240F6EA1BCCE265DE1C0FC48B3DA6`,
  capture `index.txt` `B7913D0BA28FD4B6C2520BA1BF0E27BA82FD6BB503455E2EE823627F43EFD214`.
- **Uncommitted files from this slice:** [main.cpp](../port/src/main.cpp),
  [ui_draw_probe.h](../port/src/ui_draw_probe.h), [ui_draw_probe_test.cpp](../port/test/ui_draw_probe_test.cpp) and this
  page. The lead owns the CMake/CTest registration and the `docs/README.md` index row. No game data was added and no git
  write was made.

## 20. Round 22 (2026-11-28): the session-3 completion instrument, SSE4a emulation, and the gated presenter experiments

**Status: frontier moved, menus not yet rendered.** Round 22 built the two instruments section 15.6 asked for and measured
what happens once the session-3 loading record is forced complete. The completion wall is gone; the next wall is
post-completion title execution on a stable host, and the media/UI present body still has no visible content.

### 20.1 The forced-completion arm (binary `DA291AFB…`)

`PALMETTO_TITLE_SESSION3_COMPLETE=1` writes 1 into the loading record's slot-4 completion dword `image+0x78ACB20`
(the dword the 33 ms poll `0x21B7070` tests through `0x41E8C0`) every 100 ms. In the first arm (writer active from
process start, 120 s timeout, class-0xa mitigation, `PALMETTO_THREAD_STACKS`/`WAIT_TRACE`) the title:

- reached init 2 at 20 s, loading 2 and session 1 at 64.0 s, then init 3 at 66.5 s and **session 1 -> 13 at 68.0 s**;
  the stage-3 pump ran and the 22 s loading pause did not reproduce (the loading tick advanced ~1000/s);
- stopped at 89.9 s on `[rt] illegal instruction on the host: f2 0f 78 at 1402E7FF1`, which decodes as the AMD-only
  SSE4a `INSERTQ xmm0, xmm1, length 0x10, index 0x10` inside a real half-float packing routine (12 SSE4a sites in 9
  functions in the image).

### 20.2 The fix: clean-room SSE4a emulation

`port/src/sse4a_emulation.h` (ours, from the AMD64 manual fact sheet) decodes EXTRQ/INSERTQ immediates and register
forms, applies them to the CONTEXT XMM low halves and advances RIP; `port/shims/startup_trace.h` calls it from the
existing illegal-instruction handler next to the clzero/monitorx emulations. A synthetic test (`sse4a_emulation`, 25
checks) covers INSERTQ imm (the measured site bytes `f2 0f 78 c1 10 10`), length-0-means-64, both register forms,
EXTRQ, and the refusals (short buffer, wrong opcode, memory ModRM, bad /0, field crossing bit 64). The stop at
`0x2E7FF1` is removed by construction. A full session-13 window with the emulation has not been measured yet because
the host lost the Vulkan device in every later long arm (see 20.4).

### 20.3 The gated instruments and what the arms measured

- `PALMETTO_TITLE_PRESENT_KEYS=1` forces the two media/UI present-pass bytes `image+0x71B90AD`/`0x71B90AC` to 1. Forced
  from process start, the title stalled at init 1 (window existed, client black, no presents). Gated on the boot-media
  gate byte `0x71B9048 == 2`, it still stalled at init 1; gated on stage 2 + session != 0 it froze the loading tick at
  session 1; the final gate is **init >= 3 and session != 0** so the pass is only forced in a real stage-3 window.
- `PALMETTO_TITLE_OUTRECORD_STATE2=1` is the section 15.6 one-byte redirect at `image+0x402E987` (`0x74 -> 0xEB`, after
  validating the `cmp dword [rdi+0xac],2` bytes at `0x402E980`). It applies correctly and is inert until the dispatch
  runs, but combining it with the other instruments still disturbed early boot; it is a dedicated-arm diagnostic.
- A window-level capture arm on the final gated binary shows the title's own window exists (`sgaWindowClass`, 3870x2190
  outer / 2580x1460 at 150% DPI), is visible and foreground, and its client area is **pure black** in every sample
  (83 screenshots; whole-window non-black is Windows chrome only). Presenter captures in the session arm that reached
  session 1 were 19/19 mean RGB `0 0 0`; no readback PPM was produced before that arm's device loss.
- The stage-2/session-1 state table in the retuned arms matches section 17's clean baseline (init 2 at 20-22 s, loading 1,
  session 0), and session 1 arrives at 58-80 s depending on host load; reaching session 3 needs the completion dword
  and a host that does not lose the device first.

### 20.4 Host instability and the honest verdict

Every long arm after the first forced-completion arm lost the Vulkan device (`[rt] d3d12: present failed (VkResult -4)`,
followed by the known post-loss AV family at `game+0x86F97E`/`0x86F981`/`0x3DB414`). One combined arm that included the
stage-3-gated present keys stopped at 68.3 s exactly this way, before the keys could arm. Two arms were invalidated by a
lead debugger attach (noted in the round-22 ledger and excluded). No arm yet combines: session 3 -> 13, the SSE4a
emulation active, the stage-3 present-pass keys forced, and a live device. Until that arm runs on a quiet host, no menu
pixel is claimed - the round moved the wall from "session-3 never completes" to "post-completion execution on a stable
host, then the media/UI present pass".

### 20.5 Evidence

Arms, ledger and analysis are outside the repository: `D:\leonida-work\fsrstable\boot-t41-pause` (DA291AFB,
session 13), `boot-t41-pause2` (gated, front-end exception family), `boot-r22t7-p3-2d2a`, `boot-r22t7-combined-da541`
(device loss), `boot-r22t14-windowshot{,2,3}` (window exists, client black), the loss ledger
`D:\leonida-work\fsrstable\r22-loss-ledger.txt`, window PNGs under `D:\leonida-work\evidence\menus-r22\window*`, and the
frame tooling under `D:\leonida-work\tools-r22\` (+ `tooling/scripts/frame_analysis.py` in the tree). Switches and the
site validation are in `port/src/title_frontier_patch.h`; the SSE4a decoder/test are in `port/src/sse4a_emulation.h` and
`port/test/sse4a_emulation_test.cpp`.
### 20.6 The decisive round-22 window: session 13 reached with the present pass forced, and the frame is still black

The fourth-retune binary (`DA54119D6F5AE4CFFB514B7AA5F4A7E0304CF03655569D682AEC1EBB46162DA3`) finally produced the
clean window the earlier arms could not: one 240 s light arm with `PALMETTO_TITLE_CLASS0A_SIZE=1`,
`PALMETTO_TITLE_SESSION3_COMPLETE=1`, `PALMETTO_TITLE_PRESENT_KEYS=1` and state tracing progressed to **stage 3 at 51.6 s
and init 3 / loading 2 / session 13 at 52 s**, machine current 3 / next 3, heartbeat advancing, and the title alive past the earlier INSERTQ stop; the SSE4a
emulation line itself was **not logged in this arm** (the path may not have executed that routine; the emulation is
covered by its synthetic test), and the arm ran **with zero device losses and zero present failures** (the completion
writer armed at init 2, the present keys at init 3 + session 13). The presenter produced 9 capture frames and 4 readbacks:
**every captured presented frame is black** (2564x1421, mean/min/max RGB 0 0 0), and the readbacks are one black frame
plus three exact `51 102 204` layer write-control frames - no title-content pixel. The title pane is also occluded on this
host (Chrome owns the screen centre) so the literal window screenshot could not be taken while the title was foreground;
the window-chrome-only screenshots from the earlier arms show the `sgaWindowClass` window at 3870x2190 with a black
client area.

**Verdict for P-75:** the session-3 wall is removed (session 13 held from 52 s to 198 s and the arm stayed ALIVE to the 240 s timeout) and a stable stage-3/session-13 window with the media/UI
present pass forced is now reachable on a clean host, but the presented frame is still black. The remaining wall is
therefore **the content that the media/UI present body chooses to present** (its second-plane source), not the session
completion, not the CPU-feature gap, and not the presenter path. Next instruments: the body's begin/phase subscriber
output and the movie/front-end draw target correlation (section 19's batch path with a target-binding probe), plus
`PALMETTO_UI_PROBE=draw` in the same arm that now reaches session 13.

### 20.7 Round-3 probe arm: the media/UI present pass runs once and then stops presentation

After 20.6, a 300 s arm on the bounded-burst binary (`FF7ECF51…`) repeated the stable window (init 3 / loading 2 /
session 13 from 92 s, ALIVE 300.2 s, 0 exceptions, 0 device losses) and added `PALMETTO_UI_PROBE=draw`,
`PALMETTO_MOVIE_PROBE=player2`, `PALMETTO_PRESENT_ATTEST=1` and full captures. Results:

- at 62 s the body was still `body-exits-on-keys` with keys 0/0 and 1,064 presents; the bounded 300-write key burst
  (0.6 s) fired once stage 3 + session 13 were current, and from 92 s the attestation reads `body-present-pass` with
  keys 1/1;
- **presentation then froze**: presents/submits/frame-callbacks stayed 1,118 / 2,236 / 1,118 for the remaining ~200 s.
  The media/UI present pass therefore blocks the title's frame loop instead of presenting its plane; this was not the
  repeated-write override alone (the bounded burst does the same);
- the UI 2D-batch counters finally show **post-stage-3 entries**: batch-build and batch-submit each reached 2 hits in the
  `>=3` bucket (all other hits pre-stage-3), rectangle-fill and transform-copy stayed 0 - the first measured post-stage-3
  UI draw attempts, but not enough to identify a menu draw;
- the movie player stayed state 3 / next 3, handle 0, buffers 0, `probe 1 / open 1`; decode and frame-buffer
  registration never hit;
- all 23 presenter captures are black (2564x1421 mean RGB 0 0 0) and no readback PPM was produced after the pass.

**Wall named:** the media/UI present body's present pass waits on its media/UI plane (or a media worker event) and never
completes, so the frame loop stops presenting after the first pass. The next falsifier is a wait-trace/stack sample of
the body during `body-present-pass` (semaphores `0x71B9078`/`0x71B9088`, worker events, plane resource pointer) plus a
bounded target-binding probe at the UI batch submit path; if the pass is waiting on a media-ready event, the fix belongs
in the media worker/plane producer path, not in session completion.

### 20.8 Wait-trace of the present pass: the frame thread deadlocks on the media semaphore

A second 260 s arm (`boot-menus-r23-wait`) repeated the bounded burst and sampled `PALMETTO_WAIT_TRACE` and
`PALMETTO_THREAD_STACKS` at 95/120/180/240 s. The present attestation again switched to `body-present-pass` at 92 s with
presents frozen at 946/1,892. The wait trace names the stall precisely:

- the **frame thread** (tid 74760) is parked on `NtWaitForSingleObject` with timeout `INF` on **semaphore `0x718`** at
  `game+0x56A0D`, reported by the stage-3 pump as `[session-dispatch stage3-post transition pump-loop machine-wrapper
  tick-dispatch frame-pump frame-update]`; the same wait appears at 44.9 s / 69.9 s / 129.9 s / 189.9 s with no release
  (one thread, one semaphore, counts 1/1/0/0 in the handle table);
- the **media/UI body thread** (tid 34508) waits on **event `0x918`** at `game+0x221957F` (`0x221957F` is inside the
  present-body region: the body entry is `0x22185A0`, and the body releases a semaphore at `game+0x22185D3`); its wait
  statistics are 532 waits / 531 signalled - exactly one wait is left outstanding;
- so forcing the body's present pass while the media/UI producer has no ready plane sets up a **mutual wait**: the body
  waits for the event the frame thread would signal, while the frame thread waits for the semaphore the body's producer
  would release. Presentation stops and every later capture is a pre-deadlock black frame.

**Conclusion for the round:** the present-key experiment is a deadlock trigger, not a menu path. The honest next step is
the **producer** side of the media/UI plane: why the movie player (state 3, handle 0, buffers 0) and the front-end/UI
draw target never produce a ready plane at session 13. Useful next instruments: (a) the movie open/registration call
chain (`0x2EF0E0` decode, `0x442F760` frame-buffer registration) with the player state machine, and (b) the UI batch
submit's bound render target (`0x2AACF0`, `0x517A290`) correlated with the body's plane resource pointer. Forcing the
body pass again is not productive until a producer exists.

### 20.9 Producer-side switch: the title's own tick opens AC, then a null call kills it

Section 20.8 showed that forcing the consumer keys deadlocks. Round 24 therefore added `PALMETTO_TITLE_BOOTMEDIA_OPEN=1`
(write bit 0 of the boot-media event byte `image+0x75DF0D8`, the edge the app-state handler `0x247B500` normally
delivers on event type 0x24) and `PALMETTO_TITLE_FRAME_REQUEST=1` (set the frame-request flag `image+0x71B90AD` while the
title owns AC). Three arms on the `09D07D8C` / `7753FA4A` / `08FBA618` binaries:

- **event byte at gate==2 (4 s)**: the title never left init 2 / session 0 (presents 1, body-entries kept rising to
  17,611), i.e. opening the boot-media frame loop that early stalls the main pump;
- **event byte at stage 3 (`boot-menus-r24-producer3`, 105 s)**: the title reached init 3 / session 13 at 102.0 s and
  **the title's own tick opened the present handshake**: `keys 0x0/0x1` (AC `image+0x71B90AC` = 1 by the title, AD still 0),
  with the front-end manager ready/running and 58 movie players. Then, **1.45 s after stage 3, the process died at
  `game+0x0`, access execute, address 0x0** (null function pointer) - before any state-2 request item or frame capture;
- **event byte + frame request (`boot-menus-r24-frame`)**: this arm crashed earlier, at `game+0xF229C` read wild pointer at
  95.3 s before stage 3, so the frame request never armed and the arm is not a producer measurement (host/title
  instability after the earlier arms, recorded as such).

**What this proves:** the boot-media producer chain (`0x75DF0D8` bit 0 -> media tick `0x24846D0` -> `0x2484A90` ->
`0x2218900`/`0x2218910`) works in this port and is sufficient to open AC; it is not a shim answer we are missing. The two
remaining walls are (1) the **frame-request/state-2 request-item producer** for AD (`0x71B90AD`), which the request poll
`0x2218410` would set from a queued media item, and (2) a **null call 1-1.5 s after the boot-media frame loop opens**
(execute 0x0), which must be fixed before the present pass can be driven; the likely cause is a media/UI phase callback
or plane table that only the real app-state notification sequence initializes. Next falsifier: capture the stack of the
0x0 call with the exception-time stack sampler, then either synthesize the request item through `0x2218080` or fix the
uninitialized callback.

### 20.10 Round-3 producers: the arena exhaustion, the PLM app-state shim, and the loading-worker null

Three linked results from the producer-side round:

1. **The state-13 crash is arena exhaustion.** The crash in the state-13 handler (`game+0x6EB759`, read `[0]==0` + `0x10`,
   stack `0x6EB759 <- 0x36BA2B <- 0x56B507D <- 0x275FBB5 <- 0x1D0CAB6 <- 0x21B7733 <- 0x21B7540` = the session-13 handler,
   `<-
1D839DE dispatcher <- 0x81AFB9 post-transition) is the title's buffer factory dereferencing a null placeable: the
   port's GPU address arena (default 1024 MiB) was full at stage 3 (`arena buffer: no room for 0x10 bytes`, then
   `CreatePlacedResource` `E_OUTOFMEMORY`). With `PALMETTO_ARENA_MB=3072` the same arm reached stage 3 / session 13 at
   62-102 s and stayed **ALIVE for 200-240 s with 0 exceptions and 0 device losses** - the first stable session-13
   windows of the campaign. Stage-3 arms must now use `PALMETTO_ARENA_MB=3072` (documented as a measured requirement,
   not a default change).
2. **The crash reporter can name a null call.** `stack_from` now starts the walk at `[rsp]` when `Rip == 0` (a jump to 0
   used to report an empty stack), and `PALMETTO_CRASH_STACK=1` appends 32 raw guest stack words. The state-13 crash
   above was named that way.
3. **A PLM app-state notification shim and the loading-worker null.** The producer route the docs call H2 needs the OS
   app-state notification (`RegisterAppStateChangeNotification` callback `0x2217C70` -> producer `0x2218180`). New
   `port/shims/appnotify.cpp` answers the imports from `api-ms-win-core-psm-appnotify-*`: off, it forwards to the host
   api set unchanged; `PALMETTO_TITLE_APPSTATE_CALLBACK=1` delivers one synthetic running-state notification 500 ms
   after registration (synchronous delivery re-entered the module during construction and the title never reached the
   renderer; deferred delivery renders normally). The deferred arm reached stage 3 / session 13 at 92-94 s, then
   **stopped at `game+0x5151E29` read 0x0 on the loading/media worker** (stack `0x5151E29 <- 0x5151C64 <- 0x5151C05 <-
   `0x12381A <- 0x4FD1B8D <- 0x4FD1AE5 <- 0x23CF247 <- 0x23CF104 <- 0x2215ECA` media tick) - a new title-side null in the
   loading worker path, not the arena and not the buffer factory.

**Also measured:** with `PALMETTO_TITLE_BOOTMEDIA_OPEN=1` the title's own tick opens the present handshake AC
(`keys 0x0/0x1`), the boot-media gate changes `0x2 -> 0x20` after the first frame and presentation stops; adding
`PALMETTO_TITLE_FRAME_REQUEST=1` produces exactly one `body-present-pass` with keys `1/1`, after which AC clears and the
body stops being ticked. Both are still synthetic or one-shot producer edges; the PLM shim is the route that the real
app-state sequence uses. No menu pixel is claimed.

### 20.11 The natural + app-state-notification window: continuous presentation and UI draws, still black

With `PALMETTO_TITLE_APPSTATE_CALLBACK=1` (deferred PLM notification) and the class-0xa mitigation, **no forced session-3
completion**: the title takes init 3 at 52 s and **session 1 -> 3 at 52 s**, then holds session 3 while it keeps
presenting. A 300 s arm recorded 703,326 D3D12 calls, 6,535 presents, UI batch-build/submit 4,096 (cap) with **2,332
entries in the >=3 bucket**; a 480 s arm recorded 1,738,130 D3D12 calls, **18,642 presents**, 15,604 `DrawInstanced` +
69,023 `DrawIndexedInstanced`, 43,299 `SetPipelineState`, UI batches 2,060 in the >=3 bucket, **ALIVE 480.2 s, 0
exceptions**. This is the first route where the title renders continuously for minutes instead of freezing or crashing.

Measured present: `PresentX` carries **1 plane** (3840x2160, source rect full, placement full, rule fill ->
2564x1421 swapchain; `plane 0 token ... resources 1`), and every capture (3,541 presented frames, 60 sampled) is mean
RGB 0/0/0. The loading record still does not complete: session 3 is the state-3 poll and it holds for the whole 480 s
(loading tick advances ~1000/s, so the loading worker is busy). The earlier forced-completion arms advanced session to 13
but then null-dereferenced in the loading/upload workers (`game+0x6EB759` while the arena was full; `game+0x5151E29` in
the texture-upload job when `param_1+0x38` is null) - i.e. the loaded-record completion without the streamed content
produces jobs whose resources do not exist.

**Verdict for P-75:** the correct producer route (PLM app-state notification) now runs and keeps the title presenting and
drawing for 480 s, but the loaded-content wall remains: the state-3 loading record does not complete on its own, and
completing it artificially reaches session 13 only to have the upload workers dereference resources that the real
stream never delivered. Next: instrument the loading record entry 4 (the 0x50-byte entry the 0x41E8C0 poll reads) with
its progress fields and the slot gates, to separate "still streaming" from "stuck on a missing job/answer"; if it is
streaming, extend the window; if stuck, the completion writer's condition (DirectStorage chunk job / stream gate) is the
next fix. No menu pixel is claimed.

### 20.12 Loading-record instrumentation under the natural window: DS burst ends at 20 s, completion never written

Under the natural app-state window (`PALMETTO_TITLE_APPSTATE_CALLBACK=1`, class0a, `PALMETTO_ARENA_MB=3072`, no forced
completion), `PALMETTO_DS_RECORD=1000,400` + `PALMETTO_DS_LOAD_TRACE=1` measured the loading record for 400 s:

- **the DirectStorage burst is healthy and then stops**: 8,685 requests enqueued and done, 306 submits by +20 s (the
  second burst the docs name), and **no further request/submit for the remaining 380 s**;
- **the record is not idle**: slots 0 and 4 tick advances (~1,000/s) for the whole run (slot 4 tick is the session-3 poll's
  own tick store), while slots 1-3 stay zero; **slot 4's completion dword `image+0x78ACB20` is never written** - the
  `0x41E8C0` predicate never reads true, so session 3 holds from 62 s to 400 s;
- the record header words (`+0x1A8`, `+0x1C0`, `+0x1C8`, `+0x1D0`, `+0x1D4`) are constant for the whole run;
- `PALMETTO_PIPELINE_DEFER_COMPILE=0` stalls the title earlier (init 2 / loading 1 / session 0 to 360 s), so deferred
  compile is part of the working path, not the cause.

**Delivery timing of the synthetic notification matters:** delivering at 500 ms or 3 s can race the boot-media
construction and crash at `game+0x2217DF0` (wild read within ~1.5 s of the callback, 2.4-4.4 s boot); a **20 s delay**
(after init 2, before session 3) is stable across the arm - the shim now waits 20 s.

**Wall:** this is the streamed-content stall the streamed-consumer page named in sections 6b-6d - the DirectStorage job
mechanism answered every call, and the title-side stage logic stops arming batches after the pipeline-library burst, so
the session-3 record never completes and the menu content is never requested. The natural app-state window does not
change that; it just keeps the title presenting and drawing while it waits. Next: `PALMETTO_DS_STACK=1`/`PALMETTO_DS_ARRAYS=1`
at the stall to name which array/job the stage waits for, and the stage trigger after the pipeline precompile.
