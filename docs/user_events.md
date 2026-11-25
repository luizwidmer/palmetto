# The registered user-change callback and the store *Async completions: measured, and the opt-in delivery

**Status: measured.** The boot's platform surface has exactly **one** user call - a change-event registration at
+1.9 s - and **zero** `XStore`/`XGameSave` calls. The image links 44 `XStore` thunks (16 of them `*Async`) and 20
`XGameSave` thunks, and calls none of them in any 130 s or 300 s sample, so no store or user `XAsyncBlock` is ever
created: the public reference's offline-store hang (commit `1c69700851`, "offline XStore*Async never started the
XAsyncBlock -> title hung forever") is **not reachable in this boot**, and the title's registered change callback is
never invoked because the offline user never changes. This page records that measurement, implements the completion
semantics the surface was missing in our structure (one delivered `SignedInAgain` event per registration behind
`PALMETTO_USER_CHANGE_EVENT=1`; an offline store `*Async` that starts and completes its block behind
`PALMETTO_STORE_ASYNC_COMPLETE=1`), and reports one A/B pair of 130 s boots.

**Short answer of the A/B: the delivery is consumed, and the pair is truncated by a title-side crash both arms share.**
Arm B's delivered event makes the title's own callback run once - the only way this boot runs it - and the title
answers with exactly the five user calls section 2.3's disassembly predicts; nothing else differs. Both arms then stop
on the known title-side null read at `game+0x259393a` at 35.8 s wall (a dedicated null-read slice owns the repair; the
switch is not implicated, the RVA, the wall time and the exception are identical). The pair also measures a moved
baseline: the loader now **re-arms after the 16.5 s check** (armed 1 at +18.9 to +19.2 s), so the post-30.8 s idle of
the earlier pages is no longer this tree's state. The store completion is exercised by no call in either arm, which is
what the call table already says.

## 1. Why these imports/calls are needed

The first-run measurement ([first_run_path.md](first_run_path.md)) ends the platform trace at +29.39 s with the
speech synthesizer create; [loading_gate.md](loading_gate.md) records a 300 s boot with 15 platform-service calls, all
before +21.56 s, and no polled user/session value. Two platform-side completions are still registered or linked by the
image and never delivered:

- **The user-change registration.** The image links 21 `XUserImpl`/`XUserGamertagImpl` thunks; the boot calls one of
  them, `XUserRegisterForChangeEvent`, once at +1.9 s (host clock varies, +1.93 s to +2.58 s across the measured
  boots) and never calls `XUserAddAsync`, `XUserAddResult`, `XUserGetState`, `XUserGetId`, `XUserGetGamertag` or
  `XUserUnregisterForChangeEvent` afterwards. If a frontend stage waited for a user change event rather than polling
  a value, that event is the one that never comes.
- **The store's asynchronous surface.** The image links 44 `XStoreImpl` thunks, 16 of which are `*Async`; the boot
  calls none. A title that waits on one of those blocks without checking the returned HRESULT would hang on the
  current shim, which fails the call synchronously and never starts the block - the failure the public reference's
  commit `1c69700851` repaired. Our boot cannot be gated by it, because it never makes the call.

This slice therefore measures the two completions, implements the delivery each was missing, and uses the A/B to
answer the hypothesis the task names: does delivering the registered user change (and completing any store block)
advance the post-30.8 s idle? The measured answer (section 5) is that the delivered event is consumed exactly as
section 2.3 predicts - five user calls, nothing else - and the pair is cut short by the current tree's title-side null
read (`game+0x259393a`, owned by a separate slice) before the old idle can be reached. The pair also shows the loader
now re-arms after the 16.5 s check: the read-file repair has already moved the baseline past that idle.

## 2. Measured shapes

### 2.1 What the image links and calls (from the file, plus the platform trace)

`tooling/gdk_runtime_calls.py` on the retail executable (the same census as [runtime_imports.md](runtime_imports.md)):

