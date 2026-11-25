# The offline user's persistent profile, and a save store that survives a kill

**Status: measured; one slice of [cleanroom_peers.md](cleanroom_peers.md).** The peer claims this slice tests are
public README wording only: Five Star Recomp says its loader answers the first-run screens and that "loading the game's
saved settings" makes later boots skip those screens and reach the story load in about 39 s; VIctory and XCL describe
"one signed-in offline user, licences, packages, saves that survive a kill at any step". Those are hypotheses. This
page records what our own tree does, what we implemented from that measurement, and one A/B/control set of 130 s boots
plus the isolation pair that splits the two switches.

**Short answer.** The title does call `XPersistentLocalStorageGetPath` once early in the boot (the earlier pages
recorded only the class request), and later it reaches its save subsystem: in the arms where the offline signed-in
event is delivered and the persistent record is kept, `XGameSaveInitializeProvider` runs at about 23-24 s and our
offline provider answers `S_OK`; in the control with both switches off the same subsystem takes the
`XUserAddAsync` path instead and never opens the store in the 130 s window. The isolation arm that keeps the record but
delivers no event behaves like the control, so the record alone is not the lever: the delivered sign-in event is the
necessary switch on the evidence measured here (its sufficiency alone was not booted). The stage word stays 2/2 in every
sample of every arm and no frame gains colour, so the peer's "skip the first-run screens and reach the story load
sooner" is **not** reproduced on this tree; what moves is the save subsystem's own initialisation. The title reads no
save container or blob in any of the seven measured boots, so "loading saved settings" remains untested as such.

## 1. Why these imports/calls are needed

The boot's platform surface before this slice was one user call (the change-event registration at about +1.9 s), one
`XPersistentLocalStorageImpl` class request, and **zero** `XStore` and `XGameSave` calls in 130 s and 300 s samples
([user_events.md](user_events.md), [loading_gate.md](loading_gate.md), [runtime_imports.md](runtime_imports.md)). Two
of the peer claims are about exactly the services that surface leaves empty:

- **a signed-in offline user** (VIctory: "one signed-in offline user"). Our user shim already served one user, but
  delivered no change event by default: offline the user never changes ([user_events.md](user_events.md) section 3.1).
- **settings and saves on disk that survive a kill** (VIctory and XCL), and Five Star's "loading the game's saved
  settings" skip. Our `XGameSave` shim already kept containers as files ([saves.md](saves.md)), but nothing in the
  runtime persisted a user profile, nothing exercised the store, and the store's own writes were not flushed before
  their rename.

This slice therefore (a) traces the user, persistent-storage and game-save surfaces over a boot, (b) implements our own
persistent profile record for the one offline user and serves the identity back on later boots, (c) hardens the
`XGameSave` file store against a kill and logs its operations, and (d) measures a fresh boot, a second boot on the same
store, and a control with the switches off, plus an isolation pair.

## 2. Measured shapes

All RVAs are `game_scarlett_master.exe+0x...` in the retail 1.0.2.1 image; times are the shim's process clock.

### 2.1 The user surface over a boot

