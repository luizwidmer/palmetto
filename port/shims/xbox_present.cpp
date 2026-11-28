#include "xbox_present.h"

#include "d3d12_com.h"  // WindowPresenter, create_window_presenter: the registered window presenter factory
#include "d3d12_trace.h"

#include "../src/vulkan_present.h"
#include "../src/plane_compose_rule.h"  // docs/peer_port_movie.md: the measured second-plane rule
#include "../src/movie_overlay_readiness.h"  // docs/movie_overlay_readiness.md: opt-in correlated movie-plane guard
#include "../src/present_fit.h"         // docs/peer_port_first_ui.md section 15: the fit/scale rule and its attestation
#include "../src/window_fit.h"          // docs/peer_port_first_ui.md section 16: the opt-in outer-window clamp

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

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

// PALMETTO_PRESENT_PACING: the pacing measurement aid (docs/peer_port_first_ui.md, the implemented port's A/B). Off by
// default; on, the present path prints its token and cadence series and the device proxy prints its frame waits. A
// measurement switch only: the answers never read it.
bool pacing_trace() {
    static const bool on = [] {
        char* value = nullptr;
        std::size_t length = 0;
        bool enabled = false;
        if (_dupenv_s(&value, &length, "PALMETTO_PRESENT_PACING") == 0 && value) {
            enabled = value[0] != '\0' && std::strcmp(value, "0") != 0;
            std::free(value);
        }
        return enabled;
    }();
    return on;
}

// PALMETTO_PRESENT_PER_ORIGIN has a narrow parser so a typo cannot silently opt a boot into different token semantics.
bool per_origin_option(bool driven) {
    char value[32] = {};
    std::size_t length = 0;
    const errno_t env = getenv_s(&length, value, "PALMETTO_PRESENT_PER_ORIGIN");
    if (env == ERANGE) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_PRESENT_PER_ORIGIN is too long for on/off: per-origin pacing refused\n");
        return false;
    }
    if (env != 0 || length <= 1) return false;
    switch (palmetto::parse_present_per_origin_switch(value)) {
        case palmetto::PresentPerOriginSwitch::off: return false;
        case palmetto::PresentPerOriginSwitch::invalid:
            std::fprintf(stderr,
                         "[rt] present: PALMETTO_PRESENT_PER_ORIGIN has an unrecognized value (use on/1 or off/0): legacy pacing kept\n");
            return false;
        case palmetto::PresentPerOriginSwitch::on:
            if (!driven) {
                std::fprintf(stderr,
                             "[rt] present: PALMETTO_PRESENT_PER_ORIGIN requires PALMETTO_PRESENT_DRIVEN=1: legacy pacing kept\n");
                return false;
            }
            return true;
    }
    return false;
}

// The present side of PALMETTO_PRESENT_PACING: the token series and the cadence the pacer is following. The first eight
// presents and then every thirtieth keep a boot's log small; the number is the present count, not the token.
void trace_present_pacing(palmetto::FramePacer& pacer, std::uint64_t token) {
    if (!pacing_trace()) return;
    static const auto epoch = std::chrono::steady_clock::now();
    static std::atomic<std::uint64_t> presents{0};
    const std::uint64_t n = presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 8 && n % 30 != 0) return;
    const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
    if (pacer.per_origin_pacing()) {
        std::fprintf(stderr,
                     "[rt] present pacing: present %llu at %.3f s token 0x%llx mode present-driven-per-origin cadence %u us "
                     "flipped 0x%llx fallbacks %llu origins %u queue-overflows %llu\n",
                     static_cast<unsigned long long>(n), at, static_cast<unsigned long long>(token),
                     pacer.display_cadence_us(), static_cast<unsigned long long>(pacer.flipped_token()),
                     static_cast<unsigned long long>(pacer.present_clock_fallbacks()), pacer.frames_in_flight(),
                     static_cast<unsigned long long>(pacer.per_origin_queue_overflows()));
        return;
    }
    std::fprintf(stderr,
                 "[rt] present pacing: present %llu at %.3f s token 0x%llx mode %s cadence %u us flipped 0x%llx "
                 "fallbacks %llu\n",
                 static_cast<unsigned long long>(n), at, static_cast<unsigned long long>(token),
                 pacer.present_driven() ? "present-driven" : "grid", pacer.display_cadence_us(),
                 static_cast<unsigned long long>(pacer.flipped_token()),
                 static_cast<unsigned long long>(pacer.present_clock_fallbacks()));
}

// PALMETTO_PRESENT_GEOMETRY=1: the section-15 geometry attestation (docs/peer_port_first_ui.md). Off by default, and a
// measurement only: nothing on the present path reads it to answer anything. On, the first present prints the window
// (client, outer, monitor, work area as this process sees them), the plane words the present path acts on, the fit rule
// and the output rectangle the picture will occupy, and the presenter prints the swapchain format and colour space it
// chose. One boot then attests fit, scale and colour space instead of inferring them from the code.
bool geometry_trace() {
    static const bool on = [] {
        char value[24] = {};
        std::size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_PRESENT_GEOMETRY") == 0 && length > 1 && value[0] != '0';
    }();
    return on;
}

// The fit rule the presenter was built with, from the same variable (the presenter parses it too; this copy only names
// it in the attestation line and never changes the presenter).
palmetto::present_fit::Rule fit_rule_from_env() {
    static const palmetto::present_fit::Rule rule = [] {
        char value[32] = {};
        std::size_t length = 0;
        palmetto::present_fit::Rule parsed = palmetto::present_fit::Rule::fill;
        if (getenv_s(&length, value, "PALMETTO_PRESENT_FIT") == 0 && length > 1) {
            const char* refusal = nullptr;
            if (!palmetto::present_fit::parse_rule(value, parsed, &refusal)) parsed = palmetto::present_fit::Rule::fill;
        }
        return parsed;
    }();
    return rule;
}

