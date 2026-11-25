# The idle wait after the pipeline precompile is not a package or platform gate: measured

**Status: measured, negative.** After the pipeline precompile the title's streaming loader stops posting read
batches, the loader job's armed byte stays cleared, and the title presents a stationary black two-pass loop
([streamed_consumer.md](streamed_consumer.md), [wait_state.md](wait_state.md)). The platform-service branch of the wait
was already refuted from API-call counts over 300 s in [loading_gate.md](loading_gate.md); this page re-tests it from
the **loader side**, with the loader begin/end detours and the armed-byte reader, so a change in the loader state
machine would be visible even if the platform call count did not move. Three 130 s boots of one binary compare a
control with two arms that serve the whole documented platform/package path the loading stage touches.

**Short answer: all three arms are inert.** In both package-served arms every `XPackage` request answers `S_OK`,
the readiness calls succeed and the title's own installation-progress callback runs, yet the loader runs the same
begin/end batch sequence, goes dormant after its last small batch and stays dormant: 0 further load batches, the
same pipeline population (10,241 slot-57 records, 41,545 slot-55 derivations, 51,786 graphics + 95 compute
pipelines), the same two-pass presentation and 6 presenter captures per arm, all uniformly black. The only
difference between the arms is **when** the last small batch lands relative to the boot's start, and that tracks
the boot's own pipeline-precompile speed (section 4), not the switches.

## 1. Arms, switches and the common measurement layer

One runtime binary for all boots: `port/build/palmetto.exe`, SHA-256 `0570EFBC0E174945C8DF0223BF269E35F74AE66843FAEF3BB47F610E4F8BB6D7`,
build time 2026-11-25 10:23:56. The retail package, the overlay directories and the capture directories are the
usual ones outside the repository. Every boot ran under `_setup/boot_lock.ps1 -Acquire -Who platformgate`, one at a
time, 130 s, and released the lock afterwards.

Standard environment (as every normal boot): `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`.

| arm | package/platform switches added on top of the standard environment |
|---|---|
| A control | none |
| B v4+ready | `PALMETTO_PACKAGE_V4=1` `PALMETTO_PACKAGE_READY=1` |
| C all-platform | B plus `PALMETTO_PACKAGED=1`, `PALMETTO_PACKAGE_ID=PalmettoPackageIdentifier0000000`, `PALMETTO_DEVICE_TYPE=7`, `PALMETTO_ANALYTICS_FORM=target console`, `PALMETTO_GAMERTAG=PalmettoTest` |

Common measurement layer, identical in every arm (all opt-in, all documented in [dev-tools.md](dev-tools.md), and
all change no answer):

| switch | what it records |
|---|---|
| `PALMETTO_PIPELINE_TIMELINE=10` | every 10 s: slot-57 record calls/converted/refused, slot-55 derivations, graphics and compute pipeline creations, queue submissions, presents, console-proxy calls, threads. |
| `PALMETTO_LOADER_HOOK=1` | entry detours on the loader begin/end/re-init/completion entries; each begin/end line carries the loader object, its armed byte (+0x78) and in-flight count (+0x64). |
| `PALMETTO_DS_ARRAYS=1` | every 250 ms for 25 s, then 1 Hz: the loader object's armed byte and in-flight count, and every live status array's enqueues/completions/polls. |
| `PALMETTO_DS_LOAD_TRACE=0,26` | the load-window DirectStorage trace and the per-submit/poll ordering trace (submit size, first poll after a submit, loader armed/in-flight at that instant). |
| `PALMETTO_DS_STACK=1` | 8 poll and 4 enqueue stack scans; needed because the loader-state readers take the title image base from the first scan. |
| `PALMETTO_USER_TRACE=1` | every `QueryApiImpl` request (class, interface, answered HRESULT) and every served platform slot, with process-relative time and caller module+RVA. |
| `PALMETTO_CAPTURE`, `PALMETTO_CAPTURE_EVERY=2000`, `PALMETTO_CAPTURE_LIMIT=8` | presenter captures of the swapchain image every 2,000 presented frames, at most 8, with their mean rgb. |

Documented platform/package switches that were **not** set, and why:

