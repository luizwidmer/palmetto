#pragma once

// Pure display-mode policy and record helpers shared by dxgi_com.cpp and its GPU-free tests.
// The runtime's default mode records remain the host's EnumDisplaySettingsW results.

#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rt::dxgi::mode_answer {

inline constexpr std::string_view switch_name = "PALMETTO_DXGI_MODE_ANSWER";
inline constexpr UINT title_high_refresh_numerator = 120;

static_assert(sizeof(DXGI_MODE_DESC1) == 0x20, "the measured consumer reads 0x20-byte DXGI_MODE_DESC1 records");
static_assert(sizeof(DXGI_RATIONAL) == sizeof(std::uint64_t));

enum class Policy : std::uint8_t {
    host_modes,       // unset/off: no synthetic modes, no diagnostic mode-list logging
    host_modes_trace, // explicit `host`: same answer, bounded mode-list logging
    simulate_120,     // explicit measurement aid: add current-size 120/1 if host lists no 120/1 numerator
};

enum class Refusal : std::uint8_t { none, unrecognized_value, value_too_long };

struct ParseResult {
    Policy policy = Policy::host_modes;
    Refusal refusal = Refusal::none;
};

constexpr bool ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

constexpr char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

constexpr std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && ascii_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && ascii_space(text.back())) text.remove_suffix(1);
    return text;
}

constexpr bool ascii_equal_fold(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
    return true;
}

// An absent or empty switch is the historical host enumeration. Invalid values also fall back to it,
// but carry a refusal reason so dxgi_com.cpp can name the typo instead of guessing a mode.
constexpr ParseResult parse_policy(bool present, std::string_view value, bool too_long = false) noexcept {
    if (too_long) return {Policy::host_modes, Refusal::value_too_long};
    if (!present) return {};
    value = trim_ascii(value);
    if (value.empty() || ascii_equal_fold(value, "off") || ascii_equal_fold(value, "0") ||
        ascii_equal_fold(value, "false") || ascii_equal_fold(value, "no"))
        return {};
    if (ascii_equal_fold(value, "host")) return {Policy::host_modes_trace, Refusal::none};
    if (ascii_equal_fold(value, "simulate-120")) return {Policy::simulate_120, Refusal::none};
    return {Policy::host_modes, Refusal::unrecognized_value};
}

constexpr std::string_view refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::unrecognized_value: return "expected host, simulate-120, or off/0/false/no";
    case Refusal::value_too_long: return "value exceeds the 63-byte switch limit";
    }
    return "unknown parser refusal";
}

constexpr DXGI_MODE_DESC shape_mode(UINT width, UINT height, UINT refresh_hz, DXGI_FORMAT format) noexcept {
    DXGI_MODE_DESC mode{};
    mode.Width = width;
    mode.Height = height;
    mode.RefreshRate = {refresh_hz, 1};
    mode.Format = format;
    mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    return mode;
}

constexpr DXGI_MODE_DESC1 to_mode_desc1(const DXGI_MODE_DESC& mode) noexcept {
    return {mode.Width, mode.Height, mode.RefreshRate, mode.Format, mode.ScanlineOrdering, mode.Scaling, FALSE};
}

enum class AddResult : std::uint8_t { added, duplicate, invalid };

// Retains the current shim's host-list rules: reject zero fields, deduplicate by width/height/integer Hz,
// fill the DXGI record from zero, and sort by width, height and refresh numerator before returning it.
class ModeListBuilder {
public:
    explicit ModeListBuilder(DXGI_FORMAT format) noexcept : format_(format) {}

    AddResult add(UINT width, UINT height, UINT refresh_hz) {
        if (!width || !height || !refresh_hz) return AddResult::invalid;
        if (!seen_.emplace(width, height, refresh_hz).second) return AddResult::duplicate;
        modes_.push_back(shape_mode(width, height, refresh_hz, format_));
        return AddResult::added;
    }

