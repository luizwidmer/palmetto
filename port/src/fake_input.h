#pragma once

// PALMETTO_FAKE_INPUT: the synthetic accept/start button for the loading-gate experiment (docs/input_gate.md). The
// title is measured stuck in a loading state for 1,200 s; one hypothesis is that it waits for a button press. The
// GameInput shim cannot press a real pad, so with this switch it attaches one extra SDL virtual gamepad (so the title
// has a device to read) and presses that pad's A button for a short burst after N seconds - a few times, so a title
// that polls slower than one burst still sees a press.
//
// The schedule is the whole state machine and lives here without SDL or Windows headers, so a test can drive it with
// its own clock (active_input_test). Times are microseconds, like the shim's poll clock.
//
// Default OFF: PALMETTO_FAKE_INPUT unset or empty means no pad, no timers and no other change.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace palmetto {

class FakeInputSchedule {
public:
    static constexpr std::uint64_t kDefaultBurstUs = 150'000;      // how long one press is held
    static constexpr std::uint64_t kDefaultIntervalUs = 2'000'000;  // from one press's start to the next
    static constexpr unsigned kDefaultPresses = 6;                  // presses, the first at the delay

    // Parses the environment value: the delay before the first press, in seconds, as a decimal number ("30", "30.5").
    // nullopt for null, empty, or anything that is not a plain non-negative decimal number of seconds - "0x10",
    // "inf", "1e3", a sign or trailing text are all refused, so a typo never becomes a press.
    static std::optional<FakeInputSchedule> parse(const char* text) {
        if (!text || !*text) return std::nullopt;
        bool digit = false, dot = false;
        for (const char* p = text; *p; ++p) {
            if (*p >= '0' && *p <= '9') {
                digit = true;
            } else if (*p == '.' && !dot) {
                dot = true;
            } else {
                return std::nullopt;
            }
        }
        if (!digit) return std::nullopt;
        char* end = nullptr;
        const double seconds = std::strtod(text, &end);
        if (end == text || *end != '\0' || !std::isfinite(seconds) || seconds < 0.0) return std::nullopt;
        return FakeInputSchedule(static_cast<std::uint64_t>(seconds * 1'000'000.0 + 0.5));
    }

    explicit FakeInputSchedule(std::uint64_t press_at_us = 0, std::uint64_t burst_us = kDefaultBurstUs,
                               std::uint64_t interval_us = kDefaultIntervalUs, unsigned presses = kDefaultPresses)
        : press_at_us_(press_at_us),
          burst_us_(burst_us),
          interval_us_(interval_us ? interval_us : 1),
          presses_(presses) {}

    std::uint64_t press_at_us() const { return press_at_us_; }
    std::uint64_t burst_us() const { return burst_us_; }
    std::uint64_t interval_us() const { return interval_us_; }
    unsigned presses() const { return presses_; }

    // The press (0-based) that holds the button at `elapsed_us` since the schedule started, or nullopt when released.
    // Press k starts at press_at_us + k * interval_us and lasts burst_us, so a press that runs into the next interval
    // is still one press.
    std::optional<unsigned> press_at(std::uint64_t elapsed_us) const {
        if (presses_ == 0 || burst_us_ == 0 || elapsed_us < press_at_us_) return std::nullopt;
        const std::uint64_t since = elapsed_us - press_at_us_;
        const std::uint64_t k = since / interval_us_;
        if (k >= presses_) return std::nullopt;
        if (since % interval_us_ >= burst_us_) return std::nullopt;
        return static_cast<unsigned>(k);
    }

    // The button level at `elapsed_us`: true inside a press burst, false otherwise.
    bool pressed_at(std::uint64_t elapsed_us) const { return press_at(elapsed_us).has_value(); }

    // Whether every press has ended: no press is left and none is still to come. A schedule with no presses is
    // finished at once; before the first press's delay it is not finished (that press is still to come).
    bool finished_at(std::uint64_t elapsed_us) const {
        if (presses_ == 0 || burst_us_ == 0) return true;
        const std::uint64_t last_end = press_at_us_ + (presses_ - 1) * interval_us_ + burst_us_;
        return elapsed_us >= last_end;
    }

