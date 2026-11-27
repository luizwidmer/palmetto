#pragma once

// The Xbox presentation core (docs/d3d12_vulkan.md section 4.5): the frame events and PresentX of a GDKX title, which
// presents without a DXGI swapchain. FramePacer (src/frame_pacer.h) does the timing and the token bookkeeping;
// WindowPresenter (d3d12_com.h, over palmetto::VulkanPresenter) puts the pixels in a window. This class is the
// glue, and the surface the Xbox device and queue proxy will forward to.
//
// WHAT IS KNOWN: the public GDKX samples give the call order and the argument counts; public code that is not
// Microsoft documentation gives most of the slots and the x64 layouts (docs/d3d12_xs_surface.md 7.1):
//   - ebiten (Apache-2.0) mirrors the GDK structs in Go: D3D12XBOX_PRESENT_PLANE_PARAMETERS is 0x48 bytes on x64,
//     D3D12XBOX_PRESENT_PARAMETERS 0x20 bytes, and D3D12XBOX_FRAME_PIPELINE_TOKEN is uint64.
//   - XCL (GPL-2.0-only, facts only) measured the frame calls on device slots 78-81 and PresentX on queue slot 30
//     (older runtime layout: XDefiant, Minecraft, Fortnite) or 32 (newer: Unicorn Overlord).
// WHAT IS OURS: the constant values, the HRESULT choices and the ScaleFilter/plane-flag values; the real enum
// values are still unmeasured. The SuspendX/ResumeX slots are not in any public source.
//
// Slots (0-based, QueryInterface = 0) before the proxy can forward:
//   ID3D12Device (Xbox):        SetFrameIntervalX   (output, interval, latency, flags)                    78
//                               ScheduleFrameEventX (event, offset, reserved pointer, flags)              79
//                               WaitFrameEventX     (event, timeout ms, reserved pointer, flags, &token)   80
//                               GetFrameStatisticsX (token, type mask, &count, array)                     81
//   ID3D12CommandQueue (Xbox):  PresentX (plane count, planes, params)                                    30 (older) / 32 (newer)
//                               SuspendX (flags)                                                        slot unknown
//                               ResumeX ()                                                              slot unknown
// The public numbers are measurements of other titles' runtimes, not of this game. The runtime version moves
// PresentX, so neither value may be assumed; trace the game's slots.
// The queue-side calls do not look at the queue: the Vulkan device has one VkQueue behind every D3D12 queue, and
// the present is submitted after the work already executed on it.

// The header has two layers. The lower one (the console structs and the registration hook at the end) needs only
// windows.h, d3d12.h and src/frame_pacer.h. The upper one (class XboxPresentation below) also needs the Vulkan-facing
// headers: WindowPresenter from d3d12_com.h and the PresentPlanes compositor from vulkan_present.h. A translation unit
// that only needs the hook - the Windows queue proxy, which is also compiled into GPU-free test binaries that do not
// link the Vulkan layer - includes this header with PALMETTO_XBOX_PRESENT_HOOK_ONLY defined and gets the hook without
// any Vulkan or d3d12_com.h dependency:
//     #define PALMETTO_XBOX_PRESENT_HOOK_ONLY
//     #include "xbox_present.h"
//     #undef PALMETTO_XBOX_PRESENT_HOOK_ONLY
#include "../src/frame_pacer.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#ifndef PALMETTO_XBOX_PRESENT_HOOK_ONLY
#include "d3d12_com.h"
#include "../src/vulkan_present.h"
#endif

