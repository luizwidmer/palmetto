#include "xbox_present.h"

#include "d3d12_com.h"  // WindowPresenter, create_window_presenter: the registered window presenter factory
#include "d3d12_trace.h"

#include "../src/vulkan_present.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt::d3d12 {

namespace {

using palmetto::FramePacer;
namespace hr = palmetto::pacer_hr;

HRESULT from_pacer(std::int32_t value) { return HRESULT(value); }

// The fixed text of a log line is printed once per object.
void log_once(bool& done, const char* text) {
    if (done) return;
    done = true;
    std::fprintf(stderr, "[rt] present: %s\n", text);
}

}  // namespace

XboxPresentation::XboxPresentation(std::unique_ptr<WindowPresenter> presenter, palmetto::FrameClock* clock)
    : presenter_(std::move(presenter)),
      own_clock_(clock ? nullptr : std::make_unique<palmetto::SteadyFrameClock>()),
      pacer_(clock ? *clock : *own_clock_) {
    // A present waiting for a swapchain image gives up once SuspendX has been called.
    presenter_->set_abort([this] { return pacer_.suspended(); });
    // PALMETTO_SDR_EOTF=gamma22 (or 2.2, any case) decodes SDR with a pure power of 2.2 instead of piecewise sRGB; "srgb" or
    // unset keeps the default; anything else (also a value too long for the buffer, ERANGE) is logged once and ignored.
    char eotf[32] = {};
    std::size_t eotf_length = 0;
    const errno_t env = getenv_s(&eotf_length, eotf, "PALMETTO_SDR_EOTF");
    if (env == 0 && eotf_length > 1) {
        if (!_stricmp(eotf, "gamma22") || !_stricmp(eotf, "2.2")) sdr_eotf_ = palmetto::PresentPlanes::SdrEotf::gamma22;
        else if (_stricmp(eotf, "srgb") != 0)
            std::fprintf(stderr, "[rt] present: PALMETTO_SDR_EOTF=%s is not srgb, gamma22 or 2.2: piecewise sRGB used\n", eotf);
    } else if (env == ERANGE) {
        std::fprintf(stderr, "[rt] present: PALMETTO_SDR_EOTF is too long to be srgb, gamma22 or 2.2: piecewise sRGB used\n");
    }
    // PALMETTO_PRESENT_UNCAP (any non-empty value other than "0") takes the frame-rate cap off: a WaitFrameEventX
    // hands a token out at once instead of at the next interval origin, and presents go unsynced (MAILBOX or
    // IMMEDIATE, not FIFO), so the frame rate is whatever the title renders. Registration and suspension still
    // apply, so the title keeps its call sequence.
    {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "PALMETTO_PRESENT_UNCAP") == 0 && value) {
            if (value[0] != '\0' && std::strcmp(value, "0") != 0) {
                pacer_.set_uncapped(true);
                std::fprintf(stderr, "[rt] present: PALMETTO_PRESENT_UNCAP is set: frame events return at once and "
                                     "presents do not wait for vblank\n");
            }
            std::free(value);
        }
    }
    display_white_thread_ = std::thread([this] { display_white_worker(); });
}

XboxPresentation::~XboxPresentation() {
    {
        std::lock_guard lock(worker_mutex_);
        worker_stop_ = true;
    }
    worker_wake_.notify_all();
    display_white_thread_.join();
    presenter_->set_abort(nullptr);
}

// Windows' SDR white level costs a QueryDisplayConfig and two DisplayConfigGetDeviceInfo calls (measured in
// xbox_present_test, docs section 4.5): too much for the present path, which would hitch once a second and feed the pacer.
// So it is read here, about once a second while an HDR10 swapchain is in use, and PresentX reads the cached value.
void XboxPresentation::display_white_worker() {
    std::unique_lock lock(worker_mutex_);
    while (!worker_stop_) {
        if (white_wanted_.load()) {
            lock.unlock();
            const float nits = presenter_->display_sdr_white_nits();
            if (nits > 0.0f) display_white_ = nits;  // a failed query keeps the last good value
            lock.lock();
        }
        worker_wake_.wait_for(lock, std::chrono::seconds(1), [this] { return worker_stop_; });
    }
}