    // When the last press ends, in microseconds since the schedule started.
    std::uint64_t ends_at_us() const {
        if (presses_ == 0 || burst_us_ == 0) return press_at_us_;
        return press_at_us_ + (presses_ - 1) * interval_us_ + burst_us_;
    }

private:
    std::uint64_t press_at_us_;
    std::uint64_t burst_us_;
    std::uint64_t interval_us_;
    unsigned presses_;
};

// A deliberately small, backend-clock script for the measured first-run input window. The fixed sequence is four
// GameInput button pulses (A, Menu, A, Menu); each pulse is held for 250 ms and followed by 250 ms neutral. A caller
// chooses only the delay in seconds, relative to the GameInput backend epoch. The script cannot repeat or run forever.
// This is separate from PALMETTO_FAKE_INPUT so its shorter, measured-window stimulus does not change that legacy path.
enum class FirstRunScriptRefusal : std::uint8_t {
    None,
    Off,
    TooLong,
    InvalidDelay,
    DelayOverLimit,
    SignatureFeedUnavailable,
    LegacyScheduleConflict,
};

inline const char* first_run_script_refusal_name(FirstRunScriptRefusal refusal) {
    switch (refusal) {
    case FirstRunScriptRefusal::None: return "none";
    case FirstRunScriptRefusal::Off: return "unset or empty (off)";
    case FirstRunScriptRefusal::TooLong: return "value exceeds the 31-character limit";
    case FirstRunScriptRefusal::InvalidDelay: return "expected plain non-negative decimal seconds";
    case FirstRunScriptRefusal::DelayOverLimit: return "delay exceeds the 300-second limit";
    case FirstRunScriptRefusal::SignatureFeedUnavailable: return "screen signature trigger has no measured frame feed";
    case FirstRunScriptRefusal::LegacyScheduleConflict: return "PALMETTO_FAKE_INPUT is also set; scripts are mutually exclusive";
    }
    return "unknown";
}

class FirstRunInputScript {
public:
    static constexpr unsigned kSteps = 4;
    static constexpr std::uint64_t kMaxDelayUs = 300'000'000;
    static constexpr std::uint64_t kHoldUs = 250'000;
    static constexpr std::uint64_t kIntervalUs = 500'000;  // includes a 250 ms neutral release after each pulse
    static constexpr std::uint32_t kButtonMenu = 1u << 0;
    static constexpr std::uint32_t kButtonA = 1u << 2;

    // PALMETTO_FIRST_RUN_SCRIPT accepts only a decimal delay in seconds, e.g. "22.5". A `screen:` value is named
    // and refused: this input shim has no measured presented-frame signature feed, so treating it as time would lie.
    static std::optional<FirstRunInputScript> parse(const char* text,
                                                    FirstRunScriptRefusal* refusal = nullptr) {
        const auto reject = [refusal](FirstRunScriptRefusal why) -> std::optional<FirstRunInputScript> {
            if (refusal) *refusal = why;
            return std::nullopt;
        };
        if (refusal) *refusal = FirstRunScriptRefusal::None;
        if (!text || !*text) return reject(FirstRunScriptRefusal::Off);

        std::size_t length = 0;
        for (; text[length]; ++length) {
            if (length >= 31) return reject(FirstRunScriptRefusal::TooLong);
        }
        static constexpr char kScreenPrefix[] = "screen:";
        bool screen_prefix = true;
        for (std::size_t i = 0; i < sizeof(kScreenPrefix) - 1; ++i) {
            if (text[i] != kScreenPrefix[i]) {
                screen_prefix = false;
                break;
            }
        }
        if (screen_prefix) return reject(FirstRunScriptRefusal::SignatureFeedUnavailable);

        bool digit = false, dot = false;
        for (std::size_t i = 0; i < length; ++i) {
            if (text[i] >= '0' && text[i] <= '9') {
                digit = true;
            } else if (text[i] == '.' && !dot) {
                dot = true;
            } else {
                return reject(FirstRunScriptRefusal::InvalidDelay);
            }
        }
        if (!digit) return reject(FirstRunScriptRefusal::InvalidDelay);
        char* end = nullptr;
        const double seconds = std::strtod(text, &end);
        if (end == text || *end != '\0' || !std::isfinite(seconds) || seconds < 0.0)
            return reject(FirstRunScriptRefusal::InvalidDelay);
        if (seconds > static_cast<double>(kMaxDelayUs) / 1'000'000.0)
            return reject(FirstRunScriptRefusal::DelayOverLimit);
        if (refusal) *refusal = FirstRunScriptRefusal::None;
        return FirstRunInputScript(static_cast<std::uint64_t>(seconds * 1'000'000.0 + 0.5));
    }