| class | referenced thunks | the ones on this slice's surface | calls in the measured boots |
|---|---:|---|---|
| `XUserImpl` (with `IXUserGamertagImpl`) | 21 | slots 3-5, 7-14, 19-25, 33-34; `XUserRegisterForChangeEvent` (slot 33) at argument count 4 | **1**: the registration |
| `XStoreImpl` | 44 | 16 `*Async` thunks, 10 of them answering `E_FAIL` offline (slots 33, 35, 37, 40, 49, 53, 60, 62, 71, 75) | **0** |
| `XGameSaveImpl` | 20 | slots 4-6, 8-9, 11-14, 16-17, 19-25, 28-29 | **0** |

The 130 s and 300 s traces behind [loading_gate.md](loading_gate.md) and section 5 record no `XStoreImpl` or
`XGameSaveImpl` class request and no `XStore*`/`XGameSave*` slot call. `XUserImpl` is requested once, for slot 33.

### 2.2 The change-event registration, site by site

All RVAs are `game_scarlett_master.exe+0x...` in the retail 1.0.2.1 image.

| what | RVA | how it was read |
|---|---|---|
| `XUserRegisterForChangeEvent` import thunk (the whole function) | `0x55DCD58`..`0x55DCDFB` (163 bytes) | `.pdata` entry; the shared helper call at `0x55DCD9E` and the slot call whose return is the trace's caller |
| title call site | `0x5925827` (inside `0x59257E0`..`0x5925875`, 149 bytes) | the thunk's only caller (`E8` scan) |
| callback argument (`r8`) | `0x2211C80` | `lea r8, [rip-0x3713BA1]` at `0x592581A` |
| queue argument (`rcx`) | the handle returned by the getter at `0x5925890` (`mov rax, [rip+0x1893809]`) | `mov rcx, rax` at `0x5925821` |
| context argument (`rdx`) | the object the call site was entered with (`mov rdx, rsi` at `0x5925824`) | `rsi` is saved from `rcx` at `0x59257E5` |
| token argument (`r9`) | `object+0x550` | `lea r9, [rsi+0x550]` at `0x5925813` |
| answered | `S_OK` (0x0) | `PALMETTO_USER_TRACE=1` line of section 2.4 |