float XboxPresentation::effective_sdr_white_nits() {
    if (sdr_white_override_ > 0.0f) return sdr_white_override_;
    if (!presenter_->hdr_active()) return 203.0f;  // the Windows setting belongs to HDR mode
    if (!white_wanted_.exchange(true)) {  // first HDR10 present: have it read now
        std::lock_guard lock(worker_mutex_);
        worker_wake_.notify_all();
    }
    const float cached = display_white_;
    return cached > 0.0f ? cached : 203.0f;
}

HRESULT XboxPresentation::SetFrameIntervalX(IUnknown*, UINT interval_period_us, UINT latency, UINT flags) {
    return from_pacer(pacer_.set_frame_interval(interval_period_us, latency, flags));
}

HRESULT XboxPresentation::ScheduleFrameEventX(UINT event, UINT offset_us, UINT flags) {
    return from_pacer(pacer_.schedule_frame_event(event, offset_us, flags));
}

HRESULT XboxPresentation::WaitFrameEventX(UINT event, DWORD timeout_ms, UINT flags, std::uint64_t* token) {
    return from_pacer(pacer_.wait_frame_event(event, timeout_ms, flags, token));
}

HRESULT XboxPresentation::GetFrameStatisticsX(std::uint64_t token, UINT type_mask, UINT* count,
                                              palmetto::FrameStatistic* out) {
    return from_pacer(pacer_.get_frame_statistics(token, type_mask, count, out));
}

