# The PresentX path: registration hook, frame-token bridge, window

**Status: wired and measured on one boot.** The presentation core existed and was tested but nothing could reach it: the
console queue proxy answered PresentX itself, and its translation unit is also compiled into GPU-free test binaries.
This page records the two pieces that were missing - an ownership hook the proxy can use without linking the Vulkan
layer, and a bridge for the frame tokens the device proxy issues - plus the window choice that a swapchain-less title
needs, and the exact proxy calls that complete the wiring (now applied, see sections 5 and 6).

Live status after the surface fix (console device slot 127, docs/surface_population.md): the title's present plane
carries a real display resource, so `XboxPresentation::PresentX` no longer refuses it; the wiring below is what turns
that into a presented frame. The fields marked "measured" in section 6 come from that boot.

## 1. Why a registration hook

`port/shims/d3d12_xs_device.cpp` is compiled into the runtime (`palmetto_d3d12_com`) **and** directly into the GPU-free
proxy tests (`xs_proxy_mock_test`, `device_proxy_test`), which link neither Vulkan nor `d3d12_com.cpp`. A console
PresentX handler in that file therefore cannot call `XboxPresentation` directly - the GPU-free links would fail - and
that is exactly why the repo's other cross-layer calls go through `palmetto_register_*`
(`palmetto_register_console_heap`, `palmetto_register_graphics_standin`, `palmetto_register_fence_value_watcher`).

This page's hook follows the same pattern, with the storage in the presentation header so that it costs the proxy
translation unit nothing:

* `port/shims/xbox_present.h` has two layers. The lower one - the console structs, the window struct and the
  registration hook - needs only `windows.h`, `d3d12.h` and `src/frame_pacer.h`. The upper one (class
  `XboxPresentation`) additionally includes `d3d12_com.h` and `vulkan_present.h`. A translation unit that defines
  `PALMETTO_XBOX_PRESENT_HOOK_ONLY` before the include gets **only the lower layer** and no Vulkan dependency:
  ```cpp
  #define PALMETTO_XBOX_PRESENT_HOOK_ONLY
  #include "xbox_present.h"
  #undef PALMETTO_XBOX_PRESENT_HOOK_ONLY
  ```
* The hook slot is a C++17 inline variable, `rt::d3d12::xbox_present_hook::registered`: one instance per process,
  written by `xbox_present.cpp` and read by the proxy's translation unit, with no symbol passing between them.
  The proxy calls only header-inline functions, so a GPU-free binary that includes the header still links.
* With nothing registered, `xbox_present_queue_present()` returns `HRESULT_FROM_WIN32(ERROR_INVALID_STATE)` and the
  token report is a no-op. Nothing is logged (a boot before the backend registers must not spam), and the proxy keeps
  its current answer, so this change is inert until the wiring lands.

## 2. The registration contract

`port/shims/xbox_present.h`:

```cpp
struct XboxPresentBackend {
    std::uint32_t size;      // sizeof(XboxPresentBackend)
    std::uint32_t version;   // xbox_present_backend_version (1)
    ID3D12Device* device;    // the PC Vulkan device to present through; borrowed, outlives the registration
    WindowPresenter* (*create_window_presenter)(ID3D12Device*, HWND);  // the backend's factory, ownership passes
    XboxPresentToQueueCall present;            // filled in by create_xbox_present_backend
    XboxPresentTokenIssuedCall note_issued_token;  // filled in by create_xbox_present_backend
};

const XboxPresentBackend* create_xbox_present_backend(ID3D12Device* device);  // null for a null device
extern "C" HRESULT palmetto_register_xbox_present(void* backend);   // defined in xbox_present.cpp
extern "C" void palmetto_unregister_xbox_present();                 // defined in xbox_present.cpp
```

The backend's whole job is one call after its device exists ("the backend registers the window presenter factory over
the device it presents through, and the core fills in the present-to-queue call and the token bridge"):

