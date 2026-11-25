# The loading loop's input gate: what the title polls, and the synthetic-input test

The title is measured stuck in the same two-pass blit loop for 1,200 s: presenter captures of its swapchain image are
uniformly black, full-target readbacks of the composed 4K target are black with zero alpha, and the pipeline/draw
population never changes after the loader burst ([frame_capture.md](frame_capture.md), [streamed_consumer.md](streamed_consumer.md),
[boot_frontier.md](boot_frontier.md)). This page is the input side of the remaining hypotheses: does the loading loop
wait for a button, and if so, which API does it poll?

**Short answer: the loading loop polls no input API at all.** The image imports exactly one GameInput function, calls
it once at startup, registers one gamepad device callback, and then never reads a reading, timestamp, dispatcher or
callback event for the rest of the measured window - with a synthetic gamepad connected and its A button pressed six
times in that window. Its one other input channel, the window message queue, was driven directly (typed keys, a click
and posted key messages into its own focused window for two minutes) and changed nothing either. A synthetic button
press therefore cannot advance the loading state; there is no poll to advance. The loader gate is a title state, not
this input path.

## 1. The input API the title imports

Read from the package image with `tooling/pe_inspect.py` (a read-only PE report; no game bytes are copied anywhere):

| import | kind | resolves to |
|---|---|---|
| `GameInput.dll!#1` (one function, by ordinal) | normal import | system (the host's `GameInput.dll`) |
| `CreateWindowExA`, `DefWindowProcA`, `ShowWindow` | normal imports | `ext-ms-win-rtcore-ntuser-*` (system) |
| `PeekMessageA`, `TranslateMessage`, `DispatchMessageA` | normal imports | `ext-ms-win-rtcore-ntuser-*` (system) |

- The image imports **no** XInput function, no DirectInput function, no `GetAsyncKeyState`/`GetKeyboardState`, and no
  raw-input function. The only OS input channels it could read are `GameInput.dll` and its own window's message queue.
- Ordinal 1 of the host's `C:\Windows\System32\GameInput.dll` is `GameInputCreate` (its export table: ordinal 1
  `GameInputCreate`, 2 `DllCanUnloadNow`, 3 `DllGetClassObject`). So the title calls `GameInputCreate` by ordinal from
  the PC GameInput runtime, exactly as the Xbox build calls it on console.
- The window imports matter: the title creates a real window (class `sgaWindowClass`, title `Americas`, 2564x1421
  client, seen by the presenter) and runs a `PeekMessageA`/`DispatchMessageA` loop. That is its one remaining
  input-adjacent channel, tested in section 4.

## 2. Which input path a boot actually has (the canonical build has no shim)

`port/shims/gameinput.cpp` is the GameInput shim, but it is **compiled only when SDL3 is found**, and the canonical
build tree on this host has no SDL3:

- `port/build/CMakeCache.txt`: `PALMETTO_SDL3=ON`, `PALMETTO_SDL3_FETCH=OFF`, `SDL3_DIR=SDL3_DIR-NOTFOUND`;
- `port/build/build.ninja` has no `shims/gameinput.cpp` object and no `gameinput_test` target;
- a 130 s boot of `port/build/palmetto.exe` prints **no `[rt] gameinput` line at all**, and its thread dumps show the
  host `GameInput.dll` and `gameinputredist.dll` worker threads, i.e. the host's runtime was mapped and the import
  bound to it.

So on the canonical exe the title talks to the host's GameInput runtime, and nothing in the shim can see its calls.