// The frame-token bridge's report side. Monotonic maximum: the device proxy's counter only goes up (a token below the
// watermark is one that was reported before), and a present is accepted while its token is at or below the highest
// reported one. Called before the token is returned to the title, from the thread that will present it (the counter is
// atomic, so the order holds within that thread; a cross-thread report that lands late leaves the old refusal).
void XboxPresentation::note_externally_issued_frame_token(std::uint64_t token) {
    std::uint64_t reported = externally_issued_token_.load(std::memory_order_relaxed);
    while (reported < token &&
           !externally_issued_token_.compare_exchange_weak(reported, token, std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
    }
}

HRESULT XboxPresentation::PresentX(UINT plane_count, const XboxPresentPlaneParameters* planes,
                                   const XboxPresentParameters* params) {
    // One trace line per present, whatever path the call takes out of present_x: the queue proxy's PresentX slot is
    // past the PC chain, so nothing else records it, and a frame loop's presents could not be counted from a trace.
    return d3d12_trace::result("XboxPresentation::PresentX", present_x(plane_count, planes, params));
}

HRESULT XboxPresentation::present_x(UINT plane_count, const XboxPresentPlaneParameters* planes,
                                    const XboxPresentParameters* params) {
    std::lock_guard serialise(present_mutex_);
    if (plane_count > 2 || (plane_count && !planes)) return E_INVALIDARG;
    if (pacer_.suspended()) return from_pacer(hr::invalid_state);
    // A zero, negative or NaN threshold never tears; an uncapped frame is never late for anything either.
    const float threshold = params ? params->ImmediateThresholdPercent : 0.0f;
    const bool wants_tear = threshold > 0.0f && !pacer_.uncapped();
    if (wants_tear && !presenter_->tear_supported())
        log_once(logged_threshold_,
                 "ImmediateThresholdPercent has no effect on this swapchain (tearing disabled by PALMETTO_PRESENT_NO_TEAR, "
                 "no VK_KHR/EXT_swapchain_maintenance1, or no IMMEDIATE/FIFO_RELAXED mode the surface can switch to): "
                 "a late frame waits for the next vblank");

    // Unpaced (no interval registered, or uncapped): do not hold the queue for vblanks.
    const UINT sync_interval = pacer_.registered() && !pacer_.uncapped() ? 1 : 0;

    if (plane_count == 0) {
        const HRESULT result = presenter_->present_blank(sync_interval);
        if (SUCCEEDED(result)) {
            pacer_.note_untokened_present();
            ++presents_;
        }
        return result;
    }

    // All planes carry the token of one frame.
    const std::uint64_t token = planes[0].Token;
    for (UINT i = 0; i < plane_count; ++i) {
        const XboxPresentPlaneParameters& plane = planes[i];
        if (plane.Token != token || plane.ResourceCount == 0 || !plane.ppResources || !plane.ppResources[0])
            return E_INVALIDARG;
        // Both paths read ppResources[0] only: a plane with several resources would silently lose the others and could show
        // the wrong frame, so it is refused (before anything is presented) until it is modelled. What the console does
        // with ResourceCount above 1 (views? stereo?) is unmeasured.
        if (plane.ResourceCount != 1) {
            log_once(logged_resource_count_, "PresentX plane with ResourceCount other than 1: not implemented, refused");
            return DXGI_ERROR_UNSUPPORTED;
        }
    }
    // Only a token that was never handed out is refused. Null, stale and repeated tokens are presented anyway (what the
    // console does with them is unmeasured, and a title that does it must not freeze); each kind is logged once.
    FramePacer::TokenIssue issue{};
    std::int32_t checked = pacer_.check_present_token(token, &issue);
    bool bridged = false;
    if (checked == hr::invalid_arg) {
        // The frame-token bridge (docs/present_wiring.md, the token rule). The runtime's device proxy answers
        // WaitFrameEventX from its own counter (device slots 78/79/80 are not this class's yet), so the pacer below never
        // issued the tokens the title presents and would refuse every frame. The device proxy reports each token it hands
        // out through note_externally_issued_frame_token, and a reported token is accepted here. The override is only for
        // the pacer's "never handed out" answer: tokens the pacer issued itself take the normal path unchanged, a report
        // never makes an issued token stale/repeated (or the reverse), and a token past both counters was handed out by
        // nobody and is still E_INVALIDARG. An externally issued token has no pacer Record, so present_timing below finds
        // nothing for it (no ImmediateThresholdPercent tear) and GetFrameStatisticsX has no entry for it: the frame is
        // still committed as a frame to show, at the next vblank. A missed report therefore leaves exactly the old
        // refusal - the report has to happen before the token reaches the title's PresentX.
        if (token <= externally_issued_frame_token()) {
            checked = hr::ok;
            bridged = true;
        }
    }
    if (checked != hr::ok) return from_pacer(checked);
    if (bridged)
        log_once(logged_bridged_token_,
                 "PresentX with a frame token issued by the device proxy, not by the pacer (frame-token bridge): "
                 "presented; no statistics entry and no tear for such a token");
    switch (issue) {
    case FramePacer::TokenIssue::null_token:
        log_once(logged_null_token_, "PresentX with the null token: presented anyway (real behaviour unmeasured)");
        break;
    case FramePacer::TokenIssue::stale:
        log_once(logged_stale_token_, "PresentX with a token older than the last presented: presented anyway");
        break;
    case FramePacer::TokenIssue::repeated:
        log_once(logged_repeated_token_, "PresentX with the token of the last present again: presented anyway");
        break;
    case FramePacer::TokenIssue::none:
        break;
    }

    // What each plane asks for. Colour spaces other than HDR10 are composed as the SDR default (logged once).
    const auto is_hdr10 = [&](const XboxPresentPlaneParameters& p) {
        if (p.ColorSpace != xbox_color_space_g22_p709 && p.ColorSpace != xbox_color_space_g2084_p2020)
            log_once(logged_color_space_, "a present plane's DXGI colour space is neither G22/P709 nor G2084/P2020: composed as SDR G22/P709");
        return p.ColorSpace == xbox_color_space_g2084_p2020;
    };
    // The extra fields are unmeasured, so they are read defensively: a placement that is not a sane sub-rectangle of the output
    // (also NaN) counts as none, an unknown filter as the default, unknown flag bits are ignored, extended descriptors are
    // not read; each is logged once and the frame is shown anyway (a title must not freeze on a field we misread).
    const auto placement = [&](const XboxPresentPlaneParameters& p) -> const XboxViewRect* {
        const XboxViewRect* r = p.pDestPlacementBase;
        if (!r) return nullptr;
        if (!(r->left >= 0.0f && r->left < r->right && r->right <= 1.0f && r->top >= 0.0f && r->top < r->bottom &&
              r->bottom <= 1.0f)) {
            log_once(logged_extras_, "a present plane's pDestPlacementBase is not a sub-rectangle of 0..1: ignored (whole output)");
            return nullptr;
        }
        return (r->left == 0.0f && r->top == 0.0f && r->right == 1.0f && r->bottom == 1.0f) ? nullptr : r;
    };
    const auto filter_of = [&](const XboxPresentPlaneParameters& p) {
        if (p.ScaleFilter == xbox_scale_filter_point || p.ScaleFilter == xbox_scale_filter_linear) return p.ScaleFilter;
        if (p.ScaleFilter != xbox_scale_filter_default)
            log_once(logged_extras_, "a present plane's ScaleFilter is not a known value: default filter");
        return int(xbox_scale_filter_default);
    };
    for (UINT i = 0; i < plane_count; ++i)
        if (planes[i].ExtendedDescCount)
            log_once(logged_extras_, "a present plane has extended descriptors: ignored");
    const bool hdr0 = is_hdr10(planes[0]);
    // Plane 1 is composed over plane 0 only when flagged an overlay (hypothesis). Otherwise it is not shown, as before: in
    // the public HDR sample it is the GameDVR copy of the frame, and blending a plane we cannot tell apart from it risks
    // drawing the frame twice with an alpha that was never meant to be blended.
    const bool overlay = plane_count == 2 && (planes[1].Flags & xbox_plane_flag_overlay) != 0;
    if (plane_count == 2 && !overlay)
        log_once(logged_planes_, "PresentX with 2 planes and no overlay flag on plane 1: only plane 0 is shown (plane 1 is taken "
                                 "as the GameDVR copy of the frame)");
    const bool compose = hdr0 || placement(planes[0]) || filter_of(planes[0]) != xbox_scale_filter_default || overlay;

    if (compose) {
        // Every shown plane: a 2D texture, a source rectangle inside it, a placement inside the output.
        palmetto::PresentPlanes composed;
        composed.count = overlay ? 2 : 1;
        composed.sdr_white_nits = effective_sdr_white_nits();
        composed.sdr_eotf = sdr_eotf_;
        composed.hdr_peak_nits = hdr_peak_nits_;
        for (UINT i = 0; i < composed.count; ++i) {
            const XboxPresentPlaneParameters& p = planes[i];
            ID3D12Resource* source = p.ppResources[0];
            const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
            if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return E_INVALIDARG;
            const D3D12_RECT* source_rect = p.pSrcViewRects;
            if (source_rect && (source_rect->left < 0 || source_rect->top < 0 || source_rect->left >= source_rect->right ||
                                source_rect->top >= source_rect->bottom || UINT64(source_rect->right) > source_desc.Width ||
                                UINT(source_rect->bottom) > source_desc.Height))
                return E_INVALIDARG;
            const XboxViewRect* place = placement(p);
            palmetto::PresentPlane& out = composed.plane[i];
            out.source = resource_id(source);
            if (source_rect) {
                out.source_rect = {source_rect->left, source_rect->top, source_rect->right, source_rect->bottom};
                out.has_source_rect = true;
            }
            if (place) {
                out.dest[0] = place->left;
                out.dest[1] = place->top;
                out.dest[2] = place->right;
                out.dest[3] = place->bottom;
            }
            out.point_filter = filter_of(p) == xbox_scale_filter_point;
            out.hdr10 = is_hdr10(p);
            out.straight_alpha = (p.Flags & xbox_plane_flag_straight_alpha) != 0;
        }
        bool tear = false;
        if (wants_tear && sync_interval > 0 && presenter_->tear_supported()) {
            const FramePacer::PresentTiming timing = pacer_.present_timing(token);
            tear = palmetto::decide_present_sync(timing.late_us, timing.period_us, threshold) == palmetto::PresentSync::immediate;
        }
        const HRESULT result = presenter_->present_planes(composed, sync_interval, tear);
        if (FAILED(result)) return pacer_.suspended() ? from_pacer(hr::invalid_state) : result;
        pacer_.commit_present_token(token, plane_count,
                                    tear && presenter_->last_present_tore() ? palmetto::PresentSync::immediate
                                                                            : palmetto::PresentSync::vblank);
        ++presents_;
        return S_OK;
    }

    const XboxPresentPlaneParameters& plane = planes[0];
    ID3D12Resource* resource = plane.ppResources[0];
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
    if (SUCCEEDED(resource->GetHeapProperties(&heap, &heap_flags)) && !(heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY))
        log_once(logged_display_, "a present plane's resource was created without D3D12_HEAP_FLAG_ALLOW_DISPLAY; presenting it anyway");

    const D3D12_RECT* rect = plane.pSrcViewRects;
    if (rect && (rect->left < 0 || rect->top < 0 || rect->left >= rect->right || rect->top >= rect->bottom ||
                 UINT64(rect->right) > desc.Width || UINT(rect->bottom) > desc.Height))
        return E_INVALIDARG;

    // How late the frame is, now that everything that can refuse it has: past the vblank it was due at, by what part of
    // the interval the scan has moved on. The presenter says whether it really queued it as a tear.
    FramePacer::PresentTiming timing;
    bool tear = false;
    if (wants_tear && sync_interval > 0 && presenter_->tear_supported()) {
        timing = pacer_.present_timing(token);
        tear = palmetto::decide_present_sync(timing.late_us, timing.period_us, threshold) == palmetto::PresentSync::immediate;
    }
    const HRESULT result = presenter_->present(resource, sync_interval, rect, tear);
    if (FAILED(result)) return pacer_.suspended() ? from_pacer(hr::invalid_state) : result;
    pacer_.commit_present_token(token, plane_count,
                                tear && presenter_->last_present_tore() ? palmetto::PresentSync::immediate
                                                                        : palmetto::PresentSync::vblank);
    ++presents_;
    return S_OK;
}

// SuspendX and ResumeX do not take present_mutex_: a PresentX stuck waiting for a swapchain image must not hold them
// (it polls the suspended state and gives up). A present that was already past that point finishes normally.
HRESULT XboxPresentation::SuspendX(UINT) {
    pacer_.suspend();
    return S_OK;
}

HRESULT XboxPresentation::ResumeX() {
    pacer_.resume();
    return S_OK;
}

// ---- The window of a swapchain-less title, the presentation service and the registration hook ----------------------
// docs/present_wiring.md is the contract page; the comments here are the short form of it.

namespace {

constexpr wchar_t k_present_window_class[] = L"PalmettoPresentX";
constexpr wchar_t k_present_window_title[] = L"the target game Xbox runtime - PresentX";
constexpr UINT k_default_present_width = 1280;
constexpr UINT k_default_present_height = 720;

// The default window procedure: PresentX does not need a message loop (the swapchain is driven by the present calls),
// so a window that is never pumped is fine; it still gets a real client area and a real Win32 surface.
LRESULT CALLBACK present_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

// PALMETTO_PRESENT_WIDTH / _HEIGHT: a decimal client size. Anything that is not a plausible one (320..7680) keeps the
// default and says so once.
UINT env_client_size(const char* name, UINT fallback) {
    char value[16] = {};
    std::size_t length = 0;
    if (getenv_s(&length, value, name) != 0 || length <= 1) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || end == value || *end != '\0' || parsed < 320 || parsed > 7680) {
        std::fprintf(stderr, "[rt] present: %s=%s is not a client size between 320 and 7680: %u used\n", name, value,
                     fallback);
        return fallback;
    }
    return static_cast<UINT>(parsed);
}

enum class PresentWindowVisibility { auto_show, shown, hidden };

// PALMETTO_PRESENT_WINDOW=auto (default) / shown / hidden. Anything else is auto and logged once.
PresentWindowVisibility env_visibility() {
    char value[16] = {};
    std::size_t length = 0;
    if (getenv_s(&length, value, "PALMETTO_PRESENT_WINDOW") != 0 || length <= 1)
        return PresentWindowVisibility::auto_show;
    if (!_stricmp(value, "auto")) return PresentWindowVisibility::auto_show;
    if (!_stricmp(value, "shown")) return PresentWindowVisibility::shown;
    if (!_stricmp(value, "hidden")) return PresentWindowVisibility::hidden;
    std::fprintf(stderr, "[rt] present: PALMETTO_PRESENT_WINDOW=%s is not shown, hidden or auto: auto used\n", value);
    return PresentWindowVisibility::auto_show;
}

bool register_present_window_class() {
    static const bool registered = [] {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &present_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = k_present_window_class;
        window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        if (RegisterClassW(&window_class)) return true;
        if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) return true;  // created again after a destroy
        std::fprintf(stderr, "[rt] present: RegisterClassW(%ls) failed (error %lu)\n", k_present_window_class,
                     GetLastError());
        return false;
    }();
    return registered;
}

