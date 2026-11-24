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