| switch | why it is not in an arm |
|---|---|
| `PALMETTO_CONTENT_PACKAGES` | takes `id=path;...` content-package mappings and would mount real content-package paths; this build has no additional content package to point it at, so it is skipped rather than guessed (a package path). |
| `PALMETTO_SAVE_DIR` | takes a filesystem path for the save store; the title makes no `XGameSave` call in the whole boot (zero in the 300 s table of [loading_gate.md](loading_gate.md)), so a path here cannot affect the loading stage (and it is a path). |
| `PALMETTO_HDR`, `PALMETTO_LOG_GAME_EVENTS`, `PALMETTO_PROMPTS`, `PALMETTO_PROMPTS_SWITCH_MS` | display/HDR, game-event logging and the input prompt family - not answers to a platform-state query on this path. |
| `PALMETTO_XG_TRACE`, `PALMETTO_XG_DUMP`, `PALMETTO_XG_LAYOUT`, and the other `PALMETTO_*_TRACE` graphics switches | graphics/console-slot tracing, not platform or package state. |
| `PALMETTO_DEVICE_TYPE=7`, `PALMETTO_ANALYTICS_FORM=target console`, `PALMETTO_PACKAGED=1` | already the shim's defaults (console variant 7, form "target console", packaged true); arm C sets them explicitly to cover the whole documented platform surface, so they are no-ops by construction. |
| `PALMETTO_PACKAGE_ID` | in arm C it is set to the shim's own default placeholder identifier, so the answer is unchanged; a different identifier would only change a string the title never compares. |

## 2. Is the platform path served? The per-arm platform and package lines

The loading stage's package init runs at about +2 s and asks for the `IXPackageImpl4` interface
(`0b9d556e-ab70-43f4-baa0-9999ac4cb040`) through the title's own thunks. In the control the request is refused, so no
`XPackage` slot is ever reached. In B and C the object is handed out (`S_OK`) and the title's whole package init
runs: two `XPackageIsPackagedProcess` calls, two package-identifier reads, the installation-monitor creation, the
installation-progress registration (whose callback is delivered once) and the additional-package enumeration -
every one answered `S_OK`, as a console with an installed package answers it. The lines below are every platform
line each boot printed (first 8 calls per entry point, then every tenth-power call; nothing is elided).

### Arm A (control): the package path is refused

`21` platform lines between +0.91 s and +23.74 s. The three `XPackageImpl`
requests answer `E_NOINTERFACE` (`0xffffffff80004002`), so no `XPackage` slot runs and no readiness line exists:

| time (s) | platform call | # | caller (title module+RVA) | answered |
|---:|---|---:|---|---|
| +0.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.92 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.92 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.92 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.92 | `XUserRegisterForChangeEvent` | 1 | `game_scarlett_master.exe+0x55dcdcb` | `0x0` |
| +2.94 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 1 | `game_scarlett_master.exe+0x55de08c` | `0xffffffff80004002` |
| +2.94 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 2 | `game_scarlett_master.exe+0x55de08c` | `0xffffffff80004002` |
| +2.94 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.94 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 3 | `game_scarlett_master.exe+0x55de08c` | `0xffffffff80004002` |
| +2.95 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 5 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.95 | `QueryApiImpl XPersistentLocalStorageImpl iid 41a4e10c-5a7e-41d9-8c37-37bde62a07d6` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +3.01 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 6 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +3.01 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +3.46 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 7 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +3.46 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +19.52 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 8 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +19.52 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +23.73 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +23.74 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 10 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +23.74 | `QueryApiImpl XAccessibilityImpl iid d722b373-8c4d-4692-9e51-d6ad9b37aa7d` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |

Other platform lines (the line itself carries no clock; the time is the nearest preceding platform call):

| time (s) | line |
|---:|---|
| - | `InitializeApiImplEx2(gdk=10002, gs=7901, mode=0x2, options=none) (offline)` |
| +3.01 | `XDisplayTryEnableHdrMode: HDR disabled (SDR output; PALMETTO_HDR=1 asks for HDR)` |
| +23.74 | `XAccessibilityImpl slot 9 is not available offline (E_FAIL)` |

### Arm B (V4 + READY): the package path is served