// These optional, diagnostic-only user32 queries are resolved dynamically so an older Windows build keeps the same
// window geometry path. They are reached only under PALMETTO_PRESENT_GEOMETRY or PALMETTO_WINDOW_FIT=workarea and
// never change the title's DPI context.
template<class Function>
Function user32_function(const char* name) {
    static_assert(sizeof(Function) == sizeof(FARPROC));
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC symbol = user32 ? GetProcAddress(user32, name) : nullptr;
    Function function = nullptr;
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

using GetWindowDpiContextCall = HANDLE(WINAPI*)(HWND);
using GetDpiAwarenessCall = int(WINAPI*)(HANDLE);
using GetDpiForWindowCall = UINT(WINAPI*)(HWND);

int window_dpi_awareness(HWND window) {
    static const auto get_context = user32_function<GetWindowDpiContextCall>("GetWindowDpiAwarenessContext");
    static const auto get_awareness = user32_function<GetDpiAwarenessCall>("GetAwarenessFromDpiAwarenessContext");
    if (!get_context || !get_awareness) return -1;
    const HANDLE context = get_context(window);
    return context ? get_awareness(context) : -1;
}

UINT window_dpi(HWND window) {
    static const auto get_dpi = user32_function<GetDpiForWindowCall>("GetDpiForWindow");
    return get_dpi ? get_dpi(window) : 0;
}

const char* dpi_awareness_name(int awareness) {
    switch (awareness) {
    case 0: return "unaware";
    case 1: return "system-aware";
    case 2: return "per-monitor-aware";
    default: return "unavailable";
    }
}

// The window half of the geometry attestation, from the presentation service, which owns the window handle.
void attest_window_geometry(HWND window) {
    static std::atomic<bool> done{false};
    if (!geometry_trace() || done.exchange(true, std::memory_order_relaxed)) return;
    RECT client{};
    RECT outer{};
    if (!window || !GetClientRect(window, &client) || !GetWindowRect(window, &outer)) {
        std::fprintf(stderr, "[rt] present geometry: the window has no measurable client area\n");
        return;
    }
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof monitor_info;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    const bool have_monitor = monitor && GetMonitorInfoW(monitor, &monitor_info);
    const double aspect = client.bottom ? double(client.right) / double(client.bottom) : 0.0;
    std::fprintf(stderr,
                 "[rt] present geometry: window client %ldx%ld (aspect %.3f), outer %ldx%ld; monitor %ldx%ld at %ld,%ld, "
                 "work area %ldx%ld at %ld,%ld (the process's own DPI-virtualized view)\n",
                 client.right, client.bottom, aspect, outer.right - outer.left, outer.bottom - outer.top,
                 have_monitor ? monitor_info.rcMonitor.right - monitor_info.rcMonitor.left : 0,
                 have_monitor ? monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top : 0,
                 have_monitor ? monitor_info.rcMonitor.left : 0L, have_monitor ? monitor_info.rcMonitor.top : 0L,
                 have_monitor ? monitor_info.rcWork.right - monitor_info.rcWork.left : 0,
                 have_monitor ? monitor_info.rcWork.bottom - monitor_info.rcWork.top : 0,
                 have_monitor ? monitor_info.rcWork.left : 0L, have_monitor ? monitor_info.rcWork.top : 0L);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const int awareness = window_dpi_awareness(window);
    const UINT dpi = window_dpi(window);
    std::fprintf(stderr,
                 "[rt] present geometry: style 0x%08llx ex-style 0x%08llx; DPI awareness %s (code %d), window DPI %u\n",
                 static_cast<unsigned long long>(static_cast<ULONG_PTR>(style)),
                 static_cast<unsigned long long>(static_cast<ULONG_PTR>(extended_style)), dpi_awareness_name(awareness),
                 awareness, dpi);
}

// The plane half of the geometry attestation: the plane words, the swapchain extent and where the picture goes under
// the rule in force (fill: the whole window; fit: the source's aspect ratio centred, the rest black).
void attest_plane_geometry(UINT plane_count, const XboxPresentPlaneParameters* planes, int output_width,
                           int output_height, palmetto::present_fit::Rule rule) {
    static std::atomic<bool> done{false};
    if (!geometry_trace() || !plane_count || !planes || done.exchange(true, std::memory_order_relaxed)) return;
    ID3D12Resource* resource = planes[0].ppResources ? planes[0].ppResources[0] : nullptr;
    if (!resource) return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const auto* src_rect = planes[0].pSrcViewRects;
    const auto* place = planes[0].pDestPlacementBase;
    const std::int32_t source_width =
        src_rect ? std::int32_t(src_rect->right - src_rect->left) : std::int32_t(desc.Width);
    const std::int32_t source_height = src_rect ? std::int32_t(src_rect->bottom - src_rect->top) : std::int32_t(desc.Height);
    const palmetto::present_fit::Rect picture =
        palmetto::present_fit::scale_rect(source_width, source_height, output_width, output_height, rule);
    const double scale_x = source_width > 0 ? double(picture.right - picture.left) / double(source_width) : 0.0;
    const double scale_y = source_height > 0 ? double(picture.bottom - picture.top) / double(source_height) : 0.0;
    std::fprintf(stderr,
                 "[rt] present geometry: %u plane(s), plane 0 %llux%u format %u colour space %d scale filter %d flags 0x%x, "
                 "source rect %s%ld,%ld..%ld,%ld, placement %s%.3f,%.3f..%.3f,%.3f; swapchain %dx%d; rule %s -> picture "
                 "%d,%d..%d,%d (scale %.4f x %.4f)\n",
                 plane_count, static_cast<unsigned long long>(desc.Width), desc.Height, unsigned(desc.Format),
                 planes[0].ColorSpace, planes[0].ScaleFilter, planes[0].Flags, src_rect ? "" : "none ",
                 src_rect ? src_rect->left : 0L, src_rect ? src_rect->top : 0L, src_rect ? src_rect->right : 0L,
                 src_rect ? src_rect->bottom : 0L, place ? "" : "none ",
                 static_cast<double>(place ? place->left : 0.0f), static_cast<double>(place ? place->top : 0.0f),
                 static_cast<double>(place ? place->right : 0.0f), static_cast<double>(place ? place->bottom : 0.0f),
                 output_width, output_height, palmetto::present_fit::rule_name(rule), picture.left, picture.top,
                 picture.right, picture.bottom, scale_x, scale_y);
}

// PALMETTO_WINDOW_FIT=workarea adjusts only the measured title window, and only when explicitly requested. The planner
// consumes Win32 rectangles in this process's coordinate view; no process DPI mode or other window is changed.
bool fit_adopted_title_window(HWND window, const wchar_t* class_name) {
    char value[40] = {};
    std::size_t length = 0;
    const int env_result = getenv_s(&length, value, sizeof value, "PALMETTO_WINDOW_FIT");
    if (env_result == ERANGE) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT is longer than %zu bytes: refusing to guess title geometry\n",
                     sizeof value - 1);
        return false;
    }
    if (env_result != 0) {
        std::fprintf(stderr, "[rt] present: PALMETTO_WINDOW_FIT could not be read (errno %d): refusing to resize\n",
                     int(env_result));
        return false;
    }
    palmetto::window_fit::Rule rule = palmetto::window_fit::Rule::off;
    const char* parse_refusal = nullptr;
    if (!palmetto::window_fit::parse_rule(length > 1 ? value : nullptr, rule, &parse_refusal)) {
        std::fprintf(stderr, "[rt] present: %s; title geometry is unchanged\n", parse_refusal);
        return false;
    }
    if (rule == palmetto::window_fit::Rule::off) return false;
    if (!window || !class_name || _wcsicmp(class_name, xbox_title_window_class) != 0) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea applies only to an adopted sgaWindowClass window; "
                     "refusing to resize another window\n");
        return false;
    }
    if (IsZoomed(window)) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea will not override the title's maximized state; "
                     "refusing to resize\n");
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const DWORD style_error = GetLastError();
    const DWORD_PTR style_bits = static_cast<DWORD_PTR>(style);
    if ((style == 0 && style_error != ERROR_SUCCESS) ||
        (style_bits & static_cast<DWORD_PTR>(WS_OVERLAPPEDWINDOW)) != static_cast<DWORD_PTR>(WS_OVERLAPPEDWINDOW)) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea only handles the peer-cross-checked WS_OVERLAPPEDWINDOW style; "
                     "style 0x%08llx refused\n",
                     static_cast<unsigned long long>(style_bits));
        return false;
    }

    RECT outer{};
    RECT client{};
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof monitor_info;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!GetWindowRect(window, &outer) || !GetClientRect(window, &client) || !monitor ||
        !GetMonitorInfoW(monitor, &monitor_info)) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea could not measure the title window or nearest monitor "
                     "work area (error %lu): refusing to resize\n",
                     GetLastError());
        return false;
    }
    const auto convert = [](const RECT& rect) {
        return palmetto::window_fit::Rect{rect.left, rect.top, rect.right, rect.bottom};
    };
    const palmetto::window_fit::Plan plan =
        palmetto::window_fit::fit_work_area(convert(outer), convert(client), convert(monitor_info.rcWork));
    if (plan.status == palmetto::window_fit::PlanStatus::refused) {
        std::fprintf(stderr, "[rt] present: PALMETTO_WINDOW_FIT=workarea refused: %s; title geometry is unchanged\n",
                     palmetto::window_fit::refusal_name(plan.refusal));
        return false;
    }
    if (plan.status == palmetto::window_fit::PlanStatus::unchanged) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea: the title window already fits its nearest monitor "
                     "work area; no geometry changed\n");
        return false;
    }

    constexpr UINT fit_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowPos(window, nullptr, plan.outer.left, plan.outer.top,
                      plan.outer.right - plan.outer.left, plan.outer.bottom - plan.outer.top, fit_flags)) {
        std::fprintf(stderr,
                     "[rt] present: PALMETTO_WINDOW_FIT=workarea SetWindowPos failed (error %lu); title geometry is "
                     "unchanged\n",
                     GetLastError());
        return false;
    }
    const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const int awareness = window_dpi_awareness(window);
    const UINT dpi = window_dpi(window);
    std::fprintf(stderr,
                 "[rt] present: PALMETTO_WINDOW_FIT=workarea requested outer %ld,%ld..%ld,%ld and client %dx%d inside "
                 "work area %ld,%ld..%ld,%ld (style 0x%08llx ex-style 0x%08llx; DPI awareness %s, window DPI %u); "
                 "title text and visibility unchanged\n",
                 plan.outer.left, plan.outer.top, plan.outer.right, plan.outer.bottom, plan.client_width,
                 plan.client_height, monitor_info.rcWork.left, monitor_info.rcWork.top, monitor_info.rcWork.right,
                 monitor_info.rcWork.bottom, static_cast<unsigned long long>(style_bits),
                 static_cast<unsigned long long>(static_cast<ULONG_PTR>(extended_style)),
                 dpi_awareness_name(awareness), dpi);
    return true;  // SetWindowPos accepted the asynchronous, non-activating work-area request.
}

