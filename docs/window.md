# The window the runtime presents into: adopting the title's own

Status: implementation measured on one boot (numbers in section 5). Frame **content** is a separate frontier and is not
claimed here: the window is still black for the reasons docs/page_content.md and docs/texture_layout.md measure. This
page is about window identity and count only.

The presentation core (docs/present_wiring.md) presents PresentX without a DXGI swapchain. The window it presents into
is therefore the runtime's own choice, and that choice used to produce a second window. This page records the measured
two-window state, the adoption rule that replaced it, its fallback, and its limits.

## 1. Measured state: two visible top-level windows

One harness boot with a window probe inside the boot process (enumerate the process's top-level windows once a second)
measured, at the end of a run:

| window | class | state | first seen |
|---|---|---|---|
| the title's own | `sgaWindowClass` (title "Americas") | visible, top-level, owner 0, rect 0,0 2580x1460 | about 4 s |
| the runtime's | `PalmettoPresentX` (title "Palmetto - PresentX") | visible, top-level, owner 0, 1280x720 client | about 34 s, at its first present |

Plus hidden helper windows of the same process (`NVOpenGLPbuffer`, `temp_d3d_window`, Input Method Editor windows). Both
visible windows are top-level and unowned, so the process really had two windows on screen where one is the design.

Two facts decided the fix:

* The title's window exists long before the present service does: the service is created lazily at the first PresentX,
  and the probe saw the title's window about 30 s earlier. Nothing in our tree creates `sgaWindowClass`; the title does
  it through host user32 inside this process, so the window is ours to present into.
* The presenter factory already takes an HWND (`create_window_presenter(ID3D12Device*, HWND)`), so the window was never
  the presenter's constraint - nothing handed PresentX a window, so the service made one.

## 2. The adoption rule

`create_or_adopt_xbox_present_window()` (`port/shims/xbox_present.cpp`) is what the presentation service calls at its
lazy creation, instead of `create_xbox_present_window()` directly. It:

1. enumerates the **current process's top-level windows** (`EnumWindows`, then `GetWindowThreadProcessId` must be this
   process; another program's window is never considered);
2. runs the usable-window rule over them, then presents into the chosen window with the existing presenter factory and
   the existing swapchain path.

The usable-window rule (`choose_xbox_present_window`, pure Win32-free logic, unit-tested) accepts a candidate when all of
these hold:

| condition | why |
|---|---|
| visible (`IsWindowVisible`) | a hidden window shows nothing; the title will show its own window when it wants to |
| not minimised (`IsIconic`) | same, and the swapchain would be 0x0 |
| not owned (`GW_OWNER` null) | owned windows are dialogs and helpers of the title, not its frame window |
| not a tool window (`WS_EX_TOOLWINDOW` clear) | tool windows are not the game's frame window |
| not an IME window (class name does not contain `IME`) | IME windows are visible, top-level and not ours, and presenting into one is wrong |
| non-empty client area (`GetClientRect` > 0x0) | nothing to present into yet |
| not the runtime's own class (`PalmettoPresentX`) | never adopt another one of ours |

Of the usable windows, one of the title's class (`sgaWindowClass`, the measured one) wins outright; without one, the
usable window with the **largest client area** wins (the game's frame window is the biggest thing the process owns; the
window created by the runtime, when there is one, is never chosen). The choice is logged once with the class name and
client size, never the title text:

```
[rt] present: adopting the title's own window (class "sgaWindowClass", 2580x1440 client) instead of creating a
    second one: frames are presented into it, its title, size and visibility stay the title's (docs/window.md)
```

The title keeps its window behaviour: the runtime does not write the title text, does not show/hide the window, and
installs no subclass or window procedure on it. The window is only a Vulkan surface plus an extent.

### Sizing and scaling

The presenter factory (`create_window_presenter` in `port/shims/d3d12_vulkan_device.cpp`) already builds the swapchain
on the window it is given, with an extent callback that reads `GetClientRect` and a recreate path
(`VulkanPresenter::recreate` in `port/src/vulkan_present.h`) that runs when the surface reports `VK_ERROR_OUT_OF_DATE_KHR`
or a format/vsync change. So:

* the swapchain is sized to the adopted window's client area (2580x1440-ish here, not 1280x720);
* a client-size change is followed at the next present (the swapchain is recreated against the new client size), and the
  existing blit path stretches the title's plane to the swapchain extent, so the frame is scaled, not cropped;
* a minimised adopted window makes the surface extent 0x0, and the presenter already skips the present (`S_OK`, nothing
  shown) until the window is restored.

### Environment switch

`PALMETTO_PRESENT_ADOPT=off` (or `0`) keeps today's own-window path, for an A/B and for tests that present through a
known window; unset or any other value adopts. The own window's variables (`PALMETTO_PRESENT_WINDOW`,
`PALMETTO_PRESENT_WIDTH`, `PALMETTO_PRESENT_HEIGHT`) still apply to the fallback window.