// The one presentation behind the registration hook. The window, the swapchain and the XboxPresentation are created on
// the first present (or the first reported token, so the token watermark is in place) and then live for the process:
// the service is intentionally leaked, because a static destructor would release the swapchain after the Vulkan
// device's own static teardown, and the OS reclaims the window at exit anyway.
struct PresentService {
    std::mutex mutex;  // the lazy creation and one present at a time
    ID3D12Device* device = nullptr;  // borrowed from the registered record
    WindowPresenter* (*create_window_presenter)(ID3D12Device* device, HWND window) = nullptr;
    XboxPresentWindow window;
    std::unique_ptr<XboxPresentation> presentation;
    bool failed = false;
    std::atomic<std::uint64_t> reported_token{0};  // the device proxy's tokens, heard even before the presentation exists

    void attach(ID3D12Device* new_device, WindowPresenter* (*factory)(ID3D12Device*, HWND));
    // With mutex held.
    XboxPresentation* ensure();
    void note_token(std::uint64_t token);
    HRESULT present(std::uint32_t plane_count, const XboxPresentPlaneParameters* planes,
                    const XboxPresentParameters* params);
};

PresentService& present_service() {
    static PresentService* const service = new PresentService;  // leaked on purpose, see above
    return *service;
}

void PresentService::attach(ID3D12Device* new_device, WindowPresenter* (*factory)(ID3D12Device*, HWND)) {
    std::lock_guard guard(mutex);
    device = new_device;
    create_window_presenter = factory;
}