```cpp
// port/shims/d3d12_vulkan_device.cpp, at the end of create_vulkan_device() (line ~9049), once the device is built:
if (const rt::d3d12::XboxPresentBackend* backend = rt::d3d12::create_xbox_present_backend(device)) {
    palmetto_register_xbox_present(const_cast<rt::d3d12::XboxPresentBackend*>(backend));
}
```

`palmetto_register_xbox_present` validates the record and logs the outcome once: `S_OK`; `E_INVALIDARG` for a null
record, a record shorter than this version of the struct, a version mismatch, or one without a device, a window
presenter factory or the core's callbacks. Registering again replaces the record (a recreated Vulkan device). The
record from `create_xbox_present_backend` is the module's process-lifetime storage; the backend never fills its own
copy. `create_window_presenter` (d3d12_vulkan_device.cpp) resolves the console device-proxy form of the device as well,
so either form may be registered.

The **presentation service** behind the hook (xbox_present.cpp) owns the single window, the `WindowPresenter` and the
`XboxPresentation`. It is created lazily by the **first present or the first reported token** and then lives for the
process: it is intentionally leaked, because a static destructor would release the swapchain after the Vulkan device's
own static teardown, and the OS reclaims the window at exit. If the window or the registered factory fails at that
point, the failure is logged once and the hook answers `DXGI_ERROR_UNSUPPORTED` (honest: nothing presented) rather than
pretending with `S_OK`.

## 3. The frame-token bridge (the token rule)

The runtime's device proxy answers `WaitFrameEventX` (device slot 80) from its own counter: tokens are 1, 2, 3, ...,
handed out at once (docs/display_path.md section 4). The pacer in `XboxPresentation` therefore never issued the token a
view presents, and `FramePacer::check_present_token` ("a token that was never handed out is `E_INVALIDARG`") refused
every frame. The bridge has two sides:

* **Report side.** The device proxy reports every token it hands out, before returning it to the title:
  `rt::d3d12::xbox_present_note_issued_token(value)` (device slot 80, below). The service keeps the highest reported
  token and forwards it to the presentation; a token reported before the presentation exists is applied when it is
  created.
* **Accept side.** In `XboxPresentation::present_x`, `check_present_token` still runs first and unchanged. Only its
  "never handed out" answer (`E_INVALIDARG`) is reconsidered: if the token is at or below the reported-token watermark,
  it is accepted (logged once, with no statistics entry and no tear - see the limits). Everything else keeps its old
  meaning: null, stale and repeated tokens are presented anyway as before; a present while suspended is still
  `ERROR_INVALID_STATE`; a token past both counters was handed out by nobody and is still `E_INVALIDARG`.

**The rule, one line:** *a token is accepted when the pacer issued it (its own checks exactly as before) or when the
device proxy reported it; otherwise it is refused as never handed out.*

Failure modes, all deliberately equal to the pre-bridge behaviour:

| case | result |
|---|---|
| the report is missed, or arrives after the title's PresentX | `E_INVALIDARG`, nothing presented (exactly the old refusal) |
| token above the reported watermark | `E_INVALIDARG` |
| present while suspended, externally issued token or not | `ERROR_INVALID_STATE` |
| a pacer-issued token | the pacer's checks, byte for byte the old path |
| an externally issued token below the highest report | accepted; the watermark is a counter maximum, not a set. The device proxy's counter is contiguous (1, 2, 3, ...), so every value below the watermark was in fact issued; an issuer with holes would let a hole token below the watermark through (presented, never refused) |

Limits of an externally issued token (documented, not hidden): it has no `FramePacer` record, so
`GetFrameStatisticsX` answers `S_FALSE` with count 0 for it and `ImmediateThresholdPercent` cannot measure it
(`present_timing` is 0, so it shows at a vblank, never torn). The present is still committed with
`commit_present_token`, i.e. it counts as a frame to show.

