#pragma once

// Pure, frame-fed contract for a single bounded screen-triggered input block.
// It is intentionally not wired into GameInput or the presenter: until the
// runtime has a measured presented-frame signature feed, no live code drives
// this state machine. See docs/window.md and docs/peer_port_first_ui.md.
#include "fake_input.h"

#include <cstddef>
#include <cstdint>

namespace palmetto {

enum class UiInputScriptRefusal : std::uint8_t {
    None,
    InvalidSwitchValue,
    LegacyScheduleConflict,
    InvalidButtonMask,
    InvalidTolerance,
    InvalidHoldDuration,
    InvalidRetryGap,
    InvalidTimeout,
    InvalidStability,
    MissingSignature,
    MissingPixels,
    WrongSampleCount,
    ToleranceTooLoose,
    ClockBeforeArm,
    ClockMovedBackwards,
    ExternalAbort,
    SignatureWaitTimeout,
    ScreenChangeTimeout,
};

inline const char* ui_input_script_refusal_name(UiInputScriptRefusal refusal) {
    switch (refusal) {
    case UiInputScriptRefusal::None: return "none";
    case UiInputScriptRefusal::InvalidSwitchValue: return "PALMETTO_UI_INPUT_SCRIPT must be exactly 1, 0, off, or empty";
    case UiInputScriptRefusal::LegacyScheduleConflict: return "PALMETTO_FAKE_INPUT or PALMETTO_FIRST_RUN_SCRIPT is also active";
    case UiInputScriptRefusal::InvalidButtonMask: return "script action must contain exactly one button bit";
    case UiInputScriptRefusal::InvalidTolerance: return "screen mean-error tolerance exceeds the 32-level cap";
    case UiInputScriptRefusal::InvalidHoldDuration: return "button hold must be in the range 1..1000000 microseconds";
    case UiInputScriptRefusal::InvalidRetryGap: return "neutral retry gap must be in the range 1..10000000 microseconds";
    case UiInputScriptRefusal::InvalidTimeout: return "screen-state timeout must exceed the hold and be at most 120 seconds";
    case UiInputScriptRefusal::InvalidStability: return "stable screen sample count must be in the range 1..8";
    case UiInputScriptRefusal::MissingSignature: return "screen signature was not explicitly configured";
    case UiInputScriptRefusal::MissingPixels: return "presented-frame luma pointer is null";
    case UiInputScriptRefusal::WrongSampleCount: return "expected exactly 32x18 luma samples";
    case UiInputScriptRefusal::ToleranceTooLoose: return "screen mean-error tolerance exceeds the 32-level cap";
    case UiInputScriptRefusal::ClockBeforeArm: return "input clock timestamp precedes the script arm";
    case UiInputScriptRefusal::ClockMovedBackwards: return "input clock moved backwards";
    case UiInputScriptRefusal::ExternalAbort: return "script aborted by caller";
    case UiInputScriptRefusal::SignatureWaitTimeout: return "target screen signature did not stabilize before timeout";
    case UiInputScriptRefusal::ScreenChangeTimeout: return "target screen did not change before timeout";
    }
    return "unknown";
}

inline bool ui_input_script_equal(const char* value, const char* expected) {
    if (!value || !expected) return false;
    std::size_t i = 0;
    while (value[i] && expected[i] && value[i] == expected[i]) ++i;
    return value[i] == '\0' && expected[i] == '\0';
}

// Exact opt-in: only the literal "1" enables this helper. The switch parser
// does not read the process environment; a future adapter must call it itself.
struct UiInputScriptSwitch {
    static constexpr const char* kEnvironmentName = "PALMETTO_UI_INPUT_SCRIPT";
    bool enabled = false;
    UiInputScriptRefusal refusal = UiInputScriptRefusal::None;