`32` platform lines between +0.90 s and +26.56 s. The first `XPackageImpl`
request answers `S_OK`, the title then calls `XPackageIsPackagedProcess` (returns 1),
`XPackageGetCurrentProcessPackageIdentifier` (`S_OK`), `XPackageCreateInstallationMonitor` (`S_OK`),
`XPackageRegisterInstallationProgressChanged` (`S_OK`) and `XPackageEnumeratePackages` (`S_OK`), and the callback
registered through the monitor's task queue is delivered once. The three readiness handlers print their own
one-time lines as well:

| time (s) | platform call | # | caller (title module+RVA) | answered |
|---:|---|---:|---|---|
| +0.90 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.90 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.91 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.92 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.92 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.92 | `XUserRegisterForChangeEvent` | 1 | `game_scarlett_master.exe+0x55dcdcb` | `0x0` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageIsPackagedProcess` | 1 | `game_scarlett_master.exe+0x55e0087` | `0x1` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageGetCurrentProcessPackageIdentifier` | 1 | `game_scarlett_master.exe+0x55e001f` | `0x0` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageIsPackagedProcess` | 2 | `game_scarlett_master.exe+0x55e0087` | `0x1` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageGetCurrentProcessPackageIdentifier` | 2 | `game_scarlett_master.exe+0x55e001f` | `0x0` |
| +1.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 5 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageCreateInstallationMonitor` | 1 | `game_scarlett_master.exe+0x55e0130` | `0x0` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 6 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageRegisterInstallationProgressChanged` | 1 | `game_scarlett_master.exe+0x55e029e` | `0x0` |
| +1.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 5 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 7 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.91 | `XPackageEnumeratePackages (IXPackageImpl3)` | 1 | `game_scarlett_master.exe+0x55e06fa` | `0x0` |
| +1.92 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 6 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.92 | `QueryApiImpl XPersistentLocalStorageImpl iid 41a4e10c-5a7e-41d9-8c37-37bde62a07d6` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.97 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 7 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.97 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.40 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 8 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.40 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +22.52 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +26.56 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 10 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +26.56 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +26.56 | `QueryApiImpl XAccessibilityImpl iid d722b373-8c4d-4692-9e51-d6ad9b37aa7d` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |

Other platform lines (the line itself carries no clock; the time is the nearest preceding platform call):

| time (s) | line |
|---:|---|
| - | `InitializeApiImplEx2(gdk=10002, gs=7901, mode=0x2, options=none) (offline)` |
| +1.91 | `XPackageCreateInstallationMonitor: the offline package is installed, so the monitor starts complete (PALMETTO_PACKAGE_READY=1)` |
| +1.91 | `XPackageRegisterInstallationProgressChanged: one initial "complete" notification for the installed package, through the monitor's queue (PALMETTO_PACKAGE_READY=1)` |
| +1.91 | `XPackageEnumeratePackages: no additional package is installed (PALMETTO_PACKAGE_READY=1)` |
| +1.97 | `XDisplayTryEnableHdrMode: HDR disabled (SDR output; PALMETTO_HDR=1 asks for HDR)` |
| - | `XPackage installation-progress notification delivered to the title's callback (PALMETTO_PACKAGE_READY=1)` |
| +26.56 | `XAccessibilityImpl slot 9 is not available offline (E_FAIL)` |

### Arm C (all documented platform switches): the same served chain

`32` platform lines between +0.91 s and +27.00 s - the same sequence as arm B, with
C's own timings; the extra value switches of arm C change no answer, and the one answer they do change (the
gamertag string) is never read: the whole boot makes one `XUser` call, `XUserRegisterForChangeEvent`, and no gamertag
query.

