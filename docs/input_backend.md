# The always-built input backend: XInput, the keyboard pad and the scripted first-run presses

**Status: measured; the read half of the peer claim reproduces, the save/stage half does not.** The runtime now builds
a GameInput implementation even when SDL3 is absent: an XInput source (any controller Windows exposes), a
keyboard/mouse-as-pad fallback, a hot-plug poll, a bounded scripted first-run press schedule and a call trace. The title
binds it through its measured `GameInput.dll!#1` import, a device is present and paired, and the schedule presses
confirm/start about once a second for 29 s. **The A/B (130 s pair plus a lead-approved 220 s arm) says the title does
read a paired pad: with devices present its first-run/UI code calls `GetCurrentReading(GameInputKindGamepad)` and
`GetGamepadState` at +25.4..+29.25 s, calls that are absent when no device exists. Nothing follows the read** - no
`XGameSave*` call, no save-system start, no new pipeline or population, and the presented frames stay black. So the
"reads input / pairs a pad" mechanism is reproduced on this tree, and the "starts its save system" step is not.

## 1. Why these calls are needed

The public peer projects describe their input layer in their READMEs (documentation only; their source was not read):

- **Five Star Recomp**: the loader "boots through its start-up, answers its first-run screens through a scripted
  gamepad ([docs/first-run.input](https://github.com/ThatRecompDev/five-star-recomp)) and starts loading story mode".
  That is the claim this slice tests: first-run screens advance on a scripted pad, then the save/settings path runs.
- **XCL**: "GameInput: one gamepad - the PC's XInput controller or a PlayStation controller, and keyboard and mouse as
  a gamepad"; "any controller Windows sees through XInput ... can be plugged in or out while the game runs"; and the
  keyboard/mouse mapping (WASD left stick, mouse right stick, mouse buttons the triggers, Enter/Space A, Escape/
  Backspace B, arrows the d-pad, Tab View, P Menu).
- **VIctory**: "Input: XInput pads. With none connected the keyboard is a pad while the title's window has focus",
  with a key table and a "keyboard stand-in pad".
- **OpenCommunityEdition/OpenScarlett**: "GameInput over XInput and SDL (DualSense), keyboard and mouse".

Our own measurement was the other half of the hypothesis. [input_gate.md](input_gate.md) measured the loading loop's
input surface over 300 s: one `GameInputCreate` from `game_scarlett_master.exe+0x59258c8`, one
`IGameInput::RegisterDeviceCallback` for `GameInputKindGamepad` from `game_scarlett_master.exe+0x592586e`, and then no
reading, no reading callback, no dispatcher, no device method. It also measured that the canonical build did **not**
compile the GameInput shim when SDL3 was absent, so the import bound to the host's `GameInput.dll`, and that the host
had no pad for the device callback to hear. The missing control was therefore a boot in which the runtime owns
`GameInputCreate` and a pad really connects and presses: the title's callback runs, and a first-run screen waiting on a
pad would be able to read one.

This slice builds exactly that control without SDL3, which is what the shipped build on this machine has. The peer
claim is the hypothesis; the A/B in section 5 is the measurement.

## 2. Measured input surface (re-verified for this slice)

| what | measured |
|---|---|
| imports of the retail image | `GameInput.dll!#1` as a normal import (one function, by ordinal), `CreateWindowExA`/`DefWindowProcA`/`RegisterClassExA`/`ShowWindow`, `PeekMessageA`/`TranslateMessage`/`DispatchMessageA`; **no** XInput, DirectInput, `GetAsyncKeyState`/`GetKeyboardState` or raw-input import (`tooling/pe_inspect.py`, file read only) |
| host `C:\Windows\System32\GameInput.dll` export table | ordinal base 1, three exports: ordinal 1 `GameInputCreate`, 2 `DllCanUnloadNow`, 3 `DllGetClassObject` (the export directory read with a small PE parse) |
| what the **host** runtime answers, mapped code | `GameInputCreate` `S_OK`; `GetCurrentTimestamp` a value; `GetCurrentReading(GameInputKindGamepad)` **`0x838A0003` `GAMEINPUT_E_READING_NOT_FOUND`**; "gamepad reading: none (no gamepad attached)" (`gameinput_host_test`, which maps a synthetic DLL against the host's DLL) |
| controllers on this host | `XInputGetState` slots 0-3 all return `1167` `ERROR_DEVICE_NOT_CONNECTED`; no Xbox/DualSense pad is attached |
| what our shim answers | `GameInputCreate` `S_OK`; a device callback hears one device per connected source; readings carry pad state, sequence numbers and timestamps; `GetCurrentReading` answers `GAMEINPUT_E_READING_NOT_FOUND` until a source is connected (`gameinput_nosdl_test`) |
| binding in our build | the title's call reaches this backend: the arm A boot prints `[rt] gameinput: read trace: GameInputCreate #1 from game_scarlett_master.exe+0x59258c8 -> 0x0` (only this backend prints that line), and the loader reports `xgameruntime.dll is served by the shims`; the `GameInput.dll!#1` ordinal import resolved through the shim's `#1` table entry |

The "with a controller attached" arm of the original observation cannot be measured on this machine: there is no pad to
attach. The keyboard pad and the scripted pad are the devices that stand in, and the A/B uses them (limit in section 6).

## 3. What we implemented

### 3.1 The build: one file, two backends

`port/shims/gameinput.cpp` keeps the SDL3 backend it already had. `port/CMakeLists.txt` now sets
`PALMETTO_GAMEINPUT_SDL` on `palmetto` only when an SDL3 target is found and otherwise builds the same file with the
include-only no-SDL backend `port/shims/gameinput_nosdl.h`. The image therefore always answers `GameInputCreate` with
its own implementation. The exact gating change is in section 3.6.

### 3.2 The device source

- **XInput**, loaded at run time (`xinput1_4.dll`, then `xinput9_1_0.dll`; no link-time import of its own). The four
  XInput slots are polled every poll; buttons are mapped by position (A is the south face button, as on an Xbox pad),
  the two trigger bytes become 0..1 floats, and the four stick axes become -1..1 with +Y up, XInput's own signs.
  XInput hides a pad's family, so every pad is presented to the title as an Xbox One pad, like the SDL shim does.
- **Keyboard/mouse pad** (opt-in, section 3.3): WASD or the arrows push the left stick (a diagonal sits on the round
  gate at about 0.707 an axis, opposite keys cancel), the mouse's movement pushes the right stick (7/8 kept per poll
  plus 1/32 unit per cursor pixel, so a steady 4 pixels a poll holds it full and it falls to exactly 0 in 64 polls),
  the mouse buttons are the triggers, and Enter/Space are confirm (A), Backspace cancel (B), Esc start (Menu), Tab
  View, X/Y the face buttons, Q/E the shoulders. It is one more paired gamepad; the title cannot tell it from a pad.
- **Hot-plug** is a poll of every slot on every tick: a slot that starts answering is a connected device (the same
  device object, id and player slot it had before), one that stops is a disconnected device; a reconnected pad gets
  its object back, as GameInput's device registry does.
- **The scripted pad** (opt-in, section 3.4) is one more device, connected from the first poll and neutral after its
  schedule ends, so a title that pairs pads at start has one to pair.

### 3.3 The switches

Both names already exist in this tree's documentation; this backend reads them with the same meaning, so the slice
introduces no new environment variable.

| variable | default | effect |
|---|---|---|
| `PALMETTO_KEYBOARD_PAD` | off | `1`/`focus`: the keyboard pad reads keys only while a window of this process is in the foreground; `always`: whatever has focus (the A/B uses `always`, because the boot harness is a console process); `off`: no keyboard pad. `GetAsyncKeyState`/`GetCursorPos` are the source, so no SDL video subsystem is needed. |
| `PALMETTO_FAKE_INPUT` | off | `<seconds>`: one scripted pad and a bounded press schedule: confirm (A) at every even press and start (Menu) at every odd one, one press a second, each held 150 ms, thirty presses (about 29 s), the first at `<seconds>` after the shim starts. A refused value (anything but a plain non-negative decimal number) presses nothing. |
| `PALMETTO_GAMEINPUT_THREAD` | `1` | `0` runs the poll only from the API calls instead of a 2 ms background thread (the tests use this). |
| `PALMETTO_INPUT_TRACE` | off | the bounded call trace: call 1, 10, 100 ... per entry point, caller as module+RVA, plus device connect/disconnect, callback, press and rumble events (the first 16 of each event kind, then every 8th). The traced entry points are `GameInputCreate`, `RegisterDeviceCallback`/`RegisterReadingCallback`/`RegisterGuideButtonCallback`/`RegisterKeyboardLayoutCallback`, `GetCurrentReading`/`GetNextReading`/`GetPreviousReading`/`GetCurrentTimestamp`, `IGameInputReading::GetInputKind`/`GetGamepadState`/`GetUiNavigationState`, and `IGameInputDevice::GetDeviceInfo`/`GetDeviceStatus`/`SetRumbleState`: the surface `input_gate.md` defines. |

The default stays OFF for both input sources: an unattended boot with no pad and no scripted presses behaves as
The default stays OFF for both input sources: an unattended boot with no pad and no scripted presses behaves as
before. The A/B turns them on in every arm, so an absent trace line means the title never called that entry.
### 3.4 The scripted first-run press schedule

The schedule is `ScriptedPresses` in `gameinput_nosdl.h`: a `palmetto::FakeInputSchedule` (the input-gate experiment's
own schedule state machine, `port/src/fake_input.h`) parameterised to one press a second for thirty presses, each
150 ms, with our own button sequence - confirm on the even presses, start on the odd ones. It is deterministic and
bounded: at `PALMETTO_FAKE_INPUT=3` the presses run from +3.0 s to +32.15 s and then stop, while the pad stays paired
and neutral. Confirmed and start are the two buttons a first-run screen conventionally advances on; the sequence is our
own, not copied from a peer's input script (that file was not read).

### 3.5 The GameInput ABI this backend answers

The v0 interfaces from `port/shims/gameinput_abi.h`, in vtable order:

| implemented | not implemented (the honest answer) |
|---|---|
| `GameInputCreate`; `GetCurrentTimestamp`; `GetCurrentReading`/`GetNextReading`/`GetPreviousReading` (per-device history, 128 readings, sequence numbers); `RegisterDeviceCallback` (no/async/blocking enumeration, connect and disconnect statuses); `RegisterReadingCallback`; `RegisterGuideButtonCallback`/`RegisterKeyboardLayoutCallback` (tokens exist, never fire); `StopCallback`/`UnregisterCallback`; `FindDeviceFromId` | `CreateAggregateDevice`, `GetTemporalReading` (`E_NOTIMPL`, as v0 itself), `CreateDispatcher` (`E_NOTIMPL`: callbacks are delivered from the poll, not queued), `FindDeviceFromObject`/`PlatformHandle`/`PlatformString`, `EnableOemDeviceSupport` (no-op) |
| device: `GetDeviceInfo` (320-byte v0 info, family `ConsoleVariant2`, Xbox label tables for 14 buttons and 21 UI-navigation controls, display name), `GetDeviceStatus`, `GetBatteryState`, `SetRumbleState` (recorded and traced, not played), the no-op synchronization/power calls, `SetForceFeedbackMotorGain` | force feedback, haptic motors, raw HID reports, exclusive raw access |
| reading: input kind, sequence number, timestamp, device, `GetGamepadState`, `GetUiNavigationState` (Accept/Cancel/directions/context/page/scroll derived from buttons, sticks and triggers), controller axes (6, 0..1 with sticks at 0.5) and buttons (14) | keyboard, mouse, touch, motion, arcade/flight stick and racing wheel states (a reading is a gamepad) |

The title's reading objects outlive their place in the history; devices live for the process, so a title holding one
across an unplug gets the same object back. Every pad reports a connected, input-enabled, output-enabled status; the
keyboard pad reports no rumble motors.

### 3.6 The build gating change

The change in `port/CMakeLists.txt` is the `GameInput shim` block only:

- the option text now says "Build the SDL3 GameInput backend when SDL3 is available";
- when `PALMETTO_SDL3` is on and `SDL3::SDL3-static` is found, as before: `shims/gameinput.cpp` on `palmetto`,
  `PALMETTO_GAMEINPUT_SDL` defined, SDL3 linked, and the same define on `gameinput_test`/`gameinput_probe`;
- otherwise: `shims/gameinput.cpp` is still on `palmetto` (the no-SDL branch) and the configure message names the
  always-built backend; nothing is linked and nothing is downloaded;
- a new `gameinput_nosdl_test` target and its `gameinput_nosdl` ctest are built in both cases, so the backend's tests
  run on an SDL3 build and on a bare Windows build.

`tooling/scripts/build_runtime.ps1`'s direct-`cl.exe` fallback keeps its old file list (no GameInput shim); it is only
used when CMake or Ninja is missing, and its warning already says so.

## 4. Cross-check against the public peer documentation

The peer projects are evidence, not a code source: only their READMEs were read. Their public descriptions agree with
the mechanism this slice builds, and the differences below are choices of this port, not claims about theirs.

| peer | agrees | differs here |
|---|---|---|
| Five Star Recomp | a scripted gamepad answering the first-run screens is the mechanism; our A/B reproduces the read half (the first-run/UI code polls a paired pad) and the scripted pad answers one | its `docs/first-run.input` was not read (it is peer project data, not a README claim); our sequence (30 presses, 1 s apart, A/Menu alternating) is our own generic one, and the save/stage half of its sentence did not reproduce here (section 5) |
| XCL | XInput as the pad source, hot-plug, keyboard/mouse as a gamepad with the left stick/mouse split and View on Tab | our key map is the one this task named, not XCL's: arrows are the left stick here (not the d-pad), Esc is start/Menu (XCL uses P for Menu and B for Backspace), X/Y are face buttons and Q/E the shoulders (XCL maps Q/E to X/Y); the PS-pad fallback is not in this backend (XInput only) |
| VIctory | the keyboard stand-in pad exists and the mouse drives an analog stick | theirs is active when no pad is connected and the window is focused; ours is opt-in and off by default (`PALMETTO_KEYBOARD_PAD`), so an unattended boot never types into the title |
| OpenCommunityEdition/OpenScarlett | GameInput over XInput plus a keyboard/mouse path | no SDL dependency in this backend (their description names SDL; ours is XInput plus the Win32 keyboard/mouse APIs) |

The public descriptions are about mechanism; the measured read and the missing save step are this tree's result
(section 5), not a claim that the peer projects do not reach their own first-run flow.
## 5. A/B measurement (and the approved 220 s arm)

Three boots of **the same binary** (SHA-256 `F32BCF50E3BBEB9646C53B01E95E8432DB0CB4DE82800289A6F56FBFCFEB68D2`,
built 2026-11-25 18:21), one at a time under `_setup/boot_lock.ps1`, through `tooling/boot_report.py`, with the
standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) and the same measurement layer in every arm (`PALMETTO_PIPELINE_TIMELINE=10`,
`PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_INPUT_TRACE=1`, `PALMETTO_GAMEINPUT_THREAD=1`,
presenter captures every 2,000 frames, limit 8). Arm A is the backend with no device and no schedule (130 s); arm B
adds the keyboard pad and `PALMETTO_FAKE_INPUT=3` (130 s); arm B-long is the lead-approved third arm, the same devices
with `PALMETTO_FAKE_INPUT=100` so the presses at +100..129.15 s overlap the tree's post-+115 s sign-in step (220 s).

| measurement | A: backend, no devices | B: pad + presses at +3 s | B-long: pad + presses at +100 s |
|---|---|---|---|
| timeout / verdict | 130 s, `ALIVE 130.4 s`, 0 exceptions | 130 s, `ALIVE 130.3 s`, 0 exceptions | 220 s, `ALIVE 220.3 s`, 0 exceptions |
| input calls the title made | `GameInputCreate` (+2.33 s, `S_OK`); `RegisterDeviceCallback` (`S_OK`, kind `0x40000`); **no reading, no `GetGamepadState`, no device method** | `GameInputCreate` (+2.60 s); `RegisterDeviceCallback` (`S_OK`, kind `0x40000`); the callback enumerates **2 devices** and the title calls `IGameInputDevice::GetDeviceInfo`; at **+29.24-29.25 s** `IGameInput::GetCurrentReading(GameInputKindGamepad)` -> `S_OK` and `IGameInputReading::GetGamepadState` -> `true` | same surface (create +2.39 s); the read is at **+25.44-25.46 s**, i.e. before the first scripted press at +100 s; no traced reading call after that (the trace logs call 1, 10, 100 ... per entry, so fewer than 10 reads happen in the whole 220 s) |
| reading callers (title RVAs) | - | `GetCurrentReading` from `game_scarlett_master.exe+0x2211ba3`; `GetGamepadState` from `game_scarlett_master.exe+0x6aedaa`; the callback/`GetDeviceInfo` from `game_scarlett_master.exe+0x592586e` | the same three RVAs |
| devices paired | none | keyboard pad + scripted pad connected at +2.6 s, both enumerated into the title's callback | same |
| scripted presses | off | 30 presses, +3.0 to +32.15 s (13 traced by the bound) | 30 presses, +100.0 to +129.15 s (17 traced by the bound) |
| platform calls, last | last +116.90 s (`XUserAddAsync`/`XUserAddResult`/`XUserGetId`/`XUserDuplicateHandle`) | last +121.26 s, the same user path | last +114.94 s, the same user path |
| gamesave / persistent-storage calls | `XPersistentLocalStorageGetPath` at +2.34 s; **no `XGameSave*` call** | `XPersistentLocalStorageGetPath` at +2.61 s; **no `XGameSave*` call** | `XPersistentLocalStorageGetPath` at +2.40 s; **no `XGameSave*` call** |
| loader begin/end hook records | 400 / 400 | 400 / 400 | 400 / 400 |
| pipeline population at the last timeline row | +120 s: slot57 10,241 / slot55 41,545 / graphics 51,786 / compute calls 474, created 403; submits 8,877; presents 4,438; console calls 174,450; 89 threads | +120 s: the same 10,241 / 41,545 / 51,786; compute calls 464; submits 9,266; presents 4,633; console calls 177,346; 89 threads | +210 s: the same 10,241 / 41,545 / 51,786; compute calls 568; submits 14,317; presents 7,158; console calls 215,487; 86 threads |
| presenter captures (mean rgb) | 3 frames (1, 2,001, 4,001), all `0.00 0.00 0.00` | the same 3 frames, all `0.00 0.00 0.00` | the same 3 frames, all `0.00 0.00 0.00` |
| startup trace exceptions | 0 | 0 | 0 |

Reading the table, in the order the peer claim makes them:

- **The title reads a paired pad.** Arm A, which has a working `GameInputCreate` and a registered device callback but
  no device, never takes a reading, never asks for a gamepad state and never touches a device. Arm B, whose only
  difference is the two paired pads, does all three: the device callback enumerates the devices and calls
  `GetDeviceInfo`, then at the first-run/UI phase (+29.24 s; +25.44 s in the 220 s arm) the title's own code calls
  `GetCurrentReading(GameInputKindGamepad)` and `GetGamepadState`, both succeeding. That is the mechanism half of the
  peer observation, reproduced on our tree for the first time: the loading build does not read input only because it
  has none to read.
- **The read is caused by the paired device, not by a scripted press.** In arm B-long the scripted presses start at
  +100 s, yet the title reads at +25.44 s, before any press. So a pad present at the first-run/UI phase is enough for
  this title to poll it; the scripted presses are a device-side stimulus the read does not need.
- **Nothing follows the read.** No `XGameSave*` call, no save-system start, no new pipeline or population, no stage
  change: the pipeline population is exactly the same 10,241 / 41,545 / 51,786 in all three arms, the loader records
  stay 400/400, the user/persistent-store path still moves to its sign-in step at +115..+121 s, and every presenter
  capture is uniformly black. The 220 s arm adds the whole post-sign-in window with 30 presses in it (at least the
  presses are traced) and still shows no gamesave call and no population change.
- **The boot is not input-gated, it is title-state-gated.** `input_gate.md`'s conclusion stands for the state of that
  earlier tree (the loading loop did not advance on input); this pair sharpens it: the first-run/UI code does read a
  paired pad, and the boot still does not advance past the black two-pass loop, so the gate is a title state and not
  the presence of a reading.

The tree-state note: the earlier `input_gate.md` measurement had the title register its gamepad callback at startup;
on today's tree (which carries other slices' uncommitted user/persistent-store work) the sign-in path runs at
+111.49..+121.26 s and the first-run/UI phase sits around +25..+29 s, so the read timestamp is recorded per arm above
rather than assumed from the old page. The boot order difference is the other slices', not this one's: both arms here
are the same binary and the input path differs only by the two devices.


