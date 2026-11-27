// GPU-free tests for the DXGI display-mode answer policy and the title's measured refresh selection.
// This uses synthetic modes and synthetic state words only; it creates no adapter, device, window or game image.
#include "../shims/dxgi_mode_answer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using rt::dxgi::mode_answer::AddResult;
using rt::dxgi::mode_answer::ModeListBuilder;
using rt::dxgi::mode_answer::Policy;
using rt::dxgi::mode_answer::Refusal;
using rt::dxgi::mode_answer::SimulationResult;
using rt::dxgi::mode_answer::TitleRefreshChoice;

unsigned failures = 0;
unsigned checks = 0;

void check(bool ok, const char* description) {
    ++checks;
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
}

DXGI_MODE_DESC1 desc1(UINT width, UINT height, UINT numerator, UINT denominator = 1) {
    DXGI_MODE_DESC base = rt::dxgi::mode_answer::shape_mode(width, height, numerator, DXGI_FORMAT_B8G8R8A8_UNORM);
    base.RefreshRate.Denominator = denominator;
    return rt::dxgi::mode_answer::to_mode_desc1(base);
}

static_assert(sizeof(DXGI_MODE_DESC) == 0x1C);

std::vector<DXGI_MODE_DESC> previous_host_answer_shape(DXGI_FORMAT format) {
    const std::array<std::tuple<UINT, UINT, UINT>, 5> samples{{
        {2560, 1440, 60}, {1920, 1080, 60}, {2560, 1440, 60}, {0, 1440, 60}, {1920, 1080, 0},
    }};
    std::vector<DXGI_MODE_DESC> modes;
    std::set<std::tuple<UINT, UINT, UINT>> seen;
    for (const auto& [width, height, hz] : samples) {
        if (!width || !height || !hz || !seen.emplace(width, height, hz).second) continue;
        DXGI_MODE_DESC mode{};
        mode.Width = width;
        mode.Height = height;
        mode.RefreshRate = {hz, 1};
        mode.Format = format;
        mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        modes.push_back(mode);
    }
    std::sort(modes.begin(), modes.end(), [](const DXGI_MODE_DESC& a, const DXGI_MODE_DESC& b) {
        return std::tie(a.Width, a.Height, a.RefreshRate.Numerator) <
               std::tie(b.Width, b.Height, b.RefreshRate.Numerator);
    });
    return modes;
}

void test_switch_parser() {
    using rt::dxgi::mode_answer::parse_policy;

    check(parse_policy(false, "simulate-120").policy == Policy::host_modes,
          "an unset switch must preserve the host mode path");
    check(parse_policy(true, "").policy == Policy::host_modes, "an empty switch must preserve the host mode path");
    for (std::string_view off : {"off", "0", "false", "no", " OFF "})
        check(parse_policy(true, off).policy == Policy::host_modes, "off spellings must keep host modes");
    check(parse_policy(true, "  HOST ").policy == Policy::host_modes_trace,
          "explicit host mode must enable bounded measurement logging only");
    check(parse_policy(true, "SiMuLaTe-120").policy == Policy::simulate_120,
          "simulate-120 must be case-insensitive");

    const auto unknown = parse_policy(true, "120hz-ish");
    check(unknown.policy == Policy::host_modes && unknown.refusal == Refusal::unrecognized_value,
          "an unknown value must be refused by name and fall back to host modes");
    const auto long_value = parse_policy(true, "simulate-120", true);
    check(long_value.policy == Policy::host_modes && long_value.refusal == Refusal::value_too_long,
          "an overlong value must be refused by name and fall back to host modes");
}