// PALMETTO_PRESENT_PLANE_RULE=measured: the measured second-plane rule (port/src/plane_compose_rule.h) replaces the
// invented overlay-flag gate. Unset (or any other value) keeps the flag gate, which is the present path of the tree
// before this port, byte for byte. Read once per process, like every switch on this path.
bool plane_rule_measured() {
    static const bool on = [] {
        char value[24] = {};
        std::size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_PRESENT_PLANE_RULE") == 0 && length > 1 &&
               palmetto::present_plane_rule_measured_from(value);
    }();
    return on;
}

// PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1 requires an independently correlated registration -> decode -> upload ->
// PresentX chain before plane 1 is composed. This runtime has no registration/decode/upload evidence producer yet, so the
// switch currently refuses plane 1 by the helper's first named refusal. Unset/0 preserve the existing plane decision.
palmetto::movie_overlay_readiness::RuntimeGate movie_overlay_readiness_gate_mode() {
    using palmetto::movie_overlay_readiness::RuntimeGate;
    static const RuntimeGate mode = [] {
        char value[24] = {};
        std::size_t length = 0;
        const errno_t env = getenv_s(&length, value, "PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY");
        if (env == ERANGE) {
            std::fprintf(stderr, "[rt] present: PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY is too long; switch refused, existing plane rule retained\n");
            return RuntimeGate::invalid;
        }
        if (env != 0) {
            std::fprintf(stderr, "[rt] present: PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY could not be read (errno %d); switch refused, existing plane rule retained\n", int(env));
            return RuntimeGate::invalid;
        }
        if (length <= 1) return RuntimeGate::legacy;
        const RuntimeGate parsed = palmetto::movie_overlay_readiness::parse_runtime_gate_switch(value);
        if (parsed == RuntimeGate::invalid)
            std::fprintf(stderr, "[rt] present: PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY has an unsupported value; switch refused, existing plane rule retained\n");
        return parsed;
    }();
    return mode;
}