There was a second gap under that: the shim registered `GameInputCreate` **by name** under `gameinput.dll` and
`xgameruntime.dll`, but this title imports it **by ordinal**. The loader resolves an ordinal import as `"#1"`
(`port/src/pe_loader.cpp`, `Loader::resolve`), so even with the shim built the import still bound to the host's copy
(measured: `D:\leonida-work\boot-inputgate-b`, SDL3-exe boot, zero `[rt] gameinput` lines). `port/shims/gameinput.cpp`
now also registers `{"gameinput.dll", "#1"}`; with that, the same boot prints the import as bound to the shim
(`shim  GameInput.dll!#1` in the loader's module report) and the shim's trace fires (section 3). The registration is
inert wherever SDL3 is missing, because the file is not compiled there.

## 3. What the loading loop calls (measured with caller RVAs)

Measurement build: the same `port/` sources with the shim enabled, built in an isolated tree
(`D:\leonida-work\build-sdl`, `-DPALMETTO_SDL3_FETCH=ON`, target `palmetto` only); the canonical
`port/build/palmetto.exe` is untouched. Boot b2: 130 s, standard env, `PALMETTO_INPUT_TRACE=1`,
`PALMETTO_FAKE_INPUT=10`, presenter capture on.

`PALMETTO_INPUT_TRACE=1` logs the first call and then every tenth-power call (1, 10, 100, ...) of every input entry
point, with the caller as module+RVA and the call's result. The whole Boot-inputgate-b2 window contains exactly these
input calls:

| # | entry point | caller | result |
|---|---|---|---|
| 1 | `GameInputCreate` | `game_scarlett_master.exe+0x59258c8` | `S_OK` (0x0) |
| 2 | `IGameInput::RegisterDeviceCallback` | `game_scarlett_master.exe+0x592586e` | non-null (1), kind `0x40000` = `GameInputKindGamepad` |

and **nothing else**: no `IGameInput::GetCurrentReading`, no `GetNextReading`/`GetPreviousReading`, no
`IGameInputReading::GetGamepadState`/`GetUiNavigationState`/`GetInputKind`, no `GetCurrentTimestamp`, no
`RegisterReadingCallback`, no `CreateDispatcher`, no `IGameInputDispatcher::Dispatch`, no `SetFocusPolicy` - not at
startup, not after the synthetic pad connected (2.5 s), and not during or after six injected A presses at 10.0 to
20.2 s. The two calls above are the entire input API surface the title uses: one object, one device-callback
registration, and then no reads.

That refutes the input-gate hypothesis for GameInput: a gate that "advances on a button press" would have to read a
reading (or receive a reading callback, or dispatch one), and the title does none of those. A connected gamepad alone
is excluded too: the synthetic pad existed from 2.5 s and the population did not move.

The same boot's population is stationary and the presented image stays black (section 5).

## 4. The window-message channel

The title's own window is its only other input channel (`PeekMessageA`/`DispatchMessageA` loop, `sgaWindowClass`).
`D:\leonida-work\input_inject2.ps1` (local diagnostic) finds that window, tries `AttachThreadInput` +
`SetForegroundWindow`, and then:

- sends `Enter` and `Space` down/up plus a left click at the client centre through `SendInput`, but **only while a
  window of the title's process owns the foreground** (so a failed focus change cannot type into another
  application);
- posts `WM_ACTIVATE`/`WM_SETFOCUS` and `WM_KEYDOWN`/`WM_CHAR`/`WM_KEYUP` for Enter and Space straight into the
  window, which needs no focus at all.

Result (boot-inputgate-a2, canonical exe, 130 s): the `AttachThreadInput` route did take the foreground - the title's
own window was focused (`foreground=0x...(sgaWindowClass, pid ...)`) - so every one of the 63 rounds between 10.3 s and
117 s typed Enter/Space down/up and a left click **into the title itself** (4 SendInput events and 2 mouse events per
round) and posted `WM_ACTIVATE`, `WM_SETFOCUS`, `WM_KEYDOWN`, `WM_CHAR` and `WM_KEYUP` for Enter and Space into its
window (10 messages per round). The loading state did not advance: population and captures are the same as the control
(section 5). The window-message channel is therefore not the gate either.

## 5. Measurements