XboxPresentation* PresentService::ensure() {
    if (presentation) return presentation.get();
    if (failed) return nullptr;
    window = create_xbox_present_window();
    if (!window.window) {
        failed = true;
        return nullptr;
    }
    WindowPresenter* created = create_window_presenter ? create_window_presenter(device, window.window) : nullptr;
    if (!created) {
        failed = true;
        std::fprintf(stderr,
                     "[rt] present: the registered window presenter factory returned null for device %p: PresentX "
                     "cannot present (the device was created without presentation)\n",
                     static_cast<void*>(device));
        return nullptr;
    }
    presentation = std::make_unique<XboxPresentation>(std::unique_ptr<WindowPresenter>(created));
    // Tokens reported before the presentation existed go to it now, so the first present after the window appears does
    // not hit the pacer's "never handed out" refusal.
    presentation->note_externally_issued_frame_token(reported_token.load(std::memory_order_relaxed));
    return presentation.get();
}

void PresentService::note_token(std::uint64_t token) {
    std::uint64_t reported = reported_token.load(std::memory_order_relaxed);
    while (reported < token && !reported_token.compare_exchange_weak(reported, token, std::memory_order_relaxed,
                                                                    std::memory_order_relaxed)) {
    }
    std::lock_guard guard(mutex);
    if (presentation) presentation->note_externally_issued_frame_token(token);
}