| what | site | measured |
|---|---|---|
| change-event registration | thunk `0x55DCDCB`, title call `0x5925827`, callback `0x2211C80` (from [user_events.md](user_events.md) 2.2) | 1 call per boot, +2.02 to +2.90 s, `S_OK` |
| delivered `SignedInAgain` (switch on) | our shim, same queue the title registered with | 1 event, +2.07 to +2.90 s; the title answers with `XUserFindUserByLocalId`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle`, `XUserCloseHandle` in the same second |
| no delivered event (switch off) | - | no calls at that point |
| the late save subsystem, event on | - | at +23.5 to +24.3 s: `XUserDuplicateHandle`, `XUserGetId`, `XUserGetLocalId`, `XUserGetGamertag` x9, `XUserGetIsGuest`, `XUserGetAgeGroup`, `XUserCheckPrivilege` x9 |
| the late save subsystem, event off | - | `XUserAddAsync`, `XUserAddResult`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle`, `XUserCloseHandle`, then nothing: at +107.7 s (control both off), +111.0 s (record on, event off) and +119.2 s (first pair's control) |

Read as a statement: with the signed-in event delivered the title's save subsystem already holds a user handle and
duplicates it; without the event it has to add the user itself, and on this tree it does not get past that before the
window ends. The delivered event is the only thing that differs at that point, which is why the isolation pair in
section 5.3 exists.

### 2.2 The persistent-local-storage surface

| slot | measured calls | when | answer |
|---|---:|---|---|
| `XPersistentLocalStorageImpl` class request (`IXPersistentLocalStorageImpl3`, iid `41a4e10c-5a7e-41d9-8c37-37bde62a07d6`) | 1 per boot | +2.03 to +3.92 s | `S_OK` |
| slot 4 `XPersistentLocalStorageGetPath` (caller thunk `0x55E0A07`) | **1 per boot** | +2.04 to +3.92 s | `S_OK`, the UTF-8 path of `<overlay>\.palmetto-persistent` |
| slot 3 `GetPathSize`, slot 5 `GetSpaceInfo`, slots 6/7 `PromptUserForSpace*`, slot 8 `MountForPackage` | 0 in every 130 s boot | - | - |

This is the first page to see a **slot** call on this class: earlier pages counted the class request only
([loading_gate.md](loading_gate.md) table). The title asks the runtime where it may keep persistent data and makes no
further request of that class in the window. Nothing appears in that folder that the title wrote: after every measured
boot it holds only the runtime's own record (section 3.1), or is empty when the store is switched off.

### 2.3 The game-save surface

| call | before this slice | measured now |
|---|---|---|
| `XGameSaveImpl` class request (`IXGameSaveImpl3`, iid `1bfff3af-f14a-40a3-8e35-9ada906593f9`) | 0 | 2 per boot, +23.48 and +24.32 s, only in the arms with the sign-in event and the record |
| `XGameSaveInitializeProvider("00000000-0000-0000-0000-00007952987a")` | 0 | `S_OK` from the offline provider, same second |
| `XGameSaveCreateContainer` / `SubmitBlobWrite` / `ReadBlobData` / `Enumerate*` | 0 | **0 in all seven boots** |
| `XStore*` | 0 | 0 |

So the title does reach its store initialisation in the "on" arms, and our offline provider answers it honestly, but it
never asks for a container or a blob inside the 130 s window. The save store itself remains a measured-shape server for
a caller that has not yet spoken: the one caller it did get was answered (`S_OK`) and created
`<save dir>\local-user` empty.

### 2.4 The runtime's record on disk

One 64-byte record at `<persistent dir>\settings.bin`, little-endian:

```
0x00 u32 magic 'PMPS'          0x04 u32 version = 1        0x08 u32 record size = 64   0x0C u32 boot count
0x10 u64 offline user id       0x18 u64 local id
0x20 char[16] gamertag         0x30 u64 created (unix seconds)
0x38 u32 last boot (unix)      0x3C u32 FNV-1a over the 64 bytes with this field zero
```

## 3. What we implemented

All of it is our own code in this tree's structure; the store switch is on by default because the record is the
runtime's own state and changes no answer the title currently asks for, and `PALMETTO_PERSIST_PROFILE=0` turns it off
(named, bounded, and checked by the tests).

### 3.1 The persistent profile record (`port/shims/xgameruntime.cpp`, `port/shims/persistent_store.h`)

- **Where.** `<overlay>\.palmetto-persistent\settings.bin`, the folder the title itself asks for through
  `XPersistentLocalStorageGetPath`, so `--overlay` moves it with everything else and the package stays read-only.
- **Atomic, kill-safe.** The new record is written to `settings.bin~` with `FILE_FLAG_WRITE_THROUGH`, flushed with
  `FlushFileBuffers`, and only then renamed over `settings.bin` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.
  A kill at any step leaves either the old complete record or the new one, never a truncated one. A `settings.bin~`
  left by a killed write is removed, never read.
- **Validated, not trusted.** Magic, version, size and checksum are checked; a record that fails (a truncated or
  half-written file) is treated as absent, the process starts a fresh one, and one line says so.
- **Bounded.** Read once per process; one line names the create or the load and the boot number. No content is logged.
- **Traced surface.** `PALMETTO_USER_TRACE=1` now also logs each `XPersistentLocalStorage*` slot call (first calls and
  every tenth-power call) with the caller as module+RVA, which is how section 2.2's slot call was found.

### 3.2 The offline user served from the record (`port/shims/xgameruntime_user.cpp`)

The identity the user slots answer - user id, local id, gamertag - comes from the record when one exists and from the
runtime's fixed defaults on the first boot. The record is materialised at the measured registration point, because that
is the one user call the title makes before the late save subsystem. With no overlay (or the switch off) the constants
are served exactly as before, so the default path of every earlier page is unchanged. A test seam forgets the resolved
identity so one process can model a later boot.

### 3.3 The game-save store hardened and logged (`port/shims/xgameruntime_gamesave.cpp`)

- Blob and display-name writes go through a `FILE_FLAG_WRITE_THROUGH` handle and `FlushFileBuffers` before their
  `MoveFileExW` rename; the display name now uses the same temporary-plus-rename as a blob.
- When a provider opens, `"<blob>~"` and `".display~"` temporary files left by a killed update are removed (they are
  never read as blobs: the name codec refuses a trailing `~`), so the saves served after a kill are the ones before it.
  One bounded line reports how many were removed.
- `PALMETTO_USER_TRACE=1` adds a bounded, named log of provider open, container create/delete, blob write/delete and
  submit: names and sizes only, never blob content and never game data.
- The layout, quota, name rules and answers are unchanged ([saves.md](saves.md), [runtime_imports.md](runtime_imports.md)).

### 3.4 What is not touched

`PALMETTO_PERSIST_PROFILE` is the slice's only new environment variable; its default is the documented path. The
`XGameSave`/`XStore` slot order, the persistent-storage slots, and every other class are unchanged. No CMake, `main.cpp`,
D3D12, DirectStorage or `xmem` file is edited. The opt-in `PALMETTO_STATE_TRACE` probe the round-2/3 arms use lives in
`port/src/main.cpp` and belongs to the state-machine slice, not this one.

## 4. Cross-check against the reference and the peer claims

The peer projects are evidence only; no peer source was read for this slice, and the behaviour was re-derived by
measuring our own title (the programme's rule in [cleanroom_peers.md](cleanroom_peers.md)). The claims and what our
tree says:

- **"Loading the game's saved settings makes later boots skip the first-run screens"** (Five Star Recomp, public
  README). Our title never reads a save container or blob in any measured boot, so it cannot be loading saved settings
  through the store; it does ask for the persistent folder and (in the "on" arms) initialises the save provider at about
  23-24 s, then makes no store call. No first-run screen is skipped on our tree and no stage changes. The claim is
  **not reproduced**, and the store half of it is unexercised rather than disproved.
- **"One signed-in offline user ... saves that survive a kill at any step"** (VIctory, XCL, public READMEs). Our user
  shim now serves exactly one signed-in offline user whose identity is persisted and re-served; the file store's blob
  and display writes are atomic and cleaned after a kill. The user part is **implemented and measured**; the "survive a
  kill" part is measured at the file level in the tests (a truncated record and a killed-update temporary file are both
  refused and cleaned), not by killing a console session.
- **Microsoft's public GDK reference** (XGameSave, XPersistentLocalStorage pages on learn.microsoft.com/gaming) is the
  source for slot order, error codes and the `XAsyncBlock` shapes already documented in
  [runtime_imports.md](runtime_imports.md); nothing was copied from a GDK header or library.
- **The public GPL-3.0 reference ubiquitarianism/leonida** was not read for this slice. Its only relevant documented
  behaviour - the offline `XStore` `*Async` completion fix - was cross-checked in [user_events.md](user_events.md)
  section 4, and this slice does not change that surface.

## 5. Measurement

One binary, SHA-256 `C13734CA3E10B5909C7044611231EE3BA0BA0C0E8E881086D15F6B53C541E374`, built 2026-11-25 17:42:14 from
the working tree that also carries other slices' uncommitted files (`port/src/main.cpp` probes, upscale's files); every
arm below is the same binary, so the comparisons are within one tree state. Standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_SAVE_DIR=<overlay>\saves`) plus the measurement layer: `PALMETTO_PIPELINE_TIMELINE=10`,
`PALMETTO_LOADER_HOOK=1`, `PALMETTO_DS_STACK=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_USER_TRACE=1`, presenter captures
(`PALMETTO_CAPTURE_EVERY=2000`, `PALMETTO_CAPTURE_LIMIT=8`); the round-2/3/4 arms add `PALMETTO_STATE_TRACE=2,130`.
All boots are 130 s under `_setup\boot_lock.ps1`, one at a time, overlay and captures outside the repository.

### 5.1 The pair with the state sampler (the comparable one)

| measurement | A fresh overlay | B second boot, same overlay | C control, fresh overlay |
|---|---|---|---|
| switches | event on, store on | event on, store on | event off, store off |
| verdict | ALIVE 130.4 s, 0 exceptions | ALIVE 130.3 s, 0 exceptions | ALIVE 130.3 s, 0 exceptions |
| traced D3D12 calls | 234,861 | 237,093 | 240,194 |
| platform-trace lines | 108 | 108 | 50 |
| user class requests / user slot calls | 18 / 51 | 18 / 51 | 7 / 7 |
| `XPersistentLocalStorageGetPath` | 1, +2.0 s, `S_OK` | 1, +2.0 s, `S_OK` | 1, +1.6 s, `S_OK` |
| `SignedInAgain` delivered | 1, +2.21 s | 1, +2.07 s | none |
| profile record | created, boot 1 | loaded, boot 2 | not written, not read |
| `XGameSaveImpl` class requests | 2 | 2 | 0 |
| `XGameSaveInitializeProvider` | **`S_OK`, +24.32 s** | **`S_OK`, +23.48 s** | **never** |
| save container/blob calls | 0 | 0 | 0 |
| the switch-off late path | - | - | `XUserAddAsync` path at +107.72 s, then nothing |
| stage word / requested stage (2 s samples) | 2 / 2, every sample | 2 / 2, every sample | 2 / 2, every sample |
| stage transitions | init 0->1 at 4.0 s, 1->2 at 20.0 s; loading 1 at 20.0 s | init 0->1 at 4.0 s, 1->2 at 20.0 s; loading 1 at 20.0 s | init 0->1 at 4.0 s, 1->2 at 16.0 s; loading 1 at 16.0 s |
| loader begin/end records | 200 / 200 | 200 / 200 | 200 / 200 |
| loader object/global armed at the last record | 1 / 0 | 1 / 0 | 1 / 0 |
| last pipeline-timeline row (+120 s) | slot57 10,241; slot55 41,545; graphics 51,786; compute 474/403; submits 9,242; presents 4,620 | slot57 10,241; slot55 41,545; graphics 51,786; compute 474/403; submits 9,392; presents 4,695 | slot57 10,241; slot55 41,545; graphics 51,786; compute 476/404; submits 9,458; presents 4,728 |
| captures | frames 1, 2001, 4001, 2564x1421, mean rgb 0.00 0.00 0.00 | frames 1, 2001, 4001, black | frames 1, 2001, 4001, black |
| what the boot left in the stores | `settings.bin` 64 B; `saves\local-user` empty | `settings.bin` 64 B; `saves\local-user` empty | persistent folder empty; no `local-user` |

Reading the table:

- **The two switches change the save subsystem's route, and nothing else that was measured.** A and B both open the
  offline provider with `S_OK` and both leave the stage word at 2/2 with the same loader records, the same slot-57
  population and the same black captures. C, with both switches off, takes the `XUserAddAsync` route at +107.72 s and
  never reaches the store. That is the whole measured effect, and it is not a stage move.
- **The second boot changes no answer.** B differs from A only in loading the record (boot 2 instead of creating boot 1)
  and in ordinary boot timing; the platform answers are the same.
- **The store is initialised but not used.** Both "on" arms open the provider and then make no container or blob call.
  No save data is read or written by the title here.

### 5.2 The first pair, which found the late phase

An earlier set on the same binary (same environment, no state sampler) surfaced the late phase and is kept for its
honest timing: the fresh arm was slow (114 loader records instead of 200) and never left the loading stage by 130 s, so
it could not show the late path; the second boot (store loaded, event delivered) reached `XGameSaveInitializeProvider`
at +123.56 s, `S_OK`; the control (switches off) reached the `XUserAddAsync` route at +119.16 s and then nothing. The
paired verdict is the same as section 5.1, and the slow fresh arm is why the pair in 5.1 was re-run with a sampler and
all three arms on comparable footing.

### 5.3 The isolation pair

The pair splits the two switches: D keeps the record but delivers no event, E would deliver the event but keep no
record. D was booted; E did not get a slot before the reporting deadline (the boot lock was held by other slices' arms
through the whole window), so the pair is one-sided and section 5.4 states what that does and does not establish.

| measurement | D store on, event off | E store off, event on |
|---|---|---|
| verdict | ALIVE 130.4 s, 0 exceptions | not booted |
| `XGameSaveInitializeProvider` | **never** | not measured |
| the late user path | `XUserAddAsync` route at +111.02 s, then nothing | not measured |
| stage word / requested stage | 2 / 2, every sample | not measured |
| stage transitions | init 0->1 at 4.0 s, 1->2 at 22.0 s; loading 1 at 22.0 s | not measured |
| loader begin/end records | 200 / 200 | not measured |
| last pipeline-timeline row (+120 s) | slot57 10,241; slot55 41,545; graphics 51,786; compute 474/403; submits 9,276; presents 4,637 | not measured |
| profile record | created, boot 1 (on disk, 64 B) | not measured |

Reading D against section 5.1: a boot that creates and keeps the profile record but delivers no sign-in event takes
exactly the control's route - `XUserAddAsync` at +111.02 s, no `XGameSaveImpl` request, no provider - while the record
file sits in the persistent folder for the whole boot. So the record's presence is not what moves the save subsystem;
the delivered event is the necessary switch. Whether the event alone is sufficient (E) is the open half.

### 5.4 Verdict

- The slice's implementation is verified by its tests (section 7) and by two boots on the same overlay: the record is
  created on the first boot, loaded and advanced on the second, and the identity served is the same.
- The peer observation is **not reproduced**: with the event delivered and the record kept, the title's stage word stays
  2/2 and the presented frame stays black in every sample; the late save subsystem's route changes, but no first-run
  screen is skipped and the story load does not arrive sooner.
- The differential belongs to the **delivered sign-in event**, not to the persisted record: the record-alone arm
  behaves like the control. The event's sufficiency (arm E) was not measured, so the honest statement is "necessary on
  this evidence", not "sufficient".
- The title's own "saved settings" path stays unexercised: not one save container or blob call in seven boots.

## 6. Limits

- **The isolation pair is one-sided.** D (record on, event off) was booted and behaves like the control, which makes
  the delivered event the necessary switch on this evidence; E (record off, event on) did not get a boot slot before
  the reporting deadline, so event sufficiency is not measured. The next boot of this slice should run E on the same
  binary and environment; if E opens the provider, the attribution is complete.
- **The record is our own invention.** The title never asks the runtime for a user profile or a store in a way that
  reads it, so the identity served from the record is only observable in tests and by the store provider's initialisation.
  If a later slice finds the title reading a file from the persistent folder, the record's name and layout must be
  re-derived from that read before it is treated as the title's own settings.
- **Boot timing is the machine's.** The first pair's fresh arm was slow (114 loader records) while its second boot
  finished loading, so the first pair cannot compare the late path; the second pair re-runs all three arms with 200
  loader records each. The freshest pair's A and B differ in timing only between 23.48 s and 24.32 s.
- **One machine, one binary, other slices' uncommitted code in it.** The binary was built from a working tree that also
  carries `port/src/main.cpp` probes and upscale's files, and boots were shared with other agents under the boot lock.
  Every arm is the same binary; the tree state is named in section 5.
- **A 130 s window cannot exclude a later store use.** The title opens the provider at about 24 s and the loader's
  streaming is still running at 130 s; a longer boot could reach a container call after the window.
- **No game data.** Only API names, slot numbers, call times, counts, HRESULTs, the title's own RVAs, file names and
  sizes appear here.

## 7. Tests

No new test target: the existing targets are extended (`port/test/xgameruntime_test.cpp`,
`port/test/xgameruntime_user_test.cpp`, `port/test/xgameruntime_gamesave_test.cpp`; the CMake registration is
untouched). No game data, no GPU.

- **`xgameruntime`** (167 checks, 0 failures): the new persistent-profile section creates the 64-byte record from the
  defaults, serves the recorded identity on the next boot and advances the boot counter, rejects and rewrites a
  truncated record, never serves a partial `settings.bin~` and removes it, writes and reads nothing with the switch off,
  and falls back to the constants with no overlay.
- **`xgameruntime_user`** (93 checks, 0 failures): the offline user adds with the store configured, the first boot's
  slots answer the runtime's default identity and the record holds exactly that, the next boot serves the same identity
  from the record, and the record advanced once for that boot.
- **`xgameruntime_gamesave`** (65 checks, 0 failures): a killed update's `"<blob>~"` file is on disk, the next provider
  open removes it and serves the blob from before the kill byte for byte, and a later update replaces the blob whole
  with no temporary file left; the existing container/blob/quota/handle checks are unchanged.
- All eight `xgameruntime*` ctest entries pass (`xgameruntime`, `xgameruntime_user`, `xgameruntime_gamesave`,
  `xgameruntime_store`, `xgameruntime_offline`, `xgameruntime_offline_hdr`, `xgameruntime_user_gamertag`,
  `xgameruntime_user_gamertag_long`).

## 8. Evidence and reproduction

Outside the repository, under `D:\leonida-work\savestore`:

- `arm.ps1` / `arm2.ps1` / `arm4.ps1` run one arm each (fresh, second boot on the same overlay, control, and the
  isolation pair); `analyze_arm.py` extracts the tables above from an arm's `stderr.log`, `summary.txt`, `boot.jsonl`,
  `store_listing.txt` and the capture index - counts, times, names and sizes only, no game bytes.
- Boot directories: `D:\leonida-work\boot-savestore-a-fresh`, `-b-second`, `-c-control` (first pair) and
  `boot-savestore2-a-fresh`, `-b-second`, `-c-control` (section 5.1), plus `boot-savestore4-d-store-only`
  (section 5.3); captures in `D:\leonida-work\savestore\capture*`. The E arm's script and overlay
  (`savestore\overlay4-e-event-only`) exist, but its boot did not run.
- Analysis JSON for every arm that booted: `D:\leonida-work\savestore\arm1-*.json`, `arm2-*.json`,
  `arm4-d-store-only.json`.

Reproduce one arm (from the checkout root, after `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`):

```powershell
python -I tooling\boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay <work>\overlay --out <work>\boot-arm --timeout 130 `
    --env PALMETTO_SHADERS=<work>\shaders\spirv --env PALMETTO_D3D12=vulkan --env PALMETTO_SKIP_INT3=1 `
    --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_PIPELINE_TIMELINE=10 --env PALMETTO_LOADER_HOOK=1 `
    --env PALMETTO_DS_STACK=1 --env PALMETTO_DS_ARRAYS=1 --env PALMETTO_USER_TRACE=1 `
    --env PALMETTO_STATE_TRACE=2,130 --env PALMETTO_SAVE_DIR=<work>\overlay\saves `
    --env PALMETTO_PERSIST_PROFILE=1 --env PALMETTO_USER_CHANGE_EVENT=1
```