namespace rt::d3d12 {

class WindowPresenter;  // d3d12_com.h: the swapchain window presenter (complete only where it is used).

// Provisional (ours): D3D12XBOX_FRAME_INTERVAL_* as periods in microseconds.
inline constexpr std::uint32_t xbox_frame_interval_30hz_us = 33333;
inline constexpr std::uint32_t xbox_frame_interval_60hz_us = 16667;
inline constexpr std::uint32_t xbox_frame_interval_120hz_us = 8333;

// D3D12XBOX_PRESENT_PLANE_PARAMETERS. The names and the x64 layout are confirmed by the public ebiten Go binding
// (Apache-2.0, internal/graphicsdriver/directx/d3d12_windows.go, _D3D12XBOX_PRESENT_PLANE_PARAMETERS) and by XCL's
// reads of the first three fields: 0x48 bytes, Token @0x00, ResourceCount @0x08, ppResources @0x10, pSrcViewRects
// @0x18, pDestPlacementBase @0x20, ColorSpace @0x28, ScaleFilter @0x2C, ExtendedDescCount @0x30, pExtendedDescs
// @0x38, Flags @0x40. The enum values and flag bits remain unmeasured; the values below are placeholders that make
// the composition (section 4.5) testable.
//
// Zero everything is the default and means "as before": the whole output, bilinear when scaling, SDR, plane 1
// premultiplied and not composed.
struct XboxViewRect {  // D3D12XBOX_VIEW_RECT: four floats; as fractions 0..1 of the output (hypothesis, section 4.5)
    float left, top, right, bottom;
};
// DXGI_COLOR_SPACE_TYPE values (documented DXGI numbers). Only these two are composed; any other is treated as the first.
inline constexpr int xbox_color_space_g22_p709 = 0;       // DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: SDR
inline constexpr int xbox_color_space_g2084_p2020 = 12;   // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: HDR10
// Provisional (ours): ScaleFilter and Flags.
inline constexpr int xbox_scale_filter_default = 0;  // bilinear (point when not scaled makes no difference)
inline constexpr int xbox_scale_filter_point = 1;
inline constexpr int xbox_scale_filter_linear = 2;
// Plane 1 is composed over plane 0 as an overlay only with this flag (hypothesis: whether the console blends a second plane
// at all is unmeasured; the public HDR sample's plane 1 is the GameDVR copy of the frame, not shown, so without the flag
// plane 1 is not composed, as before). This is the tree's gate off by default; PALMETTO_PRESENT_PLANE_RULE=measured
// replaces it with the rule of src/plane_compose_rule.h (docs/peer_port_movie.md, item 6), which composes plane 1 when its
// own descriptor places it or asks for its own filtering and refuses the unmeasured no-placement case by name.
inline constexpr UINT xbox_plane_flag_overlay = 1;
inline constexpr UINT xbox_plane_flag_straight_alpha = 2;  // plane 1's colour is not premultiplied by its alpha

struct XboxPresentPlaneParameters {
    // Field order, types and x64 offsets: the public ebiten binding above; XCL reads Token, ResourceCount and
    // ppResources at the same offsets. This is the console's layout on x64.
    std::uint64_t Token;                  // from WaitFrameEventX; 64-bit storage confirmed, 0 = ..._NULL
    UINT ResourceCount;                   // 1 in the samples
    ID3D12Resource* const* ppResources;   // ResourceCount committed resources with ALLOW_DISPLAY, in PRESENT
    const D3D12_RECT* pSrcViewRects;      // null, or one rectangle per resource (the HDR sample sets it)
    const XboxViewRect* pDestPlacementBase;  // null: the whole output
    int ColorSpace;                       // a DXGI_COLOR_SPACE_TYPE (see above)
    int ScaleFilter;                      // xbox_scale_filter_*; anything unknown is the default
    UINT ExtendedDescCount;               // not used (logged once when non-zero)
    const void* pExtendedDescs;
    UINT Flags;                           // xbox_plane_flag_*; unknown bits are ignored
};

// D3D12XBOX_PRESENT_PARAMETERS. The console's object is 0x20 bytes on x64 (ebiten): ImmediateThresholdPercent @0x00,
// ViewCount @0x04, ExtendedDescCount @0x08, pExtendedDescs @0x10, Flags @0x18. This struct is the first 4 bytes;
// it is only read through a pointer, so never size a copy by sizeof(XboxPresentParameters).
struct XboxPresentParameters {
    // 0 (the default): a frame that missed its vblank waits for the next one. Above 0: a frame that missed it is shown
    // at once, mid-scan, when the scan is not further than this percentage down the screen (100: wherever it is).
    // Needs VK_KHR/EXT_swapchain_maintenance1 and an IMMEDIATE or FIFO_RELAXED mode the swapchain can switch to;
    // without them it is ignored (logged once). What late means: see FramePacer::present_timing.
    float ImmediateThresholdPercent;
};

#ifndef PALMETTO_XBOX_PRESENT_HOOK_ONLY
// The presentation core (docs/d3d12_vulkan.md section 4.5, docs/present_wiring.md): one window, one pacer, one
// presenter. Not part of the hook-only layer, see the include guard at the top.
class XboxPresentation {
public:
    // `clock` null: the process's shared device frame clock (the present-driven clock the device frame-event slots use,
    // see XboxPresentBackend::frame_events; real time). Tests pass a palmetto::ManualFrameClock, which must outlive this
    // object, and then this presentation owns its pacer as before. `presenter` is what create_window_presenter returned.
    explicit XboxPresentation(std::unique_ptr<WindowPresenter> presenter, palmetto::FrameClock* clock = nullptr);
    ~XboxPresentation();
    XboxPresentation(const XboxPresentation&) = delete;
    XboxPresentation& operator=(const XboxPresentation&) = delete;