void log_movie_overlay_gate_once(const palmetto::movie_overlay_readiness::GateDecision& decision) {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (logged.test_and_set(std::memory_order_relaxed)) return;
    if (decision.allow_overlay)
        std::fprintf(stderr, "[rt] present: movie overlay readiness chain confirmed; plane 1 allowed\n");
    else
        std::fprintf(stderr, "[rt] present: movie overlay readiness refused plane 1: %s\n", decision.readiness.reason());
}

// PALMETTO_PRESENT_PLANE_DUMP=1: the sampled plane-word dump the second-plane rule was measured from
// (docs/peer_port_movie.md section 8). Diagnostic only: it reads the words the console entry already validated and never
// changes what is presented. The first calls, the powers of ten to a million, and the first calls that carry anything
// other than the one plain plane (a second plane, another colour space or filter, a placement, any flag bit) are dumped,
// so a boot that never presents a second plane says so by the absence of such a line.
void dump_present_planes(UINT plane_count, const XboxPresentPlaneParameters* planes) {
    static const bool on = [] {
        char value[24] = {};
        std::size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_PRESENT_PLANE_DUMP") == 0 && length > 1 && value[0] != '0';
    }();
    if (!on || !planes) return;
    static std::atomic<std::uint64_t> calls{0};
    static std::atomic<unsigned> unusual{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool sampled = call <= 2 || call == 1000 || call == 10000 || call == 100000 || call == 1000000;
    bool interesting = plane_count != 1;
    for (UINT i = 0; i < plane_count && i < 2; ++i) {
        const XboxPresentPlaneParameters& p = planes[i];
        if (p.Flags || p.ColorSpace || p.ScaleFilter || p.pDestPlacementBase || p.pSrcViewRects ||
            p.ExtendedDescCount || p.ResourceCount != 1 || (i > 0 && p.Token != planes[0].Token))
            interesting = true;
    }
    const bool print = sampled || (interesting && unusual.fetch_add(1, std::memory_order_relaxed) < 8u);
    if (!print) return;
    std::fprintf(stderr, "[rt] present planes: call %llu: %u plane(s)%s\n", static_cast<unsigned long long>(call),
                 plane_count, plane_count > 2 ? " (more than the composed pair; refused)" : "");
    for (UINT i = 0; i < plane_count && i < 2; ++i) {
        const XboxPresentPlaneParameters& p = planes[i];
        const auto* rect = p.pSrcViewRects;
        const auto* place = p.pDestPlacementBase;
        std::fprintf(stderr,
                     "[rt]   plane %u: token 0x%llx resources %u resource0 %p, colour space %d scale filter %d flags "
                     "0x%x, src rect %s%ld,%ld..%ld,%ld, placement %s%.3f,%.3f..%.3f,%.3f, extended %u\n",
                     i, static_cast<unsigned long long>(p.Token), p.ResourceCount,
                     p.ppResources ? static_cast<void*>(p.ppResources[0]) : nullptr, p.ColorSpace, p.ScaleFilter,
                     p.Flags, rect ? "" : "none", rect ? rect->left : 0L, rect ? rect->top : 0L,
                     rect ? rect->right : 0L, rect ? rect->bottom : 0L, place ? "" : "none",
                     static_cast<double>(place ? place->left : 0.0f), static_cast<double>(place ? place->top : 0.0f),
                     static_cast<double>(place ? place->right : 0.0f),
                     static_cast<double>(place ? place->bottom : 0.0f), p.ExtendedDescCount);
    }
}

}  // namespace