Drop `PALMETTO_PERSIST_PROFILE`/`PALMETTO_USER_CHANGE_EVENT` for the control; hold `_setup/boot_lock.ps1` around each
run and re-use the same `--overlay` for the second boot. The switch's default is the persistent record on and the
change event off, so the two `--env` values above are the two probe arms, not the defaults.

## 9. Attribution

This slice was implemented from this project's own measurements. The behaviour it tests was pointed at by the public
wording of the peer projects summarised in [cleanroom_peers.md](cleanroom_peers.md) - Five Star Recomp's README
("loading the game's saved settings" skips the first-run screens) and VIctory's and XCL's public descriptions ("one
signed-in offline user, licences, packages, saves that survive a kill at any step") - read as documentation only; no
peer source code was read or copied for this slice. The API shapes, slot order and error codes are Microsoft's public
GDK reference (XGameSave, XPersistentLocalStorage, XUser pages on learn.microsoft.com/gaming), and the store's file
layout is this project's own ([saves.md](saves.md)). The user-change-event switch and the offline store completion were
cross-checked in [user_events.md](user_events.md) section 4 against the public GPL-3.0 project
ubiquitarianism/leonida; this page adds no new reference code or behaviour from that tree. The upstream projects remain
the copyright of their contributors and their licenses continue to apply to their own trees.
