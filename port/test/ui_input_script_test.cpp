// Pure synthetic tests for the opt-in screen-state input block. No presenter,
// GameInput shim, SDL, controller, GPU, title image, or game data is used.
#include "../src/ui_input_script.h"

#include <cstdio>
#include <iterator>

using namespace palmetto;

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

ScreenSignature flat(std::uint8_t value) {
    ScreenSignature signature;
    signature.luma.fill(value);
    return signature;
}

UiInputScriptAction action_for(const ScreenSignature& screen) {
    UiInputScriptAction action;
    action.screen = screen;
    action.has_screen_signature = true;
    action.button = UiInputScript::kButtonA;
    action.timing.hold_us = 300'000;
    action.timing.retry_gap_us = 1'200'000;
    action.timing.timeout_us = 10'000'000;
    action.timing.stable_frames = 2;
    action.timing.max_mean_error = 0;
    return action;
}

int test_exact_switch_and_default_off() {
    const UiInputScriptSwitch unset = UiInputScriptSwitch::parse(nullptr);
    const UiInputScriptSwitch empty = UiInputScriptSwitch::parse("");
    const UiInputScriptSwitch off = UiInputScriptSwitch::parse("off");
    const UiInputScriptSwitch zero = UiInputScriptSwitch::parse("0");
    EXPECT(!unset.enabled && unset.refusal == UiInputScriptRefusal::None);
    EXPECT(!empty.enabled && !off.enabled && !zero.enabled);
    EXPECT(UiInputScriptSwitch::parse("1").enabled);
    EXPECT(!UiInputScriptSwitch::parse("true").enabled);
    EXPECT(UiInputScriptSwitch::parse("true").refusal == UiInputScriptRefusal::InvalidSwitchValue);
    EXPECT(UiInputScriptSwitch::parse("01").refusal == UiInputScriptRefusal::InvalidSwitchValue);
    EXPECT(UiInputScriptSwitch::parse("1", true).refusal == UiInputScriptRefusal::LegacyScheduleConflict);

    const ScreenSignature screen = flat(64);
    const UiInputScriptAction action = action_for(screen);
    UiInputScript script(unset, action, 100);
    const UiInputScriptResult result = script.advance(100, screen.luma.data(), screen.luma.size());
    EXPECT(result.state == UiInputScriptState::Disabled);
    EXPECT(result.buttons == 0 && result.edge == UiInputScriptEdge::None);
    EXPECT(result.refusal == UiInputScriptRefusal::None);
    return 0;
}