## 6. Limits, and the next falsifier

- **One 130 s pair plus one 220 s arm on one machine**, shared with other agents' builds and boots; boot-speed
  variance moves the loader clocks, not the durable state (population, dormancy, black captures), as earlier pages
  measured. The 220 s arm is a single run, and its later timeline row is the only sample after +120 s.
- **No physical pad is attached to this host**, so the "with a controller" arm is represented by the keyboard pad and
  the scripted pad, not by a real Xbox/DualSense pad. An XInput pad on a machine with a controller is the same code
  path (one slot answering `XInputGetState`), but it is not measured here.
- **The A/B is presence vs absence, not press vs no press.** Arm B-long shows the read happens while both pads are at
  rest (presses start at +100 s, the read is at +25.44 s), so device presence alone makes the title poll. Whether a
  scripted press is what the title then acts on is not separated by this pair: a third arm with the pad present and no
  schedule at all would be the control for "press", and it was not run (boot windows are shared). The bounded trace
  cannot count a small number of reads precisely either: it logs calls 1, 10, 100 ..., so the arms prove at least one
  read and fewer than ten, not the exact count.
- **What the title saw in the reading is not logged.** `GetGamepadState` returned `true`, but the trace records no pad
  state, so the read's buttons/sticks are unknown; in arm B-long both devices are at rest at +25.44 s, which is reason
  to think the first read saw a neutral pad.
