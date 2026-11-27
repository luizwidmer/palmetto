#pragma once

// The opt-in window-boundary rule for the title's DPI-virtualized coordinates (docs/peer_port_first_ui.md section 16).
// The title owns its window and its default size. With PALMETTO_WINDOW_FIT=workarea, the presenter may request one
// measured adjustment of the adopted title window so its outer rectangle fits the work area reported in the same
// process coordinate space. The planner is pure: it preserves the existing client aspect ratio and frame extents, and
// tests do not need Win32, a GPU or the title. The default `off` rule makes no geometry request.

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace palmetto::window_fit {

enum class Rule : std::uint8_t { off = 0, work_area = 1 };
enum class PlanStatus : std::uint8_t { unchanged = 0, changed = 1, refused = 2 };
enum class Refusal : std::uint8_t {
    none = 0,
    invalid_outer_rect,
    invalid_client_rect,
    invalid_work_area,
    client_larger_than_outer,
    frame_larger_than_work_area,
    result_out_of_range,
};

// Win32 RECT convention: left/top inclusive, right/bottom exclusive.
struct Rect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

struct Plan {
    PlanStatus status = PlanStatus::unchanged;
    Rect outer{};
    std::int32_t client_width = 0;
    std::int32_t client_height = 0;
    Refusal refusal = Refusal::none;
};

inline const char* rule_name(Rule rule) { return rule == Rule::work_area ? "workarea" : "off"; }

inline const char* refusal_name(Refusal refusal) {
    switch (refusal) {
    case Refusal::invalid_outer_rect: return "the outer window rectangle is empty or inverted";
    case Refusal::invalid_client_rect: return "the client rectangle is empty or inverted";
    case Refusal::invalid_work_area: return "the monitor work area is empty or inverted";
    case Refusal::client_larger_than_outer: return "the measured client area is larger than the outer window";
    case Refusal::frame_larger_than_work_area: return "the measured window frame leaves no client area in the work area";
    case Refusal::result_out_of_range: return "the fitted rectangle is outside the Win32 coordinate range";
    case Refusal::none: break;
    }
    return "unknown window-fit refusal";
}

// Unset/empty/off/false/no/0/leave keeps the existing geometry. on/true/yes/1/fit/workarea/work-area enables the
// work-area clamp. Unknown spellings are refused by name and leave `out` untouched rather than guessing geometry.
inline bool parse_rule(const char* value, Rule& out, const char** refusal) {
    const auto equals = [](const char* text, const char* word) {
        while (*text == ' ' || *text == '\t') ++text;
        for (; *word; ++text, ++word) {
            char a = *text;
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (a != *word) return false;
        }
        while (*text == ' ' || *text == '\t') ++text;
        return *text == '\0';
    };
    if (!value) {
        out = Rule::off;
        return true;
    }
    const char* first = value;
    while (*first == ' ' || *first == '\t') ++first;
    if (!*first) {
        out = Rule::off;
        return true;
    }
    for (const char* word : {"off", "false", "no", "0", "leave"}) {
        if (equals(value, word)) {
            out = Rule::off;
            return true;
        }
    }
    for (const char* word : {"on", "true", "yes", "1", "fit", "workarea", "work-area"}) {
        if (equals(value, word)) {
            out = Rule::work_area;
            return true;
        }
    }
    if (refusal) *refusal = "PALMETTO_WINDOW_FIT is not off or workarea: refusing to guess the title window geometry";
    return false;
}

namespace detail {
inline std::int64_t width(const Rect& rect) { return std::int64_t(rect.right) - rect.left; }
inline std::int64_t height(const Rect& rect) { return std::int64_t(rect.bottom) - rect.top; }
inline std::int64_t round_div(std::int64_t numerator, std::int64_t denominator) {
    return (numerator + denominator / 2) / denominator;  // positive inputs only
}
inline Plan refused(Rect outer, Refusal reason) {
    Plan plan;
    plan.status = PlanStatus::refused;
    plan.outer = outer;
    plan.refusal = reason;
    return plan;
}
inline bool in_i32(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
}
}  // namespace detail

// Computes a target outer rectangle that fits `work`, preserving the observed outer/client frame margins and the
// current client aspect ratio. An oversized window is uniformly reduced and centred; a window whose size fits but is
// positioned outside the work area is moved only as far as needed. Already-fitting windows are left byte-for-byte
// unchanged. No Windows API or guessed DPI multiplier is involved: all three rectangles must use the same caller view.
inline Plan fit_work_area(Rect outer, Rect client, Rect work) {
    using detail::height;
    using detail::width;
    const std::int64_t outer_width = width(outer);
    const std::int64_t outer_height = height(outer);
    const std::int64_t client_width = width(client);
    const std::int64_t client_height = height(client);
    const std::int64_t work_width = width(work);
    const std::int64_t work_height = height(work);
    if (outer_width <= 0 || outer_height <= 0) return detail::refused(outer, Refusal::invalid_outer_rect);
    if (client_width <= 0 || client_height <= 0) return detail::refused(outer, Refusal::invalid_client_rect);
    if (work_width <= 0 || work_height <= 0) return detail::refused(outer, Refusal::invalid_work_area);
    constexpr std::int64_t win32_extent_max = std::numeric_limits<std::int32_t>::max();
    if (outer_width > win32_extent_max || outer_height > win32_extent_max || client_width > win32_extent_max ||
        client_height > win32_extent_max || work_width > win32_extent_max || work_height > win32_extent_max)
        return detail::refused(outer, Refusal::result_out_of_range);
    const std::int64_t frame_width = outer_width - client_width;
    const std::int64_t frame_height = outer_height - client_height;
    if (frame_width < 0 || frame_height < 0) return detail::refused(outer, Refusal::client_larger_than_outer);
    if (frame_width >= work_width || frame_height >= work_height)
        return detail::refused(outer, Refusal::frame_larger_than_work_area);

    const std::int64_t max_client_width = work_width - frame_width;
    const std::int64_t max_client_height = work_height - frame_height;
    std::int64_t target_client_width = client_width;
    std::int64_t target_client_height = client_height;
    const bool resize = outer_width > work_width || outer_height > work_height;
    if (resize) {
        // Use integer cross-products to choose the limiting axis without floating-point drift. The rounded side can
        // differ by at most one pixel; it is then bounded so a rounding carry can never escape the work area.
        const bool width_limited = max_client_width * client_height <= max_client_height * client_width;
        if (width_limited) {
            target_client_width = max_client_width;
            target_client_height = detail::round_div(client_height * target_client_width, client_width);
        } else {
            target_client_height = max_client_height;
            target_client_width = detail::round_div(client_width * target_client_height, client_height);
        }
        target_client_width = std::clamp<std::int64_t>(target_client_width, 1, max_client_width);
        target_client_height = std::clamp<std::int64_t>(target_client_height, 1, max_client_height);
    }

    const std::int64_t target_outer_width = target_client_width + frame_width;
    const std::int64_t target_outer_height = target_client_height + frame_height;
    const std::int64_t left = resize
                                  ? std::int64_t(work.left) + (work_width - target_outer_width) / 2
                                  : std::clamp<std::int64_t>(outer.left, work.left, std::int64_t(work.right) - target_outer_width);
    const std::int64_t top = resize
                                 ? std::int64_t(work.top) + (work_height - target_outer_height) / 2
                                 : std::clamp<std::int64_t>(outer.top, work.top, std::int64_t(work.bottom) - target_outer_height);
    const std::int64_t right = left + target_outer_width;
    const std::int64_t bottom = top + target_outer_height;
    if (!detail::in_i32(left) || !detail::in_i32(top) || !detail::in_i32(right) || !detail::in_i32(bottom) ||
        !detail::in_i32(target_client_width) || !detail::in_i32(target_client_height))
        return detail::refused(outer, Refusal::result_out_of_range);

    Plan plan;
    plan.outer = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top), static_cast<std::int32_t>(right),
                  static_cast<std::int32_t>(bottom)};
    plan.client_width = static_cast<std::int32_t>(target_client_width);
    plan.client_height = static_cast<std::int32_t>(target_client_height);
    if (plan.outer.left == outer.left && plan.outer.top == outer.top && plan.outer.right == outer.right &&
        plan.outer.bottom == outer.bottom) {
        plan.status = PlanStatus::unchanged;
        return plan;
    }
    plan.status = PlanStatus::changed;
    return plan;
}

}  // namespace palmetto::window_fit