The thunk stores its four arguments in registers (`rcx`, `rdx`, `r8`, `r9` -> queue, context, callback, token) and
calls the shared `QueryApiImpl` helper at `game+0x55DE048`; the class id it passes is `XUserImpl`'s. The call site
loads the queue from a stored global handle, passes its own object as the context, and ignores the returned HRESULT
(the next instruction is a call to the title's own event registration). It is a one-shot registration during platform
init: no later boot re-reads it, unregisters it, or reads user state.

### 2.3 The title's registered callback

`game+0x2211C80` is 60 bytes: it saves the context (`rcx`) and the event (`r8d`), builds a temporary at `rsp+0x28`
through `game+0x2212160` (`rcx = &temporary`, and the `XUserLocalId` in `rdx` is stored as its first qword - the
`SignedInAgain` handler reads it back with `mov rcx, qword ptr [rdx]`), and calls `game+0x2212190` with
`(context, &temporary, event, 1)`. That function switches on the event:

| event (`XUserChangeEvent`, GDK reference) | value | handler |
|---|---:|---|
| `SignedInAgain` | 0 | `game+0x2212200` (then `0x2212930`, `0x22129A0`: re-resolve the signed-in user and refresh its state) |
| `SignedOut` | 2 | `game+0x2212310` |
| `Privileges` | 5 | `game+0x22123E0` |
| anything else | 1, 3, 4 | returns without doing anything |

So the shape a console event would have is measured: the title's own context, the user's `XUserLocalId`, and one of
the GDK enum's values. What no measurement can give is an event that happened: the offline user is signed in for the
whole boot and never transitions, so by default the callback stays uncalled (the shipping behaviour, and the reason
the field is `SignedInAgain` in the probe below - a transition into "signed in" is the only one compatible with the
measured state). The A/B confirms this at run time: delivering event 0 at +1.92 s makes the title call
`XUserFindUserByLocalId`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle` and `XUserCloseHandle` once each
(all `S_OK`, at +1.92 s) - the re-resolve-and-refresh path, to the slot.

### 2.4 The runtime trace of a 130 s boot

`PALMETTO_USER_TRACE=1`, arm A of section 5 (the default answers). The table is the same platform surface
[loading_gate.md](loading_gate.md) shows, with the registration now printing its shape (22 trace lines in this boot):

| class / entry point | calls | when (s) | caller | answered |
|---|---:|---|---|---|
| `XThreadingImpl` (XAsync/XTaskQueue plumbing) | 9 | +1.07 to +25.70 | `game+0x55DE08C` | `S_OK` |
| `XNetworkingImpl` | 1 | +1.07 | `game+0x55DE08C` | `S_OK` |
| `XUserImpl` | 1 | +1.98 | `game+0x55DE08C` | `S_OK` |
| `XUserRegisterForChangeEvent` (queue given, context given, callback `game+0x2211C80`) | 1 | +1.98 | `game+0x55DCDCB` | `S_OK`, never unregistered |
| `XPackageImpl` with `IXPackageImpl4` | 3 | +2.97 | `game+0x55DE08C` | `E_NOINTERFACE` (0x80004002) |
| `XPersistentLocalStorageImpl` | 1 | +2.98 | `game+0x55DE08C` | `S_OK` |
| `XDisplayImpl` (HDR query) | 4 | +3.03 to +25.70 | `game+0x55DE08C` | `S_OK`, HDR disabled |
| `XAccessibilityImpl` (`IXAccessibilityImpl2`) | 1 | +25.70 | `game+0x55DE08C` | `S_OK` |
| - `XSpeechSynthesizerCreate` (slot 9) | 1 | +25.70 | `game+0x55DCED0` | `E_FAIL` (0x80004005), null handle |

**The words `XStore` and `XGameSave` appear in no trace line of any measured boot, and no arm of the pair delivers a
store `*Async` completion because no store call is made.** No user or store `XAsyncBlock` is created in the trace
either: the registration takes a queue, not a block, and the `XThreadingImpl` lines are the task-queue plumbing its
thunks request. The zero-store-calls measurement holds on this build: the image links the surface, the boot never
touches it. The same boot's registration line confirms section 2.2's static shape at run time: the title registers
once, at +1.98 s, with a queue and a context, for callback `game+0x2211C80`.

## 3. What we implemented

No new source file and no build change. The default path is unchanged; both new behaviours are opt-in switches with
test seams, as [upstream_port.md](upstream_port.md) requires for a path that can change a title's observable answers.

### 3.1 The user change event (`port/shims/xgameruntime_user.cpp`, `PALMETTO_USER_CHANGE_EVENT=1`)

The registration now keeps the callback, its context and its token (it kept only the token before). The default still
never invokes the callback: offline the user never changes. With the switch on, each registration schedules **one**
delivery of `XUserChangeEvent::SignedInAgain` (0) with the registration's context and `XUserLocalId{1}` - the same
local id `XUserGetLocalId` answers - on the **same task queue the title registered with**, through that queue's
completion port, the way the neighbouring `XNetworkingRegisterConnectivityHintChanged` registration delivers the
initial notification Microsoft documents (`xgameruntime.cpp`). The callback runs on the queue's thread, not inside
`XUserRegisterForChangeEvent`, so a title that blocks in its callback cannot deadlock the registration call.

- Bounded: one delivery per registration and at most 8 per process; a title that keeps re-registering cannot stream
  events. Past the cap the registrations still succeed and nothing is scheduled (logged once).
- Unregister is honest: a delivery not yet run is dropped, `XUserUnregisterForChangeEvent(token, false)` returns
  `true`; if the callback is running, `false` is returned (pending) and `wait = true` waits for it to return.
- `PALMETTO_USER_TRACE=1` now also prints the registration's queue/context/callback shape (`callback` as module+RVA)
  and each delivered event with the process-relative clock, so the A/B can count and time it.
- A switch seam, `rt::shims::xgameruntime_set_user_change_event(bool)`, returns the previous state for the tests.

### 3.2 The offline store `*Async` completion (`port/shims/xgameruntime_store.cpp`, `PALMETTO_STORE_ASYNC_COMPLETE=1`)

Every `*Async` slot that answers "not available offline" now has two honest forms. The default is the measured one: a
synchronous `E_FAIL`, block untouched, logged once (unchanged). With the switch on, the call locates the caller's
`XAsyncBlock*` at its machine argument position and starts it with `XAsyncRun`, completing with `E_FAIL` on its
queue; the `*Async` call returns `S_OK`, the block's completion callback runs, `XAsyncGetStatus(wait = true)` returns
`E_FAIL` instead of hanging, and the `Result`/`ResultSize` partners keep returning `E_FAIL`. The positions are the
machine arguments after `this` (the shim receives them in `rdx`, `r8`, `r9` and the stack slots), which for these
slots is the GDK reference's parameter order - with one ABI-shaped case verified from the image: slot 35 takes a
by-value `GUID`, and its thunk (`game+0x55E18C0`) copies the GUID behind `r9` into a local, passes `&copy` as the
4th machine argument and the caller's `XAsyncBlock*` as the 5th. The 15 slots:

| slot | function | block is argument |
|---:|---|---:|
| 33 | `XStoreQueryConsumableBalanceRemainingAsync` | 3 |
| 35 | `XStoreReportConsumableFulfillmentAsync` | 5 |
| 37 | `XStoreGetUserCollectionsIdAsync` | 4 |
| 40 | `XStoreGetUserPurchaseIdAsync` | 4 |
| 43 | `XStoreQueryLicenseTokenAsync` | 5 |
| 49 | `XStoreShowPurchaseUIAsync` | 5 |
| 51 | `XStoreShowRateAndReviewUIAsync` | 2 |
| 53 | `XStoreShowRedeemTokenUIAsync` | 6 |
| 58 | `XStoreDownloadPackageUpdatesAsync` | 4 |
| 60 | `XStoreDownloadAndInstallPackageUpdatesAsync` | 4 |
| 62 | `XStoreDownloadAndInstallPackagesAsync` | 4 |
| 71 | `XStoreAcquireLicenseForDurablesAsync` | 3 |
| 73 | `XStoreShowAssociatedProductsUIAsync` | 4 |
| 75 | `XStoreShowProductPageUIAsync` | 3 |
| 82 | `XStoreShowGiftingUIAsync` | 5 |

Five of them (43, 51, 58, 73, 82) have no thunk in the retail image at all; they are completed anyway because the
class hands out one vtable and a partial completion would leave the same failure on the sibling slots. A switch seam,
`rt::shims::xgameruntime_set_store_async_complete(bool)`, returns the previous state for the tests. The status,
handle and product-query providers are untouched.

### 3.3 What was not touched

`xgameruntime_gamesave.cpp` is not changed: the image links 20 `XGameSave` thunks and calls none, so the measured path
does not use it. No CMake, `main.cpp`, D3D12, DirectStorage or `xmem` file is edited. `docs/runtime_imports.md`'s user
paragraph describes the default path and stays true; the switches are documented here.

## 4. Cross-check against the reference

The public reference is the GPL-3.0 project ubiquitarianism/leonida at commit `ed94fbef9`; its files and commits were
read as evidence only, no code was copied.

- **The store `*Async` fix (its commit `1c69700851`, Rüzgar Koray Genç; "store: offline *Async slots start and
  complete their XAsyncBlock with E_FAIL (M11)").** Its semantics agree with ours: the block is found by argument
  position, started with `XAsyncRun`, completed with `E_FAIL`; the Result partners keep returning `E_FAIL`; and for
  the slots both trees cover the argument positions agree (33/3, 35/5, 37/4, 40/4, 43/5, 49/5, 51/2, 53/6, 58/4,
  60/4, 62/4, 71/3, 73/4, 75/3). It was motivated by a title that hung waiting on a block, and it is on by default
  there. **Where we differ:** our measurement of *this* boot finds no store call at all, so a default answer change
  would be unmeasured; ours is therefore opt-in with today's measured answer as the default, and slot 82
  (`XStoreShowGiftingUIAsync`, block 5) is completed too, which that commit leaves as the old synchronous `E_FAIL`.
- **The story-load merge (`273b3bdf9`, Moriz82 with Plaaasma and others).** It touches
  `runtime/shims/xgameruntime_user.cpp`, but only to add first-call logging to every served user slot; its registration
  comment still says the callback is "never called: the user never changes", exactly as ours. So the task's "XUser
  stage fix" is a logging change, not a completion: there is no upstream user-event delivery to cross-check, and the
  fork's healthy-boot table reaches the story load through its rendering/streaming work
  ([first_run_path.md](first_run_path.md) section 4 already refuted the splash and first-run stage readings).
- **The offline shims' default policy.** Both trees fail service-dependent store calls offline; the difference is the
  completion of the block under the switch, and our result partners are unchanged. Nothing in the reference delivers
  a user change event offline, so the probe in section 3.1 has no counterpart to agree or disagree with.

## 5. A/B measurement

One pair, both arms on the same binary (SHA-256 `7CBFAB840216A1A7B06966ADA5B1BA9BC5CEB61B5F5094E2350E9C70D835E82A`,
built 2026-11-25 12:06:41), one at a time under `_setup/boot_lock.ps1`, own overlay and capture directories outside
the repository. A started 12:07:18, B 12:10:04. Standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus the same measurement layer in both arms:
`PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_DS_STACK=1`, `PALMETTO_DS_ARRAYS=1`,
`PALMETTO_USER_TRACE=1`, `PALMETTO_CAPTURE` with `PALMETTO_CAPTURE_EVERY=2000` and `PALMETTO_CAPTURE_LIMIT=8`. Arm B
adds `PALMETTO_USER_CHANGE_EVENT=1` and `PALMETTO_STORE_ASYNC_COMPLETE=1`.

The pair ran on a tree state in which the read-file repair (commit `aaa803f86`) has resumed the streaming loader, and
both arms stop on the known title-side null read at `game+0x259393a` long before the 130 s timeout. The pair therefore
compares a **truncated** boot; the crash is identical in both arms and is owned by a separate null-read slice, so it is
not the switch's effect. What the pair can still measure is everything the switch changes up to that point, and the
loader state that shows the baseline has moved.

| measurement | A control | B probe |
|---|---|---|
| verdict | STOP `game+0x259393a` (0xC0000005, read 0x0), wall 35.8 s, 1 exception | the same RVA, code and wall time, 1 exception |
| `boot.jsonl` events | 814 | 847 |
| traced D3D12 calls (max #) | 56,100 (#58,351) | 57,309 (#59,745) |
| platform-trace lines | 22 | 32 |
| last platform call | +25.70 s (`XSpeechSynthesizerCreate`, `E_FAIL`) | +25.34 s (`XSpeechSynthesizerCreate`, `E_FAIL`) |
| change-event registration | 1, at +1.98 s | 1, at +1.92 s |
| change event delivered to the title's callback | **none** (switch off) | **1**, at +1.92 s, `SignedInAgain` (0), to `game+0x2211C80` |
| title calls that the delivered event makes | - | `XUserFindUserByLocalId`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle`, `XUserCloseHandle`: 1 each at +1.92 s, all `S_OK` |
| `XUserImpl` class requests / all user slot calls | 1 / 1 | 6 / 6 |
| `XStoreImpl` / `XGameSaveImpl` calls | 0 / 0 | 0 / 0 |
| store `*Async` completions delivered | 0 (no store call) | 0 (no store call) |
| loader hook records | 438 | 444 |
| last loader record (DirectStorage clock) | +20.123 s | +20.047 s |
| loader at the 16.5 s check | armed 0 at +16.589 s, then armed 1 at +18.949 s (24 inflight) and +19.202 s (211 inflight) | armed 0 at +16.573 s, then armed 1 at +19.190 s (109 inflight) |
| status array #17 enqueues, last | 44 at +19.381 s | 45 at +19.239 s |
| slot-57 records / slot-55 states at the last row | 10,241 / 12,985 | 10,241 / 16,793 |
| graphics / compute pipelines at the last row | 23,221 / 95 | 27,029 / 95 |
| submits / presents at the last row | 2 / 1 | 2 / 1 |
| presenter captures (frame 1) | 1, mean rgb 0.00 0.00 0.00 | 1, mean rgb 0.00 0.00 0.00 |

Reading the table:

- **The switch is consumed, and its whole effect is the event it delivers.** Arm B has exactly 10 more trace lines
  than A: the five follow-up user calls and the five `QueryApiImpl XUserImpl` requests their thunks make. Everything
  else on the platform surface is the same or later by boot speed: no `XStore`/`XGameSave` call appears (so the store
  switch changes nothing, as the call table says), no save, package, display or streaming request is added, and the
  two arms' last platform call is the same speech create within 0.36 s.
- **The delivered event is the `SignedInAgain` path the disassembly predicted** (section 2.3): the title resolves the
  offline user by local id, reads its id and local id, duplicates the handle and closes it, all `S_OK` and none of it
  faults. The title's own state machine handled the event; this boot cannot give it a state that has anywhere to go.
- **The stage comparison is truncated by the crash, not by the switch.** Both arms stop at the same title RVA, at the
  same 35.8 s wall time, with the same exception (trace clock +26.870 s in A, +26.913 s in B) and the same stack. The
  late precompile counters differ (12,985 vs 16,793 slot-55 states) because the two arms are cut at different points
  inside the loader's parallel batch, not because the event changed the population: slot-57 is 10,241 in both, the
  compute-pipeline count is 95 in both, both present once, and the only presenter capture in each arm is uniformly
  black.
- **The loader re-arms, which the earlier pages did not see.** The pre-repair pair in
  [first_run_path.md](first_run_path.md) section 5 has "armed 0 (first sample +16.578 s), never re-armed" and 38/38
  loader records; this pair has 438/444 records and armed 1 at +18.9 to +19.2 s after the 16.5 s check. That is the
  read-file repair letting the title's streaming continue, and it is why the post-30.8 s stationary idle is not this
  tree's state any more; the crash frontier is. (The loader-hook record count includes the re-armed batches; the
  earlier pair counted 38 records.)