**Ordering / pacing note for the wiring.** The first wiring below leaves device slots 78/79 (`SetFrameIntervalX`,
`ScheduleFrameEventX`) with the device proxy and wires only slot 80's token report. The pacer's interval is then not
registered, presents are unpaced (`sync_interval` 0) and nothing queues - a correct, if vsync-less, first result. If
78/79 are forwarded to the pacer *without* also forwarding slot 80's wait (which would make the pacer the token issuer
and retire the bridge), every present commits as a frame to show with no wait-side backpressure and `pending_` grows
with the title's own frame rate. Do not forward 78/79 alone; forward the whole frame-event handshake in one step, or
leave 78/79 where they are.

## 4. The window choice for a swapchain-less title

`PresentX` has no DXGI swapchain and the title calls no `CreateSwapChain*`/`EnumOutputs` (docs/display_path.md section
4), so the runtime makes the window itself. The service creates exactly one:

| property | choice |
|---|---|
| class | `PalmettoPresentX` (window procedure `DefWindowProcW`: the swapchain is driven by the present calls, no message loop is needed) |
| title | `Palmetto - PresentX` |
| client area | 1280x720 by default; `PALMETTO_PRESENT_WIDTH` / `PALMETTO_PRESENT_HEIGHT` override it, each clamped to 320..7680 (an unusable value keeps the default and is logged once) |
| visibility | created hidden; `PALMETTO_PRESENT_WINDOW=auto` (default) shows it from the first **presented** frame, `=shown` shows it at creation, `=hidden` never shows it; any other value is `auto`, logged once |
| lifecycle | created at the first present through the hook (or the first reported token), then process lifetime; the swapchain is created against it by the registered window presenter factory |

`auto` is the default on purpose: while the title's surface is still null, a wired present refuses `E_INVALIDARG`, so
`auto` never opens a black window; the window appears exactly when a frame was actually presented. The creation, the
visibility choice and the first-frame show are each logged once:

```
[rt] present: created the PresentX window "Palmetto - PresentX" with a 1280x720 client area, hidden until
    the first presented frame (PALMETTO_PRESENT_WINDOW=auto); the window and its swapchain live for the process
[rt] present: showing the PresentX window (1280x720 client): the presented frames are in it now
```

`port/test/xbox_present_test.cpp` follows the same pattern for its own windows (a `RegisterClassW`/`CreateWindowExW`
pair it owns and destroys) and tests the choice above directly: default title/class/size/visibility, the `shown` and
`hidden` modes, the size overrides, an unusable size falling back to the default, and the destroy.

## 5. The exact proxy wiring for later

Three edits, in files this work does not own. Add the include to `port/shims/d3d12_xs_device.cpp` (near its other
includes):

```cpp
// docs/present_wiring.md: the presentation core publishes the PresentX path through this hook; the hook-only layer
// keeps this translation unit free of the Vulkan layer (it is also linked into GPU-free test binaries).
#define PALMETTO_XBOX_PRESENT_HOOK_ONLY
#include "xbox_present.h"
#undef PALMETTO_XBOX_PRESENT_HOOK_ONLY
```

1. **Queue slot 32 (PresentX)** - replace the final `E_NOTIMPL` refusal in the `K == ChildKind::queue && Slot == 32`
   branch (keep the sampled plane dump and the off-by-default `PALMETTO_PRESENT_STUB` measurement branch in front of
   it):

```cpp
        // Console PresentX through the backend's registration (docs/present_wiring.md). With nothing registered - this
        // TU in a GPU-free test, or a boot before the Vulkan device registered - the hook refuses with
        // ERROR_INVALID_STATE and the proxy's old answer is unchanged.
        const HRESULT present_hr = rt::d3d12::xbox_present_queue_present(
            self->real, static_cast<std::uint32_t>(a1),
            reinterpret_cast<const rt::d3d12::XboxPresentPlaneParameters*>(a2),
            reinterpret_cast<const rt::d3d12::XboxPresentParameters*>(a3));
        d3d12_trace::record(child_trace_name<K>(Slot), SUCCEEDED(present_hr) ? "" : "failed");
        return hr_to_u64(present_hr);
```