## 3. The fallback: today's own window, unchanged

When the rule finds nothing to adopt, or adoption is switched off, `create_xbox_present_window()` runs exactly as
before: class `PalmettoPresentX`, title `Palmetto - PresentX`, 1280x720 client (overridable), created hidden with
`PALMETTO_PRESENT_WINDOW=auto` showing it from the first presented frame, and process lifetime. `show_xbox_present_window`
and `destroy_xbox_present_window` are no-ops on an adopted window (its visibility and lifetime are the title's); on a
created window they behave as before. The fallback is logged once:

```
[rt] present: this process has no visible window to present into (the title has not made one, or it is hidden or
    minimised): creating the runtime's own PresentX window (docs/window.md)
```

## 4. What still produces two windows

The adoption decision is made **once**, when the presentation service is first needed (first PresentX, or first frame
token). It is not revisited. That leaves these cases with two windows, or with a window that stops being presented into:

* **The title creates its window after the first present** (or its first present arrives before the window exists). The
  service falls back to its own window and keeps it; the title's window appears later and the process then has two.
  Measured boot: not the case (title's window at about 4 s, first present at about 34 s).
* **The title's window is hidden or minimised at that moment.** The rule skips it (a hidden window would show nothing and
  the runtime would not show the title's window itself), so the fallback window is created.
* **The title destroys or recreates its window after adoption.** The swapchain's surface is lost; the presenter tries to
  make a new surface for the same HWND (`recreate_surface_fn_` in `port/src/vulkan_present.h`), which cannot succeed for a
  destroyed window, so the frame is not shown. The runtime does not re-run the adoption rule on a new HWND - replacing a
  live presenter mid-frame-loop is not modelled here. Unmeasured: this title has not done it.
* **A second top-level window of the title becomes the "game" window later** (e.g. a splash window first, the real frame
  window second). The largest-window tie-break picks the biggest at that moment; if a small splash window is the only
  visible one, it is adopted and the real one is never used.

## 5. Measurement

One harness boot after the change (no pixel claim):

```
python -I tooling/boot_report.py --exe port\build\palmetto.exe \
  --image <package exe> --game-dir <package> --overlay <overlay-dir> --timeout 120
```

with `PALMETTO_SHADERS=<shaders-dir>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, and a window probe polling the process's top-level windows once a second.

MEASURED (one 90 s harness boot on the moved workspace, all paths on the same drive):

* the service logged once: `adopting the title's own window (class "sgaWindowClass", 2564x1421 client) instead of creating
  a second one: frames are presented into it, its title, size and visibility are left to the title`;
* top-level windows of the process with a visible state: exactly one - the title's `sgaWindowClass` window (2580x1460
  outer, 2564x1421 client); `created the PresentX window` never appears, so the runtime's own window is not created;
  the hidden helper windows (`NVOpenGLPbuffer`, a 1x1 `temp_d3d_window`, IME windows) are unchanged;
* presents: 8,625 `PresentX ok`, 0 failed; 16,384 submissions at 69.1 s; the run was killed by the 90 s timeout;
* ALIVE: no exception recorded (0 exception events);
* window content: still black - the frame-content frontier (docs/page_content.md, docs/frame_capture.md), not claimed
  here.

## 6. Tests

`port/test/xbox_present_test.cpp` covers the choice without a Vulkan device for the rule itself, and with real Windows
windows for the adoption:

* the pure rule on synthetic candidates: no candidates; the title's class wins over a larger window; the largest client
  area otherwise; invisible, minimised, owned, tool, IME, empty-client and our own class are all skipped, including a
  hidden window of the title's class; the runtime's own window is never chosen;
* real windows: a visible window of the measured title's class is found and its class/client size are the measured ones;
  `create_or_adopt_xbox_present_window()` with the default adopts it (no second window, its visibility untouched) and
  `destroy_xbox_present_window` leaves it alive; hiding it makes the rule skip it, and with nothing visible left there is
  nothing to adopt; `PALMETTO_PRESENT_ADOPT=off` still creates the 1280x720 `PalmettoPresentX` window and destroys it.

The registered-PresentX test sets `PALMETTO_PRESENT_ADOPT=off` because it presents through, and asserts on, a window it
knows.

## 7. The title's 120 Hz display-mode answer (P-16)

This is an answer-path slice, not a desktop mode change or a pixel claim. The title decides whether its display path may
use the 120 Hz refresh choice by inspecting the mode records DXGI returns.

### 7.1 Measured consumer and existing answer

[The display measurement in `peer_port_first_ui.md`, section 14.1](peer_port_first_ui.md) locates the consumer in the
`game+0x2481CA0` wrapper. Its mode-list record is `DXGI_MODE_DESC1`-shaped and `0x20` bytes: width at `+0x0`, height at
`+0x4`, the refresh rational at `+0x8`, format at `+0x10`, scanline order at `+0x14`, scaling at `+0x18`, and the final
stereo word at `+0x1C`. The title tests the refresh **numerator at record offset `+0x8`** against `0x78` (120); if any
record matches, it stores byte `1` at `image+0x760E660` and `{120,1}` at `image+0xA9438D4`. Otherwise it stores flag `0`
and `{60,1}`. The existing `PALMETTO_STATE_TRACE` sampler reads those two title-owned words; it does not write them or
install a probe.

The previous light arms measured flag `0` and refresh `60/1` on this 60 Hz host, through the long samples (the last at
298 s); see [section 14.4](peer_port_first_ui.md). Those arms established the consumed answer, but did not dump the
mode-list length or tuples. The next `PALMETTO_DXGI_MODE_ANSWER=host` trace records the count and up to 16 shaped records
(while preserving a numerator-120 record in that bound); any other records are omitted by design. It is the bounded
falsifier for whether the current host answer offered a 120 numerator.

The existing DXGI source path is `collect_modes` in `port/shims/dxgi_com.cpp`: it enumerates `EnumDisplaySettingsW`
records (bounded to 4096), takes width/height where both fields are valid, uses a concrete `dmDisplayFrequency` when it
is greater than 1 and otherwise the current output refresh, then adds the current mode. It rejects zero dimensions/rates,
deduplicates `(width,height,integer Hz)`, and sorts by width, height and refresh numerator. The `DXGI_MODE_DESC1`
conversion retains the requested format, progressive scanline ordering and unspecified scaling, and sets stereo false.
The title's observed 32-byte layout is asserted in the pure test below.

### 7.2 Answer policy and bounded measurement switch

`PALMETTO_DXGI_MODE_ANSWER` is read when a mode list is requested:

| value | answer |
|---|---|
| unset, empty, `off`, `0`, `false` or `no` | the prior host mode list, with no injected record and no trace output |
| `host` | the same host answer, plus a bounded trace of at most 4 mode-list samples and 16 records per sample |
| `simulate-120` | keep the host list; if no record has refresh numerator 120, add one at the current output's size with rational `{120,1}`, requested format, progressive scanline order, unspecified scaling, and stereo false; trace the bounded list |
| any other value or a value longer than 63 bytes | name the refusal once and fall back to the unmodified host answer |

A host that already offers any numerator-120 record receives its native list unchanged, even when simulation is selected.
If no such record exists but the current width or height is zero, the simulation request is refused by name and the host
list remains unchanged. The switch changes only the answer exposed by the shim; it does not ask Windows to change the monitor mode, patch title memory,
or directly pace presentation. `GetDisplayModeList`, `GetDisplayModeList1` and closest-mode matching share the same
collector, so the optional mode is consistent across those methods. With the switch unset, mode creation, filtering,
deduplication, sort order and `DXGI_MODE_DESC1` conversion retain the pre-slice host shape; no trace line or synthetic
mode is produced.

### 7.3 GPU-free tests and arm readiness

The new [mode-answer test source](../port/test/dxgi_mode_answer_test.cpp) is a standalone GPU-free contract test: when the
lead wires it into CMake, it needs no DXGI factory, D3D12 device, Vulkan device, window or game image. It covers switch
parsing/refusal, host record shaping and order, duplicate/zero rejection, the default no-injection branch, current-size
120/1 injection, already-present and missing-size refusals, `DXGI_MODE_DESC1` field shape, the title's numerator-only
120 predicate, and a synthetic byte/rational store for flag `0`/`1` and `{60,1}`/`{120,1}`. The synthetic store is only a
model of the measured words; no title memory is written. CMake/CTest registration is pending lead integration. The C++
test source has **not yet been built or run**: the lead's Palmetto integration gate is still closed, and this slice did not
build or relink the runtime.

The lead confirmed that the host and boot lock were free at the time of coordination, but no arm was taken: the changed
answer is not in a permitted build yet. Readiness is one `-Light` boot with `PALMETTO_DXGI_MODE_ANSWER=simulate-120` and
the existing state trace, after the lead clears integration and the lock is rechecked. Expected observation: the bounded
DXGI trace includes a 120/1 record and the subsequent state trace reads high-refresh flag `1`, refresh `120/1`. If the
mode list contains that record but the title still stores `0`/`60/1`, the consumer-path hypothesis is falsified; if the
record is absent, the answer switch or its mode-list call path is falsified. The arm is capped at one light run; stop
research after two device losses. No game data is needed.

### 7.4 Attribution and NOTICE handoff

The implementation and test model are ours, based on this tree's recorded DXGI mode collection, the title's consumer and
the consumed words above. Public peer trees were inspected as evidence only, not as code sources: Five Star Recomp's
[`DisplayService.cs` at `34b4dfed`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed/src/FiveStar.Loader/Shims/Gdk/DisplayService.cs)
provides host-display capability context; VIctory's [`dxgi.rs` at `63bcca43`](https://github.com/Marvisak/VIctory/blob/63bcca43/crates/victory-gpu/src/dxgi.rs)
reports one 3840x2160, 60/1 mode and names the title's 120 Hz check; PercyScarlett's
[`runtime/victory` `dxgi.rs` at `e44c465e`](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465e/runtime/victory/crates/victory-gpu/src/dxgi.rs)
uses a configured-or-default size with one 60/1 mode; victory-fork's
[`dxgi.rs` at `9a9091d1`](https://github.com/fxceless/victory-fork/blob/9a9091d1/crates/victory-gpu/src/dxgi.rs)
uses the same fixed 60/1 answer; and XCL's
[`gdk_table.rs` at `623d02e4`](https://github.com/xcl-dev/xcl/blob/623d02e4/crates/xcl/src/hle/xgameruntime/gdk_table.rs)
records the display interface and slot mapping rather than this DXGI mode-list path. These references pointed to a
measurement question; no code, comment text, identifier or table was copied. `NOTICE.md` is outside this task's write
scope. **Draft NOTICE row for the lead:** “The measured DXGI display-mode answer path ([docs/window.md](window.md),
section 7) was cross-checked against Five Star Recomp (GPL-3.0-or-later, `34b4dfed`), VIctory
(GPL-3.0-or-later, `63bcca43`), PercyScarlett (GPL-2.0-only, `e44c465e`), victory-fork (GPL-3.0-or-later,
`9a9091d1`) and XCL (GPL-2.0-only, `623d02e4`), as evidence of display handling only; the implementation and tests are
ours, with no peer source, comment, identifier or table copied.”

### 7.5 P-36 follow-up: simulate-120 was not reflected in sampled state

The lead cleared the integration gate; no build or relink was run. One `-Light` 90 s arm used the integrated `palmetto.exe`
(SHA-256 `1DD3A3E84789BDB06EDBB0D078A724F940FC1E38B0C05CF4A3A7CB1F077A28E6`) and
`PALMETTO_DXGI_MODE_ANSWER=simulate-120`, with the boot template's state, timeline, slot-67 and capture/readback probes.
It ran under the shared boot lock at 10:08:51.629–10:10:22.469 (-03), wall 90.3 s; the runner ended it at the 90 s
limit, verdict `ALIVE`, zero exceptions, and no device-loss line.

At the last state sample (t=88 s), the words were `init=2`, `loading=1`, `session=0`, machine `current=2 next=2`; the
high-refresh flag remained `0` and refresh remained `60/1`. No `[rt] dxgi: display mode answer sample` or `mode[...]`
line appeared in stdout or stderr. Although §7.2 documents a trace for `simulate-120`, this integrated run did not emit it;
the trace may be emitted only under the `host` policy in this binary, or the relevant mode-list enumeration may not have
been reached. Neither the call nor the simulated `{120,1}` answer was observed; absence of a trace is not evidence that the
title rejected a returned 120/1 record.

The template recorded 12 presenter captures at 2564x1421, each mean RGB `0.00 0.00 0.00`; the PPMs remain only in the
outside-repository capture folder. Raw boot evidence (`meta.txt`, `stderr.log`, `boot.jsonl`, `summary.txt`) is under
`D:\leonida-work\fsrstable\boot-p36-120-20261127-1008` and captures are under
`D:\leonida-work\fsrstable\capture-p36-120-20261127-1008`; no image or game content was copied here.

**Next falsifier:** first observe a `host`-policy mode-list sample (or otherwise establish that the title reaches the
mode-list call), then in a separately authorized bounded simulation arm verify a returned current-size 120/1 record and
sample the consumer words. A confirmed 120/1 answer followed by flag `0` / refresh `60/1` would falsify the expected
consumer path; this arm alone does not.
