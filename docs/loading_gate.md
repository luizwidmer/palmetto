# The loading gate as a platform user/session wait: measured

The title's loading state does not advance for 1,200 s: it presents a stationary two-pass blit loop, the presented image
is uniformly black, the streaming loader is armed in shrinking small batches until about 16-20 s and then never re-armed, and DirectStorage, GPU fences
and input are excluded ([frame_capture.md](frame_capture.md), [streamed_consumer.md](streamed_consumer.md),
[input_gate.md](input_gate.md), [wait_state.md](wait_state.md)). This page tests the last live class of shim-side gate:
a platform-service wait, i.e. the game state machine polling a user/session/package value that our GDK/XGameRuntime
shims answer with something that never becomes "ready".

**Short answer: no user/session/platform API is polled.** The title makes 15 platform-service calls (plus 10-99
XAsync/task-queue plumbing calls) in total, all between +0.92 s and +21.56 s of a 300 s boot, and then **none for the
remaining 278 s** while it presents 61,440 submissions of the same black two-pass loop. A value that is read once
cannot be "the value that never transitions", and no platform value is re-read at all. The one platform sequence the
loading stage does consume (the XPackage readiness chain) was found to be **entirely dead on the default build** - the
title asks for a newer XPackage interface version than the shim served, so every XPackage call returned
`E_NOINTERFACE` - and completing that chain honestly (interface version, installation monitor, progress notification,
package enumeration) changes nothing measurable: same pipeline population, same draw loop, presenter captures and
4K-target readbacks still uniformly black. The platform-service-wait branch is therefore refuted for this build; the
gate is the title-side stage dispatcher.

## 1. Why a QueryApiImpl count is the platform API call count

The retail image imports no `xgameruntime.dll`. Its 161 `xgameruntime.lib` thunks call a shared helper which asks
`QueryApiImpl` for the class **on every call**, then calls one vtable slot with `this` first and finally `Release`
(measured statically; [runtime_imports.md](runtime_imports.md), "Class layouts, measured"). Confirmed again on this
image, from the file only: the helper is at `game+0x55DE048` and its only call is the CFG-dispatched `QueryApiImpl`
(`call 0x56A2070`) with the class/interface ids; every thunk calls that helper and then one slot, e.g. the XPackage
slot-5 thunk at `game+0x55E00A4`, the slot-9 thunk at `game+0x55E0230` and the slot-39 thunk at `game+0x55E0684`.

So `PALMETTO_USER_TRACE=1` (section 8) counts **actual platform API calls**, one line per (class, interface) request,
logging the first eight calls of each and then the tenth, hundredth, ... with the process-relative time, the caller as
module+RVA and the answered HRESULT. The shim-side wrappers on the 21 `XUser*` slots the executable imports add the
slot name and the returned value.

## 2. The API table

300 s acceptance boot (fix on, section 6), `PALMETTO_USER_TRACE=1`, callers as module+RVA. The "answered" column is
the HRESULT/return the shim gave.

