#pragma once

// Pure display-mode policy and record helpers shared by dxgi_com.cpp and its GPU-free tests.
// The runtime's default mode records remain the host's EnumDisplaySettingsW results.

#include <dxgi1_6.h>

#include <algorithm>
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