`self->real` is the PC queue behind the console proxy; the presentation reads no queue (there is one window), it is
passed for the trace. `a1`/`a2`/`a3` are the measured console arguments: plane count, the title's
`D3D12XBOX_PRESENT_PLANE_PARAMETERS` array, `D3D12XBOX_PRESENT_PARAMETERS`. The plane memory is the title's own
(0x48-byte entries documented in xbox_present.h) and is read only during the call.

2. **Device slot 80 (WaitFrameEventX)** - in `proxy_wait_frame_event_x`, after `*token = value;`:

```cpp
    // docs/present_wiring.md: report the token to the frame-token bridge before the title can present it.
    rt::d3d12::xbox_present_note_issued_token(value);
```

This is the only token-bridge edit: the proxy keeps issuing tokens, the report makes them presentable.

3. **Backend registration** - at the end of `create_vulkan_device` in `port/shims/d3d12_vulkan_device.cpp` (line
   ~9049), include `xbox_present.h` there and add:

```cpp
    if (const rt::d3d12::XboxPresentBackend* backend = rt::d3d12::create_xbox_present_backend(device)) {
        palmetto_register_xbox_present(const_cast<rt::d3d12::XboxPresentBackend*>(backend));
    }
```

The GPU build already links `xbox_present.cpp` through `palmetto_d3d12_com`; the GPU-free binaries only see the header's
hook-only layer. `device` is the Vulkan-backed `ID3D12Device` that `create_vulkan_device` returns (`create_window_presenter`
accepts the console proxy form too, if the registration is moved to where only that form exists).

After those edits the whole path is: title `PresentX` -> queue proxy slot 32 -> `xbox_present_queue_present` ->
registered `present` -> `XboxPresentation::PresentX` -> `WindowPresenter` on the runtime window; and every title frame
token -> device proxy slot 80 -> `xbox_present_note_issued_token` -> pacer bridge. A boot's present calls then either
succeed (a resource is there) or refuse with the presentation core's own honest `E_INVALIDARG` (the surface is still
null), and the `[rt] present:` lines name which one happened.

## 6. Measured: one 150 s boot with the wiring in