| class / entry point | calls | when (s) | caller | answered |
|---|---:|---|---|---|
| `XThreadingImpl` (XAsync/XTaskQueue thunks) | 10-99 | +0.92 to +21.56 | `game+0x55DE08C` (helper) | `S_OK` |
| `XNetworkingImpl` | 1 | +0.93 | `game+0x55DE08C` | `S_OK` |
| `XUserImpl` + `XUserRegisterForChangeEvent` | 1 | +1.93 | `game+0x55DCDCB` | `S_OK` (callback registered, never invoked - the user never changes offline) |
| `XPackageImpl`, iid `IXPackageImpl4` | 7 | +2.93 | `game+0x55DE08C` | `S_OK` with `PALMETTO_PACKAGE_V4`; `E_NOINTERFACE` (0x80004002) without it (section 4) |
| - `XPackageIsPackagedProcess` | 2 | +2.93 | `game+0x55E0087` | `1` (packaged) |
| - `XPackageGetCurrentProcessPackageIdentifier` | 2 | +2.93 | `game+0x55E001F` | `S_OK` + identifier |
| - `XPackageCreateInstallationMonitor` (slot 5) | 1 | +2.93 | `game+0x55E0130` | `E_NOTIMPL` by default; `S_OK` with `PALMETTO_PACKAGE_READY` (section 5) |
| - `XPackageRegisterInstallationProgressChanged` (slot 9) | 1 | +2.93 | `game+0x55E029E` | `E_NOTIMPL` by default; `S_OK` + one delivered notification with `PALMETTO_PACKAGE_READY` |
| - `XPackageEnumeratePackages` (IXPackageImpl3, slot 39) | 1 | +2.93 | `game+0x55E06FA` | `E_NOTIMPL` by default; `S_OK` (no additional package) with `PALMETTO_PACKAGE_READY` |
| `XPersistentLocalStorageImpl` (IXPersistentLocalStorageImpl3) | 1 | +2.94 | `game+0x55DE08C` | `S_OK` |
| `XDisplayImpl` (slot 4, `XDisplayTryEnableHdrMode`) | 4 | +2.99, +3.43, +18.14, +21.56 | `game+0x55DE08C` | `S_OK`, HDR disabled (SDR) |
| `XAccessibilityImpl` (IXAccessibilityImpl2; the boot's one slot-9 `E_FAIL` line) | 1 | +21.56 | `game+0x55DE08C` | `E_FAIL` for the speech-synthesizer slot, as documented offline |

**Zero calls** in the whole 300 s for every other GDK class the executable has thunks for: `XStoreImpl`,
`XGameSaveImpl`, `XSystemImpl`, `XGameImpl`, `XGameUiImpl`, `XGameEventImpl`, `XGameStreamingImpl`,
`XUserDeviceImpl`, `XGameRuntimeFeatureImpl`, `XSystemAnalyticsImpl` (and `XAppCaptureImpl`, which the shim does not
serve but which is never requested either). This is the full platform surface the title touches: **15 platform-service
calls plus 10-99 async/task-queue calls, all between +0.92 s and +21.56 s, nothing after.**

## 3. What that means for the wait hypothesis

- There is no polled user API: the only `XUser*` call in 300 s is one `XUserRegisterForChangeEvent` at +1.93 s. No
  `XUserAddAsync`/`AddResult`, no `XUserGetState`, no `GetId`, no gamertag, no privilege check, no local-id resolution.
- There is no polled session/activation/network value: one networking call, no store/save/game/UI/system class request.
- The package-readiness sequence runs once at +2.93 s (section 5) and is never re-read.
- With the fix off, the same list appears with the XPackage request answered `E_NOINTERFACE`; with it on the package
  chain runs and completes. In both cases the platform calls end by +21.56 s and the frame loop continues unchanged.

## 4. The one real platform defect found: `IXPackageImpl4` was not served

The retail image's 17 referenced XPackage thunks were linked against **`IXPackageImpl4`** and pass that interface id to
`QueryApiImpl` (`tooling/gdk_runtime_calls.py`, "requested with an interface id other than the class id"), while
`port/shims/xgameruntime.cpp` served the class up to `IXPackageImpl3`. The measured answer was therefore
`E_NOINTERFACE` (0x80004002) for **every XPackage call the title makes** - silently: `QueryApiImpl` only logged classes
it does not serve at all, and this class was served, just not that interface version. The whole XPackage surface was
dead: two `IsPackagedProcess`, two package-identifier reads and the readiness calls were all failing.

`IXPackageImpl4` adds exactly one method, `XPackageGetPackageKind`, at slot 41
(`tooling/gdk_runtime_classes.json`); the title calls slots 3-7, 9, 12, 14, 16-19, 25-26, 34, 37-39, all at slot 40 or
below, where v4's layout is v3's. `PALMETTO_PACKAGE_V4=1` accepts the v4 id and hands out the same 41-entry vtable.
Measured with it on: the sequence moves from one refused request to the 7-request package init of section 2.

## 5. The package-readiness chain, and `PALMETTO_PACKAGE_READY`

With the interface id accepted, the loading stage's package init reaches three console calls the shim answered
`E_NOTIMPL`, i.e. the "package is ready" state the loading flow asks for could never be reported:

| slot | call | default answer | `PALMETTO_PACKAGE_READY` answer |
|---|---|---|---|
| 5 | `XPackageCreateInstallationMonitor(identifier, selectorCount, selectors, intervalMs, queue, monitor)` | `E_NOTIMPL` | `S_OK`, one static monitor, state "installed, complete"; the out pointer is written only after `VirtualQuery` proves the range writable |
| 9 | `XPackageRegisterInstallationProgressChanged(monitor, context, callback, token)` | `E_NOTIMPL` | `S_OK` + token; the callback is invoked **once**, through the queue the monitor was created with, with the installed package's complete snapshot |
| 39 | `XPackageEnumeratePackages(kind, scope, callback, context)` (IXPackageImpl3 overload) | `E_NOTIMPL` | `S_OK`, callback not invoked (no additional package is installed, so the callback ABI is not guessed) |
| 6 | `XPackageCloseInstallationMonitorHandle(monitor)` | `E_NOTIMPL` | `S_OK` (drops the registration) |

The slot-5, slot-9 and slot-39 signatures were not guessed: they were read from the title's own thunks
(`game+0x55E00A4`, `game+0x55E0230`, `game+0x55E0684`) before the handlers were written, and the handlers are called
through those thunks with no crash (300 s ALIVE, 0 exceptions). The delivered notification is logged and the title's
callback does run (`XPackage installation-progress notification delivered to the title's callback`).

Measured result of completing the chain: the package init completes at +2.93 s, the title's callback runs, and the
title then **does nothing further with the package**: no `XPackageGetInstallationProgress`, no monitor close, no new
loading stage, no population change (section 6). The package-readiness answer is not what the loading loop waits for.

## 6. The measured result: population and presented target (300 s acceptance)

All boots: harness `python -I tooling/boot_report.py`, standard env (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL`), own overlay/out dirs under `D:\leonida-work`, boot lock held.

| boot | s | fix | d3d12 calls | slot-55 derived-graphics-PSO creates | slot-57 record builds | replay pipelines registered | `DrawInstanced` | submissions (last) | captures | 4K target readback |
|---|---:|---|---:|---:|---:|---:|---:|---|---|---|
| boot-userstate-a | 130 | off | 530,904 | 40,000 | 10,000 | 8 | 25,262 | 24,576 @ 105.9 s | - | - |
| boot-userstate-b | 130 | v4 | 565,635 | 40,000 | 10,000 | 8 | 27,062 | 24,576 @ 99.3 s | - | - |
| boot-userstate-c | 300 | v4 + ready(slots 5/39) | 1,303,982 | 40,000 | 10,000 | 8 | 65,916 | 65,536 @ 278.3 s | 17, all mean 0 0 0 | black |
| boot-userstate-f (acceptance) | 300 | v4 + ready(5/6/9/39) | 1,280,554 | 40,000 | 10,000 | 8 | 64,576 | 61,440 @ 268.7 s | 17, all mean 0 0 0 | black |
| pre-fix 300 s control (input_gate.md boot) | 300 | off | 1,315,060 | 40,000 | 10,000 | 8 | 66,524 | 65,536 @ 278.1 s | all mean 0 0 0 | black |

The pipeline population is **identical in every boot**, fix on or off: console slot 55 reaches 40,000 derived
graphics-PSO creates (its summary prints every 5,000 calls, so 8 summaries is the complete count), the slot-57
deserialized-record path reaches 10,000 (5 summaries), and the replay layer registers 8 graphics pipelines and 2
compute pipelines. That creation is confined to the first ~4.3 s: the eight replay registrations and the 10,000
slot-57 record builds all precede the `submission 1 at 0.0 s` line, and the 40,000 slot-55 creates all fall between
submission 1 and `submission 16 at 4.3 s` (its every-5,000-calls summary prints 8 times and then never again). After
that the boot only runs the stationary two-pass loop: 0 `Dispatch`, 0 `CopyBufferRegion`/`CopyTextureRegion`/
`ResolveSubresource`, exactly two draws and one clear per frame (`64,576` draws vs `32,290` clears in the acceptance
boot), no new pipeline.

The presented target stays black in the acceptance boot (full fix on):

- **Presenter captures** (`PALMETTO_CAPTURE`, swapchain image every 2,000 presented frames): 17 captures (frames
  1..32,001), every one `2564x1421, mean rgb 0.00 0.00 0.00`.
- **Full-target readbacks** (`PALMETTO_READBACK_AT_DRAW=60000,60001 PALMETTO_READBACK_FULL=1`, a late sample at
  ~274 s): the composed 3840x2160 target and the presented display resource each `mean 0 0 0, non-black 0/8,294,400,
  peak 0, alpha mean 0`, and the 8x8 tile occupancy map is all zeros.

The 130 s draw counts and the 300 s draw/submission counts differ by a few per cent between boots (host frame-rate
variance; the pre-fix 300 s control and boot-userstate-c agree with the acceptance boot's structure, not its exact
frame count). No new draw, pipeline, module or presented content appears in any window.

## 7. Conclusion

- **The platform-service-wait hypothesis is refuted for this build.** The loading loop does not poll any user, session,
  store, save, package, system, game, UI, networking or accessibility API: 15 platform-service calls plus 10-99
  async/task-queue calls in a startup burst between +0.92 s and +21.56 s, zero in the remaining 278 s of the 300 s
  acceptance window, while the black two-pass loop keeps running.
- **Completing the one platform sequence the loading stage consumes does not advance it.** The XPackage surface was
  found dead by measurement (`IXPackageImpl4` refused, package readiness `E_NOTIMPL`) and is now answered as a console
  would answer it behind opt-in switches; the boot's pipeline population and presented image are unchanged.
- **The gate remains title-side**, consistent with the loader measurement in [streamed_consumer.md](streamed_consumer.md)
  section 6b and the wait samples in [wait_state.md](wait_state.md): a loading stage above the streaming job is never
  dispatched again, and no shim value is read after +21.56 s.
- Two real port defects were found along the way and are fixed behind switches (section 4 and section 5): an interface
  version the shim did not serve, which made every XPackage call fail silently, and the package-readiness calls plus
  the progress registration, which could never report "ready".

## 8. Switches, trace and how to reproduce

All switches are opt-in, default off, and change nothing when unset (the same lines, answers and call counts as
before).

| switch | effect |
|---|---|
| `PALMETTO_USER_TRACE=1` | platform-call trace: every `QueryApiImpl` request (class, interface, answer) and every served `XUser*` slot (name, answer), first 8 calls per entry then powers of 10, with process-relative time and caller `module+RVA`; also logs the XPackage readiness calls. Off: no line, no counter. |
| `PALMETTO_PACKAGE_V4=1` | accepts `IXPackageImpl4`'s interface id in `QueryApiImpl`; the default answer for it stays `E_NOINTERFACE`. |
| `PALMETTO_PACKAGE_READY=1` | answers XPackage slots 5/6/9/39 as a console with an installed package (monitor created, one delivered progress notification, enumeration of additional packages empty, close); the default answers stay `E_NOTIMPL` with the same one-time log. Only reachable together with `PALMETTO_PACKAGE_V4`. |

```powershell
$env:PALMETTO_SHADERS='D:\leonida-work\shaders\spirv'; $env:PALMETTO_D3D12='vulkan'
$env:PALMETTO_SKIP_INT3='1'; $env:PALMETTO_SEED_GLOBAL='75DF268:D8'
$env:PALMETTO_USER_TRACE='1'; $env:PALMETTO_PACKAGE_V4='1'; $env:PALMETTO_PACKAGE_READY='1'
$env:PALMETTO_CAPTURE='D:\leonida-work\capture-userstate-f'; $env:PALMETTO_CAPTURE_EVERY='2000'
$env:PALMETTO_READBACK_AT_DRAW='60000,60001'; $env:PALMETTO_READBACK_FULL='1'
python -I tooling/boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay D:\leonida-work\overlay-userstate-f --out D:\leonida-work\boot-userstate-f --timeout 300
```

Measured evidence: `D:\leonida-work\boot-userstate-{a,b,c,e,f}` (stderr logs, summaries, JSONL traces),
`D:\leonida-work\capture-userstate-f` (presenter captures), `D:\leonida-work\gdk-calls` (the static runtime-call
report). All ALIVE to their timeout, 0 exceptions.

## 9. Tests

`xgameruntime_user_test` (58 checks, 0 failures) covers the new faces:

- `IXPackageImpl4` is `E_NOINTERFACE` by default; with the seam on it gets the package object, and
  `IXPackageImpl3` still gets the same vtable;
- slots 5, 9 and 39 are `E_NOTIMPL` by default (and slot 5 writes no output);
- with `PALMETTO_PACKAGE_READY`: the monitor is created and written; a null monitor pointer and a read-only monitor
  pointer are refused (not written); the progress registration returns a token and the registered callback is
  delivered exactly once through the queue the monitor was created with; enumerate returns `S_OK`; close succeeds.

The trace wrappers are exercised through the same vtable face the boot uses (the test's default run has the trace off;
the traced face is selected when `PALMETTO_USER_TRACE` is set).

## 10. Limits

- The trace names the class and interface of each call; the exact slot is only traced for the XUser and XPackage
  entry points. For the remaining calls the slot comes from the static thunk list: `XDisplayImpl` has one referenced
  slot (4), `XPersistentLocalStorageImpl` has slots 4-5 referenced and one call was made (either `GetPath` or
  `GetSpaceInfo`).
- `XThreadingImpl`'s count is bounded to 10..99 by the tenth-power logging (its 10th call is the last line); it is
  async/task-queue plumbing, not a platform state query, and it is never read repeatedly after +21.56 s.
- This page only excludes the platform-service class of wait. Naming the title-side stage dispatcher is the next
  measurement ([streamed_consumer.md](streamed_consumer.md) section 9): sample its stack and watch its stage word.

No game data, package paths, shader bytes or extracted tables are reproduced here; only API names, call counts,
HRESULTs, the title's own RVAs and readback statistics.
