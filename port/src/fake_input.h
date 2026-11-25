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

#include <cmath>
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

}  // namespace palmetto
