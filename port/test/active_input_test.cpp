// ActiveInputTracker (src/active_input.h) on synthetic samples with its own clock: what counts as deliberate input
// (edges only, thresholds, baselines), the minimum time between changes, several devices of one family, a device that
// goes and comes back, and a forced family. Also the PALMETTO_FAKE_INPUT schedule (src/fake_input.h): when the shim's
// synthetic pad holds its A button, and when the schedule is over. No SDL, no controller, no GPU.
#include "../src/active_input.h"
#include "../src/fake_input.h"

#include <cstdio>
#include <iterator>
#include <string_view>

using namespace palmetto;

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr std::uint64_t ms = 1000;  // the tracker's clock is in microseconds

InputSample rest() { return {}; }

InputSample button(std::uint32_t bits) {
    InputSample s;
    s.buttons = bits;
    return s;
}

InputSample left_stick(float x, float y) {
    InputSample s;
    s.left_x = x;
    s.left_y = y;
    return s;
}

InputSample right_stick(float x, float y) {
    InputSample s;
    s.right_x = x;
    s.right_y = y;
    return s;
}

InputSample triggers(float left, float right) {
    InputSample s;
    s.left_trigger = left;
    s.right_trigger = right;
    return s;
}

// Device keys and families used below.
constexpr std::uint64_t xbox = 1, dualsense = 2, ds4 = 3, keyboard = 4, xbox2 = 5, switch_pro = 6;

int test_start_and_baseline() {
    ActiveInputTracker t;
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 0 && !t.forced());

    // A first sample is a baseline, whatever it holds: a pad connecting with a button down changes nothing.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(1), 0) == InputEdge::None);
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 0);
    // Still held: no edge.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(1), 10 * ms) == InputEdge::None);
    // Released, then pressed again: that press is the first deliberate input, and the first change needs no wait.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, rest(), 20 * ms) == InputEdge::None);
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(1), 30 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::PlayStation5 && t.generation() == 1);
    return 0;
}

int test_drift_and_thresholds() {
    ActiveInputTracker t;
    EXPECT(t.observe(xbox, InputFamily::Xbox, rest(), 0) == InputEdge::None);
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, rest(), 0) == InputEdge::None);

    // A worn stick drifting around inside the radius never counts, however long it wanders.
    for (int i = 0; i < 200; ++i) {
        const float d = 0.05f + 0.25f * static_cast<float>(i % 7) / 6.0f;  // 0.05 .. 0.30
        EXPECT(t.observe(dualsense, InputFamily::PlayStation5, left_stick(d, -d * 0.5f), (1 + i) * ms) ==
               InputEdge::None);
    }
    // Just inside the radius (0.35 along a diagonal is 0.247 per axis): nothing.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, left_stick(0.24f, 0.24f), 300 * ms) == InputEdge::None);
    // Leaving it is an edge.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, left_stick(0.3f, 0.3f), 310 * ms) == InputEdge::Stick);
    EXPECT(t.active() == InputFamily::PlayStation5 && t.generation() == 1);
    // Held over the radius: no further edge from the same stick.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, left_stick(1.0f, 0.0f), 320 * ms) == InputEdge::None);

    // Triggers: resting at 0.29 is not a pull; rising past 0.3 is.
    EXPECT(t.observe(xbox, InputFamily::Xbox, triggers(0.29f, 0.0f), 700 * ms) == InputEdge::None);
    EXPECT(t.active() == InputFamily::PlayStation5);
    EXPECT(t.observe(xbox, InputFamily::Xbox, triggers(0.31f, 0.0f), 710 * ms) == InputEdge::Trigger);
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 2);
    // A trigger held further down does not count again.
    EXPECT(t.observe(xbox, InputFamily::Xbox, triggers(0.9f, 0.0f), 720 * ms) == InputEdge::None);
    return 0;
}

