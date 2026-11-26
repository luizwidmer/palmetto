# The first-run, UI and movie path: the boot-media gate, the frame handshake and the calls behind the first visible frame

**Status: static decompilation, no boot.** This page maps the title's own first-run/UI/movie path from the image
alone: the boot-media gate the stage-2 boot sequence waits on, the frame loop that gate releases, the first-run,
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