    // Device side. `output` (an IDXGIOutput*, which the samples fetch from the device) is not used: there is one
    // window. `interval_period_us` is the period of a frame interval, e.g. xbox_frame_interval_60hz_us; `latency`
    // is the frames allowed in flight (the samples pass the buffer count minus 1, or 1).
    HRESULT SetFrameIntervalX(IUnknown* output, UINT interval_period_us, UINT latency, UINT flags);
    // Only the origin event (xbox_frame_event_origin) exists. `offset_us` delays it inside the frame. The pointer
    // argument the samples pass as null is not modelled.
    HRESULT ScheduleFrameEventX(UINT event, UINT offset_us, UINT flags);
    // Blocks for the next origin and while the latency is used up. HRESULT_FROM_WIN32(WAIT_TIMEOUT) after `timeout_ms`
    // (INFINITE waits for ever); `*token` is untouched then. E_INVALIDARG for a null token pointer or another event;
    // HRESULT_FROM_WIN32(ERROR_INVALID_STATE) when nothing is registered (not yet, or since SuspendX).
    // XCL's public implementation returns S_FALSE on a finite timeout and writes the token on timeout only with
    // flag 2, and E_INVALIDARG when the event is not scheduled; the WAIT_TIMEOUT and ERROR_INVALID_STATE here are
    // this layer's choices and known divergences (docs/d3d12_xs_surface.md 7.1).
    HRESULT WaitFrameEventX(UINT event, DWORD timeout_ms, UINT flags, std::uint64_t* token);
    // See FramePacer::get_frame_statistics: S_FALSE and count 0 when nothing is available.
    HRESULT GetFrameStatisticsX(std::uint64_t token, UINT type_mask, UINT* count, palmetto::FrameStatistic* out);

    // The frame-token bridge (docs/present_wiring.md, "the token rule"): with the present-driven clock off, the
    // runtime's device proxy answers WaitFrameEventX from its own counter (device slots 78/79/80 belong to it), so the
    // frame tokens the title presents were never issued by this object's FramePacer, which refuses them. The device
    // proxy reports every token it hands out here before returning it to the title; a present carrying a reported token
    // is then accepted. The pacer's own checks are untouched: a token the pacer issued is checked exactly as before,
    // and a token past both counters (handed out by nobody) is still E_INVALIDARG. With the present-driven clock on
    // (PALMETTO_PRESENT_DRIVEN=1), the proxy issues no token itself: it forwards slots 78-80 to the shared pacer and
    // this bridge is not used. An externally issued token has no pacer record, so it gets no GetFrameStatisticsX entry
    // and cannot be shown mid-scan by ImmediateThresholdPercent; it shows at the next vblank. Failure mode: a missed
    // report leaves exactly the old refusal (E_INVALIDARG), so the report has to happen before the token reaches the
    // title's PresentX.
    void note_externally_issued_frame_token(std::uint64_t token);
    // Highest token the bridge has heard of (0: none). For tests and the once-only log line.
    std::uint64_t externally_issued_frame_token() const {
        return externally_issued_token_.load(std::memory_order_relaxed);
    }