    bool contains_refresh_numerator(UINT numerator) const noexcept {
        return std::any_of(modes_.begin(), modes_.end(), [numerator](const DXGI_MODE_DESC& mode) {
            return mode.RefreshRate.Numerator == numerator;
        });
    }

    std::vector<DXGI_MODE_DESC> finish_sorted() && {
        std::sort(modes_.begin(), modes_.end(), [](const DXGI_MODE_DESC& a, const DXGI_MODE_DESC& b) {
            return std::tie(a.Width, a.Height, a.RefreshRate.Numerator) <
                   std::tie(b.Width, b.Height, b.RefreshRate.Numerator);
        });
        return std::move(modes_);
    }

    const std::vector<DXGI_MODE_DESC>& modes() const noexcept { return modes_; }

private:
    DXGI_FORMAT format_;
    std::set<std::tuple<UINT, UINT, UINT>> seen_;
    std::vector<DXGI_MODE_DESC> modes_;
};

enum class SimulationResult : std::uint8_t { not_requested, already_offered, added, refused_no_current_size };

inline constexpr std::size_t max_trace_samples = 8;
inline constexpr std::size_t max_trace_records = 16;

enum class TraceCallSite : std::uint8_t {
    get_display_mode_list,
    get_display_mode_list1,
    find_closest_matching_mode,
    find_closest_matching_mode1,
};

constexpr std::string_view trace_call_site_name(TraceCallSite site) noexcept {
    switch (site) {
    case TraceCallSite::get_display_mode_list: return "IDXGIOutput::GetDisplayModeList";
    case TraceCallSite::get_display_mode_list1: return "IDXGIOutput1::GetDisplayModeList1";
    case TraceCallSite::find_closest_matching_mode: return "IDXGIOutput::FindClosestMatchingMode";
    case TraceCallSite::find_closest_matching_mode1: return "IDXGIOutput1::FindClosestMatchingMode1";
    }
    return "unknown DXGI output call";
}

enum class TracePhase : std::uint8_t { count_query, fill, closest };

constexpr std::string_view trace_phase_name(TracePhase phase) noexcept {
    switch (phase) {
    case TracePhase::count_query: return "count";
    case TracePhase::fill: return "fill";
    case TracePhase::closest: return "closest";
    }
    return "unknown";
}

constexpr bool trace_enabled(Policy policy) noexcept {
    return policy == Policy::host_modes_trace || policy == Policy::simulate_120;
}

constexpr std::string_view policy_name(Policy policy) noexcept {
    switch (policy) {
    case Policy::host_modes: return "off";
    case Policy::host_modes_trace: return "host";
    case Policy::simulate_120: return "simulate-120";
    }
    return "unknown";
}

constexpr std::string_view simulation_result_name(SimulationResult result) noexcept {
    switch (result) {
    case SimulationResult::not_requested: return "not-requested";
    case SimulationResult::already_offered: return "already-offered";
    case SimulationResult::added: return "added";
    case SimulationResult::refused_no_current_size: return "refused-no-current-size";
    }
    return "unknown";
}

// This is a pure summary of the DXGI call at the shim boundary. `available_count` is the complete
// answer, while `copied_count` and the trace-record span describe what a caller with an output buffer
// actually received. No title-owned memory is needed to construct the trace.
struct TraceCall {
    Policy policy = Policy::host_modes;
    TraceCallSite call_site = TraceCallSite::get_display_mode_list;
    TracePhase phase = TracePhase::count_query;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    SimulationResult simulation = SimulationResult::not_requested;
    std::uint32_t sample = 0;
    std::size_t available_count = 0;
    std::size_t input_capacity = 0;
    std::size_t reported_count = 0;
    std::size_t copied_count = 0;
    std::int32_t status = 0;
    bool available_has_120 = false;
};

struct TraceShape {
    std::array<std::size_t, max_trace_records> indices{};
    std::size_t record_count = 0;
    std::size_t shown_count = 0;
    std::size_t omitted_count = 0;
    bool has_120 = false;
    bool shown_has_120 = false;
};