    std::uint64_t first_press_us() const { return first_press_us_; }
    std::uint64_t ends_at_us() const { return first_press_us_ + (kSteps - 1) * kIntervalUs + kHoldUs; }
    unsigned presses() const { return kSteps; }

    // The active step at a backend-epoch timestamp, or nullopt during the initial wait, the explicit release gaps,
    // and after the final release. Time comparisons are half-open: a sample at the hold end sees neutral input.
    std::optional<unsigned> step_at(std::uint64_t elapsed_us) const {
        if (elapsed_us < first_press_us_) return std::nullopt;
        const std::uint64_t since = elapsed_us - first_press_us_;
        const std::uint64_t step = since / kIntervalUs;
        if (step >= kSteps || since % kIntervalUs >= kHoldUs) return std::nullopt;
        return static_cast<unsigned>(step);
    }

    std::uint32_t buttons_at(std::uint64_t elapsed_us) const {
        const std::optional<unsigned> step = step_at(elapsed_us);
        if (!step) return 0;
        return (*step & 1u) ? kButtonMenu : kButtonA;
    }
    bool finished_at(std::uint64_t elapsed_us) const { return elapsed_us >= ends_at_us(); }

private:
    explicit FirstRunInputScript(std::uint64_t first_press_us) : first_press_us_(first_press_us) {}
    std::uint64_t first_press_us_;
};

// A signature is only a pure matcher input here: 32x18 8-bit luma samples are the fixed comparison shape. This port
// has not measured or wired a presented-frame feed, so runtime scripts use the timed source above; tests use synthetic
// signatures and no title pixels or other game data.
struct ScreenSignature {
    static constexpr std::size_t kWidth = 32;
    static constexpr std::size_t kHeight = 18;
    static constexpr std::size_t kSamples = kWidth * kHeight;
    std::array<std::uint8_t, kSamples> luma{};
};

enum class ScreenSignatureRefusal : std::uint8_t { None, MissingPixels, WrongSampleCount, ToleranceTooLoose };

inline const char* screen_signature_refusal_name(ScreenSignatureRefusal refusal) {
    switch (refusal) {
    case ScreenSignatureRefusal::None: return "none";
    case ScreenSignatureRefusal::MissingPixels: return "pixel sample pointer is null";
    case ScreenSignatureRefusal::WrongSampleCount: return "expected exactly 32x18 luma samples";
    case ScreenSignatureRefusal::ToleranceTooLoose: return "mean-error tolerance exceeds the 32-level cap";
    }
    return "unknown";
}

struct ScreenSignatureMatch {
    bool matched = false;
    std::uint16_t mean_absolute_error = 0;
    ScreenSignatureRefusal refusal = ScreenSignatureRefusal::None;
};

// Compares only same-sized reduced luma arrays. The mean is rounded for reporting, while the threshold check uses the
// exact accumulated error so one changed sample cannot disappear through integer rounding at a zero tolerance.
inline ScreenSignatureMatch match_screen_signature(const std::uint8_t* actual, std::size_t actual_count,
                                                    const ScreenSignature& reference,
                                                    std::uint8_t max_mean_absolute_error = 8) {
    ScreenSignatureMatch result;
    if (!actual) {
        result.refusal = ScreenSignatureRefusal::MissingPixels;
        return result;
    }
    if (actual_count != ScreenSignature::kSamples) {
        result.refusal = ScreenSignatureRefusal::WrongSampleCount;
        return result;
    }
    if (max_mean_absolute_error > 32) {
        result.refusal = ScreenSignatureRefusal::ToleranceTooLoose;
        return result;
    }
    std::uint32_t total_error = 0;
    for (std::size_t i = 0; i < ScreenSignature::kSamples; ++i) {
        const int difference = static_cast<int>(actual[i]) - static_cast<int>(reference.luma[i]);
        total_error += static_cast<std::uint32_t>(difference < 0 ? -difference : difference);
    }
    result.mean_absolute_error = static_cast<std::uint16_t>((total_error + ScreenSignature::kSamples / 2) /
                                                            ScreenSignature::kSamples);
    result.matched = total_error <= static_cast<std::uint32_t>(max_mean_absolute_error) * ScreenSignature::kSamples;
    return result;
}

}  // namespace palmetto