void test_host_list_shape_and_order() {
    ModeListBuilder builder(DXGI_FORMAT_B8G8R8A8_UNORM);
    check(builder.add(2560, 1440, 60) == AddResult::added, "first host mode should be added");
    check(builder.add(1920, 1080, 60) == AddResult::added, "second host mode should be added");
    check(builder.add(2560, 1440, 60) == AddResult::duplicate,
          "width/height/integer-Hz duplicates must be suppressed as before");
    check(builder.add(0, 1440, 60) == AddResult::invalid && builder.add(1920, 1080, 0) == AddResult::invalid,
          "zero dimensions and rates must be rejected");

    const std::vector<DXGI_MODE_DESC> modes = std::move(builder).finish_sorted();
    const std::vector<DXGI_MODE_DESC> previous = previous_host_answer_shape(DXGI_FORMAT_B8G8R8A8_UNORM);
    const bool byte_identical = modes.size() == previous.size() &&
                                (modes.empty() ||
                                 std::memcmp(modes.data(), previous.data(), modes.size() * sizeof(DXGI_MODE_DESC)) == 0);
    check(byte_identical, "unset host mode records must be byte-identical to the previous answer shape");
    check(modes.size() == 2, "host-only mode shaping must not add a synthetic record");
    if (modes.size() != 2) return;
    check(modes[0].Width == 1920 && modes[0].Height == 1080 && modes[0].RefreshRate.Numerator == 60,
          "mode order must remain width, height, then refresh numerator");
    check(modes[1].Width == 2560 && modes[1].Height == 1440 && modes[1].RefreshRate.Numerator == 60,
          "second sorted host record must preserve its dimensions and refresh");
    for (const DXGI_MODE_DESC& mode : modes) {
        check(mode.RefreshRate.Denominator == 1, "host integer-Hz modes must be shaped as rational n/1");
        check(mode.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "the caller's requested format must be copied");
        check(mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE,
              "the measured answer uses progressive scanline ordering");
        check(mode.Scaling == DXGI_MODE_SCALING_UNSPECIFIED, "the measured answer uses unspecified scaling");
    }

    const DXGI_MODE_DESC shaped = rt::dxgi::mode_answer::shape_mode(
        3840, 2160, 120, DXGI_FORMAT_R10G10B10A2_UNORM);
    const DXGI_MODE_DESC1 console_record = rt::dxgi::mode_answer::to_mode_desc1(shaped);
    const DXGI_MODE_DESC1 expected_console_record{3840, 2160, {120, 1}, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                   DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE,
                                                   DXGI_MODE_SCALING_UNSPECIFIED, FALSE};
    check(sizeof(console_record) == 0x20, "the title-facing DXGI_MODE_DESC1 record must remain 0x20 bytes");
    check(std::memcmp(&console_record, &expected_console_record, sizeof(console_record)) == 0,
          "the complete console-shaped 0x20-byte record must match its measured field shape");
    check(console_record.Width == 3840 && console_record.Height == 2160 &&
              console_record.RefreshRate.Numerator == 120 && console_record.RefreshRate.Denominator == 1,
          "the 120 Hz record must use the requested size and rational");
    check(console_record.Format == DXGI_FORMAT_R10G10B10A2_UNORM, "the DXGI_MODE_DESC1 format must be retained");
    check(console_record.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE &&
              console_record.Scaling == DXGI_MODE_SCALING_UNSPECIFIED && !console_record.Stereo,
          "the DXGI_MODE_DESC1 shape must be progressive, unspecified-scaled and non-stereo");
}

void test_simulated_120_injection() {
    ModeListBuilder host(DXGI_FORMAT_R8G8B8A8_UNORM);
    host.add(2560, 1440, 60);
    const SimulationResult off = rt::dxgi::mode_answer::ensure_simulated_120(host, Policy::host_modes, 2560, 1440);
    const SimulationResult traced =
        rt::dxgi::mode_answer::ensure_simulated_120(host, Policy::host_modes_trace, 2560, 1440);
    check(off == SimulationResult::not_requested && traced == SimulationResult::not_requested &&
              host.modes().size() == 1,
          "unset/default and explicit-host trace policies must leave the host mode list unchanged");

    ModeListBuilder simulated(DXGI_FORMAT_R8G8B8A8_UNORM);
    simulated.add(1920, 1080, 60);
    simulated.add(2560, 1440, 60);
    const SimulationResult added =
        rt::dxgi::mode_answer::ensure_simulated_120(simulated, Policy::simulate_120, 2560, 1440);
    check(added == SimulationResult::added && simulated.modes().size() == 3,
          "simulate-120 must append one current-size 120/1 mode when none is offered");
    check(simulated.contains_refresh_numerator(120), "the simulated list must expose numerator 120 to the consumer");
    const auto sorted = std::move(simulated).finish_sorted();
    bool found_simulated_shape = false;
    for (const DXGI_MODE_DESC& mode : sorted) {
        if (mode.Width == 2560 && mode.Height == 1440 && mode.RefreshRate.Numerator == 120) {
            found_simulated_shape = mode.RefreshRate.Denominator == 1 &&
                                    mode.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                                    mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE &&
                                    mode.Scaling == DXGI_MODE_SCALING_UNSPECIFIED;
        }
    }
    check(found_simulated_shape, "the synthetic record must follow the measured DXGI mode shape");

    ModeListBuilder native_120(DXGI_FORMAT_B8G8R8A8_UNORM);
    native_120.add(1920, 1080, 60);
    native_120.add(1280, 720, 120);
    const SimulationResult already =
        rt::dxgi::mode_answer::ensure_simulated_120(native_120, Policy::simulate_120, 1920, 1080);
    check(already == SimulationResult::already_offered && native_120.modes().size() == 2,
          "a host that already offers a 120 numerator must not get another injected record");

    ModeListBuilder no_size(DXGI_FORMAT_B8G8R8A8_UNORM);
    no_size.add(1920, 1080, 60);
    const SimulationResult refused =
        rt::dxgi::mode_answer::ensure_simulated_120(no_size, Policy::simulate_120, 0, 0);
    check(refused == SimulationResult::refused_no_current_size && no_size.modes().size() == 1,
          "simulation must refuse a missing current resolution without changing host modes");
}