// ---- The device frame clock: the present-driven token source for device slots 78-80 (PALMETTO_PRESENT_DRIVEN) -----
// docs/peer_port_first_ui.md, the implemented port. The device proxy's frame events used to be its own atomics: an
// interval stored and never used, a wait answered at once from a private counter (docs/present_wiring.md section 3).
// With PALMETTO_PRESENT_DRIVEN on, the same frame handshake is answered here by one process-lifetime FramePacer, and
// the presenting XboxPresentation commits its presents into it, so the present marks its token flipped and a held wait
// is released at that present (frame_pacer.h, "Present-driven"). With the switch off the pacer is still the one the
// runtime presentation uses (byte-identical pacing and token checks), and the device proxy keeps its own answer.
namespace {

struct DeviceFrameClock {
    DeviceFrameClock()
        : clock(std::make_unique<palmetto::SteadyFrameClock>()),
          pacer(std::make_unique<palmetto::FramePacer>(*clock)) {
        char* value = nullptr;
        std::size_t length = 0;
        bool on = false;
        if (_dupenv_s(&value, &length, "PALMETTO_PRESENT_DRIVEN") == 0 && value) {
            on = value[0] != '\0' && std::strcmp(value, "0") != 0;
            std::free(value);
        }
        const bool per_origin = per_origin_option(on);
        if (on) {
            pacer->set_present_driven(true);
            if (per_origin) pacer->set_per_origin_pacing(true);
            std::fprintf(stderr,
                         "[rt] present: PALMETTO_PRESENT_DRIVEN is set: device slots 78/79/80 are answered by the frame "
                         "pacer, a present marks its token flipped, and a wait held for an in-flight frame is released "
                         "at that present\n");
            if (per_origin)
                std::fprintf(stderr,
                             "[rt] present: PALMETTO_PRESENT_PER_ORIGIN is enabled: each plane's token is tracked and flipped "
                             "independently\n");
        }
    }
    std::unique_ptr<palmetto::SteadyFrameClock> clock;
    std::unique_ptr<palmetto::FramePacer> pacer;
    bool enabled() const { return pacer->present_driven(); }
};

// Leaked on purpose, like the presentation service: the window and swapchain outlive the CRT's static teardown, and a
// pacer destructor waiting on a condition variable after that would race it.
DeviceFrameClock& device_frame_clock() {
    static DeviceFrameClock* const service = new DeviceFrameClock;
    return *service;
}
palmetto::FramePacer& device_frame_clock_pacer() { return *device_frame_clock().pacer; }

BOOL WINAPI frame_events_enabled() { return device_frame_clock().enabled() ? TRUE : FALSE; }
HRESULT WINAPI frame_events_set_interval(std::uint32_t interval_us, std::uint32_t latency, std::uint32_t flags) {
    return from_pacer(device_frame_clock_pacer().set_frame_interval(interval_us, latency, flags));
}
HRESULT WINAPI frame_events_schedule(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags) {
    return from_pacer(device_frame_clock_pacer().schedule_frame_event(event, offset_us, flags));
}
HRESULT WINAPI frame_events_wait(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                 std::uint64_t* token) {
    return from_pacer(device_frame_clock_pacer().wait_frame_event(event, timeout_ms, flags, token));
}
HRESULT WINAPI frame_events_wait_flip(std::uint32_t timeout_ms) {
    return from_pacer(device_frame_clock_pacer().wait_flip(timeout_ms));
}
std::uint64_t WINAPI frame_events_last_token() { return device_frame_clock_pacer().last_issued_token(); }
const XboxFrameEventBackend g_frame_event_backend{&frame_events_enabled, &frame_events_set_interval,
                                                  &frame_events_schedule,  &frame_events_wait,
                                                  &frame_events_wait_flip, &frame_events_last_token};

}  // namespace