- **No `RegisterReadingCallback`** appears in any arm: the title polls, it does not subscribe. The backend implements
  reading callbacks anyway (tests cover them).
- **No rumble, force feedback, haptics, raw HID or motion** in this backend: the title's `SetRumbleState` is recorded
  and traced, never played (no arm calls it). The SDL3 backend keeps all of those; without SDL3 the runtime loses them
  (it previously lost all of the shim, so this is still strictly more than before).
- **No dispatcher queue**: `CreateDispatcher` answers `E_NOTIMPL` and callbacks run from the poll thread. The measured
  title never creates a dispatcher; a title that does would see its callbacks from a different thread than it expects.
- **Callbacks for a connection are delivered synchronously during `RegisterDeviceCallback`** when it asks for
  blocking enumeration (the measured registration enumerates: the title's callback ran before the call returned).
  Microsoft's documented delivery is through the poll/dispatcher; a title re-entering GameInput from inside the
  callback would see a recursive `lock` in this backend, unlike the SDL shim.
- **This slice does not advance the boot.** The read is real and measured, and the black two-pass loop, the pipeline
  population and the absence of gamesave calls are unchanged by it. The loading state is gated by something the
  title still does not reach, not by the absence of input.
- Next falsifier: an arm with the pad present and no scripted presses, then an arm whose press lands inside the
  read window (the title's first-run/UI phase at +25..+30 s on this tree) and moves the pad past its deadzone, to
  separate "pad present" from "a press it can act on". If the stage or a gamesave call follows that, the second half
  of the peer claim is reachable here; if it does not, the gate is title state, as `input_gate.md` concluded for its
  earlier tree state.
## 7. Tests

`gameinput_nosdl_test` (CTest `gameinput_nosdl`) builds and runs with or without SDL3, needs no controller and no game
data; it installs a fake XInput source and a fake key/cursor source and drives the backend with its own clock.

- **XInput mapping**: every XInput button into its positional GameInput button (values equal to
  `GameInputGamepadButtons`), triggers 0..1, stick signs (+Y up), `PadGuide` kept out of the gamepad bits.
- **Hot-plug and pairing**: connect/disconnect events, one event per change, steady state is not an event, the
  keyboard pad and scripted pad are separate devices, the scripted pad is connected while installed and neutral after
  the schedule ends.
- **Readings**: a first reading, no reading when unchanged, appending on change, current/next/previous by sequence,
  the 128-reading ring, and the UI-navigation bits a first-run screen would read.
- **Keyboard/mouse pad**: confirm/cancel/start/View, the face buttons, the shoulders, WASD and arrows on the left
  stick, the round-gate diagonal, opposite keys cancelling, the mouse buttons as triggers, mouse movement driving the
  right stick and decaying to exactly 0, the Focused/Always modes and the release on focus loss.
- **Scripted schedule**: the parse cases (including the refused typo forms), the press windows (held 150 ms, released,
  one press a second), the A/Menu alternation, 30 presses and the bounded end.
- **Active-input record**: the tracker fed by the backend changes to the keyboard family on a keyboard edge, ignores
  another family inside its 300 ms window, and follows the Xbox pad after it.
- **The v0 ABI over a fake source**: `GameInputCreate`, `RegisterDeviceCallback` with blocking enumeration, the
  320-byte device info and its label tables, `GetCurrentReading`/`GetGamepadState`/`GetUiNavigationState`, axes and
  buttons, next/previous readings, unplug (callback with `NoStatus`, reading not found) and replug (same device
  object, connected again).
- **The scripted pad over the ABI**: the schedule is relative to the backend's epoch while the poll clock is
  absolute, so the test arms the epoch at 0.5 s, 1.05 s and 2.05 s into the schedule and checks the scripted pad
  answers A and Menu in the right windows through `GetCurrentReading`/`GetGamepadState`.

Result: **8 of 8 cases pass**.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against public
documentation of the peer projects: Five Star Recomp's README (ThatRecompDev/five-star-recomp, GPL-3.0-or-later), XCL's
README (xcl-dev/xcl), VIctory's README (marvisak/victory), and OpenCommunityEdition/OpenScarlett's README; their
public claims are quoted in the sections above. **No peer source code, input script or asset was read or copied.** The
GameInput v0 declarations are this repository's own `port/shims/gameinput_abi.h`, written against Microsoft's public
GameInput reference and the GDK header. XInput's button bits and `XINPUT_STATE` layout are Microsoft's public XInput
documentation. The licence of this project is GPL-3.0; the upstream projects remain the copyright of their
contributors and their licences continue to apply to their own trees.

## 9. Evidence and reproduction

Outside the repository, under `D:\leonida-work\inputpad`:

- `arm.ps1` runs one arm (130 s or a given timeout) and records the binary SHA-256 in the arm's `arm_meta.txt`;
  `batch-final.ps1` is the one-acquisition batch (rebuild, then the three arms); `analyze_arm.py` extracts the numbers
  of this page (input trace, backend events, platform calls, loader records, timeline row, captures); none of them
  reads game bytes.
- `boot-inputpad-a-control`, `boot-inputpad-b-input`, `boot-inputpad-b-long`: each arm's `stderr.log`, `summary.txt`,
  `boot.jsonl`, `arm_meta.txt` (binary SHA-256) and presenter captures under `capture-a-control`/`capture-b-input`/
  `capture-b-long`; `a-control-final.json`, `b-input-final.json` and `b-long-final.json` are the analyses of the three
  arms of the table in section 5. The earlier runs on the first binary (SHA `72E4A8FF...`) are superseded by these and
  are kept only to show the trace-coverage fix: `batch-arms.log`/`b-input.json`/`b-long.json` are those older runs.
- The binary of the three arms: `port/build/palmetto.exe`, SHA-256
  `F32BCF50E3BBEB9646C53B01E95E8432DB0CB4DE82800289A6F56FBFCFEB68D2`, built 2026-11-25 18:21.
- The host-runtime measurement is `gameinput_host_test` on the same machine (no pad attached), and the test run is
  `port/build/gameinput_nosdl_test.exe` (CTest `gameinput_nosdl`).

Reproduce one arm (from the checkout root, after `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`):

```powershell
powershell -File D:\leonida-work\inputpad\arm.ps1 -Arm a-control
powershell -File D:\leonida-work\inputpad\arm.ps1 -Arm b-input
powershell -File D:\leonida-work\inputpad\arm.ps1 -Arm b-long -TimeoutSec 220
```

Each holds the boot lock inside the script; arm B adds `PALMETTO_KEYBOARD_PAD=always` and `PALMETTO_FAKE_INPUT=3`;
arm B-long adds the same pad and `PALMETTO_FAKE_INPUT=100`, with `-TimeoutSec 220`. Run them one at a time.