constexpr TraceShape shape_trace_records(std::span<const DXGI_MODE_DESC> modes) noexcept {
    TraceShape shape{};
    shape.record_count = modes.size();
    std::size_t high_index = modes.size();
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].RefreshRate.Numerator == title_high_refresh_numerator) {
            high_index = i;
            break;
        }
    }
    shape.has_120 = high_index != modes.size();

    const std::size_t prefix_limit =
        shape.has_120 && high_index >= max_trace_records ? max_trace_records - 1 : max_trace_records;
    const std::size_t prefix_count = std::min(modes.size(), prefix_limit);
    for (std::size_t i = 0; i < prefix_count; ++i) shape.indices[shape.shown_count++] = i;
    if (shape.has_120 && high_index >= prefix_count && shape.shown_count < shape.indices.size())
        shape.indices[shape.shown_count++] = high_index;

    shape.omitted_count = shape.record_count - shape.shown_count;
    shape.shown_has_120 = shape.has_120 && high_index < modes.size() &&
                          std::find(shape.indices.begin(), shape.indices.begin() + shape.shown_count, high_index) !=
                              shape.indices.begin() + shape.shown_count;
    return shape;
}

enum class TraceEventKind : std::uint8_t { header, mode, omitted };

struct TraceEvent {
    TraceEventKind kind = TraceEventKind::header;
    TraceCall call{};
    TraceShape shape{};
    std::size_t index = 0;
    std::size_t omitted_count = 0;
    DXGI_MODE_DESC mode{};
};

// The runtime formats these bounded events for stderr; tests use the same gate and shape with a
// counting/collecting sink. An off policy never invokes the sink, so the default path has no output.
template <class Sink>
void emit_trace_events(const TraceCall& call, std::span<const DXGI_MODE_DESC> records, Sink&& sink) {
    if (!trace_enabled(call.policy)) return;

    const TraceShape shape = shape_trace_records(records);
    TraceEvent header{};
    header.kind = TraceEventKind::header;
    header.call = call;
    header.shape = shape;
    sink(header);

    for (std::size_t i = 0; i < shape.shown_count; ++i) {
        const std::size_t index = shape.indices[i];
        TraceEvent event{};
        event.kind = TraceEventKind::mode;
        event.call = call;
        event.shape = shape;
        event.index = index;
        event.mode = records[index];
        sink(event);
    }
    if (shape.omitted_count) {
        TraceEvent event{};
        event.kind = TraceEventKind::omitted;
        event.call = call;
        event.shape = shape;
        event.omitted_count = shape.omitted_count;
        sink(event);
    }
}

inline SimulationResult ensure_simulated_120(ModeListBuilder& builder, Policy policy, UINT current_width,
                                             UINT current_height) {
    if (policy != Policy::simulate_120) return SimulationResult::not_requested;
    if (builder.contains_refresh_numerator(title_high_refresh_numerator)) return SimulationResult::already_offered;
    const AddResult result = builder.add(current_width, current_height, title_high_refresh_numerator);
    if (result == AddResult::invalid) return SimulationResult::refused_no_current_size;
    return result == AddResult::added ? SimulationResult::added : SimulationResult::already_offered;
}

struct TitleRefreshChoice {
    std::uint8_t high_refresh_flag;
    DXGI_RATIONAL refresh;
};

// This is a pure testable model of the measured title predicate: the record field is at byte offset +8,
// and the consumer compares its numerator to 120. It deliberately does not normalize by denominator.
constexpr TitleRefreshChoice choose_title_refresh(std::span<const DXGI_MODE_DESC1> modes) noexcept {
    for (const DXGI_MODE_DESC1& mode : modes) {
        if (mode.RefreshRate.Numerator == title_high_refresh_numerator)
            return {1, {title_high_refresh_numerator, 1}};
    }
    return {0, {60, 1}};
}

}  // namespace rt::dxgi::mode_answer