struct SyntheticTitleWords {
    std::uint8_t high_refresh_flag = 0xA5;
    std::array<std::uint8_t, 7> padding{};
    DXGI_RATIONAL refresh{0xA5A5A5A5u, 0x5A5A5A5Au};
};
static_assert(offsetof(SyntheticTitleWords, refresh) == 8);

void model_title_store(SyntheticTitleWords& words, std::span<const DXGI_MODE_DESC1> modes) {
    const TitleRefreshChoice choice = rt::dxgi::mode_answer::choose_title_refresh(modes);
    words.high_refresh_flag = choice.high_refresh_flag;
    std::memcpy(&words.refresh, &choice.refresh, sizeof(choice.refresh));
}

std::uint64_t packed_refresh(const DXGI_RATIONAL& refresh) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &refresh, sizeof(bits));
    return bits;
}

void test_consumed_refresh_and_synthetic_writes() {
    const std::array<DXGI_MODE_DESC1, 2> sixty_only{desc1(1920, 1080, 60), desc1(2560, 1440, 60)};
    const TitleRefreshChoice low = rt::dxgi::mode_answer::choose_title_refresh(sixty_only);
    check(low.high_refresh_flag == 0 && low.refresh.Numerator == 60 && low.refresh.Denominator == 1,
          "without a numerator-120 mode, the title model must choose flag 0 and refresh 60/1");

    const std::array<DXGI_MODE_DESC1, 3> mixed{desc1(1920, 1080, 60), desc1(2560, 1440, 120),
                                               desc1(3840, 2160, 60)};
    const TitleRefreshChoice high = rt::dxgi::mode_answer::choose_title_refresh(mixed);
    check(high.high_refresh_flag == 1 && high.refresh.Numerator == 120 && high.refresh.Denominator == 1,
          "any record whose numerator is 120 must select flag 1 and refresh 120/1");

    const std::array<DXGI_MODE_DESC1, 1> numerator_only{desc1(1280, 720, 120, 2)};
    check(rt::dxgi::mode_answer::choose_title_refresh(numerator_only).high_refresh_flag == 1,
          "the measured check is on the numerator field, not a normalized rational comparison");
    const std::array<DXGI_MODE_DESC1, 1> near_miss{desc1(1280, 720, 119)};
    check(rt::dxgi::mode_answer::choose_title_refresh(near_miss).high_refresh_flag == 0,
          "a near-miss numerator must not be rounded up to the 120 path");

    // Model only the two measured fields with separate synthetic words; this never writes into a game image.
    SyntheticTitleWords words{};
    words.padding.fill(0x3C);
    model_title_store(words, sixty_only);
    check(words.high_refresh_flag == 0 && words.refresh.Numerator == 60 && words.refresh.Denominator == 1,
          "synthetic consumer stores must write the low flag byte and qword 60/1");
    check(packed_refresh(words.refresh) == 0x000000010000003Cull,
          "the synthetic low refresh qword must have the measured numerator/denominator order");
    check(words.padding == std::array<std::uint8_t, 7>{0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C, 0x3C},
          "synthetic refresh writes must leave unrelated bytes untouched");

    model_title_store(words, mixed);
    check(words.high_refresh_flag == 1 && words.refresh.Numerator == 120 && words.refresh.Denominator == 1,
          "synthetic consumer stores must write the high flag byte and qword 120/1");
    check(packed_refresh(words.refresh) == 0x0000000100000078ull,
          "the synthetic high refresh qword must have the measured numerator/denominator order");
}

}  // namespace

int main() {
    test_switch_parser();
    test_host_list_shape_and_order();
    test_simulated_120_injection();
    test_consumed_refresh_and_synthetic_writes();

    if (failures) {
        std::fprintf(stderr, "dxgi_mode_answer: %u/%u checks failed\n", failures, checks);
        return 1;
    }
    std::printf("dxgi_mode_answer: %u GPU-free checks passed\n", checks);
    return 0;
}