    static UiInputScriptSwitch parse(const char* value, bool legacy_schedule_active = false) {
        if (!value || !*value || ui_input_script_equal(value, "0") || ui_input_script_equal(value, "off"))
            return {};
        if (ui_input_script_equal(value, "1")) {
            if (legacy_schedule_active) return {false, UiInputScriptRefusal::LegacyScheduleConflict};
            return {true, UiInputScriptRefusal::None};
        }
        return {false, UiInputScriptRefusal::InvalidSwitchValue};
    }
};

enum class UiInputScriptState : std::uint8_t {
    Disabled,
    WaitingForScreen,
    Holding,
    WaitingForChange,
    Complete,
    TimedOut,
    Aborted,
    Refused,
};

inline const char* ui_input_script_state_name(UiInputScriptState state) {
    switch (state) {
    case UiInputScriptState::Disabled: return "disabled";
    case UiInputScriptState::WaitingForScreen: return "waiting-for-screen";
    case UiInputScriptState::Holding: return "holding";
    case UiInputScriptState::WaitingForChange: return "waiting-for-change";
    case UiInputScriptState::Complete: return "complete";
    case UiInputScriptState::TimedOut: return "timed-out";
    case UiInputScriptState::Aborted: return "aborted";
    case UiInputScriptState::Refused: return "refused";
    }
    return "unknown";
}

enum class UiInputScriptEdge : std::uint8_t { None, Press, Release };

struct UiInputScriptTiming {
    std::uint64_t hold_us = 300'000;       // 300 ms press, then an explicit neutral sample
    std::uint64_t retry_gap_us = 1'200'000; // 300 ms hold + 1.2 s neutral = 1.5 s start-to-start
    std::uint64_t timeout_us = 30'000'000;  // hard bound from arm, including retries
    std::uint8_t stable_frames = 2;
    std::uint8_t max_mean_error = 8;
};

struct UiInputScriptAction {
    ScreenSignature screen{};
    bool has_screen_signature = false;
    std::uint32_t button = 0;
    UiInputScriptTiming timing{};
};

struct UiInputScriptResult {
    UiInputScriptState state = UiInputScriptState::Disabled;
    std::uint32_t buttons = 0;
    UiInputScriptEdge edge = UiInputScriptEdge::None;
    UiInputScriptRefusal refusal = UiInputScriptRefusal::None;
    std::uint16_t mean_absolute_error = 0;
    bool screen_changed = false;
};

// A one-block, deterministic screen-state action: wait for a stable match,
// hold one button, release explicitly, retry while the same screen remains,
// then finish on a stable screen change. Every timestamp is caller supplied.
class UiInputScript {
public:
    static constexpr std::uint32_t kButtonMenu = 1u << 0;
    static constexpr std::uint32_t kButtonA = 1u << 2;
    static constexpr std::uint64_t kMaxHoldUs = 1'000'000;
    static constexpr std::uint64_t kMaxRetryGapUs = 10'000'000;
    static constexpr std::uint64_t kMaxTimeoutUs = 120'000'000;
    static constexpr std::uint8_t kMaxStableFrames = 8;

    UiInputScript(UiInputScriptSwitch option, const UiInputScriptAction& action, std::uint64_t armed_at_us)
        : action_(action), armed_at_us_(armed_at_us) {
        if (option.refusal != UiInputScriptRefusal::None) {
            state_ = UiInputScriptState::Refused;
            refusal_ = option.refusal;
            return;
        }
        if (!option.enabled) {
            state_ = UiInputScriptState::Disabled;
            return;
        }
        refusal_ = validate(action_);
        if (refusal_ != UiInputScriptRefusal::None) {
            state_ = UiInputScriptState::Refused;
            return;
        }
        state_ = UiInputScriptState::WaitingForScreen;
    }

    UiInputScriptResult advance(std::uint64_t now_us, const std::uint8_t* luma, std::size_t luma_count, bool abort = false) {
        if (terminal() || state_ == UiInputScriptState::Disabled || state_ == UiInputScriptState::Refused)
            return result();
        if (abort) return stop(UiInputScriptState::Aborted, UiInputScriptRefusal::ExternalAbort);
        if (now_us < armed_at_us_) return stop(UiInputScriptState::Aborted, UiInputScriptRefusal::ClockBeforeArm);
        if (have_time_ && now_us < last_time_us_)
            return stop(UiInputScriptState::Aborted, UiInputScriptRefusal::ClockMovedBackwards);
        last_time_us_ = now_us;
        have_time_ = true;

        const std::uint64_t elapsed = now_us - armed_at_us_;
        if (elapsed >= action_.timing.timeout_us) {
            const UiInputScriptRefusal why = state_ == UiInputScriptState::WaitingForScreen
                ? UiInputScriptRefusal::SignatureWaitTimeout : UiInputScriptRefusal::ScreenChangeTimeout;
            return stop(UiInputScriptState::TimedOut, why);
        }

        const ScreenSignatureMatch match = match_screen_signature(
            luma, luma_count, action_.screen, action_.timing.max_mean_error);
        if (match.refusal != ScreenSignatureRefusal::None)
            return stop(UiInputScriptState::Aborted, map_refusal(match.refusal));

        switch (state_) {
        case UiInputScriptState::WaitingForScreen:
            if (match.matched) {
                increment(match_count_);
                if (match_count_ >= action_.timing.stable_frames) return press(now_us, match.mean_absolute_error);
            } else {
                match_count_ = 0;
            }
            return result(UiInputScriptEdge::None, false, match.mean_absolute_error);

        case UiInputScriptState::Holding:
            if (match.matched) {
                different_count_ = 0;
            } else {
                increment(different_count_);
                if (different_count_ >= action_.timing.stable_frames) {
                    state_ = UiInputScriptState::Complete;
                    return result(UiInputScriptEdge::Release, true, match.mean_absolute_error);
                }
            }
            if (now_us - pressed_at_us_ >= action_.timing.hold_us) {
                state_ = UiInputScriptState::WaitingForChange;
                released_at_us_ = now_us;
                return result(UiInputScriptEdge::Release, false, match.mean_absolute_error);
            }
            return result(UiInputScriptEdge::None, false, match.mean_absolute_error);

        case UiInputScriptState::WaitingForChange:
            if (match.matched) {
                different_count_ = 0;
                if (now_us - released_at_us_ >= action_.timing.retry_gap_us)
                    return press(now_us, match.mean_absolute_error);
            } else {
                increment(different_count_);
                if (different_count_ >= action_.timing.stable_frames) {
                    state_ = UiInputScriptState::Complete;
                    return result(UiInputScriptEdge::None, true, match.mean_absolute_error);
                }
            }
            return result(UiInputScriptEdge::None, false, match.mean_absolute_error);

        case UiInputScriptState::Disabled:
        case UiInputScriptState::Complete:
        case UiInputScriptState::TimedOut:
        case UiInputScriptState::Aborted:
        case UiInputScriptState::Refused:
            return result(UiInputScriptEdge::None, false, match.mean_absolute_error);
        }
        return result();
    }