All boots: harness `python -I tooling/boot_report.py`, standard env (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL`), own overlay and out dirs under `D:\leonida-work`, presenter capture
switched on (`PALMETTO_CAPTURE`, every 2000 presented frames), one boot at a time under `_setup/boot_lock.ps1`.

| boot | exe / build | input switches | d3d12 calls (max #) | replay submissions at end | DrawInstanced | CreateGraphicsPipelineState | captures |
|---|---|---|---|---|---|---|---|
| boot-load1 (control, 130 s) | canonical `port/build/palmetto.exe` (no shim) | none | 566,248 (#567,539) | 24,576 at 99.3 s | 28,035 | 15 | not captured |
| boot-inputgate-a (130 s) | canonical (no shim) | OS-level script v1: window found, focus refused, 0 keys sent | 544,922 (#547,108) | 24,576 at 105.3 s | 25,978 | 23 | 6 frames, all mean 0.00 0.00 0.00, peak 0 |
| boot-inputgate-a2 (130 s) | canonical (no shim) | OS-level script v2: title window focused; 63 rounds x (4 keys + click + 10 window messages), 10.3-117 s | 556,204 (#558,204) | 24,576 at 103.4 s | 26,562 | 23 | 6 frames, all mean 0.00 0.00 0.00, peak 0 |
| boot-inputgate-b (130 s) | SDL3 exe, shim built but import by name only -> host path | `PALMETTO_INPUT_TRACE=1 PALMETTO_FAKE_INPUT=10` | 558,220 (#560,147) | 24,576 at 102.7 s | 26,627 | 23 | 6 frames, all mean 0.00 0.00 0.00, peak 0 |
| boot-inputgate-b2 (130 s) | SDL3 exe, shim bound (`GameInput.dll!#1`) | `PALMETTO_INPUT_TRACE=1 PALMETTO_FAKE_INPUT=10` (pad at 2.5 s, six A presses 10.0-20.2 s) | 557,481 (#559,433) | 24,576 at 103.2 s | 26,627 | 23 | 6 frames, all mean 0.00 0.00 0.00, peak 0 |
| boot-inputgate-c (300 s acceptance) | SDL3 exe, shim bound | `PALMETTO_INPUT_TRACE=1 PALMETTO_FAKE_INPUT=30` (six A presses 30.0-40.2 s), full-target readbacks at draws 60000/60001 | 1,315,060 (#1,317,567) | 65,536 at 278.1 s | 66,524 | 23 | 7 frames (1..30001), all mean 0.00 0.00 0.00, peak 0 |

The 300 s acceptance run's other measurements:

- the full-target readback of the composed 4K target at draw 60,001 (the late sample, roughly 280 s in) is
  `3840x2160, mean 0 0 0, non-black 0/8294400, peak 0, alpha mean 0` - uniformly black, exactly as before the
  injection;
- every one of the 23 `CreateGraphicsPipelineState` calls happens before the injection (last at boot line 63,827, the
  injection starts at line 111,223), so no new pipeline or module appears after it;
- the GameInput trace has the same two startup calls and the six injected presses and nothing else: `GetCurrentReading`,
  `GetNextReading`, `GetPreviousReading`, `GetGamepadState`, `GetUiNavigationState`, `GetCurrentTimestamp`,
  `RegisterReadingCallback`, `CreateDispatcher`, `Dispatch` and `SetFocusPolicy` occur **zero** times in 300 s. This is
  the same negative as the 130 s run, over the full acceptance window.

Population: the d3d12 call count and the submission count grow linearly at the same rate in every run (the 130 s
figures differ by a few percent run to run, boot-load1's pipeline count differs because it is an earlier binary), the
draw/pipeline population is the same stationary two-pass blit loop, and every presenter capture is uniformly black.
Nothing new appears after the injection.

Controls: boot-inputgate-a/a2 (canonical build, no shim, so no synthetic pad at all - and in a's case no key was even
sent) and boot-inputgate-b (pad and presses attempted on a boot whose import still bound to the host runtime) are the
no-injection comparison at 130 s; their population matches b2 and c. No 300 s control run was needed: the 300 s run's
own input trace proves no input entry point is called at all, so a control without presses cannot differ - there is
nothing for the press to reach.

## 6. The injector: `PALMETTO_FAKE_INPUT`

Default OFF and byte-identical when unset: no pad, no timers, no logs beyond the existing ones.

`PALMETTO_FAKE_INPUT=<seconds>` attaches **one extra SDL virtual gamepad** named `Synthetic pad (Palmetto FAKE_INPUT)`
- the title needs a device to read at all - and then holds that pad's A button (the conventional accept/start
button) for a 150 ms burst, six times, two seconds apart, starting `<seconds>` after the pad attaches. It logs the
attach, each press and release, and the end of the schedule; the pad stays attached afterwards, so a control run with
the delay past the end of the run differs from a press run only in the press itself. `PALMETTO_FAKE_INPUT=30` is the
acceptance setting; `600` is the pad-only control.

- The schedule is `palmetto::FakeInputSchedule` in `port/src/fake_input.h` - no SDL and no Windows headers, so a test
  drives it with its own clock: `parse` ("30", "30.5", "0"; anything else is refused so a typo cannot press anything),
  `press_at` (which 150 ms window covers a time), `pressed_at`, `finished_at`, `ends_at_us`.
- `active_input_test` covers it: the parse cases, the press-window boundaries (pressed at the start, released at
  start+burst), the six presses two seconds apart, a zero delay, a custom short burst, and a schedule with no presses.
- The shim keeps the default path unchanged: with the switch unset, `update_fake_pad()` returns before attaching or
  setting anything.
- Checked end to end without the game: `gameinput_probe` with `PALMETTO_FAKE_INPUT=1` lists the synthetic pad, prints
  each press/release, and a reading during a burst carries the A button down (`buttons=0004`, `ui=000004`).

## 7. The read-path trace: `PALMETTO_INPUT_TRACE`

`PALMETTO_INPUT_TRACE=1` logs, for `GameInputCreate`, `GetCurrentReading`, `GetNextReading`, `GetPreviousReading`,
`GetCurrentTimestamp`, `GetGamepadState`, `GetUiNavigationState`, `GetInputKind`, the callback registrations,
`CreateDispatcher`, `SetFocusPolicy`, `CreateAggregateDevice` and `Dispatch`: the call ordinal, the caller as
module+RVA (`rt::shims::module_offset`, which resolves a mapped title image) and the result. It logs call 1, 10, 100,
1000, ... per entry point, so a title polling every frame cannot flood the log while the rate stays readable between the
last two lines. Off by default, no behaviour change.

## 8. Conclusion

- The loading loop polls **no input API**. Its entire GameInput use is one `GameInputCreate` from
  `game_scarlett_master.exe+0x59258c8` and one gamepad `RegisterDeviceCallback` from
  `game_scarlett_master.exe+0x592586e`; it never reads a reading, never registers a reading callback, never creates or
  dispatches a dispatcher, and never mentions the keyboard or mouse kinds.
- A synthetic gamepad with six injected A presses does not change the population or the presented image, because
  nothing reads it.
- Its one other input channel, the window message queue, was driven directly: 63 rounds of typed Enter/Space/clicks
  into the focused title window plus posted `WM_KEYDOWN`/`WM_CHAR`/`WM_KEYUP`/`WM_ACTIVATE`/`WM_SETFOCUS` over two
  minutes, with no change either.
- The input gate is therefore **not the gate**, and no synthetic input can advance the loading state on this build.
  What remains is the title-state hypothesis (a loader condition the state machine waits for and never transitions),
  which is the other branch of the investigation.
- Separately, two build/path findings are recorded: the canonical build does not compile the GameInput shim (no
  SDL3), and the shim's by-name-only export registration could never catch this title's ordinal import; both are now
  fixed in the shim source (the ordinal entry) or documented (the SDL3 requirement), and the measurement build path is
  recorded above so the next measurement can reproduce it.

No game data, package paths, shader bytes or extracted tables are reproduced here; only counts, API names, call
results and the title's own RVAs.

## Uncommitted files

- `port/shims/gameinput.cpp` - `PALMETTO_INPUT_TRACE` read-path trace; `PALMETTO_FAKE_INPUT` synthetic pad; the
  `GameInput.dll!#1` ordinal registration.
- `port/src/fake_input.h` - the schedule state machine (new).
- `port/test/active_input_test.cpp` - the schedule cases; `port/test/gameinput_test.cpp` unchanged (its 363 checks
  pass on the SDL3 build).
- `docs/input_gate.md` - this page.