HRESULT PresentService::present(std::uint32_t plane_count, const XboxPresentPlaneParameters* planes,
                                const XboxPresentParameters* params) {
    std::lock_guard guard(mutex);
    XboxPresentation* target = ensure();
    if (!target) return DXGI_ERROR_UNSUPPORTED;  // logged by ensure(); nothing presented
    const HRESULT result = target->PresentX(plane_count, planes, params);
    if (SUCCEEDED(result) && !window.shown && window.show_on_first_frame) show_xbox_present_window(window);
    return result;
}

HRESULT WINAPI present_to_queue(void* queue, std::uint32_t plane_count, const XboxPresentPlaneParameters* planes,
                                const XboxPresentParameters* params) {
    (void)queue;  // one window: the queue is not read (XboxPresentation::PresentX writes the trace line itself)
    return present_service().present(plane_count, planes, params);
}

void WINAPI note_issued_token(std::uint64_t token) {
    present_service().note_token(token);
}

WindowPresenter* create_window_presenter_pointer(ID3D12Device* device, HWND window) {
    return create_window_presenter(device, window).release();
}

XboxPresentBackend g_backend_record;  // create_xbox_present_backend's process-lifetime storage

}  // namespace

XboxPresentWindow create_xbox_present_window() {
    XboxPresentWindow created;
    if (!register_present_window_class()) return created;
    created.width = env_client_size("PALMETTO_PRESENT_WIDTH", k_default_present_width);
    created.height = env_client_size("PALMETTO_PRESENT_HEIGHT", k_default_present_height);
    const PresentWindowVisibility visibility = env_visibility();
    RECT frame_rect{0, 0, static_cast<LONG>(created.width), static_cast<LONG>(created.height)};
    AdjustWindowRect(&frame_rect, WS_OVERLAPPEDWINDOW, FALSE);
    created.window = CreateWindowExW(0, k_present_window_class, k_present_window_title, WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, frame_rect.right - frame_rect.left,
                                     frame_rect.bottom - frame_rect.top, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!created.window) {
        std::fprintf(stderr, "[rt] present: CreateWindowExW for the PresentX window failed (error %lu)\n", GetLastError());
        created.width = created.height = 0;
        return created;
    }
    created.show_on_first_frame = visibility == PresentWindowVisibility::auto_show;
    std::fprintf(stderr,
                 "[rt] present: created the PresentX window \"%ls\" with a %ux%u client area, %s; the window and its "
                 "swapchain live for the process\n",
                 k_present_window_title, created.width, created.height,
                 visibility == PresentWindowVisibility::shown    ? "shown (PALMETTO_PRESENT_WINDOW=shown)"
                 : visibility == PresentWindowVisibility::hidden ? "hidden (PALMETTO_PRESENT_WINDOW=hidden)"
                                                                 : "hidden until the first presented frame "
                                                                   "(PALMETTO_PRESENT_WINDOW=auto)");
    if (visibility == PresentWindowVisibility::shown) show_xbox_present_window(created);
    return created;
}