**Verdict: the delivery works as measured and the stage is unchanged up to the shared crash.** The event is consumed
exactly as section 2.3 predicted, adds no request outside its own five user calls, and neither the crash nor the
presentation changes. The pair cannot answer whether delivering the event would have moved the old post-30.8 s idle,
because this tree no longer reaches that idle: it stops on an owned title-side null read at +26.9 s of the trace
clock. The pre-repair measurements remain the valid answer for the stage hypothesis on their tree state, and this
pair's loader re-arm is evidence that the baseline moved past it. The store completion is unexercised in both arms.


## 6. Limits

- **The A/B pair is truncated.** Both arms stop at 35.8 s on the title-side null read at `game+0x259393a` that a
  separate slice is repairing, so the pair compares the boot up to that crash, not a 130 s idle. It was not repeated
  after the crash: the switch's effect is the same ten trace lines whether the title later crashes or not, and the
  post-30.8 s idle it was meant to probe is no longer this tree's state (the loader re-arms after the 16.5 s check).
  Once the null-read slice lands, the same switch pair should be re-run on the repaired tree to measure the stage.
- The probe's event is **fabricated**: the offline user never signs out and in again, so a `SignedInAgain` event is
  the only transition compatible with the measured state, not a transition that happened. That is why it is a switch;
  what would make the *delivery* a default is a measured console session in which the user state changes and the
  title's callback is the documented way it learns.