    // Queue side.
    //   0 planes: a black frame (what a title does before it releases its resources); no token is looked at.
    //   1 plane:  plane 0's first resource, which must be a single-sample 2D colour texture in the PRESENT state,
    //             is blitted to the window (format conversion by the blit; an sRGB format gets an sRGB swapchain),
    //             limited to pSrcViewRects[0] when that is set. This is the whole behaviour while the plane asks for
    //             nothing else (SDR colour space, whole output, default filter).
    //   Composed: a plane with pDestPlacementBase, a point or linear ScaleFilter, or the HDR10 colour space (and any
    //             second plane that is an overlay) is composed by a compute pass (VulkanPresenter::present_planes): plane 0
    //             from its source rectangle to its destination placement, plane 1 alpha-blended over it, colour spaces
    //             converted to the swapchain's (HDR10 when the surface has it, else HDR10 is tone mapped to SDR). Needs sampled,
    //             single-sample, non-integer sources; others give DXGI_ERROR_UNSUPPORTED.
    //   2 planes: the two share one token. Plane 1 is composed over plane 0 only when xbox_plane_flag_overlay is set on it
    //             (hypothesis), unless PALMETTO_PRESENT_PLANE_RULE=measured, which applies src/plane_compose_rule.h: plane 1
    //             is composed when its descriptor has a sane placement or a non-default ScaleFilter, and the no-placement
    //             default-filter case (the unmeasured GameDVR shape) is not shown, logged once. Without the flag AND the
    //             switch the second plane is not shown (the tree's behaviour).
    //             Placement, ScaleFilter and Flags values that are not plausible are ignored and logged once, not refused.
    //             PALMETTO_PRESENT_PLANE_DUMP=1 adds the sampled plane-word dump the rule was measured from (diagnostic).
    //   More than 2: E_INVALIDARG (the UMD limit, section 4.5).
    //   ResourceCount above 1 on any plane: DXGI_ERROR_UNSUPPORTED (logged once), nothing presented; only resource 0 is read.
    // Token: the one from WaitFrameEventX. Real behaviour for a null, repeated or out-of-order token is unmeasured and a
    // title must not freeze on it, so those are presented anyway and logged once per kind; only a token that was never
    // handed out gives E_INVALIDARG (nothing presented). A token the device proxy issued (its own WaitFrameEventX counter,
    // reported through note_externally_issued_frame_token) is accepted by the frame-token bridge; see that function.
    // Skipping a token is fine. Sources the GPU cannot blit
    // (multisampled, integer, block-compressed) give DXGI_ERROR_UNSUPPORTED and present nothing; resolving a
    // multisampled plane into a temporary is not done. While suspended: HRESULT_FROM_WIN32(ERROR_INVALID_STATE), also
    // for a present that was waiting for a swapchain image when SuspendX came.
    // `params` may be null (threshold 0). ImmediateThresholdPercent: see XboxPresentParameters; a frame that was shown
    // at once has its display statistic's percent_scanned set and is counted in immediate_presents().
    // With --trace-d3d12 every call writes one `[d3d12-trace] ... XboxPresentation::PresentX <ok|failed>`
    // line (shims/d3d12_trace.h), so a run's presents can be counted and a refused one is visible.
    HRESULT PresentX(UINT plane_count, const XboxPresentPlaneParameters* planes, const XboxPresentParameters* params);
    // SuspendX drops the interval, the origin event and the frames in flight, and refuses presents. ResumeX allows
    // presents again; SetFrameIntervalX and ScheduleFrameEventX must be called again before the next wait.
    HRESULT SuspendX(UINT flags);
    HRESULT ResumeX();

    // What SDR white is in nits when SDR and HDR10 planes meet (docs section 4.5). `nits` > 0 overrides everything; 0 goes
    // back to automatic: while the swapchain is HDR10, the SDR content brightness Windows has for the output the window
    // is on (a background thread refreshes it once a second; PresentX only reads the cached value), else 203 (ITU-R
    // BT.2408's reference white). Applies to the next PresentX. The setters may be called from any thread.
    void set_sdr_white_nits(float nits) { sdr_white_override_ = nits > 0.0f ? nits : 0.0f; }
    // How SDR planes are decoded and an SDR output is encoded: piecewise sRGB (default) or gamma 2.2. The environment
    // variable PALMETTO_SDR_EOTF=gamma22 (or 2.2; any case; "srgb" is the default; other values are logged once and
    // ignored) chooses when the object is made.
    void set_sdr_eotf(palmetto::PresentPlanes::SdrEotf eotf) { sdr_eotf_ = eotf; }
    // Peak of HDR10 content in nits, the white point of the tone map onto an SDR output (default 1000; no metadata is read).
    void set_hdr_peak_nits(float nits) { hdr_peak_nits_ = nits > 0.0f ? nits : 1000.0f; }

    // Test hook for the measured second-plane rule (docs/peer_port_movie.md, item 6): the runtime reads
    // PALMETTO_PRESENT_PLANE_RULE once per process; the pixel test compares the flagged gate and the measured rule on one
    // GPU by setting this per object. -1 (constructed) follows the environment, 0 keeps the invented flag gate, 1 applies
    // src/plane_compose_rule.h.
    void set_plane_rule_measured(bool on) { plane_rule_override_.store(on ? 1 : 0, std::memory_order_relaxed); }