| time (s) | platform call | # | caller (title module+RVA) | answered |
|---:|---|---:|---|---|
| +0.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.91 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.91 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.93 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.93 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +0.93 | `XUserRegisterForChangeEvent` | 1 | `game_scarlett_master.exe+0x55dcdcb` | `0x0` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageIsPackagedProcess` | 1 | `game_scarlett_master.exe+0x55e0087` | `0x1` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageGetCurrentProcessPackageIdentifier` | 1 | `game_scarlett_master.exe+0x55e001f` | `0x0` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageIsPackagedProcess` | 2 | `game_scarlett_master.exe+0x55e0087` | `0x1` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageGetCurrentProcessPackageIdentifier` | 2 | `game_scarlett_master.exe+0x55e001f` | `0x0` |
| +1.93 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 5 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageCreateInstallationMonitor` | 1 | `game_scarlett_master.exe+0x55e0130` | `0x0` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 6 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageRegisterInstallationProgressChanged` | 1 | `game_scarlett_master.exe+0x55e029e` | `0x0` |
| +1.93 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 5 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040` | 7 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.93 | `XPackageEnumeratePackages (IXPackageImpl3)` | 1 | `game_scarlett_master.exe+0x55e06fa` | `0x0` |
| +1.94 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 6 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.94 | `QueryApiImpl XPersistentLocalStorageImpl iid 41a4e10c-5a7e-41d9-8c37-37bde62a07d6` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.99 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 7 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +1.99 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.44 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 8 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +2.44 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 2 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +23.05 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 3 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +27.00 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428` | 10 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +27.00 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3` | 4 | `game_scarlett_master.exe+0x55de08c` | `0x0` |
| +27.00 | `QueryApiImpl XAccessibilityImpl iid d722b373-8c4d-4692-9e51-d6ad9b37aa7d` | 1 | `game_scarlett_master.exe+0x55de08c` | `0x0` |

Other platform lines (the line itself carries no clock; the time is the nearest preceding platform call):

| time (s) | line |
|---:|---|
| - | `InitializeApiImplEx2(gdk=10002, gs=7901, mode=0x2, options=none) (offline)` |
| +1.93 | `XPackageCreateInstallationMonitor: the offline package is installed, so the monitor starts complete (PALMETTO_PACKAGE_READY=1)` |
| +1.93 | `XPackageRegisterInstallationProgressChanged: one initial "complete" notification for the installed package, through the monitor's queue (PALMETTO_PACKAGE_READY=1)` |
| +1.93 | `XPackageEnumeratePackages: no additional package is installed (PALMETTO_PACKAGE_READY=1)` |
| +1.99 | `XDisplayTryEnableHdrMode: HDR disabled (SDR output; PALMETTO_HDR=1 asks for HDR)` |
| - | `XPackage installation-progress notification delivered to the title's callback (PALMETTO_PACKAGE_READY=1)` |
| +27.00 | `XAccessibilityImpl slot 9 is not available offline (E_FAIL)` |

**So the platform path is served in B and C**: every request the loading stage makes of the package/user/platform
surface is answered as a console with an installed package answers it, including the readiness chain that the
control could never reach. The rest of the page measures what the loader does next.

## 3. The loader state machine under each arm

The loader object lives at the title's `[[game+0x79F6D70]]`; byte `+0x78` is its armed flag and dword `+0x64` its
in-flight count. The begin/end detours (`game+0xE88F0` and `game+0xE8960`, both verified with the E9 jumper present)
record every batch of the loader that owns the global, and the 250 ms armed-byte reader records the state between
them. All clocks in this section are the DirectStorage shim's own clock (seconds since its first traced call), not
process time; the platform times of section 2 are process-relative and are not mixed into this table.

| loader event (DS clock) | A control | B v4+ready | C all-platform |
|---|---:|---:|---:|
| begin/end cycles on the loader global | 17 | 18 | 18 |
| big batch (3,676 requests in flight) end | +1.509 | +2.111 | +2.117 |
| single-request batch 1 end | +11.484 | +15.747 | +16.169 |
| single-request batch 2 end | +16.639 | +20.554 | +20.888 |
| final small-batch burst (by batch ends) | 10 batches, +16.643 to +16.666 | 11 batches, +20.556 to +20.590 | 11 batches, +20.892 to +20.924 |
| last loader-detour record of any object | +16.685 | +20.592 | +20.927 |
| any loader begin/end record after that, to 130 s | none | none | none |