- The delivery time is the title's queue dispatch time, not a measured console clock: the registration is at +1.9 s
  and the callback runs when the queue's thread pool runs it. The A/B reports the measured invocation clock.
- The title's callback branches on `SignedInAgain`, `SignedOut` and `Privileges`; the probe only exercises the
  `SignedInAgain` branch (`game+0x2212200`). The `SignedOut` and `Privileges` handlers (`game+0x2212310`,
  `game+0x22123E0`) are entered by no measurement, and neither are `SigningOut`, `Gamertag` or `GamerPicture`.
- One pair on one machine, shared with other agents' builds and boots; the boot clock varies (the registration is
  +1.92 s in B and +1.98 s in A, +2.58 s in [first_run_path.md](first_run_path.md) section 2.2). The stable state
  (slot-57 population, compute-pipeline count, one black present) does not depend on it, the precompile tail does.
- The store completion has **no measured caller**: its argument positions come from the published GDK signatures and
  the image's thunks (the slot-35 `GUID` ABI is read from its thunk, section 3.2), not from an executing call site, and
  the A/B cannot exercise it. What would make it default is a title that calls one of the ten linked `*Async` slots and
  waits on the block.
- A 130 s window cannot exclude a stage after 130 s; the 600 s boot of [long_boot.md](long_boot.md) shows no title-side
  work after it either.