XboxPresentation::XboxPresentation(std::unique_ptr<WindowPresenter> presenter, palmetto::FrameClock* clock)
    : presenter_(std::move(presenter)),
      own_pacer_(clock ? std::make_unique<palmetto::FramePacer>(*clock) : nullptr),
      pacer_(clock ? *own_pacer_ : device_frame_clock_pacer()) {
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
    // The title's measured frame handshake has two calls: event 0 (origin) is the token handshake, and event 2, waited
    // on with an INFINITE timeout before the frame's work, is the blocking flip wait (decompiled call sites
    // game+0x248244E and game+0x248240B; docs/display_path.md section 4). The origin wait issues tokens; the flip wait
    // carries none and is released by the present that flips an in-flight frame. Any other event is a later frame
    // point, which this layer treats as a flip wait as well.
    if (event == xbox_frame_event_origin) return from_pacer(pacer_.wait_frame_event(event, timeout_ms, flags, token));
    (void)token;  // event 2 and the later frame points: the flip wait carries no token
    if (event >= 1 && event <= 2) return from_pacer(pacer_.wait_flip(timeout_ms));
    return E_INVALIDARG;  // an event value the public frame points do not cover
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
            trace_present_pacing(pacer_, 0);
        }
        return result;
    }

    // Legacy mode keeps the measured single-token rule. The opt-in mode treats each plane's token as its own origin.
    const bool per_origin = pacer_.per_origin_pacing();
    const std::uint64_t token = planes[0].Token;
    if (!per_origin) {
        // Keep the old validation sequence and outcomes intact when PALMETTO_PRESENT_PER_ORIGIN is off.
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
    } else {
        for (UINT i = 0; i < plane_count; ++i) {
            const XboxPresentPlaneParameters& plane = planes[i];
            if (plane.ResourceCount == 0 || !plane.ppResources || !plane.ppResources[0]) return E_INVALIDARG;
            if (plane.ResourceCount != 1) {
                log_once(logged_resource_count_, "PresentX plane with ResourceCount other than 1: not implemented, refused");
                return DXGI_ERROR_UNSUPPORTED;
            }
        }
    }
    std::uint64_t plane_tokens[palmetto::max_present_planes]{};
    for (UINT i = 0; i < plane_count; ++i) plane_tokens[i] = planes[i].Token;
    // PALMETTO_PRESENT_GEOMETRY=1: once, the plane words and the geometry the presenter acts on (docs/peer_port_first_ui.md
    // section 15). The swapchain extent is the window client area the swapchain was made for. Nothing is read or printed
    // when the switch is off.
    if (geometry_trace()) {
        const VkExtent2D output = presenter_->extent();
        attest_plane_geometry(plane_count, planes, int(output.width), int(output.height), fit_rule_from_env());
    }
    // Only a token that was never handed out is refused. Null, stale and repeated tokens are presented anyway (what the
    // console does with them is unmeasured, and a title that does it must not freeze); each kind is logged once.
    // docs/peer_port_movie.md section 8: the sampled plane-word dump (PALMETTO_PRESENT_PLANE_DUMP=1), diagnostic only.
    dump_present_planes(plane_count, planes);
    FramePacer::TokenIssue issue{};
    std::int32_t checked = hr::ok;
    bool bridged = false;
    if (per_origin) {
        bool have_issue = false;
        for (UINT i = 0; i < plane_count; ++i) {
            FramePacer::TokenIssue plane_issue{};
            checked = pacer_.check_present_token_for_plane(plane_tokens[i], i, &plane_issue);
            if (checked != hr::ok) {
                if (checked == hr::invalid_arg)
                    log_once(logged_per_origin_token_,
                             "PALMETTO_PRESENT_PER_ORIGIN refused a plane token that the frame pacer never issued");
                return from_pacer(checked);
            }
            if (!have_issue && plane_issue != FramePacer::TokenIssue::none) {
                issue = plane_issue;
                have_issue = true;
            }
        }
    } else {
        checked = pacer_.check_present_token(token, &issue);
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
    }
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
    // The second plane's gate (docs/peer_port_movie.md, item 6). The tree's path is the invented overlay flag: the plane
    // flag values were never measured, and the public HDR sample's plane 1 is the GameDVR copy of the frame, so without
    // the flag plane 1 is not shown. With PALMETTO_PRESENT_PLANE_RULE=measured the measured rule of
    // port/src/plane_compose_rule.h replaces the flag: plane 1 is composed only when its own descriptor places it or asks
    // for its own filtering (the fields whose meaning this present path already measured for plane 0), and the
    // no-placement/default-filter case - the unmeasured GameDVR shape - is refused by name.
    bool overlay = false;
    if (plane_count == 2) {
        const int rule_override = plane_rule_override_.load(std::memory_order_relaxed);
        const bool measured_rule = rule_override < 0 ? plane_rule_measured() : rule_override != 0;
        if (measured_rule) {
            palmetto::SecondPlaneWords words;
            words.plane_count = plane_count;
            words.resource_count = planes[1].ResourceCount;
            words.has_placement = placement(planes[1]) != nullptr;
            words.scale_filter = filter_of(planes[1]);
            const palmetto::SecondPlanePlan plan = palmetto::compose_second_plane(words);
            switch (plan.decision) {
            case palmetto::SecondPlaneDecision::compose:
                overlay = true;
                log_once(logged_plane_rule_compose_,
                         "PresentX with 2 planes: the measured plane rule composes plane 1 over plane 0 (its descriptor "
                         "places it or asks its own filtering), premultiplied unless the straight-alpha bit is set");
                break;
            case palmetto::SecondPlaneDecision::refuse_unmeasured:
                log_once(logged_plane_rule_unmeasured_,
                         "PresentX with 2 planes: only plane 0 is shown (the measured plane rule refuses plane 1: no "
                         "placement and the default filter, the unmeasured GameDVR shape)");
                break;
            case palmetto::SecondPlaneDecision::refuse_resource_count:
            case palmetto::SecondPlaneDecision::refuse_plane_count:
            case palmetto::SecondPlaneDecision::not_requested:
                break;  // refused or answered above the rule (ResourceCount and the plane limit are checked earlier)
            }
        } else {
            overlay = (planes[1].Flags & xbox_plane_flag_overlay) != 0;
            if (!overlay)
                log_once(logged_planes_, "PresentX with 2 planes and no overlay flag on plane 1: only plane 0 is shown (plane 1 is taken "
                                         "as the GameDVR copy of the frame)");
        }
    }
    // PresentX does not identify plane 1 as movie versus another overlay; the strict opt-in therefore applies to every
    // otherwise-selected plane 1. Nothing here guesses a movie frame from PresentX alone. Until Bink/upload evidence exists,
    // an opted-in candidate remains refused at frame-registration-not-observed.
    if (overlay) {
        const palmetto::movie_overlay_readiness::RuntimeGate gate_mode = movie_overlay_readiness_gate_mode();
        if (gate_mode == palmetto::movie_overlay_readiness::RuntimeGate::require_ready) {
            palmetto::movie_overlay_readiness::Evidence evidence{};
            evidence.opt_in = true;
            const palmetto::movie_overlay_readiness::GateDecision gate =
                palmetto::movie_overlay_readiness::gate_second_plane(gate_mode, overlay, evidence);
            overlay = gate.allow_overlay;
            log_movie_overlay_gate_once(gate);
        }
    }
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
            const FramePacer::PresentTiming timing =
                per_origin ? pacer_.present_timing_for_planes(plane_tokens, plane_count) : pacer_.present_timing(token);
            tear = palmetto::decide_present_sync(timing.late_us, timing.period_us, threshold) == palmetto::PresentSync::immediate;
        }
        const HRESULT result = presenter_->present_planes(composed, sync_interval, tear);
        if (FAILED(result)) return pacer_.suspended() ? from_pacer(hr::invalid_state) : result;
        const palmetto::PresentSync sync =
            tear && presenter_->last_present_tore() ? palmetto::PresentSync::immediate : palmetto::PresentSync::vblank;
        if (per_origin) {
            pacer_.commit_present_tokens_for_planes(plane_tokens, plane_count, sync);
            if (pacer_.per_origin_queue_overflows())
                log_once(logged_per_origin_overflow_,
                         "PALMETTO_PRESENT_PER_ORIGIN reached 64 unresolved origins; the oldest is no longer counted for latency");
        } else {
            pacer_.commit_present_token(token, plane_count, sync);
        }
        ++presents_;
        trace_present_pacing(pacer_, token);
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
        timing = per_origin ? pacer_.present_timing_for_planes(plane_tokens, plane_count) : pacer_.present_timing(token);
        tear = palmetto::decide_present_sync(timing.late_us, timing.period_us, threshold) == palmetto::PresentSync::immediate;
    }
    const HRESULT result = presenter_->present(resource, sync_interval, rect, tear);
    if (FAILED(result)) return pacer_.suspended() ? from_pacer(hr::invalid_state) : result;
    const palmetto::PresentSync sync =
        tear && presenter_->last_present_tore() ? palmetto::PresentSync::immediate : palmetto::PresentSync::vblank;
    if (per_origin) {
        pacer_.commit_present_tokens_for_planes(plane_tokens, plane_count, sync);
        if (pacer_.per_origin_queue_overflows())
            log_once(logged_per_origin_overflow_,
                     "PALMETTO_PRESENT_PER_ORIGIN reached 64 unresolved origins; the oldest is no longer counted for latency");
    } else {
        pacer_.commit_present_token(token, plane_count, sync);
    }
    ++presents_;
    trace_present_pacing(pacer_, token);
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
constexpr wchar_t k_present_window_title[] = L"Palmetto - PresentX";
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