void show_xbox_present_window(XboxPresentWindow& window) {
    if (!window.window || window.shown) return;
    ShowWindow(window.window, SW_SHOWNOACTIVATE);
    UpdateWindow(window.window);
    window.shown = true;
    window.show_on_first_frame = false;
    std::fprintf(stderr, "[rt] present: showing the PresentX window (%ux%u client): the presented frames are in it now\n",
                 window.width, window.height);
}

void destroy_xbox_present_window(XboxPresentWindow& window) {
    if (window.window) DestroyWindow(window.window);
    window = XboxPresentWindow{};
}

const XboxPresentBackend* create_xbox_present_backend(ID3D12Device* device) {
    if (!device) return nullptr;
    g_backend_record.size = sizeof(XboxPresentBackend);
    g_backend_record.version = xbox_present_backend_version;
    g_backend_record.device = device;
    g_backend_record.create_window_presenter = &create_window_presenter_pointer;
    g_backend_record.present = &present_to_queue;
    g_backend_record.note_issued_token = &note_issued_token;
    return &g_backend_record;
}

}  // namespace rt::d3d12

// The registration hook itself: extern "C" and defined here, so the GPU-free proxy binaries never need to link this
// translation unit for it (they only read the inline slot through the header).
extern "C" HRESULT palmetto_register_xbox_present(void* backend) {
    const auto* record = static_cast<const rt::d3d12::XboxPresentBackend*>(backend);
    const char* refusal = nullptr;
    if (!record) refusal = "null record";
    else if (record->size < sizeof(rt::d3d12::XboxPresentBackend)) refusal = "record shorter than this version";
    else if (record->version != rt::d3d12::xbox_present_backend_version) refusal = "record version mismatch";
    else if (!record->device) refusal = "no PC device to present through";
    else if (!record->create_window_presenter) refusal = "no window presenter factory";
    else if (!record->present || !record->note_issued_token) refusal = "record not built by create_xbox_present_backend";
    if (refusal) {
        std::fprintf(stderr, "[rt] present: palmetto_register_xbox_present refused: %s\n", refusal);
        return E_INVALIDARG;
    }
    rt::d3d12::xbox_present_hook::registered = record;
    rt::d3d12::present_service().attach(record->device, record->create_window_presenter);
    std::fprintf(stderr,
                 "[rt] present: registered the PresentX backend: PC device %p, window presenter factory, frame-token "
                 "bridge\n",
                 static_cast<void*>(record->device));
    return S_OK;
}

extern "C" void palmetto_unregister_xbox_present() {
    rt::d3d12::xbox_present_hook::registered = nullptr;
}