    UiInputScriptState state() const { return state_; }
    UiInputScriptRefusal refusal() const { return refusal_; }

private:
    static UiInputScriptRefusal validate(const UiInputScriptAction& action) {
        if (!action.has_screen_signature) return UiInputScriptRefusal::MissingSignature;
        if (action.button == 0 || (action.button & (action.button - 1)) != 0)
            return UiInputScriptRefusal::InvalidButtonMask;
        if (action.timing.max_mean_error > 32) return UiInputScriptRefusal::InvalidTolerance;
        if (action.timing.hold_us == 0 || action.timing.hold_us > kMaxHoldUs)
            return UiInputScriptRefusal::InvalidHoldDuration;
        if (action.timing.retry_gap_us == 0 || action.timing.retry_gap_us > kMaxRetryGapUs)
            return UiInputScriptRefusal::InvalidRetryGap;
        if (action.timing.timeout_us <= action.timing.hold_us || action.timing.timeout_us > kMaxTimeoutUs)
            return UiInputScriptRefusal::InvalidTimeout;
        if (action.timing.stable_frames == 0 || action.timing.stable_frames > kMaxStableFrames)
            return UiInputScriptRefusal::InvalidStability;
        return UiInputScriptRefusal::None;
    }

    static void increment(std::uint8_t& value) {
        if (value != 0xff) ++value;
    }

    static UiInputScriptRefusal map_refusal(ScreenSignatureRefusal refusal) {
        switch (refusal) {
        case ScreenSignatureRefusal::None: return UiInputScriptRefusal::None;
        case ScreenSignatureRefusal::MissingPixels: return UiInputScriptRefusal::MissingPixels;
        case ScreenSignatureRefusal::WrongSampleCount: return UiInputScriptRefusal::WrongSampleCount;
        case ScreenSignatureRefusal::ToleranceTooLoose: return UiInputScriptRefusal::ToleranceTooLoose;
        }
        return UiInputScriptRefusal::WrongSampleCount;
    }

    bool terminal() const {
        return state_ == UiInputScriptState::Complete || state_ == UiInputScriptState::TimedOut ||
               state_ == UiInputScriptState::Aborted;
    }

    UiInputScriptResult result(UiInputScriptEdge edge = UiInputScriptEdge::None, bool changed = false,
                               std::uint16_t mean_error = 0) const {
        UiInputScriptResult out;
        out.state = state_;
        out.buttons = state_ == UiInputScriptState::Holding ? action_.button : 0;
        out.edge = edge;
        out.refusal = refusal_;
        out.mean_absolute_error = mean_error;
        out.screen_changed = changed;
        return out;
    }

    UiInputScriptResult press(std::uint64_t now_us, std::uint16_t mean_error) {
        state_ = UiInputScriptState::Holding;
        pressed_at_us_ = now_us;
        match_count_ = 0;
        different_count_ = 0;
        return result(UiInputScriptEdge::Press, false, mean_error);
    }

    UiInputScriptResult stop(UiInputScriptState terminal_state, UiInputScriptRefusal why) {
        const bool was_holding = state_ == UiInputScriptState::Holding;
        state_ = terminal_state;
        refusal_ = why;
        return result(was_holding ? UiInputScriptEdge::Release : UiInputScriptEdge::None);
    }

    UiInputScriptAction action_{};
    std::uint64_t armed_at_us_ = 0;
    std::uint64_t last_time_us_ = 0;
    std::uint64_t pressed_at_us_ = 0;
    std::uint64_t released_at_us_ = 0;
    std::uint8_t match_count_ = 0;
    std::uint8_t different_count_ = 0;
    bool have_time_ = false;
    UiInputScriptState state_ = UiInputScriptState::Disabled;
    UiInputScriptRefusal refusal_ = UiInputScriptRefusal::None;
};

}  // namespace palmetto