// PALMETTO_PRESENT_ADOPT=off|0 keeps today's own PresentX window; unset, empty, on, auto or 1 (or anything else, logged)
// adopts the title's window when the rule finds one. docs/window.md.
bool env_adopt_title_window() {
    char value[16] = {};
    std::size_t length = 0;
    if (getenv_s(&length, value, "PALMETTO_PRESENT_ADOPT") != 0 || length <= 1) return true;
    if (!_stricmp(value, "off") || !_stricmp(value, "0")) return false;
    if (!_stricmp(value, "on") || !_stricmp(value, "auto") || !_stricmp(value, "1")) return true;
    std::fprintf(stderr,
                 "[rt] present: PALMETTO_PRESENT_ADOPT=%s is not on, off, auto, 1 or 0: the title's window is adopted "
                 "when there is one\n",
                 value);
    return true;
}

// An IME window by class name ("IME", "MSCTFIME UI", ...): it is visible, top-level and not ours, but presenting into it
// would be wrong. Everywhere else the rule uses the window's own state.
bool class_name_mentions_ime(const wchar_t* name) {
    for (const wchar_t* at = name; at[0]; ++at)
        if ((at[0] == L'I' || at[0] == L'i') && (at[1] == L'M' || at[1] == L'm') && (at[2] == L'E' || at[2] == L'e'))
            return true;
    return false;
}

// EnumWindows context: the candidate array, and whether it was too small.
struct WindowEnumeration {
    XboxPresentWindowCandidate* out;
    std::size_t capacity;
    std::size_t count;
    bool truncated;
};

BOOL CALLBACK collect_window_callback(HWND window, LPARAM parameter) {
    auto* enumeration = reinterpret_cast<WindowEnumeration*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return TRUE;  // another program's window: not ours to present into
    if (enumeration->count >= enumeration->capacity) {
        enumeration->truncated = true;
        return FALSE;  // enough: stop the enumeration
    }
    XboxPresentWindowCandidate& candidate = enumeration->out[enumeration->count++];
    candidate.window = window;
    GetClassNameW(window, candidate.class_name,
                  static_cast<int>(sizeof(candidate.class_name) / sizeof(candidate.class_name[0])));
    candidate.visible = IsWindowVisible(window) != FALSE;
    candidate.minimised = IsIconic(window) != FALSE;
    candidate.owned = GetWindow(window, GW_OWNER) != nullptr;
    candidate.tool_window = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    candidate.ime = class_name_mentions_ime(candidate.class_name);
    RECT client{};
    if (GetClientRect(window, &client)) {
        candidate.client_width = static_cast<UINT>(client.right - client.left);
        candidate.client_height = static_cast<UINT>(client.bottom - client.top);
    }
    return TRUE;
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
    // The title's own window when this process has one (docs/window.md): a swapchain-less title has usually made it long
    // before its first present. Only when there is none - or PALMETTO_PRESENT_ADOPT=off - is our own window created.
    window = create_or_adopt_xbox_present_window();
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
    attest_window_geometry(window.window);  // PALMETTO_PRESENT_GEOMETRY=1: once, see the helper
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
    if (!window.window || window.shown || window.adopted) return;  // an adopted window is the title's to show
    ShowWindow(window.window, SW_SHOWNOACTIVATE);
    UpdateWindow(window.window);
    window.shown = true;
    window.show_on_first_frame = false;
    std::fprintf(stderr, "[rt] present: showing the PresentX window (%ux%u client): the presented frames are in it now\n",
                 window.width, window.height);
}