int test_min_switch_time() {
    ActiveInputTracker t;
    t.observe(xbox, InputFamily::Xbox, rest(), 0);
    t.observe(dualsense, InputFamily::PlayStation5, rest(), 0);

    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(2), 1000 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::PlayStation5 && t.generation() == 1);
    // 100 ms later the Xbox pad's press is inside the window: ignored, and not remembered for later.
    EXPECT(t.observe(xbox, InputFamily::Xbox, button(1), 1100 * ms) == InputEdge::None);
    EXPECT(t.active() == InputFamily::PlayStation5);
    EXPECT(t.observe(xbox, InputFamily::Xbox, button(1), 1400 * ms) == InputEdge::None);  // still held: no edge
    EXPECT(t.active() == InputFamily::PlayStation5);
    // Released and pressed again after the window: that one counts.
    t.observe(xbox, InputFamily::Xbox, rest(), 1500 * ms);
    EXPECT(t.observe(xbox, InputFamily::Xbox, button(1), 1600 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 2);
    // Exactly at the window's end counts (the window is "less than min_switch_us").
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(2 | 4), 1900 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::PlayStation5 && t.generation() == 3);

    // A zero window lets every deliberate input switch at once.
    ActiveInputTracker fast(InputFamily::Xbox, {0.35f, 0.3f, 0});
    fast.observe(xbox, InputFamily::Xbox, rest(), 0);
    fast.observe(keyboard, InputFamily::Keyboard, rest(), 0);
    EXPECT(fast.observe(keyboard, InputFamily::Keyboard, button(8), 1) == InputEdge::Button);
    EXPECT(fast.observe(xbox, InputFamily::Xbox, button(8), 2) == InputEdge::Button);
    EXPECT(fast.active() == InputFamily::Xbox && fast.generation() == 2);
    return 0;
}

int test_same_family_and_holds() {
    ActiveInputTracker t;
    for (std::uint64_t d : {xbox, xbox2}) t.observe(d, InputFamily::Xbox, rest(), 0);
    t.observe(keyboard, InputFamily::Keyboard, rest(), 0);

    // Input from a second pad of the family already in use changes nothing.
    EXPECT(t.observe(xbox2, InputFamily::Xbox, button(1), 10 * ms) == InputEdge::None);
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 0);

    // The keyboard pad: W held pushes the left stick to full. The first push is the edge; holding it is not.
    EXPECT(t.observe(keyboard, InputFamily::Keyboard, left_stick(0.0f, 1.0f), 20 * ms) == InputEdge::Stick);
    EXPECT(t.active() == InputFamily::Keyboard && t.generation() == 1);
    // The player picks up the Xbox pad while W is still held down: the pad takes over after the window and the held
    // key does not take it back.
    EXPECT(t.observe(xbox, InputFamily::Xbox, button(1), 400 * ms) == InputEdge::Button);
    for (int i = 0; i < 50; ++i)
        EXPECT(t.observe(keyboard, InputFamily::Keyboard, left_stick(0.0f, 1.0f), (410 + 10 * i) * ms) ==
               InputEdge::None);
    EXPECT(t.active() == InputFamily::Xbox && t.generation() == 2);

    // The mouse moves the keyboard pad's right stick: a small jitter stays inside the radius, a real movement
    // leaves it.
    t.observe(keyboard, InputFamily::Keyboard, rest(), 1000 * ms);
    EXPECT(t.observe(keyboard, InputFamily::Keyboard, right_stick(0.03f, 0.0f), 1010 * ms) == InputEdge::None);
    EXPECT(t.observe(keyboard, InputFamily::Keyboard, right_stick(0.5f, -0.1f), 1020 * ms) == InputEdge::Stick);
    EXPECT(t.active() == InputFamily::Keyboard && t.generation() == 3);
    return 0;
}