- No game data: only API names, slot numbers, call times, counts, HRESULTs, the title's own RVAs and image means.

## 7. Tests

Both tests run as their existing ctest targets (`xgameruntime_user`, `xgameruntime_user_gamertag`,
`xgameruntime_user_gamertag_long`, `xgameruntime_store`), extended in `port/test/xgameruntime_user_test.cpp` and
`port/test/xgameruntime_store_test.cpp`. No new target, no game data, no GPU.

- **`xgameruntime_user_test`** (88 checks, 0 failures): the default registration delivers nothing; with the probe on,
  the registered callback runs once with its own context, `XUserLocalId{1}` and `SignedInAgain` (0); unregistering a
  delivery that has not run drops it (a manual queue dispatches nothing); the probe stops after its cap; the switch
  seam reports the previous state. The package-readiness section of the same file is unchanged and still passes.
- **`xgameruntime_store_test`** (35 checks, 0 failures): with the switch off the `*Async` answer is the synchronous
  `E_FAIL` and the block's callback does not run; with it on, slots 51 (2nd argument), 33 (3rd), 49 and 82 (5th) and
  53 (6th, on the stack) start, complete with `E_FAIL`, run the block's completion callback and keep their Result
  partner at `E_FAIL`; a null block is `E_POINTER`; switching back off restores the old answer. `xgameruntime_gamesave`
  and `xgameruntime_offline` are untouched and still pass.