void destroy_xbox_present_window(XboxPresentWindow& window) {
    // Never destroy an adopted window: it is the title's, which may still be using it (and its messages).
    if (window.window && !window.adopted) DestroyWindow(window.window);
    window = XboxPresentWindow{};
}

std::size_t collect_xbox_present_window_candidates(XboxPresentWindowCandidate* out, std::size_t capacity,
                                                   bool* truncated) {
    WindowEnumeration enumeration{out, out ? capacity : 0, 0, false};
    if (enumeration.capacity) EnumWindows(&collect_window_callback, reinterpret_cast<LPARAM>(&enumeration));
    if (truncated) *truncated = enumeration.truncated;
    return enumeration.count;
}

const XboxPresentWindowCandidate* choose_xbox_present_window(const XboxPresentWindowCandidate* candidates,
                                                             std::size_t count, HWND own_window) {
    if (!candidates) return nullptr;
    const XboxPresentWindowCandidate* largest = nullptr;
    std::uint64_t largest_area = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const XboxPresentWindowCandidate& candidate = candidates[i];
        if (!candidate.window || candidate.window == own_window) continue;
        // Usable: a real window this rule can present into (docs/window.md).
        if (!candidate.visible || candidate.minimised || candidate.owned || candidate.tool_window || candidate.ime)
            continue;
        if (candidate.client_width == 0 || candidate.client_height == 0) continue;  // nothing to present into yet
        if (!_wcsicmp(candidate.class_name, k_present_window_class)) continue;      // another of our own: never adopt it
        // The measured title's class: its own window wins outright over any other visible window of the process.
        if (!_wcsicmp(candidate.class_name, xbox_title_window_class)) return &candidate;
        const std::uint64_t area = std::uint64_t(candidate.client_width) * candidate.client_height;
        if (!largest || area > largest_area) {  // the biggest visible window is the best guess for the game's own
            largest = &candidate;
            largest_area = area;
        }
    }
    return largest;
}

bool find_xbox_present_title_window(XboxPresentWindowCandidate* out) {
    XboxPresentWindowCandidate candidates[xbox_window_candidate_capacity]{};
    bool truncated = false;
    const std::size_t count = collect_xbox_present_window_candidates(candidates, xbox_window_candidate_capacity,
                                                                     &truncated);
    if (truncated)
        std::fprintf(stderr,
                     "[rt] present: this process has more than %zu top-level windows: only the first %zu are "
                     "considered for adoption\n",
                     xbox_window_candidate_capacity, xbox_window_candidate_capacity);
    const XboxPresentWindowCandidate* chosen = choose_xbox_present_window(candidates, count, nullptr);
    if (!chosen) return false;
    if (out) *out = *chosen;
    return true;
}

XboxPresentWindow create_or_adopt_xbox_present_window() {
    if (!env_adopt_title_window()) return create_xbox_present_window();
    XboxPresentWindowCandidate chosen;
    if (!find_xbox_present_title_window(&chosen)) {
        std::fprintf(stderr,
                     "[rt] present: this process has no visible window to present into (the title has not made one, or "
                     "it is hidden or minimised): creating the runtime's own PresentX window (docs/window.md)\n");
        return create_xbox_present_window();
    }
    if (!IsWindow(chosen.window)) {  // destroyed between the enumeration and here: the own-window path, not a stale handle
        std::fprintf(stderr, "[rt] present: the window chosen for adoption is gone: creating the runtime's own PresentX "
                             "window (docs/window.md)\n");
        return create_xbox_present_window();
    }
    const bool fit_requested = fit_adopted_title_window(chosen.window, chosen.class_name);
    XboxPresentWindow adopted;
    adopted.window = chosen.window;
    adopted.width = chosen.client_width;
    adopted.height = chosen.client_height;
    adopted.shown = chosen.visible;
    adopted.show_on_first_frame = false;  // the title owns its window's visibility
    adopted.adopted = true;
    if (fit_requested) {
        std::fprintf(stderr,
                     "[rt] present: adopting the title's own window (class \"%ls\", %ux%u client before the requested "
                     "work-area fit); its title and visibility stay title-owned (docs/window.md)\n",
                     chosen.class_name, adopted.width, adopted.height);
    } else {
        std::fprintf(stderr,
                     "[rt] present: adopting the title's own window (class \"%ls\", %ux%u client) instead of creating a "
                     "second one: frames are presented into it, its title, size and visibility stay the title's "
                     "(docs/window.md)\n",
                     chosen.class_name, adopted.width, adopted.height);
    }
    return adopted;
}

const XboxPresentBackend* create_xbox_present_backend(ID3D12Device* device) {
    if (!device) return nullptr;
    g_backend_record.size = sizeof(XboxPresentBackend);
    g_backend_record.version = xbox_present_backend_version;
    g_backend_record.device = device;
    g_backend_record.create_window_presenter = &create_window_presenter_pointer;
    g_backend_record.present = &present_to_queue;
    g_backend_record.note_issued_token = &note_issued_token;
    g_backend_record.frame_events = &g_frame_event_backend;  // the present-driven frame events (PALMETTO_PRESENT_DRIVEN)
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