int test_reconnect() {
    ActiveInputTracker t;
    t.observe(ds4, InputFamily::PlayStation4, rest(), 0);
    EXPECT(t.observe(ds4, InputFamily::PlayStation4, button(1), 10 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::PlayStation4);

    // Unplugged while the button was down; it comes back with the button still down. The new first sample is a
    // baseline, so there is no edge to count, and no stale state from before.
    t.forget(ds4);
    t.observe(switch_pro, InputFamily::Nintendo, rest(), 0);
    EXPECT(t.observe(switch_pro, InputFamily::Nintendo, button(1), 500 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::Nintendo);
    EXPECT(t.observe(ds4, InputFamily::PlayStation4, button(1), 900 * ms) == InputEdge::None);
    EXPECT(t.active() == InputFamily::Nintendo);
    t.observe(ds4, InputFamily::PlayStation4, rest(), 910 * ms);
    EXPECT(t.observe(ds4, InputFamily::PlayStation4, button(1), 920 * ms) == InputEdge::Button);
    EXPECT(t.active() == InputFamily::PlayStation4 && t.generation() == 3);
    return 0;
}

int test_forced() {
    ActiveInputTracker t;
    t.observe(dualsense, InputFamily::PlayStation5, rest(), 0);
    t.observe(keyboard, InputFamily::Keyboard, rest(), 0);

    t.force(InputFamily::Keyboard);
    EXPECT(t.forced() && t.active() == InputFamily::Keyboard && t.generation() == 1);
    // Forcing the same family again changes nothing.
    t.force(InputFamily::Keyboard);
    EXPECT(t.generation() == 1);

    // Tracking still runs underneath: the DualSense becomes the tracked family, but the answer stays forced and the
    // generation does not move.
    EXPECT(t.observe(dualsense, InputFamily::PlayStation5, button(1), 10 * ms) == InputEdge::Button);
    EXPECT(t.tracked() == InputFamily::PlayStation5 && t.active() == InputFamily::Keyboard && t.generation() == 1);

    // Clearing the force goes straight back to the device in use.
    t.force(std::nullopt);
    EXPECT(!t.forced() && t.active() == InputFamily::PlayStation5 && t.generation() == 2);

    // Forcing the family that is already tracked does not change the answer, so no new generation.
    t.force(InputFamily::PlayStation5);
    EXPECT(t.forced() && t.active() == InputFamily::PlayStation5 && t.generation() == 2);
    t.force(std::nullopt);
    EXPECT(t.generation() == 2);
    return 0;
}

int test_names() {
    EXPECT(std::string_view(input_family_name(InputFamily::PlayStation5)) == "PlayStation 5");
    EXPECT(std::string_view(input_family_name(InputFamily::Keyboard)) == "keyboard and mouse");
    EXPECT(std::string_view(input_edge_name(InputEdge::Stick)) == "stick movement");
    return 0;
}

// ---- PALMETTO_FAKE_INPUT: the synthetic press schedule (src/fake_input.h) ----------------------------------------

int test_fake_input_parse() {
    // Unset or empty is off; the value is the delay before the first press in seconds.
    EXPECT(!FakeInputSchedule::parse(nullptr).has_value());
    EXPECT(!FakeInputSchedule::parse("").has_value());
    EXPECT(FakeInputSchedule::parse("0").has_value() && FakeInputSchedule::parse("0")->press_at_us() == 0);
    EXPECT(FakeInputSchedule::parse("30")->press_at_us() == 30 * 1000 * 1000);
    EXPECT(FakeInputSchedule::parse("30.5")->press_at_us() == 30 * 1000 * 1000 + 500 * 1000);
    // Anything that is not a finite non-negative number of seconds is refused, so a typo never becomes a press.
    for (const char* bad : {"abc", "-1", "-0.5", "30s", "1e", "inf", "nan", "0x10", "1,5", "30 40"})
        EXPECT(!FakeInputSchedule::parse(bad).has_value());
    // Defaults: six 150 ms presses 2 s apart, the first at the delay.
    const FakeInputSchedule s = *FakeInputSchedule::parse("5");
    EXPECT(s.burst_us() == FakeInputSchedule::kDefaultBurstUs);
    EXPECT(s.interval_us() == FakeInputSchedule::kDefaultIntervalUs);
    EXPECT(s.presses() == FakeInputSchedule::kDefaultPresses);
    return 0;
}

int test_fake_input_press_windows() {
    constexpr std::uint64_t sec = 1000 * 1000;
    const FakeInputSchedule s(30 * sec);  // first press at 30 s, 150 ms each, six of them 2 s apart

    // Waiting for the first press: the button is up and the schedule is not over.
    EXPECT(!s.pressed_at(0));
    EXPECT(!s.pressed_at(30 * sec - 1));
    EXPECT(!s.finished_at(0));
    // The press window is [start, start + burst): the first sample at the start reads pressed, the first sample at
    // the end does not.
    EXPECT(s.pressed_at(30 * sec));
    EXPECT(s.press_at(30 * sec) == 0u);
    EXPECT(s.pressed_at(30 * sec + FakeInputSchedule::kDefaultBurstUs - 1));
    EXPECT(!s.pressed_at(30 * sec + FakeInputSchedule::kDefaultBurstUs));
    // Released between two presses, pressed again when the next one starts a poll later.
    EXPECT(!s.pressed_at(30 * sec + 1 * sec));
    EXPECT(s.pressed_at(32 * sec) && s.press_at(32 * sec) == 1u);
    // The last press starts at 30 s + 5 * 2 s and ends 150 ms later.
    EXPECT(s.pressed_at(40 * sec + 100 * 1000));
    EXPECT(s.press_at(40 * sec) == 5u);
    EXPECT(!s.pressed_at(40 * sec + FakeInputSchedule::kDefaultBurstUs));
    EXPECT(s.ends_at_us() == 40 * sec + FakeInputSchedule::kDefaultBurstUs);
    EXPECT(!s.finished_at(s.ends_at_us() - 1));
    EXPECT(s.finished_at(s.ends_at_us()));
    EXPECT(!s.pressed_at(s.ends_at_us() + 60 * sec));

    // A zero delay presses at once (a schedule clock that starts at the pad's attach).
    EXPECT(FakeInputSchedule(0).pressed_at(0));
    // A short burst and interval on a custom schedule: press 0 at 0, press 1 at 1000 us; 500 us is between them.
    const FakeInputSchedule fast(0, 100, 1000, 2);
    EXPECT(fast.pressed_at(0) && fast.press_at(0) == 0u);
    EXPECT(!fast.pressed_at(500));
    EXPECT(fast.pressed_at(1000) && fast.press_at(1000) == 1u);
    EXPECT(!fast.pressed_at(1100));
    EXPECT(fast.finished_at(1100));
    // A schedule with no presses never holds the button and is over immediately.
    const FakeInputSchedule none(0, 100, 1000, 0);
    EXPECT(!none.pressed_at(0) && !none.pressed_at(5000));
    EXPECT(none.finished_at(0) && none.ends_at_us() == 0);
    return 0;
}

}  // namespace

int main() {
    struct Case {
        const char* name;
        int (*run)();
    };
    const Case cases[] = {
        {"start and baseline", test_start_and_baseline},
        {"drift and thresholds", test_drift_and_thresholds},
        {"minimum time between changes", test_min_switch_time},
        {"one family, held input", test_same_family_and_holds},
        {"reconnect", test_reconnect},
        {"forced family", test_forced},
        {"names", test_names},
        {"fake input parse", test_fake_input_parse},
        {"fake input press windows", test_fake_input_press_windows},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const int r = c.run();
        std::printf("%s: %s\n", c.name, r ? "FAIL" : "ok");
        failures += r;
    }
    const int total = static_cast<int>(std::size(cases));
    std::printf("active input: %d of %d cases pass\n", total - failures, total);
    return failures ? 1 : 0;
}