int test_hold_release_retry_until_screen_changes() {
    constexpr std::uint64_t start = 1'000'000;
    const ScreenSignature screen = flat(64);
    const UiInputScriptAction action = action_for(screen);
    UiInputScript script(UiInputScriptSwitch::parse("1"), action, start);

    UiInputScriptResult r = script.advance(start, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForScreen && r.buttons == 0);
    r = script.advance(start + 1'000, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.buttons == UiInputScript::kButtonA);
    EXPECT(r.edge == UiInputScriptEdge::Press);

    r = script.advance(start + 300'999, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.buttons == UiInputScript::kButtonA);
    EXPECT(r.edge == UiInputScriptEdge::None);
    const std::uint64_t released_at = start + 301'000;
    r = script.advance(released_at, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForChange && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release);

    r = script.advance(released_at + 1'199'999, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForChange && r.buttons == 0);
    r = script.advance(released_at + 1'200'000, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.buttons == UiInputScript::kButtonA);
    EXPECT(r.edge == UiInputScriptEdge::Press);

    const ScreenSignature changed = flat(100);
    r = script.advance(released_at + 1'210'000, changed.luma.data(), changed.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.buttons == UiInputScript::kButtonA);
    r = script.advance(released_at + 1'220'000, changed.luma.data(), changed.luma.size());
    EXPECT(r.state == UiInputScriptState::Complete && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release && r.screen_changed);
    r = script.advance(released_at + 1'230'000, changed.luma.data(), changed.luma.size());
    EXPECT(r.state == UiInputScriptState::Complete && r.buttons == 0 && r.edge == UiInputScriptEdge::None);
    return 0;
}

int test_signature_wait_timeout_and_release_timeout() {
    const ScreenSignature screen = flat(64);
    const ScreenSignature other = flat(20);
    UiInputScriptAction wait_action = action_for(screen);
    wait_action.timing.hold_us = 200;
    wait_action.timing.retry_gap_us = 100;
    wait_action.timing.timeout_us = 500;
    wait_action.timing.stable_frames = 1;
    UiInputScript waiting(UiInputScriptSwitch::parse("1"), wait_action, 100);
    UiInputScriptResult r = waiting.advance(599, other.luma.data(), other.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForScreen);
    r = waiting.advance(600, other.luma.data(), other.luma.size());
    EXPECT(r.state == UiInputScriptState::TimedOut && r.buttons == 0);
    EXPECT(r.refusal == UiInputScriptRefusal::SignatureWaitTimeout);

    UiInputScript retrying(UiInputScriptSwitch::parse("1"), wait_action, 1'000);
    r = retrying.advance(1'000, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.edge == UiInputScriptEdge::Press);
    r = retrying.advance(1'200, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForChange && r.edge == UiInputScriptEdge::Release);
    r = retrying.advance(1'300, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.edge == UiInputScriptEdge::Press);
    r = retrying.advance(1'500, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::TimedOut && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release);
    EXPECT(r.refusal == UiInputScriptRefusal::ScreenChangeTimeout);
    return 0;
}

int test_abort_and_bad_sample_release() {
    const ScreenSignature screen = flat(64);
    const UiInputScriptAction action = action_for(screen);
    UiInputScript aborted(UiInputScriptSwitch::parse("1"), action, 10);
    UiInputScriptResult r = aborted.advance(10, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForScreen);
    r = aborted.advance(11, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding && r.edge == UiInputScriptEdge::Press);
    r = aborted.advance(12, screen.luma.data(), screen.luma.size(), true);
    EXPECT(r.state == UiInputScriptState::Aborted && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release && r.refusal == UiInputScriptRefusal::ExternalAbort);

    UiInputScript bad_sample(UiInputScriptSwitch::parse("1"), action, 20);
    bad_sample.advance(20, screen.luma.data(), screen.luma.size());
    r = bad_sample.advance(21, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding);
    r = bad_sample.advance(22, nullptr, screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Aborted && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release && r.refusal == UiInputScriptRefusal::MissingPixels);

    UiInputScript wrong_count(UiInputScriptSwitch::parse("1"), action, 30);
    r = wrong_count.advance(30, screen.luma.data(), screen.luma.size() - 1);
    EXPECT(r.state == UiInputScriptState::Aborted);
    EXPECT(r.refusal == UiInputScriptRefusal::WrongSampleCount);
    return 0;
}

int test_clock_validation_and_action_refusals() {
    const ScreenSignature screen = flat(64);
    const UiInputScriptAction valid = action_for(screen);
    UiInputScript backwards(UiInputScriptSwitch::parse("1"), valid, 100);
    UiInputScriptResult r = backwards.advance(100, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::WaitingForScreen);
    r = backwards.advance(101, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Holding);
    r = backwards.advance(100, screen.luma.data(), screen.luma.size());
    EXPECT(r.state == UiInputScriptState::Aborted && r.buttons == 0);
    EXPECT(r.edge == UiInputScriptEdge::Release && r.refusal == UiInputScriptRefusal::ClockMovedBackwards);

    UiInputScriptAction invalid = valid;
    invalid.has_screen_signature = false;
    UiInputScript refused_signature(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_signature.refusal() == UiInputScriptRefusal::MissingSignature);
    invalid = valid;
    invalid.button = 0;
    UiInputScript refused_button(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_button.state() == UiInputScriptState::Refused);
    EXPECT(refused_button.refusal() == UiInputScriptRefusal::InvalidButtonMask);
    invalid = valid;
    invalid.button = UiInputScript::kButtonA | UiInputScript::kButtonMenu;
    UiInputScript refused_mask(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_mask.refusal() == UiInputScriptRefusal::InvalidButtonMask);
    invalid = valid;
    invalid.timing.max_mean_error = 33;
    UiInputScript refused_tolerance(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_tolerance.refusal() == UiInputScriptRefusal::InvalidTolerance);
    invalid = valid;
    invalid.timing.hold_us = 0;
    UiInputScript refused_hold(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_hold.refusal() == UiInputScriptRefusal::InvalidHoldDuration);
    invalid = valid;
    invalid.timing.retry_gap_us = 0;
    UiInputScript refused_gap(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_gap.refusal() == UiInputScriptRefusal::InvalidRetryGap);
    invalid = valid;
    invalid.timing.timeout_us = invalid.timing.hold_us;
    UiInputScript refused_timeout(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_timeout.refusal() == UiInputScriptRefusal::InvalidTimeout);
    invalid = valid;
    invalid.timing.stable_frames = 0;
    UiInputScript refused_stability(UiInputScriptSwitch::parse("1"), invalid, 0);
    EXPECT(refused_stability.refusal() == UiInputScriptRefusal::InvalidStability);
    return 0;
}

}  // namespace

int main() {
    struct Case { const char* name; int (*run)(); };
    const Case cases[] = {
        {"exact opt-in and default off", test_exact_switch_and_default_off},
        {"hold, release, retry and stable screen change", test_hold_release_retry_until_screen_changes},
        {"signature wait and screen-change timeouts", test_signature_wait_timeout_and_release_timeout},
        {"abort and invalid sample release", test_abort_and_bad_sample_release},
        {"clock and action validation", test_clock_validation_and_action_refusals},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const int failed = c.run();
        std::printf("%s: %s\n", c.name, failed ? "FAIL" : "ok");
        failures += failed;
    }
    const int total = static_cast<int>(std::size(cases));
    std::printf("ui_input_script: %d of %d cases pass (synthetic frames only)\n", total - failures, total);
    return failures ? 1 : 0;
}