The sequence is the same in every arm: a first burst of five batches (42, 6, 1, 474 and 3,676 requests in flight
as the detours recorded them), then two single-request batches, then a final burst of 10-11 short batches, and
then nothing. The completion detour
(`game+0x4F65820`) records a single early hit in every arm (caller `game+0x43A492C`, loader global still 0 at that
point); the re-init and spinner detours never fired in any arm. The one extra begin/end cycle in B and C belongs to
that final burst (one more short batch than the control posts).

The armed byte at and after the fixed 16.5 s check (250 ms sampling; the final burst batches are shorter than the
sampling step, so the detour records above are the primary evidence and the sampler is the cross-check):

| loader armed state | A control | B v4+ready | C all-platform |
|---|---|---|---|
| first sample at or after 16.5 s | +16.575 armed 0 in-flight 0 | +16.612 armed 0 in-flight 0 | +16.615 armed 0 in-flight 0 |
| last nonzero armed sample | +1.509 armed 1 in-flight 3677 | +20.556 armed 1 in-flight 4 | +2.117 armed 1 in-flight 3677 |
| nonzero armed samples at or after 16.5 s | none | one: +20.556 armed 1 in-flight 4 | none |
| status array #17 (the loader's owner) last enqueue / last completion | +16.665 / +16.665 | +20.587 / +20.590 | +20.920 / +20.924 |
| status array #17 enqueues / completions, last sample (+75 s / +76 s / +76 s) | 17 / 17 | 18 / 18 | 16 / 16 |
| last status-array enqueue of any array | +16.683 (array #3) | +20.591 (array #3) | +20.925 (array #3) |
| DirectStorage heartbeat, last printed line | +20.1 s: 4235 enqueued, 18 submits | +20.2 s: 4200 enqueued, 6 submits | +20.2 s: 4200 enqueued, 6 submits |

Reading the table: at the 16.5 s check **all three arms have the loader disarmed** (armed 0, in-flight 0). In the
control it is never re-armed afterwards. In B the last burst lands at +20.55 s and one 250 ms sample catches it
armed (`+20.556`, in-flight 4); in C the last burst lands at +20.89 s between two samples, so the sampler misses it.
After each arm's last burst the loader is never armed again, and the last enqueue of any status array in the
whole boot is the one that burst makes: array #3's single enqueue. The array dump keeps sampling to about +76 s
and every array's enqueue and completion counters are unchanged from that point. **No arm opens a new loader
stage; no arm leaves the loader armed; no arm changes what the loader asks for.**

## 4. Why the last batch lands later in B and C: boot speed, not the switches

The three arms did not run at the same speed. The pipeline timeline printed at fixed wall-clock marks is the direct
evidence: at +20 s the control had already derived 40,229 of its 41,545 slot-55 graphics states, while B had derived
1,434 and C 452. The loader's last small batch lands at the end of that precompile phase, so a slower precompile
pushes it later. A second no-switch boot (the first control run of this slice, kept as evidence) sits between the
two cases and shows the same correlation: it was slower than the control at +20 s (30,150 of 41,545 derived) and
its last loader record is correspondingly later (+17.34 s, against the control's +16.69 s).

| boot | slot-55 derived at +10 s | slot-55 derived at +20 s | last loader-detour record (DS clock) |
|---|---:|---:|---:|
| A control | 0 | 40,229 | +16.685 |
| B v4+ready | 0 | 1,434 | +20.592 |
| C all-platform | 0 | 452 | +20.927 |
| first control run (no switches, instrumentation-repair run) | 0 | 30,150 | +17.341 |

So the 4 s gap between the control and the package arms in section 3 is the same boot-speed variable, not a switch
effect: the arm that ran fastest (A) posts its last batch first, and the two slower arms post theirs later by
roughly the amount their graphics precompile slipped. By +30 s every arm has the identical full population
(41,545 slot-55, 51,786 graphics, 95 compute). One boot per arm cannot put a bound on that variance - section 8
names the repeat measurement that would - but the durable loader state does not depend on it, as the rest of the
section shows.

## 5. Population, presents and captures

| measurement at +120 s (last full timeline row of the 130 s boot) | A control | B v4+ready | C all-platform |
|---|---:|---:|---:|
| slot-57 record calls (converted / refused) | 10,241 (10,241 / 0) | 10,241 (10,241 / 0) | 10,241 (10,241 / 0) |
| slot-55 derived graphics states | 41,545 | 41,545 | 41,545 |
| graphics pipelines created | 51,786 | 51,786 | 51,786 |
| compute pipelines created | 95 | 95 | 95 |
| queue submissions (frame loop) | 22,598 | 21,492 | 21,666 |
| presents | 11,298 | 10,745 | 10,832 |
| threads | 85 | 85 | 85 |
| D3D12 trace calls, whole boot | 507,212 | 495,087 | 497,576 |
| presenter captures | 6 | 6 | 6 |
| captures not uniformly black | 0 | 0 | 0 |

- A control: 6 captures, every one mean rgb 0.00 0.00 0.00
- B v4+ready: 6 captures, every one mean rgb 0.00 0.00 0.00
- C all-platform: 6 captures, every one mean rgb 0.00 0.00 0.00

Every arm stops growing its pipeline population at the same values and then runs the same stationary two-pass
frame loop: presents and submissions diverge by a few per cent only because the three boots did not reach the
loop at exactly the same wall-clock (the frame count at +120 s is 11,298 / 10,745 / 10,832). No new pipeline, draw,
module or presented content appears in any arm, and no capture is anything but black.

## 6. Verdict

- **The post-precompile idle wait is not a packaging or platform gate.** Serving the complete documented platform
  path - the `IXPackageImpl4` interface, the installed-package readiness chain, the one progress notification and the
  empty additional-package enumeration, plus every other documented platform switch with a console-faithful value -
  changes nothing durable in the loader: the same begin/end batch sequence, the same dormancy after the last small
  batch, no further load batch to 130 s, the same pipeline population, the same black two-pass presentation.
- **The platform path is demonstrably served, not silently refused.** In B and C the log lines show every `XPackage`
  request answered `S_OK`, the three readiness handlers running their console answers, and the title's callback
  receiving the notification; this is the same load-time package init that the control never reaches.
- **The only inter-arm difference is timing, and it tracks boot speed.** The last small batch lands at +16.7 /
  +20.6 / +20.9 s (DS clock) in A/B/C, matching how fast each boot got through its 41,545-state graphics precompile
  (40,229 / 1,434 / 452 states at +20 s), and the second no-switch boot sits between them (+17.3 s), following the
  same precompile-rate ordering. The end state is invariant across all of them.
- **The gate stays title-side**, as [streamed_consumer.md](streamed_consumer.md) section 6b and [loading_gate.md](loading_gate.md)
  concluded: a loading stage above the streaming job is never dispatched again, and no shim value is read after the
  startup burst. The package/platform switches were the last untested shim-side lever on this path (except a
  content-package mount, which needs a content package this build does not have); they are now measured inert
  on the loader side too.

## 7. Limits

1. One 130 s boot per arm. The boots did not run at the same speed (section 4), so they bound the durable state, not
   the last-batch clock; a per-arm timing effect smaller than the host-load variance cannot be excluded with n = 1.
2. The two clocks are kept apart: platform-call times are process-relative (`PALMETTO_USER_TRACE`), loader and
   DirectStorage times are the DirectStorage shim's clock. A loader line is placed at the nearest preceding
   DirectStorage clock line; entries with no clock line within a few lines are shown as `-` rather than guessed.
3. `PALMETTO_DS_ARRAYS` samples the armed byte every 250 ms; the final burst batches complete in well under that, so
   the sampler misses them in A and C and catches one sample in B. The detour records are the primary evidence and
   the sampler is the cross-check, as stated in section 3.
4. The loader `completion` and `spinner` detours were installed and verified (`E9` present at 5 s). The completion
   detour recorded one early call per boot (caller `game+0x43A492C`, before the loader global existed) and the
   spinner detour recorded none, so "completion" here is the begin/end detour pair plus the status array, not a
   per-poll completion line. The verify-at-60 s pass and the raw RIP sample did not reach the log before the 130 s
   timeout in these boots.
5. Presenter captures are one swapchain size, every 2,000 presented frames up to 8; a content change that appeared
   and vanished between two captures would be missed, but the loader and population evidence is continuous.
6. The host was shared with other agents' builds and tools during the boots; the boot lock only serialises `palmetto`
   runs. This is the likely source of the boot-speed spread documented in section 4.
7. Arm C's extra switches are the shim defaults (`PALMETTO_PACKAGED=1`, `PALMETTO_DEVICE_TYPE=7`,
   `PALMETTO_ANALYTICS_FORM=target console`, the default `PALMETTO_PACKAGE_ID`) plus one string the title never reads
   (`PALMETTO_GAMERTAG`), so C duplicates B by construction; it is kept as the "every documented platform switch
   on" arm.
8. No game data, package paths, shader bytes or extracted tables are reproduced here: only API names, call counts,
   HRESULTs, the title's own RVAs, clocks and image means.

## 8. Next falsifiable measurement

1. **Repeat the arms to bound the timing.** Interleave A, B, A, B, A, B (six 130 s boots) and use two statistics:
   the DirectStorage clock of the last loader begin, and the slot-55 count at +20 s. If the switches are inert, each
   arm's last-begin clock is a function of its own precompile rate and the two arms do not separate once that rate is
   controlled for; if B is systematically later at the same precompile rate, the package chain does move a stage
   boundary and that boundary is worth instrumenting.
2. **If a shift is confirmed**, instrument the stage dispatcher above the streaming job - the indirect table entry at
   the title `.rdata` RVA `0x62FC780` and the code around `game+0x248E070` named in [streamed_consumer.md](streamed_consumer.md)
   section 6a - and watch whether the package-ready state is read by that stage.
3. **Content packages.** `PALMETTO_CONTENT_PACKAGES` is the one documented switch left out because it needs real
   content-package paths, which this package set does not have; if a content package becomes available, the
   additional-package branch (slots 23-26 and 39) is the remaining package sub-path to serve.
4. The title-side stage dispatch remains the named gate ([streamed_consumer.md](streamed_consumer.md) section 9);
   this measurement removes the package/platform class from the shim-side hypothesis list for good.

## 9. Reproduction and evidence

The common layer and the arm switch sets are in the arm script used for all three boots; the harness is the
repository's boot reporter (invoked from the checkout root, capture folders outside the repository):

```powershell
python -I tooling/boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay <work>\overlay-<arm> --out <work>\boot-platformgate-<arm> --timeout 130 `
    --env PALMETTO_SHADERS=<work>\shaders\spirv --env PALMETTO_D3D12=vulkan `
    --env PALMETTO_SKIP_INT3=1 --env PALMETTO_SEED_GLOBAL=75DF268:D8 `
    --env PALMETTO_PIPELINE_TIMELINE=10 --env PALMETTO_LOADER_HOOK=1 `
    --env PALMETTO_USER_TRACE=1 --env PALMETTO_DS_STACK=1 --env PALMETTO_DS_ARRAYS=1 `
    --env PALMETTO_DS_LOAD_TRACE=0,26 --env PALMETTO_CAPTURE=<work>\capture-<arm> `
    --env PALMETTO_CAPTURE_EVERY=2000 --env PALMETTO_CAPTURE_LIMIT=8
```

Arm B adds `--env PALMETTO_PACKAGE_V4=1 --env PALMETTO_PACKAGE_READY=1`; arm C adds the five value switches
listed in section 1. Evidence outside the repository: boot directories `boot-platformgate-a-control`,
`boot-platformgate-b-v4ready` and `boot-platformgate-c-allplatform`, each with `stderr.log`, `stdout.log`,
`summary.txt`, `boot.jsonl`, an `arm_meta.txt` naming the binary hash and start/end times, and its presenter
captures; plus `boot-platformgate-ctrl` (the first control run, kept because section 4 cites its timing). All four
boots are `ALIVE 130.2 s` with 0 exceptions: the runtime only ends because the harness kills it at the timeout.

The working tree has since built a newer runtime binary (another slice committed and rebuilt after these boots); the
four evidence boots all ran the hash above, which each boot's `arm_meta.txt` records, so a re-run on a later build
should repeat the arms before being compared. No source file was changed for this measurement; the only
repository change is this new page. Gates: none of the runtime/build/test gates are affected (no code change);
the repository's documentation checks apply to the page and it is written to satisfy them (tracked links,
repository paths, flags and environment names all resolve).