- The A/B boots are measurement, not tests: they are not in the repository and no test depends on game files.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project ubiquitarianism/leonida, files `runtime/shims/xgameruntime_store.cpp`,
`runtime/shims/xgameruntime_user.cpp`, `runtime/shims/xgameruntime_gamesave.cpp`, `docs/runtime_imports.md` and
`linux_proton.md`, commits `1c69700851` (Rüzgar Koray Genç), `273b3bdf9` (Moriz82, Plaaasma and the
RT-PROTON-STORY-LOAD contributors) and `ed94fbef9` (aslan2142's merge). The `XUserChangeEvent` values and the
`XStore*Async` signatures are Microsoft's public GDK reference (XUser.h, XStore.h pages on learn.microsoft.com/gaming).
No code from that project was copied. The upstream project remains the copyright of its contributors and its license
continues to apply to its own tree. The reference's own notes were read for the store-fix commit message and the
user-shim comment, not copied. The licence text of this project is GPL-3.0.

## 9. Evidence and reproduction

Outside the repository, under `D:\leonida-work`:

- `userevents\arm.ps1` runs one arm; `userevents\analyze_arm.py` extracts the tables of this page (platform calls,
  the registration shape, delivered events, store/gamesave call counts, loader state, timeline row, captures);
  `userevents\find_calls.py` is the static call-site scan used for section 2.2. No game bytes are read by the analysis.
- `boot-userevents-a-control` - arm A: stderr log, `summary.txt`, startup trace, captures in
  `userevents\capture-a-control`; `userevents\a-control.json` is its analysis.
- `boot-userevents-b-probe` - arm B, the same files; `userevents\b-probe.json`. Both `summary.txt` files record the
  same `VERDICT: STOP game_scarlett_master.exe+0x259393a`; the pair is truncated as section 5 states.
- `userevents\docs_check.py` runs the repository's docs checks with this page included (the checks enumerate tracked
  Markdown only, and this page is untracked until the lead commits it).

Reproduce one arm (from the checkout root, after `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`):

```powershell
python -I tooling\boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay <work>\overlay-b --out <work>\boot-userevents-b-probe --timeout 130 `
    --env PALMETTO_SHADERS=<work>\shaders\spirv --env PALMETTO_D3D12=vulkan --env PALMETTO_SKIP_INT3=1 `
    --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_PIPELINE_TIMELINE=10 --env PALMETTO_LOADER_HOOK=1 `
    --env PALMETTO_DS_STACK=1 --env PALMETTO_DS_ARRAYS=1 --env PALMETTO_USER_TRACE=1 `
    --env PALMETTO_CAPTURE=<work>\capture-b --env PALMETTO_CAPTURE_EVERY=2000 --env PALMETTO_CAPTURE_LIMIT=8 `
    --env PALMETTO_USER_CHANGE_EVENT=1 --env PALMETTO_STORE_ASYNC_COMPLETE=1
```

Drop the last two `--env` for arm A; hold `_setup/boot_lock.ps1` around each run.