run: `palmetto.exe --run <title> --game-dir <package> --overlay <overlay-dir> --trace <out>/boot.jsonl
--trace-d3d12` through `tooling/boot_report.py` (env: `PALMETTO_SHADERS=<shaders-dir>`,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`), 150 s timeout, retail image
1.0.2.1. The surface fix (console device slot 127) was in the same binary. The boot log lines, in order:

```
[rt] present: registered the PresentX backend: PC device ..., window presenter factory, frame-token bridge
[rt] present: created the PresentX window "Palmetto - PresentX" with a 1280x720 client area, hidden until
     the first presented frame (PALMETTO_PRESENT_WINDOW=auto); the window and its swapchain live for the process
[rt] present: PresentX with a frame token issued by the device proxy, not by the pacer (frame-token bridge):
     presented; no statistics entry and no tear for such a token
[rt] present: showing the PresentX window (1280x720 client): the presented frames are in it now
```

* **PresentX succeeds**: 62 `XboxPresentation::PresentX ok` trace lines, 0 failed, and no `[rt] d3d12: present failed`
  (VkResult) line. The queue proxy's console PresentX (slot 32) is what produced them; the frame-token bridge's report
  was accepted once and from then on every frame token is accepted (the bridge log line appears once, by design).
* **Window**: the service created the 1280x720 `Palmetto - PresentX` window at the first present, hidden, and
  showed it from the first successfully presented frame (`auto`), so the window is visible for the rest of the run.
  A second boot polled the process's top-level windows: the window appears as `Palmetto - PresentX`,
  `IsWindowVisible` true, 1280x720 client, first seen at t=34 s (its first present is earlier in that second), while the
  title's own `the title's window` window is already up at t=4 s. The window title and class are the documented ones; nothing
  about the title's package is in them.
* **Run health**: ALIVE 150.2 s, killed by the harness timeout; 0 exception events in the JSONL trace; 0 failed
  `Close`; the boot did not hang, crash or drop submits because of the present.
* **Frame-loop shape and the present's cost**: the presents are the boot's tail - the traced call sequence reaches
  #55,837 and the first PresentX is #53,635 - because the 150 s is dominated by the state/pipeline phase
  (`XboxID3D12Device::Slot57` 7,760, `CreateReservedResource1 failed` 41,062 from the separate slot-55 frontier,
  `ID3D12GraphicsCommandList::Close ok` 752). A control boot with the same tree and the present stubbed out
  (`PALMETTO_PRESENT_STUB=1`, S_OK without a presenter or window) has the same shape: 57 queue submits, the first at
  trace #52,906 of #53,993 - so the late, short frame loop is the tree's startup phase, not the wired present. The
  present did not cost the loop frames: the wired boot produced 124 queue submits / 62 presents where the stub produced
  57 / 57.
* **Submits** (the baseline to keep): `ExecuteCommandLists` 124 calls, 120 of them failed at replay (the separate
  unresolved-resource frontier, docs/replay_resolution.md), 4 succeeded; 124 queue signals, 127 list resets, 64
  `ClearRenderTargetView`, 0 close failures. The present path submits nothing itself, so these numbers are the tree's,
  and they are unchanged by the wiring.
* **What is in the window**: the swapchain receives the title's own display resource (the slot-127 surface) through the
  blit, and the present returns S_OK on it. Whether that surface holds the rendered frame depends on the replay
  frontier above - 120 of 124 command lists (the clears and draws) did not execute - so the honest reading is: the
  window is up and presenting; its content is the display resource the title hands over, which the failing submits
  leave at whatever the 4 successful lists put there. Pixel verification from inside the runtime is not wired
  (`WindowPresenter::enable_capture` is a test hook), so this page does not claim the drawn frame is visible.

## 7. Tests

All new coverage is in the presentation tests, which need a window and a Vulkan GPU (they exit 77 as skipped without
one); no new test needs the game or movie data.

`port/test/xbox_present_test.cpp`:

* the window choice (title, class, client size, the three visibility modes, size overrides, fallback, destroy);
* the registration hook with nothing registered (`ERROR_INVALID_STATE`), with an incomplete/short/null/version-mismatched
  record (`E_INVALIDARG`), and that a refused record leaves the slot empty;
* the frame-token bridge on a real `XboxPresentation`: unreported external token refused, reported token accepted and
  its pixels read back, a token past the watermark refused, a lower report not lowering the watermark, no statistics
  entry for an external token, and the pacer's own next token still issued/presented;
* the whole registered path end to end over the test's Vulkan device: a frame presented through
  `xbox_present_queue_present` with a reported token is `S_OK`; a token past the bridge is `E_INVALIDARG`; a null
  plane resource is `E_INVALIDARG`; after `palmetto_unregister_xbox_present` the call is back to
  `ERROR_INVALID_STATE`.

`port/test/d3d12_boot_movie_test.cpp` (package-gated, exits 77 without `PALMETTO_GAME_DIR`/`PALMETTO_MOVIE`): the same
registered path over a real decoded boot-movie frame, plus the null-plane refusal through the hook.

The existing presentation cases (blank frame, 1 and 2 planes, composition, tokens, suspend/resume, statistics,
tearing, the real-time loop, unsupported sources) are unchanged by the bridge in the normal case and still assert the
same pixels.

Results: all of the above passes (`xbox_present_test: all cases pass`), and `xs_proxy_mock_test` passes with its slot-32
expectation updated to the hook's `ERROR_INVALID_STATE` (that file is also compiled GPU-free and has no presentation
registered). The gates are `the local build script` -> `PALMETTO_OK` and `the local test build script` -> `TESTS_BUILT`; the known
pre-existing failure `d3d12_fence_timing_test` is untouched.

While making the existing presentation cases runnable again, one pre-existing test-side defect had to be fixed outside
the feature: `xbox_present_test`'s clear helper (and the boot-movie test's submit helper) called
`ID3D12CommandQueue::Signal` through the PC vtable index (14), but the device proxy hands out console-ABI command
objects, where Signal is console slot 16 and slots 14/15 are the console's two inserted methods - so the Signal was
refused and the clear never ran. The helpers now call console slot 16 (the same call the title makes); no product code
changed for this.