    palmetto::FramePacer& pacer() { return pacer_; }
    WindowPresenter& presenter() { return *presenter_; }
    std::uint64_t presents() const { return presents_; }
    // Frames shown mid-scan because of ImmediateThresholdPercent.
    std::uint64_t immediate_presents() const { return pacer_.immediate_presents(); }

private:
    // PresentX without its `[d3d12-trace]` line.
    HRESULT present_x(UINT plane_count, const XboxPresentPlaneParameters* planes, const XboxPresentParameters* params);
    float effective_sdr_white_nits();  // override, else the cached Windows setting while HDR10, else 203
    void display_white_worker();       // refreshes display_white_ off the present path
    std::unique_ptr<WindowPresenter> presenter_;
    // Only when the caller supplied a clock (tests): the pacer is this object's and uses that clock. In the runtime the
    // pacer is the process's shared device frame clock, so the device frame-event slots and the present commit share one.
    std::unique_ptr<palmetto::FramePacer> own_pacer_;
    palmetto::FramePacer& pacer_;
    std::mutex present_mutex_;  // PresentX, SuspendX and ResumeX one at a time
    std::atomic<std::uint64_t> presents_{0};
    std::atomic<std::uint64_t> externally_issued_token_{0};  // frame-token bridge watermark (highest reported token)
    bool logged_planes_ = false, logged_threshold_ = false, logged_display_ = false, logged_color_space_ = false,
         logged_extras_ = false, logged_resource_count_ = false;
    // The measured second-plane rule's once-per-kind lines (docs/peer_port_movie.md, item 6).
    bool logged_plane_rule_compose_ = false, logged_plane_rule_unmeasured_ = false;
    // -1: follow PALMETTO_PRESENT_PLANE_RULE; 0/1: the test hook set_plane_rule_measured.
    std::atomic<int> plane_rule_override_{-1};
    std::atomic<float> sdr_white_override_{0.0f};  // > 0: set_sdr_white_nits
    std::atomic<palmetto::PresentPlanes::SdrEotf> sdr_eotf_{palmetto::PresentPlanes::SdrEotf::srgb};
    std::atomic<float> hdr_peak_nits_{1000.0f};
    // The last good Windows SDR white level (nits, 0: none yet), refreshed by display_white_thread_ about once a second while
    // an HDR10 swapchain is in use (white_wanted_); a failed query keeps the previous value.
    std::atomic<float> display_white_{0.0f};
    std::atomic<bool> white_wanted_{false};
    std::mutex worker_mutex_;
    std::condition_variable worker_wake_;
    bool worker_stop_ = false;
    std::thread display_white_thread_;
    bool logged_null_token_ = false, logged_stale_token_ = false, logged_repeated_token_ = false,
         logged_bridged_token_ = false, logged_per_origin_token_ = false, logged_per_origin_overflow_ = false;
};
#endif  // PALMETTO_XBOX_PRESENT_HOOK_ONLY

inline constexpr UINT xbox_frame_event_origin = palmetto::frame_event_origin;

// ---- The window of a swapchain-less title (docs/window.md) ---------------------------------------------------------
// PresentX presents without a DXGI swapchain, so the runtime has to present into a window: the title calls no
// CreateSwapChain* and no EnumOutputs (docs/display_path.md 4). A swapchain-less title usually makes its own top-level
// window long before its first PresentX (the measured title: one visible window of class "sgaWindowClass" at about 4 s,
// while the first present is at about 34 s), so the right design is to present into that window and create none. The
// measured boot used to have two visible top-level windows because the service always made one of its own:
// class "PalmettoPresentX", title "Palmetto - PresentX", client area 1280x720 by default (PALMETTO_PRESENT_WIDTH /
// PALMETTO_PRESENT_HEIGHT override, 320..7680 each), created hidden, with PALMETTO_PRESENT_WINDOW=auto showing it from
// the first frame that is actually presented. That own window is now the fallback: create_or_adopt_xbox_present_window()
// below adopts the title's window when this process has one the adoption rule accepts.
// The window lives for the process (the OS reclaims it at exit; a static destructor here would race the Vulkan device's
// own teardown) and the swapchain is created against it by the registered window presenter factory, which sizes it to the
// window's client area and recreates it when that size changes.
struct XboxPresentWindow {
    HWND window = nullptr;
    UINT width = 0;                     // client area
    UINT height = 0;
    bool shown = false;                 // visible now
    bool show_on_first_frame = false;   // auto mode: the service shows it from the first presented frame
    // The title's own window (docs/window.md), not created here: its title text, size and visibility are the title's, so
    // show_xbox_present_window does not show it and destroy_xbox_present_window does not destroy it.
    bool adopted = false;
};
// Logs the choice once. `window` is null when the class or the window could not be created (error logged).
XboxPresentWindow create_xbox_present_window();
// Shows it (SW_SHOWNOACTIVATE, pumped by nothing in particular: the swapchain does not need a message loop). Logged.
// A no-op for an adopted window: the title owns its window's visibility.
void show_xbox_present_window(XboxPresentWindow& window);
// Destroys a window created here; an adopted window is left alone (the title may still be using it) and only forgotten.
void destroy_xbox_present_window(XboxPresentWindow& window);

// ---- Adopting the title's own window (docs/window.md) -------------------------------------------------------------
// The pieces of the adoption rule, in the order they run: enumerate this process's top-level windows, choose one by the
// rule, and only create our own window when there is none. The rule is pure and unit-tested (port/test/
// xbox_present_test.cpp); only the enumeration and the creation touch Win32.
struct XboxPresentWindowCandidate {  // one top-level window of this process, as the rule sees it
    HWND window = nullptr;
    wchar_t class_name[64] = {};  // GetClassNameW, truncated
    bool visible = false;         // IsWindowVisible
    bool minimised = false;       // IsIconic
    bool owned = false;           // has an owner window (GW_OWNER): a dialog or a tool/helper of the title
    bool tool_window = false;     // WS_EX_TOOLWINDOW
    bool ime = false;             // an Input Method Editor window: "IME" in its class name (MSCTFIME UI and the like)
    UINT client_width = 0;        // GetClientRect
    UINT client_height = 0;
};
// The class the measured title's window has. The rule prefers it by name; the name is an observation of a window the
// title creates through host user32 inside this process, not something our tree creates.
inline constexpr wchar_t xbox_title_window_class[] = L"sgaWindowClass";
// How many of this process's top-level windows are collected; a process with more (none measured) keeps the first.
inline constexpr std::size_t xbox_window_candidate_capacity = 64;

// Fills `out` with the current process's top-level windows (EnumWindows plus the process check); returns how many were
// written. `truncated` (may be null) is set when there were more candidates than `capacity`.
std::size_t collect_xbox_present_window_candidates(XboxPresentWindowCandidate* out, std::size_t capacity,
                                                   bool* truncated = nullptr);
// The adoption rule, no Win32 calls: the best candidate to present into, or null when there is none. A window is usable
// when it is visible, not minimised, not owned, not a tool window, not an IME window, not this module's own class, and
// has a non-empty client area. A usable window of the title's class wins outright (the first in enumeration order);
// otherwise the usable one with the largest client area wins. `own_window` (the one created here, when there is one) is
// never chosen. PALMETTO_WINDOW_FIT=workarea (off by default) may request a non-activating fit for the measured title
// class only; unknown styles, geometry or switch spellings are refused by name. No process DPI awareness is changed.
const XboxPresentWindowCandidate* choose_xbox_present_window(const XboxPresentWindowCandidate* candidates,
                                                             std::size_t count, HWND own_window = nullptr);
// The rule over a fresh enumeration of this process's windows: true and `out` filled when there is one to adopt.
bool find_xbox_present_title_window(XboxPresentWindowCandidate* out);
// What the presentation service calls instead of create_xbox_present_window(): the title's own window when the rule finds
// one, else create_xbox_present_window(), unchanged. PALMETTO_PRESENT_ADOPT=off (or 0) keeps today's own window on
// purpose (an A/B switch and what the tests that present through a known window set); unset or any other value adopts.
// Logs the choice once: the class name and client size, never the title text.
XboxPresentWindow create_or_adopt_xbox_present_window();

// ---- The queue proxy's PresentX path: a registration hook (docs/present_wiring.md) --------------------------------
// d3d12_xs_device.cpp's queue proxy is compiled into GPU-free test binaries (xs_proxy_mock_test, device_proxy_test)
// that do not link the Vulkan layer, so it must not call this module directly: it goes through a palmetto_register_*
// hook, the pattern of palmetto_register_console_heap in d3d12_xs_device.cpp. The Vulkan backend registers once, after
// its PC device exists; from then on the queue proxy's console PresentX and the device proxy's frame tokens reach the
// registered presentation service through the function pointers below. With nothing registered every call refuses
// honestly with HRESULT_FROM_WIN32(ERROR_INVALID_STATE) (and the token report is a no-op), so a GPU-free build keeps
// its current answer. This part of the header is header-inline and pulls in neither Vulkan nor d3d12_com.h, which is
// what lets those binaries include it without linking xbox_present.cpp.

// Console PresentX (queue slot 32): (queue, plane count, the title's plane array, present params). `queue` is the
// object the console call was made on; it is passed for the trace only, there is one window. S_OK when the frame was
// presented; E_INVALIDARG for a null plane resource and every other refusal XboxPresentation::PresentX documents.
using XboxPresentToQueueCall = HRESULT(WINAPI*)(void* queue, std::uint32_t plane_count,
                                                const XboxPresentPlaneParameters* planes,
                                                const XboxPresentParameters* params);
// Device slot 80 (WaitFrameEventX) handed out `token`: the device proxy reports it here before the title can present it.
// This is the other half of the frame-token bridge (XboxPresentation::note_externally_issued_frame_token). With the
// present-driven clock on the device proxy issues no token and reports nothing: the frame events are the pacer's.
using XboxPresentTokenIssuedCall = void(WINAPI*)(std::uint64_t token);

// ---- The device frame events, owned by the pacer (the present-driven clock) --------------------------------------
// Device slots 78 (SetFrameIntervalX), 79 (ScheduleFrameEventX) and 80 (WaitFrameEventX) are the frame handshake. With
// PALMETTO_PRESENT_DRIVEN on, xbox_present.cpp answers them from one process-lifetime FramePacer (the same pacer the
// presenting XboxPresentation commits its presents into), so the pacer is the token source and a present marks its
// frame token flipped (frame_pacer.h, "Present-driven"). The device proxy calls these through the registered record;
// `enabled` false means the proxy keeps its own immediate-token answer, byte for byte the behaviour without the
// switch. The wait may block (that is the point: a held wait is released at the present that flips a frame); the
// proxy falls back to its own token on any failure, so a failure here cannot wedge a boot. The separate, default-off
// PALMETTO_PRESENT_PER_ORIGIN switch is accepted only with PALMETTO_PRESENT_DRIVEN=1; it permits independent tokens on
// the at-most-two PresentX planes and records each token's last plane without changing the legacy path when unset.
using XboxFrameEventEnabledCall = BOOL(WINAPI*)();
using XboxFrameEventSetIntervalCall = HRESULT(WINAPI*)(std::uint32_t interval_us, std::uint32_t latency,
                                                      std::uint32_t flags);
using XboxFrameEventScheduleCall = HRESULT(WINAPI*)(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags);
using XboxFrameEventWaitCall = HRESULT(WINAPI*)(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                                std::uint64_t* token);
// Event 2 and the later frame points: the blocking flip wait (the title's measured event-2 wait, INFINITE). No token
// is issued; the wait is released by the present that flips an in-flight frame.
using XboxFrameEventWaitFlipCall = HRESULT(WINAPI*)(std::uint32_t timeout_ms);
// The last token the origin wait handed out (0: none). The origin poll's flag 2 writes it when no new origin is due.
using XboxFrameEventLastTokenCall = std::uint64_t(WINAPI*)();

struct XboxFrameEventBackend {
    XboxFrameEventEnabledCall enabled = nullptr;  // non-zero: the calls below answer the frame events
    XboxFrameEventSetIntervalCall set_frame_interval = nullptr;
    XboxFrameEventScheduleCall schedule_frame_event = nullptr;
    XboxFrameEventWaitCall wait_frame_event = nullptr;
    XboxFrameEventWaitFlipCall wait_flip = nullptr;
    XboxFrameEventLastTokenCall last_token = nullptr;
};

// What the Vulkan backend registers: its window presenter factory over the PC device, plus the presentation core's
// callbacks, which create_xbox_present_backend fills in (a backend does not set them by hand).
struct XboxPresentBackend {
    std::uint32_t size = 0;     // sizeof(XboxPresentBackend)
    std::uint32_t version = 0;  // xbox_present_backend_version
    // The PC Vulkan device to present through (the one behind the console device proxy). Borrowed: it must outlive the
    // registration. create_window_presenter resolves the console proxy form of it as well.
    ID3D12Device* device = nullptr;
    // The backend's factory (create_window_presenter in d3d12_vulkan_device.cpp, adapted to a raw pointer): a window
    // presenter over `device` and `window`, or null when that device cannot present. Ownership of the result goes to
    // the presentation service. This is the "window presenter factory" of the registration.
    WindowPresenter* (*create_window_presenter)(ID3D12Device* device, HWND window) = nullptr;
    // Filled in by create_xbox_present_backend: the present-to-queue-call, the token bridge's report, and the
    // present-driven frame events.
    XboxPresentToQueueCall present = nullptr;
    XboxPresentTokenIssuedCall note_issued_token = nullptr;
    const XboxFrameEventBackend* frame_events = nullptr;
};
inline constexpr std::uint32_t xbox_present_backend_version = 2;

// Builds the registration record for `device` (the PC Vulkan device created by create_vulkan_device*), with the
// presentation core's callbacks filled in; null for a null device. The record is this module's process-lifetime
// storage, so the backend only passes it on:
//     const auto* backend = create_xbox_present_backend(device);
//     if (backend) palmetto_register_xbox_present(const_cast<XboxPresentBackend*>(backend));
const XboxPresentBackend* create_xbox_present_backend(ID3D12Device* device);

// The queue proxy's side, header-inline on purpose (see above). One instance per process: an inline variable, so the
// proxy's translation unit and xbox_present.cpp share it without either pushing a symbol into the other.
namespace xbox_present_hook {
inline const XboxPresentBackend* registered = nullptr;
}  // namespace xbox_present_hook

inline bool xbox_present_registered() {
    return xbox_present_hook::registered != nullptr && xbox_present_hook::registered->present != nullptr;
}
// The queue proxy's console PresentX call. With nothing registered: HRESULT_FROM_WIN32(ERROR_INVALID_STATE), nothing
// presented, nothing logged (a boot before the backend registers must not spam the log).
inline HRESULT xbox_present_queue_present(void* queue, std::uint32_t plane_count,
                                         const XboxPresentPlaneParameters* planes,
                                         const XboxPresentParameters* params) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    if (!backend || !backend->present) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return backend->present(queue, plane_count, planes, params);
}
// The device proxy's token report (device slot 80), the token bridge's report side. A no-op with nothing registered.
inline void xbox_present_note_issued_token(std::uint64_t token) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    if (backend && backend->note_issued_token) backend->note_issued_token(token);
}
// The present-driven frame events (device slots 78-80; see XboxFrameEventBackend). `enabled` is false with nothing
// registered, with a record that has no frame events, or with PALMETTO_PRESENT_DRIVEN off: the proxy then answers the
// slots itself exactly as before.
inline bool xbox_frame_events_enabled() {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    return backend && backend->frame_events && backend->frame_events->enabled &&
           backend->frame_events->enabled() != FALSE;
}
inline HRESULT xbox_frame_event_set_interval(std::uint32_t interval_us, std::uint32_t latency, std::uint32_t flags) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    const XboxFrameEventBackend* events = backend ? backend->frame_events : nullptr;
    if (!events || !events->set_frame_interval) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return events->set_frame_interval(interval_us, latency, flags);
}
inline HRESULT xbox_frame_event_schedule(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    const XboxFrameEventBackend* events = backend ? backend->frame_events : nullptr;
    if (!events || !events->schedule_frame_event) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return events->schedule_frame_event(event, offset_us, flags);
}
inline HRESULT xbox_frame_event_wait(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                     std::uint64_t* token) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    const XboxFrameEventBackend* events = backend ? backend->frame_events : nullptr;
    if (!events || !events->wait_frame_event) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return events->wait_frame_event(event, timeout_ms, flags, token);
}
inline HRESULT xbox_frame_event_wait_flip(std::uint32_t timeout_ms) {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    const XboxFrameEventBackend* events = backend ? backend->frame_events : nullptr;
    if (!events || !events->wait_flip) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    return events->wait_flip(timeout_ms);
}
inline std::uint64_t xbox_frame_event_last_token() {
    const XboxPresentBackend* backend = xbox_present_hook::registered;
    const XboxFrameEventBackend* events = backend ? backend->frame_events : nullptr;
    return events && events->last_token ? events->last_token() : 0;
}

}  // namespace rt::d3d12

// The registration hook, defined in xbox_present.cpp (C linkage, like palmetto_register_console_heap: a backend can
// name it with a local extern declaration and never include this header). S_OK; E_INVALIDARG for a null record, one
// shorter than this version of XboxPresentBackend, a version mismatch, or a record that cannot present (no device,
// factory or callbacks). Registering again replaces the record (a recreated device); both outcomes are logged.
// unregister empties the slot (shutdown and tests): the presentation service stays allocated, as its window and
// swapchain are process-lifetime by design.
extern "C" HRESULT palmetto_register_xbox_present(void* backend);
extern "C" void palmetto_unregister_xbox_present();