## 8. Limits

* One machine, one image, one Vulkan backend: one 150 s boot with the wiring (section 6), and the pre-wiring tree was
  not booted with presents refused on the same commit, so the frame-loop rate comparison rests on the traced call
  distribution, not on a controlled A/B boot. The present itself is measured (62 ok / 0 failed / window shown); the
  window's pixels are not (no capture in the runtime path).
* The bridge's watermark assumes the device proxy's token counter is contiguous and monotonic, which is how the proxy
  issues them today; a per-token set would be the alternative if that ever changes.
* Externally issued tokens get no frame statistics and no `ImmediateThresholdPercent` tearing until the frame-event
  handshake (slots 78/79/80) is forwarded to the presentation core, which would replace the bridge rather than extend
  it.
* The window is one per process and process-lifetime by design (see section 2); a recreation after a lost device is
  not modelled.

## 9. Measured colour-space path and HDR10 output diagnostic

This slice measures and preserves the existing PresentX default, then adds an opt-in HDR10 request for the composed-plane
path. The code and captures below are this tree's own; public peer ports were checked only for behavioural evidence.

### 9.1 Measured default path

The presenter measurement in [peer_port_first_ui.md](peer_port_first_ui.md#151-what-was-measured-before-the-port) records
one full-plane present: source `VK_FORMAT_R8G8B8A8_UNORM` (format 37), 3840x2160, plane colour-space word 0 (G22/P709),
whole-plane source rectangle and full-output placement. That placement takes the plain `present()` blit, not the compositor.
The output was 2564x1421 `VK_FORMAT_B8G8R8A8_UNORM` (format 44),
`VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` (0), with no HDR10 request or ST.2084 surface entry. The blit is UNORM-to-UNORM,
so the captured 8-bit values are copied without EOTF decode/encode; the bilinear rescale changes geometry, not colour
values. This is the measured reason not to force an sRGB target or conversion on the default plane path.

The existing plain-blit family rule remains: an sRGB source requests an sRGB 8-bit family; a non-sRGB source requests
UNORM; an HDR10 composed plane requests a listed 10-bit ST.2084 target. The selector in `present_colour.h` keeps B8 before
R8, keeps `VK_FORMAT_A2B10G10R10_UNORM_PACK32` before `VK_FORMAT_A2R10G10B10_UNORM_PACK32`, and pairs an HDR format only
with `VK_COLOR_SPACE_HDR10_ST2084_EXT`. An sRGB-source request falls back to UNORM only when the surface lists no
matching sRGB format. If no policy entry matches, the prior behavior of selecting the surface's first entry is retained.
For composed output the shader already writes transfer-encoded values into an UNORM/10-bit swapchain image; format-family
selection must not add another transfer encode.

### 9.2 Conversion reference and opt-in output

`port/src/present_compose.comp` is the GPU conversion path. It decodes SDR as sRGB or gamma 2.2 and HDR10 with ST.2084
PQ, transforms linear Rec.709 and Rec.2020 values with the compositor's 709/2020 matrices, blends premultiplied colour in
linear light, uses the configured SDR white (default 203 nits) and HDR peak (default 1000 nits), and encodes HDR output
as PQ. HDR-to-SDR converts to Rec.709, normalizes by SDR white and applies the existing luminance shoulder above its
0.8-white knee. [The GPU-free reference header](../port/src/present_colour.h) mirrors those transfer/matrix/shoulder values and the
format-selection/parser policy; it does not replace or execute the shader. CPU assertions and GPU captures below keep the
reference and shader anchored to the same known colours.

`PALMETTO_PRESENT_HDR10=output` is the opt-in *output* request (unset by default). On the `present_planes()` compose
path, it asks for a listed HDR10/ST.2084 surface even when the input is SDR; the composer converts SDR Rec.709 to linear
Rec.2020, scales by SDR white and applies PQ. It does not change the source-plane EOTF: the caller's
`PresentPlanes::sdr_eotf` remains authoritative (the existing `PALMETTO_SDR_EOTF=gamma22` option covers the measured
word-0 G22/P709 tag). Values `1`, `on`, `true`, `yes` and `diagnostic` are also accepted case-insensitively; `0`, `off`,
`false` and `no` disable it. Unset/empty is off. Any other spelling is refused by name and the diagnostic stays off.

If the surface has no 10-bit ST.2084 entry, or its listed HDR10 format cannot be a blit destination, the presenter prints
a named `PALMETTO_PRESENT_HDR10=output refused` reason and retains SDR output (HDR10 input is tone-mapped as before).
This switch only affects `present_planes()`; the measured whole-plane `present()` path bypasses composition, so its default
commands, output format and captured bytes stay unchanged. With the switch off, SDR planes retain the prior SDR swapchain
selection; HDR-source requests retain their prior behavior.

### 9.3 Verification and limits

* `port/test/vk_presenter_capture_colour_test.cpp` remains GPU-free. It now checks sRGB and gamma-2.2 round trips, PQ
  anchors (100/1000/10000 nits), the 709/2020 matrix inverse, the SDR-white/HDR-peak conversion and shoulder, measured
  B8 UNORM + sRGB-nonlinear selection, family preference/fallback, HDR10 selection, missing-HDR fallback, and strict
  switch parsing. Built and run standalone through the existing CMake target: passed.
* [The GPU test source](../port/test/vk_presenter_colour_test.cpp) uploads a synthetic RGBA8 pattern with four known bands,
  captures the plain `present()` blit and the real composition, compares the default G22/P709 SDR bytes and the shader's
  PQ/Rec.2020 encoding against the CPU reference, then toggles the production HDR10-output request. The GPU test was
  compiled and run on an NVIDIA GeForce RTX 4090: raw plain-blit values matched (tolerance 1), composed SDR round-trip
  matched (tolerance 2), and forced-PQ UNORM8 matched (tolerance 3); the WSI surface did not expose HDR10, so the named refusal was observed and the SDR fallback
  capture matched. No HDR10 10-bit capture is claimed for this host. The test ended with zero Vulkan validation errors.
* The GPU source is registered as a separate Windows test target (`vk_presenter_colour_test`, CTest name
  `vk_presenter_colour`), labelled `gpu;vulkan;present`, with a 60-second timeout and return code 77 treated as skipped
  when the Vulkan/Win32 capture path is unavailable. Its CMake stanza is adjacent to the presenter sync test and links
  `Vulkan::Headers` plus `user32`; the integration owner adds that stanza separately to avoid overlapping CMake edits.
  After integration, build it with `cmake --build port/build --target vk_presenter_colour_test` and run
  `ctest --test-dir port/build -C RelWithDebInfo -R "^vk_presenter_colour$" --output-on-failure`.
* No title boot or `PALMETTO_OK` build/relink was run or claimed; the current WSI surface also gives no local HDR10
  display proof. The next HDR falsifier is to run this GPU capture on a surface listing a usable HDR10/ST.2084 format
  and check the captured packed 10-bit PQ values. The opt-in output cannot alter the measured plain-blit/title path unless
  that caller already routes through `present_planes()`.

### 9.4 NOTICE draft row (not applied to `NOTICE.md`)

> The PresentX colour/HDR10 attestation and opt-in composed HDR10 output ([docs/present_wiring.md](present_wiring.md), section 9) were independently implemented from this tree's measured UNORM/G22/P709 plane and Vulkan surface. Behaviour was cross-checked against Five Star Recomp (GPL-3.0-or-later, `34b4dfed`), VIctory (GPL-3.0-or-later, `63bcca43`), PercyScarlett (GPL-2.0-only, `e44c465e`), victory-fork (GPL-3.0-or-later, `9a9091d1`) and XCL (GPL-2.0-only, `623d02e4`), including Five Star's format-family and ST.2084 HDR10 rule. Those projects were read as behavioural evidence only: no source, comments, identifiers or tables were copied; each upstream remains the copyright of its contributors under its own licence.
