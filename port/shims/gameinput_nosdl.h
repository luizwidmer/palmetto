// gameinput_nosdl.h: the always-built, no-SDL GameInput backend and the device source it reads - XInput pads
// (any controller Windows exposes through xinput1_4.dll / xinput9_1_0.dll) plus a keyboard/mouse-as-pad fallback, a
// hot-plug poll, a deterministic scripted-press schedule for first-run screens, and a bounded trace.
//
// This header is include-only on purpose: it is not a build target of its own. `shims/gameinput.cpp` includes it as
// the whole shim when the build has no SDL3, and `test/gameinput_nosdl_test.cpp` includes it to drive the source
// layer (mapping, hot-plug, readings, keyboard pad, schedule) with a fake XInput source: no controller, no game data.
// The GameInput ABI classes at the bottom are the same v0 interface `gameinput_abi.h` declares and the SDL shim
// answers; a title reaches them through the registry entry `gameinput.cpp` adds for `gameinput.dll!#1`.
//
// The claim this slice tests (docs/input_backend.md): a public peer README says its loader "answers its first-run
// screens through a scripted gamepad", and our own measurement of the loading loop is one `GameInputCreate` and one
// `RegisterDeviceCallback` with no reading ever taken (docs/input_gate.md). This backend makes the runtime side
// honest and observable: a device is present and paired, the scripted schedule presses confirm/start about once a
// second, and `PALMETTO_INPUT_TRACE=1` logs every call that matters. Whether the title reads any of it is measured by
// the A/B boot, not assumed here.
//
// Switches (all read from the environment once, on the first GameInputCreate; the names before PALMETTO_PAD_PRESENT
// are already documented in docs/gameinput.md and docs/input_gate.md, and PALMETTO_PAD_PRESENT is documented by the
// first-run slice, docs/decomp_first_ui.md section 12):
//   PALMETTO_PAD_PRESENT=0|off             removes the paired pad the always-built backend presents by default. A
//                                         console boot always has a pad connected; an unattended host does not, and
//                                         the measured A/B (docs/input_backend.md) shows the title only takes its
//                                         first-run reading when a device is present. The default is one neutral pad
//                                         (no presses); the switch is the fallback.
//   PALMETTO_KEYBOARD_PAD=1|focus|always  the keyboard/mouse pad, off by default (same meaning as the SDL shim)
//   PALMETTO_FAKE_INPUT=<seconds>         the legacy 30-press diagnostic schedule: confirm then start, about once a
//                                         second, each held 150 ms. The delay is the first press.
//   PALMETTO_FIRST_RUN_SCRIPT=<seconds>   opt-in A/Menu/A/Menu sequence, each held 250 ms with 250 ms neutral; the
//                                         decimal delay is relative to the first GameInputCreate, max 300 s. A `screen:`
//                                         trigger is refused until a presented-frame signature feed is measured.
//   PALMETTO_GAMEINPUT_THREAD=0           no background poll thread; only the API calls poll (tests)
//   PALMETTO_INPUT_TRACE=1                bounded call trace: call 1, 10, 100 ... per entry point plus device/press
//                                         events, with the caller as module+RVA
//
// Limits, stated where they are: the source has no rumble output (SetRumbleState is recorded, not played), no raw
// HID, no motion, no force feedback, no aggregate device and no dispatcher queue (CreateDispatcher answers
// E_NOTIMPL); device callbacks are delivered synchronously from the poll, not through the title's dispatcher. Every
// pad is presented as an Xbox One pad, as the SDL shim does.
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../src/active_input.h"
#include "../src/fake_input.h"
#include "gameinput_abi.h"
#include "shims.h"

namespace rt::gameinput::nosdl {

// ---- the device source (no Windows headers below this line until the ABI classes) -------------------------------

// Positional gamepad buttons. The values are GameInput's own (gameinput_abi.h GameInputGamepadButtons) so a
// translated pad state can be copied into the ABI state; `PadGuide` has no GameInput gamepad bit and is only used
// for the active-input edge rule.
enum PadButton : uint32_t {
    PadMenu = 0x00000001,
    PadView = 0x00000002,
    PadA = 0x00000004,
    PadB = 0x00000008,
    PadX = 0x00000010,
    PadY = 0x00000020,
    PadDPadUp = 0x00000040,
    PadDPadDown = 0x00000080,
    PadDPadLeft = 0x00000100,
    PadDPadRight = 0x00000200,
    PadLeftShoulder = 0x00000400,
    PadRightShoulder = 0x00000800,
    PadLeftThumbstick = 0x00001000,
    PadRightThumbstick = 0x00002000,
    PadGuide = 0x00004000,
};

// XInput's button bits (the public XINPUT_GAMEPAD_* values), for the default source and the fake one.
enum XInputButton : uint16_t {
    XiDPadUp = 0x0001,
    XiDPadDown = 0x0002,
    XiDPadLeft = 0x0004,
    XiDPadRight = 0x0008,
    XiStart = 0x0010,
    XiBack = 0x0020,
    XiLeftThumb = 0x0040,
    XiRightThumb = 0x0080,
    XiLeftShoulder = 0x0100,
    XiRightShoulder = 0x0200,
    XiA = 0x1000,
    XiB = 0x2000,
    XiX = 0x4000,
    XiY = 0x8000,
};

// One XInput pad as xinput1_4's XINPUT_STATE reports it: buttons, the two trigger bytes 0..255, and the four stick
// axes, where +Y is up (XInput's own convention).
struct XInputPad {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};

using XInputPollFn = bool (*)(uint32_t slot, XInputPad* out);  // false: no pad in that slot

// Where the pads come from. The default (nullptr) loads xinput1_4.dll, then xinput9_1_0.dll; a test installs its own
// and there is no controller or Windows input involved.
struct XInputSource {
    XInputPollFn poll = nullptr;
    const char* name = "none";
};
void set_xinput_source(const XInputSource* source);  // nullptr restores the default
const XInputSource& xinput_source();

// The default XInput source and the default keyboard source (implemented below, after the source layer): declared
// here so the source layer can read the current one.
struct KeyboardSource;
namespace detail {
bool default_xinput_poll(uint32_t slot, XInputPad* out);
const KeyboardSource& default_keyboard(void);
}  // namespace detail

// GameInput's gamepad shape: buttons in PadButton bits, triggers 0..1, sticks -1..1 with +Y up.
struct PadState {
    uint32_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
inline bool same_pad(const PadState& a, const PadState& b) { return std::memcmp(&a, &b, sizeof(PadState)) == 0; }

inline float stick_axis(int16_t v) {
    if (v >= 0) return static_cast<float>(v) / 32767.0f;
    return v <= -32767 ? -1.0f : static_cast<float>(v) / 32767.0f;
}

// XInput state -> GameInput pad state. Buttons are positional (A is the south face button), so the mapping is a
// renaming, not a per-device table; sticks keep XInput's sign, where +Y is up, which is GameInput's too.
inline PadState translate_xinput(const XInputPad& in) {
    PadState out;
    if (in.buttons & XiA) out.buttons |= PadA;
    if (in.buttons & XiB) out.buttons |= PadB;
    if (in.buttons & XiX) out.buttons |= PadX;
    if (in.buttons & XiY) out.buttons |= PadY;
    if (in.buttons & XiDPadUp) out.buttons |= PadDPadUp;
    if (in.buttons & XiDPadDown) out.buttons |= PadDPadDown;
    if (in.buttons & XiDPadLeft) out.buttons |= PadDPadLeft;
    if (in.buttons & XiDPadRight) out.buttons |= PadDPadRight;
    if (in.buttons & XiLeftShoulder) out.buttons |= PadLeftShoulder;
    if (in.buttons & XiRightShoulder) out.buttons |= PadRightShoulder;
    if (in.buttons & XiLeftThumb) out.buttons |= PadLeftThumbstick;
    if (in.buttons & XiRightThumb) out.buttons |= PadRightThumbstick;
    if (in.buttons & XiStart) out.buttons |= PadMenu;
    if (in.buttons & XiBack) out.buttons |= PadView;
    out.left_trigger = in.left_trigger / 255.0f;
    out.right_trigger = in.right_trigger / 255.0f;
    out.left_x = stick_axis(in.left_x);
    out.left_y = stick_axis(in.left_y);
    out.right_x = stick_axis(in.right_x);
    out.right_y = stick_axis(in.right_y);
    return out;
}

// The keyboard/mouse pad's sources. Each is replaceable so a test drives it with its own keys and cursor; the shim
// fills them with GetAsyncKeyState, GetAsyncKeyState(VK_LBUTTON/VK_RBUTTON), GetCursorPos and the foreground check.
struct KeyboardSource {
    bool (*key_down)(int virtual_key) = nullptr;  // Win32 virtual-key code; null means every key is up
    bool (*mouse_down)(int button) = nullptr;      // 0 left, 1 right; null means neither
    bool (*cursor)(int* x, int* y) = nullptr;      // screen pixels; false when it cannot be read
    bool (*focused)() = nullptr;                   // a window of this process is in the foreground; null means yes
};
void set_keyboard_source(const KeyboardSource* source);  // nullptr restores the default
const KeyboardSource& default_keyboard_source();

// The Win32 virtual-key codes the keyboard pad binds. They are the public virtual-key values, no header needed.
enum : int {
    kVkReturn = 0x0D,
    kVkSpace = 0x20,
    kVkBack = 0x08,
    kVkEscape = 0x1B,
    kVkTab = 0x09,
    kVkLeft = 0x25,
    kVkUp = 0x26,
    kVkRight = 0x27,
    kVkDown = 0x28,
    kVkQ = 0x51,
    kVkE = 0x45,
    kVkW = 0x57,
    kVkA = 0x41,
    kVkS = 0x53,
    kVkD = 0x44,
    kVkX = 0x58,
    kVkY = 0x59,
};

enum class KeyboardPadMode { Off, Focused, Always };

// The keyboard/mouse pad: WASD or the arrows push the left stick, the mouse's movement the right stick, Enter/Space
// are confirm (A), Backspace is cancel (B), Esc is start (Menu), Tab is View, X/Y the face buttons, Q/E the
// shoulders, the mouse buttons the triggers. A real stick reports about 0.707 on each axis of a diagonal, and this
// pad does too; opposite keys cancel. The right stick follows movement, not position: each poll keeps 63/64 of its
// value and adds 1/32 of a unit per cursor pixel, so a steady 4 pixels a poll holds it at full and it falls from
// full to exactly 0 in 64 polls (128 ms at the 2 ms poll). Those constants are a starting point, as the SDL shim's
// are (docs/gameinput.md VERIFY).
class KeyboardPad {
public:
    static constexpr float kMouseStep = 1.0f / 32.0f;  // right-stick units per cursor pixel per poll
    static constexpr float kMouseDecay = 1.0f / 64.0f;  // units removed from the right stick each poll

    // One poll of the current state. In Focused mode a loss of focus releases everything and drops the cursor
    // baseline, so the right stick does not jump when focus returns.
    PadState poll(const KeyboardSource& source, KeyboardPadMode mode) {
        PadState out;
        if (mode == KeyboardPadMode::Off) return out;
        const bool focused = !source.focused || source.focused();
        if (mode == KeyboardPadMode::Focused && !focused) {
            have_cursor_ = false;
            right_x_ = right_y_ = 0;
            return out;
        }
        auto down = [&](int vk) { return source.key_down && source.key_down(vk); };
        const bool w = down(kVkW), a = down(kVkA), s = down(kVkS), d = down(kVkD);
        const bool up = w || down(kVkUp), dn = s || down(kVkDown);
        const bool left = a || down(kVkLeft), right = d || down(kVkRight);
        float dx = static_cast<float>((right ? 1 : 0) - (left ? 1 : 0));
        float dy = static_cast<float>((up ? 1 : 0) - (dn ? 1 : 0));
        if (dx != 0 && dy != 0) {  // the round gate: a diagonal is 1/sqrt(2) on each axis, not 1
            dx *= 0.70710678f;
            dy *= 0.70710678f;
        }
        out.left_x = dx;
        out.left_y = dy;
        if (down(kVkReturn) || down(kVkSpace)) out.buttons |= PadA;
        if (down(kVkBack)) out.buttons |= PadB;
        if (down(kVkX)) out.buttons |= PadX;
        if (down(kVkY)) out.buttons |= PadY;
        if (down(kVkQ)) out.buttons |= PadLeftShoulder;
        if (down(kVkE)) out.buttons |= PadRightShoulder;
        if (down(kVkEscape)) out.buttons |= PadMenu;
        if (down(kVkTab)) out.buttons |= PadView;
        if (source.mouse_down && source.mouse_down(0)) out.right_trigger = 1.0f;  // fire
        if (source.mouse_down && source.mouse_down(1)) out.left_trigger = 1.0f;   // aim

        // The right stick: decay, then add this poll's movement.
        right_x_ -= right_x_ > 0 ? std::min(right_x_, kMouseDecay) : -std::min(-right_x_, kMouseDecay);
        right_y_ -= right_y_ > 0 ? std::min(right_y_, kMouseDecay) : -std::min(-right_y_, kMouseDecay);
        int cx = 0, cy = 0;
        if (source.cursor && source.cursor(&cx, &cy)) {
            if (have_cursor_) {
                right_x_ = std::clamp(right_x_ + (cx - last_x_) * kMouseStep, -1.0f, 1.0f);
                right_y_ = std::clamp(right_y_ - (cy - last_y_) * kMouseStep, -1.0f, 1.0f);  // screen down is -Y
            }
            last_x_ = cx;
            last_y_ = cy;
            have_cursor_ = true;
        }
        out.right_x = right_x_;
        out.right_y = right_y_;
        return out;
    }
    void reset() {
        right_x_ = right_y_ = 0;
        have_cursor_ = false;
    }
    float right_x() const { return right_x_; }
    float right_y() const { return right_y_; }

private:
    float right_x_ = 0, right_y_ = 0;
    int last_x_ = 0, last_y_ = 0;
    bool have_cursor_ = false;
};

// The scripted pad: one extra gamepad the always-built backend presents so a boot has a device to pair, as a console
// always does. Without a script it is present and neutral (no presses) - the measured presence the title's first-run
// read needs (docs/input_backend.md); PALMETTO_PAD_PRESENT=0|off removes it. PALMETTO_FAKE_INPUT remains its legacy
// 30-press diagnostic schedule. PALMETTO_FIRST_RUN_SCRIPT=<seconds> instead runs the short four-step A/Menu/A/Menu
// sequence from src/fake_input.h, with explicit release gaps. Both are backend-epoch relative, bounded and opt-in; a
// `screen:` trigger is refused because this backend has no measured frame-signature feed.
class ScriptedPresses {
public:
    static constexpr uint64_t kBurstUs = palmetto::FakeInputSchedule::kDefaultBurstUs;  // 150 ms
    static constexpr uint64_t kIntervalUs = 1'000'000;                                 // one press a second
    static constexpr unsigned kPresses = 30;                                            // bounded: about 29 s

    static std::optional<ScriptedPresses> parse(const char* text) {
        const auto delay = palmetto::FakeInputSchedule::parse(text);
        if (!delay) return std::nullopt;
        return ScriptedPresses(delay->press_at_us());
    }
    static std::optional<ScriptedPresses> parse_first_run(const char* text,
                                                          palmetto::FirstRunScriptRefusal* refusal = nullptr) {
        const auto script = palmetto::FirstRunInputScript::parse(text, refusal);
        if (!script) return std::nullopt;
        return ScriptedPresses(*script);
    }
    // The default paired pad: a schedule with no presses, so the pad is connected and neutral for the whole boot.
    static ScriptedPresses presence() { return ScriptedPresses(0, 0); }
    explicit ScriptedPresses(uint64_t first_press_us = 0, unsigned presses = kPresses)
        : schedule_(first_press_us, kBurstUs, kIntervalUs, presses) {}

    std::optional<unsigned> press_index_at(uint64_t process_us) const {
        return first_run_ ? first_run_->step_at(process_us) : schedule_.press_at(process_us);
    }
    // Both modes express GameInput button bits. The legacy switch retains its old alternating A/Menu sequence; the
    // measured-window switch uses its own four-step script and explicit neutral intervals.
    uint32_t buttons_at(uint64_t process_us) const {
        if (first_run_) return first_run_->buttons_at(process_us);
        const std::optional<unsigned> press = press_index_at(process_us);
        if (!press) return 0;
        return (*press % 2 == 0) ? PadA : PadMenu;
    }
    bool pressed_at(uint64_t process_us) const { return buttons_at(process_us) != 0; }
    bool finished_at(uint64_t process_us) const {
        return first_run_ ? first_run_->finished_at(process_us) : schedule_.finished_at(process_us);
    }
    uint64_t ends_at_us() const { return first_run_ ? first_run_->ends_at_us() : schedule_.ends_at_us(); }
    uint64_t first_press_us() const { return first_run_ ? first_run_->first_press_us() : schedule_.press_at_us(); }
    unsigned presses() const { return first_run_ ? first_run_->presses() : schedule_.presses(); }
    bool first_run_mode() const { return first_run_.has_value(); }

private:
    explicit ScriptedPresses(const palmetto::FirstRunInputScript& script) : first_run_(script) {}
    palmetto::FakeInputSchedule schedule_;
    std::optional<palmetto::FirstRunInputScript> first_run_;
};

// Which family the player is using, for the runtime's own prompt glyphs: the active-input tracker
// (src/active_input.h) fed one sample per device per poll. The keyboard pad is the Keyboard family; every XInput pad
// and the scripted pad are Xbox. The title is told every pad is an Xbox One pad, so this record is the port's own.
class ActiveInputFeed {
public:
    void observe(uint32_t device, palmetto::InputFamily family, const PadState& pad, uint64_t now_us) {
        palmetto::InputSample sample;
        sample.buttons = pad.buttons;
        sample.left_trigger = pad.left_trigger;
        sample.right_trigger = pad.right_trigger;
        sample.left_x = pad.left_x;
        sample.left_y = pad.left_y;
        sample.right_x = pad.right_x;
        sample.right_y = pad.right_y;
        tracker_.observe(device, family, sample, now_us);
    }
    palmetto::InputFamily family() const { return tracker_.active(); }
    uint64_t generation() const { return tracker_.generation(); }

private:
    palmetto::ActiveInputTracker tracker_;
};

// ---- hot-plug ---------------------------------------------------------------------------------------------------

constexpr uint32_t kXInputSlots = 4;    // XInput's own limit
constexpr uint32_t kKeyboardSlot = 4;   // the keyboard/mouse pad's device slot
constexpr uint32_t kScriptedSlot = 5;   // the scripted pad's device slot
constexpr uint32_t kDeviceSlots = 6;

enum class DeviceEventKind { Connected, Disconnected };
struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::Connected;
    uint32_t slot = 0;
};

// One pollable device slot: whether it is connected, its latest pad state, and the identity the device object
// carries. A slot that goes away and comes back keeps its id and its object (the title gets the same device back).
struct SlotState {
    uint32_t slot = 0;
    bool keyboard = false;
    bool scripted = false;
    bool connected = false;
    bool seen = false;  // has been connected at least once
    uint16_t vendor = 0, product = 0;
    PadState pad{};
};

// Polls the four XInput slots, the keyboard pad and the scripted pad, and reports connections and disconnections. A
// slot is connected exactly while its source reports it; the scripted pad is connected from the first poll after
// the schedule is installed and stays connected after the schedule ends with a neutral state.
class DeviceHub {
public:
    // `scripted_us` is the scripted schedule's clock (microseconds since the shim's epoch), not the poll clock.
    const std::vector<DeviceEvent>& poll(uint64_t scripted_us, const XInputSource& xinput, const KeyboardSource* keyboard,
                                         KeyboardPadMode keyboard_mode, KeyboardPad* keyboard_pad,
                                         const ScriptedPresses* scripted) {
        events_.clear();
        for (uint32_t s = 0; s < kXInputSlots; ++s) {
            XInputPad raw;
            const bool present = xinput.poll && xinput.poll(s, &raw);
            SlotState& slot = slots_[s];
            slot.slot = s;
            slot.keyboard = false;
            slot.scripted = false;
            if (present) {
                const PadState pad = translate_xinput(raw);
                set_connected(slot, true);
                slot.pad = pad;
            } else {
                set_connected(slot, false);
            }
        }
        SlotState& keys = slots_[kKeyboardSlot];
        keys.slot = kKeyboardSlot;
        keys.keyboard = true;
        keys.scripted = false;
        if (keyboard && keyboard_mode != KeyboardPadMode::Off) {
            set_connected(keys, true);
            keys.pad = keyboard_pad->poll(*keyboard, keyboard_mode);
        } else {
            set_connected(keys, false);
            keys.pad = {};
            keyboard_pad->reset();
        }
        SlotState& scripted_slot = slots_[kScriptedSlot];
        scripted_slot.slot = kScriptedSlot;
        scripted_slot.keyboard = false;
        scripted_slot.scripted = true;
        if (scripted) {
            set_connected(scripted_slot, true);
            scripted_slot.pad = {};
            scripted_slot.pad.buttons = scripted->buttons_at(scripted_us);
        } else {
            set_connected(scripted_slot, false);
            scripted_slot.pad = {};
        }
        return events_;
    }

    const SlotState& slot(uint32_t s) const { return slots_[s]; }
    bool connected(uint32_t s) const { return slots_[s].connected; }
    const std::vector<DeviceEvent>& events() const { return events_; }

private:
    void set_connected(SlotState& slot, bool now_connected) {
        if (now_connected == slot.connected) return;
        slot.connected = now_connected;
        slot.seen = slot.seen || now_connected;
        events_.push_back({now_connected ? DeviceEventKind::Connected : DeviceEventKind::Disconnected, slot.slot});
    }
    SlotState slots_[kDeviceSlots];
    std::vector<DeviceEvent> events_;
};

// ---- readings ---------------------------------------------------------------------------------------------------

// One reading of one device: the pad state at a poll, its timestamp (the shim's clock, microseconds) and a sequence
// number that goes up by one per reading on that device.
struct ReadingData {
    PadState pad{};
    uint64_t timestamp_us = 0;
    uint64_t sequence = 0;
};

// A device's reading history: a reading is appended when the pad state changes (or on connect), so a press and
// release between two of the title's frames is still reachable with GetNextReading/GetPreviousReading. Bounded to
// 128 readings; the newest is the current reading. The title can hold a reading object after it leaves the stream.
class ReadingStream {
public:
    static constexpr size_t kCap = 128;

    bool update(const PadState& pad, uint64_t now_us, bool force) {
        if (!force && !readings_.empty() && same_pad(readings_.back().pad, pad)) return false;
        ReadingData next;
        next.pad = pad;
        next.timestamp_us = now_us;
        next.sequence = ++sequence_;
        readings_.push_back(next);
        if (readings_.size() > kCap) readings_.pop_front();
        return true;
    }
    void clear() {
        readings_.clear();
        sequence_ = 0;
    }
    bool current(ReadingData* out) const {
        if (readings_.empty()) return false;
        *out = readings_.back();
        return true;
    }
    bool next(const ReadingData& reference, ReadingData* out) const {
        for (const ReadingData& r : readings_)
            if (r.sequence > reference.sequence) {
                *out = r;
                return true;
            }
        return false;
    }
    bool previous(const ReadingData& reference, ReadingData* out) const {
        bool found = false;
        for (const ReadingData& r : readings_) {
            if (r.sequence >= reference.sequence) break;
            *out = r;
            found = true;
        }
        return found;
    }
    bool find(uint64_t sequence, ReadingData* out) const {
        for (const ReadingData& r : readings_)
            if (r.sequence == sequence) {
                *out = r;
                return true;
            }
        return false;
    }
    size_t size() const { return readings_.size(); }

private:
    std::deque<ReadingData> readings_;
    uint64_t sequence_ = 0;
};

// The UI-navigation state derived from a pad: what a first-run screen would read to advance. A accept, B cancel,
// the d-pad or the left stick past half for the directions, X/Y as context 1/2, the stick clicks context 3/4, the
// shoulders page left/right, the triggers page up/down, Menu and View as themselves, the right stick scrolling. The
// ABI's UiNavigation bits are used directly so the ABI class can copy the value.
inline uint32_t ui_navigation_bits(const PadState& pad) {
    uint32_t bits = 0;
    if (pad.buttons & PadMenu) bits |= 0x00000001;   // Menu
    if (pad.buttons & PadView) bits |= 0x00000002;   // View
    if (pad.buttons & PadA) bits |= 0x00000004;      // Accept
    if (pad.buttons & PadB) bits |= 0x00000008;      // Cancel
    if ((pad.buttons & PadDPadUp) || pad.left_y > 0.5f) bits |= 0x00000010;
    if ((pad.buttons & PadDPadDown) || pad.left_y < -0.5f) bits |= 0x00000020;
    if ((pad.buttons & PadDPadLeft) || pad.left_x < -0.5f) bits |= 0x00000040;
    if ((pad.buttons & PadDPadRight) || pad.left_x > 0.5f) bits |= 0x00000080;
    if (pad.buttons & PadX) bits |= 0x00000100;  // Context1
    if (pad.buttons & PadY) bits |= 0x00000200;  // Context2
    if (pad.buttons & PadLeftThumbstick) bits |= 0x00000400;
    if (pad.buttons & PadRightThumbstick) bits |= 0x00000800;
    if (pad.buttons & PadLeftShoulder) bits |= 0x00004000;   // PageLeft
    if (pad.buttons & PadRightShoulder) bits |= 0x00008000;  // PageRight
    if (pad.left_trigger > 0.5f) bits |= 0x00001000;         // PageUp
    if (pad.right_trigger > 0.5f) bits |= 0x00002000;        // PageDown
    if (pad.right_y > 0.5f) bits |= 0x00010000;              // ScrollUp
    if (pad.right_y < -0.5f) bits |= 0x00020000;             // ScrollDown
    if (pad.right_x < -0.5f) bits |= 0x00040000;             // ScrollLeft
    if (pad.right_x > 0.5f) bits |= 0x00080000;              // ScrollRight
    return bits;
}

// The GameInput button bits present in a PadState (PadGuide has no gamepad bit). Kept next to the button enum so the
// ABI class and the tests use one mapping.
inline uint32_t gamepad_button_bits(uint32_t pad_buttons) { return pad_buttons & 0x00003FFF; }

// ---- the default sources ----------------------------------------------------------------------------------------
//
// XInput is loaded at run time, so the shim builds and starts with no xinput import of its own: xinput1_4.dll first
// (Windows 8 and up: all four slots and the trigger bytes), then xinput9_1_0.dll (the older in-box build). The pad
// state is XINPUT_STATE's layout, which the public XInput header fixes: packet number, then buttons, two trigger
// bytes and four signed axes.

namespace detail {

struct XInputApi {
    HMODULE module = nullptr;
    DWORD (WINAPI* get_state)(DWORD slot, void* state) = nullptr;
    const char* name = "none";
};

inline XInputApi load_xinput() {
    XInputApi api;
    const struct {
        const char* name;
        HMODULE module;
    } candidates[] = {{"xinput1_4.dll", LoadLibraryA("xinput1_4.dll")},
                      {"xinput9_1_0.dll", LoadLibraryA("xinput9_1_0.dll")}};
    for (const auto& c : candidates) {
        if (!c.module) continue;
        api.module = c.module;
        api.get_state = reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(GetProcAddress(c.module, "XInputGetState"));
        if (api.get_state) {
            api.name = c.name;
            return api;
        }
        FreeLibrary(c.module);
    }
    return api;
}

inline const XInputApi& xinput_api() {
    static const XInputApi api = load_xinput();
    return api;
}

// XINPUT_STATE as the public header lays it out: dwPacketNumber, then XINPUT_GAMEPAD.
struct RawXInputState {
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
static_assert(sizeof(RawXInputState) == 16, "XINPUT_STATE is 16 bytes on x64");

inline bool default_xinput_poll(uint32_t slot, XInputPad* out) {
    if (slot >= kXInputSlots || !out) return false;
    const XInputApi& api = xinput_api();
    if (!api.get_state) return false;
    RawXInputState state;
    // ERROR_SUCCESS; anything else (1167 = no device) means the slot is empty.
    if (api.get_state(slot, &state) != 0) return false;
    out->buttons = state.buttons;
    out->left_trigger = state.left_trigger;
    out->right_trigger = state.right_trigger;
    out->left_x = state.left_x;
    out->left_y = state.left_y;
    out->right_x = state.right_x;
    out->right_y = state.right_y;
    return true;
}

inline bool default_key_down(int virtual_key) { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }
inline bool default_mouse_down(int button) { return default_key_down(button == 0 ? VK_LBUTTON : VK_RBUTTON); }
inline bool default_cursor(int* x, int* y) {
    POINT p;
    if (!GetCursorPos(&p)) return false;
    *x = p.x;
    *y = p.y;
    return true;
}
// A window of this process is in the foreground, so typing in another application presses nothing.
inline bool default_focused() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

inline const KeyboardSource& default_keyboard() {
    static const KeyboardSource source{default_key_down, default_mouse_down, default_cursor, default_focused};
    return source;
}

inline const XInputSource*& custom_xinput() {
    static const XInputSource* source = nullptr;
    return source;
}
inline const KeyboardSource*& custom_keyboard() {
    static const KeyboardSource* source = nullptr;
    return source;
}

}  // namespace detail

inline void set_xinput_source(const XInputSource* source) { detail::custom_xinput() = source; }
inline const XInputSource& xinput_source() {
    static const XInputSource kDefault{&detail::default_xinput_poll, "xinput1_4.dll"};
    const XInputSource* custom = detail::custom_xinput();
    return custom ? *custom : kDefault;
}
inline void set_keyboard_source(const KeyboardSource* source) { detail::custom_keyboard() = source; }
inline const KeyboardSource& default_keyboard_source() {
    const KeyboardSource* custom = detail::custom_keyboard();
    return custom ? *custom : detail::default_keyboard();
}

}  // namespace rt::gameinput::nosdl

// =================================================================================================================
// The GameInput v0 ABI over the source above. Everything below is Windows/COM; the source layer above is portable.
// =================================================================================================================
namespace rt::gameinput::nosdl {

class Device;
class Reading;

namespace detail {

// ---- clocks, environment and logging -----------------------------------------------------------------------------

inline uint64_t now_us() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart / freq.QuadPart * 1'000'000 +
                                 c.QuadPart % freq.QuadPart * 1'000'000 / freq.QuadPart);
}

inline std::string env(const char* name) {
    char buf[64];
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (n == 0 || n >= sizeof(buf)) return {};
    return std::string(buf, n);
}

inline bool trace_enabled() {
    static const bool on = [] {
        const std::string v = env("PALMETTO_INPUT_TRACE");
        return !v.empty() && v != "0" && v != "off";
    }();
    return on;
}

// Bounded call trace, the same budget as the SDL shim's: call 1, 10, 100 ... per entry point, so a title polling every
// frame cannot flood the log while the rate stays readable between the last two lines. The caller is resolved to a
// mapped module + RVA (`rt::shims::module_offset`), so a line names the title's own code.
inline void trace_call(const char* entry, void* caller, long long result, unsigned detail = 0) {
    if (!trace_enabled()) return;
    static std::mutex mu;
    static std::map<const char*, uint64_t> counts;
    uint64_t n = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        n = ++counts[entry];
        if (n != 1) {
            uint64_t p = 10;
            while (p < n) p *= 10;
            if (p != n) return;
        }
    }
    const std::string where = rt::shims::module_offset(reinterpret_cast<uint64_t>(caller));
    if (detail)
        std::fprintf(stderr, "[rt] gameinput: read trace: %s #%llu from %s -> 0x%llx (kind 0x%x)\n", entry,
                     static_cast<unsigned long long>(n), where.c_str(),
                     static_cast<unsigned long long>(static_cast<uint64_t>(result)), detail);
    else
        std::fprintf(stderr, "[rt] gameinput: read trace: %s #%llu from %s -> 0x%llx\n", entry,
                     static_cast<unsigned long long>(n), where.c_str(),
                     static_cast<unsigned long long>(static_cast<uint64_t>(result)));
}

// A bounded event trace for things that happen once (a device connects, the scripted schedule presses): the first 16
// of each kind are printed, then every 8th, so a long run stays bounded.
inline void trace_event(const char* what, uint32_t slot, uint32_t detail) {
    if (!trace_enabled()) return;
    static std::mutex mu;
    static std::map<const char*, uint64_t> counts;
    uint64_t n = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        n = ++counts[what];
        if (n > 16 && n % 8 != 0) return;
    }
    std::fprintf(stderr, "[rt] gameinput: nosdl trace: %s #%llu slot %u detail 0x%x\n", what,
                 static_cast<unsigned long long>(n), slot, detail);
}

inline bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

// Devices live for the process: the registry keeps every device it has seen, so a title holding one across a
// disconnect gets the same object back. The count never drops below one.
inline ULONG release_floor(std::atomic<ULONG>& refs) {
    ULONG r = refs.load();
    while (r > 1 && !refs.compare_exchange_weak(r, r - 1)) {}
    return r > 1 ? r - 1 : 1;
}

inline void make_device_id(const char* tag, uint32_t slot, gi::AppLocalDeviceId* out) {
    std::memset(out->value, 0, sizeof(out->value));
    char text[24];
    std::snprintf(text, sizeof(text), "%s:%u", tag, slot);
    const size_t n = std::min(std::strlen(text), sizeof(out->value));
    std::memcpy(out->value, text, n);
}

}  // namespace detail

// ---- backend state -----------------------------------------------------------------------------------------------

struct CallbackRecord {
    gi::GameInputCallbackToken token = 0;
    uint32_t kind_filter = 0;
    uint32_t status_filter = 0;
    gi::GameInputEnumerationKind enumeration = gi::GameInputNoEnumeration;
    void* context = nullptr;
    gi::GameInputDeviceCallback device = nullptr;
    gi::GameInputReadingCallback reading = nullptr;
    gi::GameInputGuideButtonCallback guide = nullptr;
    gi::GameInputKeyboardLayoutCallback keyboard = nullptr;
    bool active = true;
};

struct Backend {
    std::mutex mu;                     // guards everything below and the device objects' shared state
    DeviceHub hub;
    KeyboardPad keyboard_pad;
    KeyboardSource keyboard_source{};
    XInputSource xinput{};
    std::optional<ScriptedPresses> scripted;
    ActiveInputFeed active;
    std::vector<CallbackRecord> callbacks;
    uint64_t next_token = 1;
    uint64_t epoch_us = 0;
    uint64_t last_poll_us = 0;
    bool initialised = false;
    bool thread_on = true;
    bool thread_started = false;
    bool keyboard_enabled = false;
    KeyboardPadMode keyboard_mode = KeyboardPadMode::Off;
    Device* devices[kDeviceSlots] = {};
    unsigned last_press = static_cast<unsigned>(-1);  // last scripted press index traced
    std::atomic<unsigned> pending_presses{0};
};

// The process's single backend (one GameInput instance per process, as the SDL shim has).
inline Backend& backend() {
    static Backend b;
    return b;
}

// Reads the environment (first create, or a test's re-init). Does not touch the device objects.
inline void configure_from_env(Backend* b) {
    const std::string keys = detail::env("PALMETTO_KEYBOARD_PAD");
    b->keyboard_mode = KeyboardPadMode::Off;
    if (keys == "1" || keys == "focus") b->keyboard_mode = KeyboardPadMode::Focused;
    else if (keys == "always") b->keyboard_mode = KeyboardPadMode::Always;
    else if (!keys.empty() && keys != "0" && keys != "off")
        std::fprintf(stderr, "[rt] gameinput: PALMETTO_KEYBOARD_PAD=%s unknown (1, always or off), keyboard pad off\n",
                     keys.c_str());
    b->keyboard_enabled = b->keyboard_mode != KeyboardPadMode::Off;

    b->scripted.reset();
    const std::string fake = detail::env("PALMETTO_FAKE_INPUT");
    char first_run_buf[64] = {};
    const DWORD first_run_length = GetEnvironmentVariableA("PALMETTO_FIRST_RUN_SCRIPT", first_run_buf,
                                                            static_cast<DWORD>(sizeof first_run_buf));
    const bool first_run_too_long = first_run_length >= sizeof first_run_buf;
    const std::string first_run = first_run_length > 0 && !first_run_too_long
                                      ? std::string(first_run_buf, first_run_length)
                                      : std::string();
    const bool fake_requested = !fake.empty() && fake != "0" && fake != "off";
    const bool fake_configured = !fake.empty() && fake != "off";  // conflict check, including a malformed legacy value
    const bool first_run_requested = first_run_too_long || (!first_run.empty() && first_run != "off");
    if (first_run_too_long) {
        std::fprintf(stderr, "[rt] gameinput: PALMETTO_FIRST_RUN_SCRIPT refused: %s\n",
                     palmetto::first_run_script_refusal_name(palmetto::FirstRunScriptRefusal::TooLong));
    } else if (fake_configured && first_run_requested) {
        std::fprintf(stderr, "[rt] gameinput: PALMETTO_FIRST_RUN_SCRIPT refused: %s; no scripted presses\n",
                     palmetto::first_run_script_refusal_name(palmetto::FirstRunScriptRefusal::LegacyScheduleConflict));
    } else if (first_run_requested) {
        palmetto::FirstRunScriptRefusal refusal = palmetto::FirstRunScriptRefusal::None;
        b->scripted = ScriptedPresses::parse_first_run(first_run.c_str(), &refusal);
        if (!b->scripted)
            std::fprintf(stderr, "[rt] gameinput: PALMETTO_FIRST_RUN_SCRIPT=%s refused: %s\n", first_run.c_str(),
                         palmetto::first_run_script_refusal_name(refusal));
        else
            std::fprintf(stderr,
                         "[rt] gameinput: first-run script: %u A/Menu steps, %llu ms held, %llu ms interval, "
                         "first at %.3f s from the GameInput epoch, ending at %.3f s\n",
                         b->scripted->presses(),
                         static_cast<unsigned long long>(palmetto::FirstRunInputScript::kHoldUs / 1000),
                         static_cast<unsigned long long>(palmetto::FirstRunInputScript::kIntervalUs / 1000),
                         b->scripted->first_press_us() / 1e6, b->scripted->ends_at_us() / 1e6);
    } else if (fake_requested) {
        b->scripted = ScriptedPresses::parse(fake.c_str());
        if (!b->scripted)
            std::fprintf(stderr,
                         "[rt] gameinput: PALMETTO_FAKE_INPUT=%s is not a number of seconds, no presses\n", fake.c_str());
        else
            std::fprintf(stderr,
                         "[rt] gameinput: scripted first-run pad: %u presses, 1 s apart, 150 ms each, first at "
                         "%.3f s\n",
                         ScriptedPresses::kPresses, b->scripted->first_press_us() / 1e6);
    }
    // The always-built backend's default presence: one paired, neutral pad, the device a console boot has. The
    // measured first-run read only happens when a pad exists (docs/input_backend.md section 5), so the default makes
    // the console answer and PALMETTO_PAD_PRESENT=0|off restores an unattended host with no pad. The switch is also
    // what the no-device test arming uses.
    if (!b->scripted) {
        const std::string present = detail::env("PALMETTO_PAD_PRESENT");
        if (present != "0" && present != "off") {
            b->scripted = ScriptedPresses::presence();
            static std::atomic<bool> s_logged{false};
            if (!s_logged.exchange(true))
                std::fprintf(stderr, "[rt] gameinput: a pad is present and neutral by default "
                                     "(PALMETTO_PAD_PRESENT=0 removes it)\n");
        }
    }
    const std::string thread = detail::env("PALMETTO_GAMEINPUT_THREAD");
    b->thread_on = thread != "0";
}

// ---- devices -----------------------------------------------------------------------------------------------------

class Reading final : public gi::IGameInputReading {
public:
    Reading(Device* device, const ReadingData& data);
    ~Reading();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    uint32_t STDMETHODCALLTYPE GetInputKind() override;
    uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t input_kind) override;
    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return data_.timestamp_us; }
    void STDMETHODCALLTYPE GetDevice(gi::IGameInputDevice** device) override;
    bool STDMETHODCALLTYPE GetRawReport(void** report) override {
        if (report) *report = nullptr;
        return false;
    }
    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override { return 6; }
    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t count, float* state) override;
    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override { return 14; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t count, bool* state) override;
    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, gi::GameInputSwitchPosition*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMouseState(void*) override { return false; }
    uint32_t STDMETHODCALLTYPE GetTouchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMotionState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetArcadeStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetFlightStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetGamepadState(gi::GameInputGamepadState* state) override;
    bool STDMETHODCALLTYPE GetRacingWheelState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetUiNavigationState(gi::GameInputUiNavigationState* state) override;

    const ReadingData& data() const { return data_; }
    Device* device() const { return device_; }

private:
    Device* device_;
    ReadingData data_;
    std::atomic<ULONG> refs_{1};
};

class Device final : public gi::IGameInputDevice {
public:
    Device(uint32_t slot, bool keyboard, bool scripted, const char* name);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return detail::release_floor(refs_); }

    const gi::GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() override {
        detail::trace_call("IGameInputDevice::GetDeviceInfo", _ReturnAddress(), 1);
        return &info_;
    }
    uint32_t STDMETHODCALLTYPE GetDeviceStatus() override {
        const uint32_t status = status_.load();
        detail::trace_call("IGameInputDevice::GetDeviceStatus", _ReturnAddress(), status);
        return status;
    }
    void STDMETHODCALLTYPE GetBatteryState(gi::GameInputBatteryState* state) override;
    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void** effect) override {
        if (effect) *effect = nullptr;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) override { return false; }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) override {}
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetRumbleState(const gi::GameInputRumbleParams* params) override;
    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {}
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {}
    void STDMETHODCALLTYPE PowerOff() override {}
    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, int32_t, void** report) override {
        if (report) *report = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void** report) override {
        if (report) *report = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void** response) override {
        if (response) *response = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t* out_size) override {
        if (out_size) *out_size = 0;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {}

    uint32_t slot() const { return slot_; }
    bool keyboard() const { return keyboard_; }
    bool scripted() const { return scripted_; }
    ReadingStream& stream() { return stream_; }
    const std::atomic<uint32_t>& status() const { return status_; }
    std::atomic<uint32_t>& status() { return status_; }

private:
    void fill_info(const char* name);

    uint32_t slot_ = 0;
    bool keyboard_ = false, scripted_ = false;
    gi::GameInputDeviceInfo info_{};
    gi::GameInputControllerAxisInfo axis_info_[6]{};
    gi::GameInputControllerButtonInfo button_info_[14]{};
    gi::GameInputGamepadInfo gamepad_info_{};
    gi::GameInputUiNavigationInfo ui_info_{};
    std::string name_;
    gi::GameInputString display_name_{};
    ReadingStream stream_;
    std::atomic<uint32_t> status_{gi::GameInputDeviceNoStatus};
    std::atomic<ULONG> refs_{1};
};

// ---- the IGameInput object ---------------------------------------------------------------------------------------

class Dispatcher final : public gi::IGameInputDispatcher {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (detail::same_guid(riid, IID_IUnknown) || detail::same_guid(riid, gi::IID_IGameInputDispatcher)) {
            AddRef();
            *out = static_cast<gi::IGameInputDispatcher*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    bool STDMETHODCALLTYPE Dispatch(uint64_t) override { return false; }  // no queue: see docs/input_backend.md limits
    HRESULT STDMETHODCALLTYPE OpenWaitHandle(HANDLE* wait_handle) override {
        if (wait_handle) *wait_handle = nullptr;
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> refs_{1};
};

class GameInput final : public gi::IGameInput {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return detail::release_floor(refs_); }

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override {
        const uint64_t timestamp = detail::now_us();
        detail::trace_call("IGameInput::GetCurrentTimestamp", _ReturnAddress(), 0);
        return timestamp;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t input_kind, gi::IGameInputDevice* device,
                                                gi::IGameInputReading** reading) override;
    HRESULT STDMETHODCALLTYPE GetNextReading(gi::IGameInputReading* reference, uint32_t input_kind,
                                             gi::IGameInputDevice* device, gi::IGameInputReading** reading) override;
    HRESULT STDMETHODCALLTYPE GetPreviousReading(gi::IGameInputReading* reference, uint32_t input_kind,
                                                 gi::IGameInputDevice* device, gi::IGameInputReading** reading) override;
    HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, gi::IGameInputDevice*,
                                                 gi::IGameInputReading** reading) override {
        if (reading) *reading = nullptr;
        return E_NOTIMPL;  // never implemented in v0 either
    }
    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(gi::IGameInputDevice* device, uint32_t input_kind,
                                                      float, void* context,
                                                      gi::GameInputReadingCallback callback,
                                                      gi::GameInputCallbackToken* token) override;
    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(gi::IGameInputDevice* device, uint32_t input_kind,
                                                     uint32_t status_filter,
                                                     gi::GameInputEnumerationKind enumeration, void* context,
                                                     gi::GameInputDeviceCallback callback,
                                                     gi::GameInputCallbackToken* token) override;
    HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(gi::IGameInputDevice*, void*,
                                                          gi::GameInputGuideButtonCallback,
                                                          gi::GameInputCallbackToken* token) override;
    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(gi::IGameInputDevice*, void*,
                                                             gi::GameInputKeyboardLayoutCallback,
                                                             gi::GameInputCallbackToken* token) override;
    void STDMETHODCALLTYPE StopCallback(gi::GameInputCallbackToken token) override;
    bool STDMETHODCALLTYPE UnregisterCallback(gi::GameInputCallbackToken token, uint64_t) override;
    HRESULT STDMETHODCALLTYPE CreateDispatcher(gi::IGameInputDispatcher** dispatcher) override;  // E_NOTIMPL
    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(uint32_t, gi::IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromId(const gi::AppLocalDeviceId* value,
                                               gi::IGameInputDevice** device) override;
    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, gi::IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, gi::IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, gi::IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override { return S_OK; }
    void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) override {}

private:
    std::atomic<ULONG> refs_{1};
};

// ---- polling and callback delivery -------------------------------------------------------------------------------

inline void poll_backend(uint64_t now_us);
inline void ensure_init();
inline void poll_if_due();

// The poll under the backend lock. Returns the callbacks to deliver after the lock is released.
struct DueCallbacks {
    struct DeviceNotification {
        CallbackRecord callback;
        uint32_t slot = 0;
        uint32_t status = 0;           // the status the callback reports
        uint32_t previous_status = 0;  // the status before this poll
    };
    std::vector<DeviceNotification> device;
    std::vector<CallbackRecord> reading;
};

inline void poll_locked(uint64_t now_us, DueCallbacks* due) {
    Backend& b = backend();
    const uint64_t scripted_us = now_us >= b.epoch_us ? now_us - b.epoch_us : 0;
    const std::vector<DeviceEvent>& events =
        b.hub.poll(scripted_us, b.xinput, b.keyboard_enabled ? &b.keyboard_source : nullptr, b.keyboard_mode,
                   &b.keyboard_pad, b.scripted ? &*b.scripted : nullptr);
    b.last_poll_us = now_us;
    bool any_reading = false;
    for (uint32_t s = 0; s < kDeviceSlots; ++s) {
        Device* d = b.devices[s];
        if (!d) continue;
        const SlotState& slot = b.hub.slot(s);
        const uint32_t old_status = d->status().load();
        const uint32_t status = slot.connected
                                    ? (gi::GameInputDeviceConnected | gi::GameInputDeviceInputEnabled |
                                       gi::GameInputDeviceOutputEnabled)
                                    : gi::GameInputDeviceNoStatus;
        if (slot.connected) {
            const bool was_connected = old_status != gi::GameInputDeviceNoStatus;
            if (d->stream().update(slot.pad, now_us, !was_connected)) any_reading = true;
            b.active.observe(s, slot.keyboard ? palmetto::InputFamily::Keyboard : palmetto::InputFamily::Xbox, slot.pad,
                             now_us);
        } else if (old_status != gi::GameInputDeviceNoStatus) {
            d->stream().update({}, now_us, true);
        }
        if (status != old_status) {
            d->status().store(status);
            detail::trace_event(slot.connected ? "device connected" : "device disconnected", s, status);
        }
    }
    // Scripted presses: log the start of each press once (the release is the next state change of that pad). The
    // schedule is relative to the epoch (the first GameInputCreate), while the poll clock is absolute.
    if (b.scripted) {
        const uint64_t elapsed = now_us >= b.epoch_us ? now_us - b.epoch_us : 0;
        const std::optional<unsigned> press = b.scripted->press_index_at(elapsed);
        if (press && *press != b.last_press) {
            b.last_press = *press;
            detail::trace_event("scripted press", kScriptedSlot, b.scripted->buttons_at(elapsed));
        }
        if (!press) b.last_press = static_cast<unsigned>(-1);
    }
    // Device callbacks: one per connection or disconnection of a device that supports the filter's kinds (every pad
    // here is a gamepad; a filter that asks for a kind no pad supports hears nothing, as Microsoft's documented
    // filter behaviour says). The notification carries both statuses, as GameInput's callback does.
    for (const DeviceEvent& event : events) {
        const uint32_t status = event.kind == DeviceEventKind::Connected
                                    ? (gi::GameInputDeviceConnected | gi::GameInputDeviceInputEnabled |
                                       gi::GameInputDeviceOutputEnabled)
                                    : gi::GameInputDeviceNoStatus;
        const uint32_t previous = event.kind == DeviceEventKind::Connected ? gi::GameInputDeviceNoStatus
                                                                           : (gi::GameInputDeviceConnected |
                                                                              gi::GameInputDeviceInputEnabled |
                                                                              gi::GameInputDeviceOutputEnabled);
        for (const CallbackRecord& cb : b.callbacks) {
            if (!cb.active || !cb.device) continue;
            if ((cb.kind_filter & gi::GameInputKindGamepad) == 0) continue;
            due->device.push_back({cb, event.slot, status, previous});
        }
    }
    if (any_reading) {
        for (const CallbackRecord& cb : b.callbacks)
            if (cb.active && cb.reading) due->reading.push_back(cb);
    }
}

// The backend's poll entry point (the background thread and every API call use it). Callbacks are delivered outside
// the lock, as GameInput runs them on its own threads.
inline void poll_backend(uint64_t now_us) {
    Backend& b = backend();
    DueCallbacks due;
    {
        std::lock_guard<std::mutex> lock(b.mu);
        poll_locked(now_us, &due);
    }
    // Device callbacks: the device is handed out with a reference the callback owns, as GameInput does.
    for (const DueCallbacks::DeviceNotification& note : due.device) {
        Device* d = nullptr;
        {
            std::lock_guard<std::mutex> lock(b.mu);
            d = b.devices[note.slot];
        }
        if (!d) continue;
        d->AddRef();
        note.callback.device(note.callback.token, note.callback.context, static_cast<gi::IGameInputDevice*>(d),
                             detail::now_us(), note.status, note.previous_status);
        d->Release();
        detail::trace_event("device callback", note.slot, note.callback.kind_filter);
    }
    // Reading callbacks: a reading object per callback, from the newest reading of any connected pad.
    std::vector<CallbackRecord> reading_callbacks = due.reading;
    if (!reading_callbacks.empty()) {
        Reading* reading = nullptr;
        {
            std::lock_guard<std::mutex> lock(b.mu);
            for (uint32_t s = 0; s < kDeviceSlots && !reading; ++s) {
                Device* d = b.devices[s];
                if (!d || d->status().load() == gi::GameInputDeviceNoStatus) continue;
                ReadingData data;
                if (d->stream().current(&data)) reading = new Reading(d, data);
            }
        }
        if (reading) {
            for (const CallbackRecord& cb : reading_callbacks) {
                cb.reading(cb.token, cb.context, static_cast<gi::IGameInputReading*>(reading), false);
                detail::trace_event("reading callback", 0, 0);
            }
            reading->Release();
        }
    }
}

// Starts the background poll thread (2 ms, the SDL shim's period) unless PALMETTO_GAMEINPUT_THREAD=0.
inline void start_poll_thread() {
    Backend& b = backend();
    if (!b.thread_on || b.thread_started) return;
    b.thread_started = true;
    std::thread([] {
        for (;;) {
            const uint64_t start = detail::now_us();
            poll_backend(start);
            const uint64_t spent = detail::now_us() - start;
            const uint64_t period = 2'000;
            if (spent < period) std::this_thread::sleep_for(std::chrono::microseconds(period - spent));
        }
    }).detach();
}

// Re-initialises the backend from the environment and rebuilds its device objects (first create, and tests).
inline void ensure_init() {
    Backend& b = backend();
    if (b.initialised) return;
    configure_from_env(&b);
    b.epoch_us = detail::now_us();
    b.xinput = xinput_source();
    b.keyboard_source = default_keyboard_source();
    for (uint32_t s = 0; s < kDeviceSlots; ++s) {
        if (!b.devices[s]) b.devices[s] = new Device(s, s == kKeyboardSlot, s == kScriptedSlot, nullptr);
    }
    b.initialised = true;
    poll_backend(b.epoch_us);
    start_poll_thread();
}

// Polls if the caller is late (the API path; with the background thread on this usually does nothing).
inline void poll_if_due() {
    Backend& b = backend();
    const uint64_t now = detail::now_us();
    uint64_t last = 0;
    {
        std::lock_guard<std::mutex> lock(b.mu);
        last = b.last_poll_us;
    }
    if (now - last >= 2'000) poll_backend(now);
}

// Selects the device a reading query looks at: the requested device, or every connected device. Returns the
// connected slots that support `input_kind`, in slot order.
inline std::vector<Device*> devices_for(uint32_t input_kind, gi::IGameInputDevice* device) {
    std::vector<Device*> out;
    Backend& b = backend();
    if (device) {
        Device* d = static_cast<Device*>(device);
        if (d->status().load() != gi::GameInputDeviceNoStatus &&
            (d->GetDeviceInfo()->supportedInput & input_kind) != 0)
            out.push_back(d);
        return out;
    }
    for (uint32_t s = 0; s < kDeviceSlots; ++s) {
        Device* d = b.devices[s];
        if (d && d->status().load() != gi::GameInputDeviceNoStatus &&
            (d->GetDeviceInfo()->supportedInput & input_kind) != 0)
            out.push_back(d);
    }
    return out;
}

// The newest reading over the given devices, at or after `after_sequence` when one is given.
inline bool newest_reading(const std::vector<Device*>& devices, std::optional<uint64_t> after_sequence,
                           Device** found_device, ReadingData* out) {
    bool any = false;
    for (Device* d : devices) {
        ReadingData data;
        if (!d->stream().current(&data)) continue;
        if (after_sequence && data.sequence <= *after_sequence) continue;
        if (!any || data.timestamp_us > out->timestamp_us || (data.timestamp_us == out->timestamp_us && data.sequence > out->sequence)) {
            *out = data;
            *found_device = d;
            any = true;
        }
    }
    return any;
}

// ---- Device, Reading and GameInput method bodies -----------------------------------------------------------------

inline Device::Device(uint32_t slot, bool keyboard, bool scripted, const char* name)
    : slot_(slot), keyboard_(keyboard), scripted_(scripted) {
    fill_info(name);
}

inline void Device::fill_info(const char* name) {
    info_ = {};
    info_.infoSize = sizeof(gi::GameInputDeviceInfo);
    info_.usage = {0x01, 0x05};  // HID generic desktop / game pad
    info_.deviceFamily = gi::GameInputFamilyConsoleVariant2;  // every pad is an Xbox One pad to the title
    info_.supportedInput = gi::GameInputKindControllerAxis | gi::GameInputKindControllerButton |
                           gi::GameInputKindGamepad | gi::GameInputKindUiNavigation;
    info_.supportedRumbleMotors = keyboard_ ? 0 : (gi::GameInputRumbleLowFrequency | gi::GameInputRumbleHighFrequency);
    info_.controllerAxisCount = 6;
    info_.controllerButtonCount = 14;
    detail::make_device_id(keyboard_ ? "palmetto-keys" : scripted_ ? "palmetto-scripted" : "palmetto-xinput", slot_,
                           &info_.deviceId);
    info_.deviceRootId = info_.deviceId;
    for (uint32_t a = 0; a < 6; ++a) {
        axis_info_[a] = {};
        axis_info_[a].mappedInputKinds = gi::GameInputKindGamepad;
        axis_info_[a].isContinuous = true;
        axis_info_[a].hasRestValue = true;
        axis_info_[a].restValue = a < 4 ? 0.5f : 0.0f;
        axis_info_[a].resolution = 65536;
        axis_info_[a].legacyHidIndex = static_cast<uint16_t>(a);
        axis_info_[a].legacyDInputIndex = static_cast<uint16_t>(a);
    }
    for (uint32_t i = 0; i < 14; ++i) {
        button_info_[i] = {};
        button_info_[i].mappedInputKinds = gi::GameInputKindGamepad;
        button_info_[i].legacyHidIndex = static_cast<uint16_t>(i);
        button_info_[i].legacyDInputIndex = static_cast<uint16_t>(i);
        button_info_[i].label = gi::GameInputLabelUnknown;
    }
    // Every pad is presented as an Xbox One pad, so every control carries the Xbox label of its position (the same
    // convention as the SDL shim, docs/gameinput.md "Mapping").
    const gi::GameInputLabel face[4] = {gi::GameInputLabelXboxA, gi::GameInputLabelXboxB, gi::GameInputLabelXboxX,
                                        gi::GameInputLabelXboxY};
    const gi::GameInputLabel menu = gi::GameInputLabelXboxMenu, view = gi::GameInputLabelXboxView;
    const gi::GameInputLabel up = gi::GameInputLabelXboxDPadUp, down = gi::GameInputLabelXboxDPadDown;
    const gi::GameInputLabel left = gi::GameInputLabelXboxDPadLeft, right = gi::GameInputLabelXboxDPadRight;
    const gi::GameInputLabel lb = gi::GameInputLabelXboxLeftShoulder, rb = gi::GameInputLabelXboxRightShoulder;
    const gi::GameInputLabel ls = gi::GameInputLabelXboxLeftStickButton, rs = gi::GameInputLabelXboxRightStickButton;
    const gi::GameInputLabel lt = gi::GameInputLabelXboxLeftTrigger, rt = gi::GameInputLabelXboxRightTrigger;
    const gi::GameInputLabel none = gi::GameInputLabelNone;
    gamepad_info_ = {menu, view, face[0], face[1], face[2], face[3], up, down, left, right, lb, rb, ls, rs};
    ui_info_ = {menu, view, face[0], face[1], up, down, left, right, face[2], face[3], ls, rs, lt, rt, lb, rb,
                none, none, none, none, gi::GameInputLabelXboxGuide};
    axis_info_[4].label = lt;
    axis_info_[5].label = rt;
    const struct {
        uint32_t gamepad_bit;
        gi::GameInputLabel label;
    } labelled[] = {
        {PadA, face[0]},          {PadB, face[1]},          {PadX, face[2]},          {PadY, face[3]},
        {PadView, view},          {PadMenu, menu},          {PadLeftThumbstick, ls},  {PadRightThumbstick, rs},
        {PadLeftShoulder, lb},    {PadRightShoulder, rb},   {PadDPadUp, up},          {PadDPadDown, down},
        {PadDPadLeft, left},      {PadDPadRight, right},
    };
    (void)labelled;  // labels are already in gamepad_info_; button_info_ keeps Unknown for every entry as the host does
    info_.controllerAxisInfo = axis_info_;
    info_.controllerButtonInfo = button_info_;
    info_.gamepadInfo = &gamepad_info_;
    info_.uiNavigationInfo = &ui_info_;
    name_ = name ? name : (keyboard_ ? "Keyboard pad (Palmetto)" : scripted_ ? "Scripted pad (Palmetto FAKE_INPUT)"
                                                                             : "XInput pad (Palmetto)");
    uint32_t points = 0;
    for (unsigned char c : name_) points += (c & 0xC0) != 0x80;
    display_name_ = {static_cast<uint32_t>(name_.size()), points, name_.c_str()};
    info_.displayName = &display_name_;
}

inline HRESULT STDMETHODCALLTYPE Device::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (detail::same_guid(riid, IID_IUnknown) || detail::same_guid(riid, gi::IID_IGameInputDevice)) {
        AddRef();
        *out = static_cast<gi::IGameInputDevice*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

inline void STDMETHODCALLTYPE Device::GetBatteryState(gi::GameInputBatteryState* state) {
    if (!state) return;
    *state = {0.0f, 0.0f, 0.0f, 0.0f, gi::GameInputBatteryUnknown};
}

inline void STDMETHODCALLTYPE Device::SetRumbleState(const gi::GameInputRumbleParams* params) {
    // No output path in the no-SDL backend: the values are counted and traced, never played (docs/input_backend.md).
    detail::trace_call("IGameInputDevice::SetRumbleState", _ReturnAddress(), params ? 1 : 0);
    detail::trace_event("SetRumbleState", slot_,
                        params ? static_cast<uint32_t>(params->lowFrequency * 255.0f) : 0u);
}

inline Reading::Reading(Device* device, const ReadingData& data) : device_(device), data_(data) { device_->AddRef(); }
inline Reading::~Reading() { device_->Release(); }

inline HRESULT STDMETHODCALLTYPE Reading::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (detail::same_guid(riid, IID_IUnknown) || detail::same_guid(riid, gi::IID_IGameInputReading)) {
        AddRef();
        *out = static_cast<gi::IGameInputReading*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
inline uint32_t STDMETHODCALLTYPE Reading::GetInputKind() {
    const uint32_t kind = gi::GameInputKindControllerAxis | gi::GameInputKindControllerButton |
                          gi::GameInputKindGamepad | gi::GameInputKindUiNavigation;
    detail::trace_call("IGameInputReading::GetInputKind", _ReturnAddress(), kind);
    return kind;
}
inline uint64_t STDMETHODCALLTYPE Reading::GetSequenceNumber(uint32_t input_kind) {
    return (input_kind & GetInputKind()) ? data_.sequence : 0;
}
inline void STDMETHODCALLTYPE Reading::GetDevice(gi::IGameInputDevice** device) {
    if (!device) return;
    device_->AddRef();
    *device = static_cast<gi::IGameInputDevice*>(device_);
}
inline uint32_t STDMETHODCALLTYPE Reading::GetControllerAxisState(uint32_t count, float* state) {
    if (!state || count == 0) return 0;
    const float axes[6] = {0.5f + 0.5f * data_.pad.left_x,  0.5f + 0.5f * data_.pad.left_y,
                           0.5f + 0.5f * data_.pad.right_x, 0.5f + 0.5f * data_.pad.right_y,
                           data_.pad.left_trigger,          data_.pad.right_trigger};
    const uint32_t n = std::min<uint32_t>(count, 6);
    for (uint32_t i = 0; i < n; ++i) state[i] = axes[i];
    return n;
}
inline uint32_t STDMETHODCALLTYPE Reading::GetControllerButtonState(uint32_t count, bool* state) {
    if (!state || count == 0) return 0;
    const uint32_t bits = gamepad_button_bits(data_.pad.buttons);
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 14); ++i) state[i] = (bits & (1u << i)) != 0;
    return std::min<uint32_t>(count, 14);
}
inline bool STDMETHODCALLTYPE Reading::GetGamepadState(gi::GameInputGamepadState* state) {
    detail::trace_call("IGameInputReading::GetGamepadState", _ReturnAddress(), state ? 1 : 0);
    if (!state) return false;
    state->buttons = gamepad_button_bits(data_.pad.buttons);
    state->leftTrigger = data_.pad.left_trigger;
    state->rightTrigger = data_.pad.right_trigger;
    state->leftThumbstickX = data_.pad.left_x;
    state->leftThumbstickY = data_.pad.left_y;
    state->rightThumbstickX = data_.pad.right_x;
    state->rightThumbstickY = data_.pad.right_y;
    return true;
}
inline bool STDMETHODCALLTYPE Reading::GetUiNavigationState(gi::GameInputUiNavigationState* state) {
    detail::trace_call("IGameInputReading::GetUiNavigationState", _ReturnAddress(), state ? 1 : 0);
    if (!state) return false;
    state->buttons = ui_navigation_bits(data_.pad);
    return true;
}

inline HRESULT STDMETHODCALLTYPE GameInput::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (detail::same_guid(riid, IID_IUnknown) || detail::same_guid(riid, gi::IID_IGameInput)) {
        AddRef();
        *out = static_cast<gi::IGameInput*>(this);
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

inline HRESULT STDMETHODCALLTYPE GameInput::GetCurrentReading(uint32_t input_kind, gi::IGameInputDevice* device,
                                                             gi::IGameInputReading** reading) {
    if (!reading) return E_POINTER;
    *reading = nullptr;
    ensure_init();
    poll_if_due();
    std::lock_guard<std::mutex> lock(backend().mu);
    const std::vector<Device*> devices = devices_for(input_kind, device);
    Device* found = nullptr;
    ReadingData data;
    if (input_kind == 0 || !newest_reading(devices, std::nullopt, &found, &data)) {
        detail::trace_call("IGameInput::GetCurrentReading", _ReturnAddress(), gi::GAMEINPUT_E_READING_NOT_FOUND,
                           input_kind);
        return gi::GAMEINPUT_E_READING_NOT_FOUND;
    }
    *reading = new Reading(found, data);
    detail::trace_call("IGameInput::GetCurrentReading", _ReturnAddress(), S_OK, input_kind);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::GetNextReading(gi::IGameInputReading* reference, uint32_t input_kind,
                                                          gi::IGameInputDevice* device,
                                                          gi::IGameInputReading** reading) {
    if (!reading) return E_POINTER;
    *reading = nullptr;
    if (!reference) return E_INVALIDARG;
    ensure_init();
    poll_if_due();
    std::lock_guard<std::mutex> lock(backend().mu);
    Reading* ref = static_cast<Reading*>(reference);
    Device* found = nullptr;
    ReadingData data;
    bool any = false;
    for (Device* d : devices_for(input_kind, device)) {
        if (device && d != ref->device()) continue;
        ReadingData next;
        if (!d->stream().next(ref->data(), &next)) continue;
        if (!any || next.sequence < data.sequence) {
            data = next;
            found = d;
            any = true;
        }
    }
    if (!any) {
        detail::trace_call("IGameInput::GetNextReading", _ReturnAddress(), gi::GAMEINPUT_E_READING_NOT_FOUND, input_kind);
        return gi::GAMEINPUT_E_READING_NOT_FOUND;
    }
    *reading = new Reading(found, data);
    detail::trace_call("IGameInput::GetNextReading", _ReturnAddress(), S_OK, input_kind);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::GetPreviousReading(gi::IGameInputReading* reference, uint32_t input_kind,
                                                              gi::IGameInputDevice* device,
                                                              gi::IGameInputReading** reading) {
    if (!reading) return E_POINTER;
    *reading = nullptr;
    if (!reference) return E_INVALIDARG;
    ensure_init();
    poll_if_due();
    std::lock_guard<std::mutex> lock(backend().mu);
    Reading* ref = static_cast<Reading*>(reference);
    Device* found = nullptr;
    ReadingData data;
    bool any = false;
    for (Device* d : devices_for(input_kind, device)) {
        if (device && d != ref->device()) continue;
        ReadingData prev;
        if (!d->stream().previous(ref->data(), &prev)) continue;
        if (!any || prev.sequence > data.sequence) {
            data = prev;
            found = d;
            any = true;
        }
    }
    if (!any) {
        detail::trace_call("IGameInput::GetPreviousReading", _ReturnAddress(), gi::GAMEINPUT_E_READING_NOT_FOUND,
                           input_kind);
        return gi::GAMEINPUT_E_READING_NOT_FOUND;
    }
    *reading = new Reading(found, data);
    detail::trace_call("IGameInput::GetPreviousReading", _ReturnAddress(), S_OK, input_kind);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::RegisterReadingCallback(gi::IGameInputDevice*, uint32_t input_kind,
                                                                    float, void* context,
                                                                    gi::GameInputReadingCallback callback,
                                                                    gi::GameInputCallbackToken* token) {
    if (!token) return E_POINTER;
    *token = 0;
    if (!callback) return E_INVALIDARG;
    ensure_init();
    std::lock_guard<std::mutex> lock(backend().mu);
    CallbackRecord cb;
    cb.token = backend().next_token++;
    cb.kind_filter = input_kind ? input_kind : gi::GameInputKindGamepad;
    cb.context = context;
    cb.reading = callback;
    backend().callbacks.push_back(cb);
    *token = cb.token;
    detail::trace_call("IGameInput::RegisterReadingCallback", _ReturnAddress(), S_OK, cb.kind_filter);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::RegisterDeviceCallback(gi::IGameInputDevice*, uint32_t input_kind,
                                                                   uint32_t status_filter,
                                                                   gi::GameInputEnumerationKind enumeration, void* context,
                                                                   gi::GameInputDeviceCallback callback,
                                                                   gi::GameInputCallbackToken* token) {
    if (!token) return E_POINTER;
    *token = 0;
    if (!callback) return E_INVALIDARG;
    ensure_init();
    poll_if_due();
    CallbackRecord cb;
    cb.token = backend().next_token++;
    cb.kind_filter = input_kind ? input_kind : gi::GameInputKindGamepad;
    cb.status_filter = status_filter;
    cb.enumeration = enumeration;
    cb.context = context;
    cb.device = callback;
    std::vector<uint32_t> existing;
    {
        std::lock_guard<std::mutex> lock(backend().mu);
        backend().callbacks.push_back(cb);
        for (uint32_t s = 0; s < kDeviceSlots; ++s) {
            Device* d = backend().devices[s];
            if (d && d->status().load() != gi::GameInputDeviceNoStatus &&
                (d->GetDeviceInfo()->supportedInput & cb.kind_filter) != 0)
                existing.push_back(s);
        }
    }
    *token = cb.token;
    if (enumeration != gi::GameInputNoEnumeration) {
        for (uint32_t s : existing) {
            Device* d = backend().devices[s];
            d->AddRef();
            callback(cb.token, context, static_cast<gi::IGameInputDevice*>(d), detail::now_us(),
                     d->status().load(), gi::GameInputDeviceNoStatus);
            d->Release();
            detail::trace_event("device callback enumeration", s, cb.kind_filter);
        }
    }
    detail::trace_call("IGameInput::RegisterDeviceCallback", _ReturnAddress(), S_OK, cb.kind_filter);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::RegisterGuideButtonCallback(gi::IGameInputDevice*, void* context,
                                                                        gi::GameInputGuideButtonCallback callback,
                                                                        gi::GameInputCallbackToken* token) {
    if (!token) return E_POINTER;
    *token = 0;
    if (!callback) return E_INVALIDARG;
    ensure_init();
    std::lock_guard<std::mutex> lock(backend().mu);
    CallbackRecord cb;
    cb.token = backend().next_token++;
    cb.context = context;
    cb.guide = callback;
    backend().callbacks.push_back(cb);
    *token = cb.token;
    detail::trace_call("IGameInput::RegisterGuideButtonCallback", _ReturnAddress(), S_OK);
    return S_OK;
}
inline HRESULT STDMETHODCALLTYPE GameInput::RegisterKeyboardLayoutCallback(gi::IGameInputDevice*, void* context,
                                                                          gi::GameInputKeyboardLayoutCallback callback,
                                                                          gi::GameInputCallbackToken* token) {
    if (!token) return E_POINTER;
    *token = 0;
    if (!callback) return E_INVALIDARG;
    ensure_init();
    std::lock_guard<std::mutex> lock(backend().mu);
    CallbackRecord cb;
    cb.token = backend().next_token++;
    cb.context = context;
    cb.keyboard = callback;
    backend().callbacks.push_back(cb);
    *token = cb.token;
    detail::trace_call("IGameInput::RegisterKeyboardLayoutCallback", _ReturnAddress(), S_OK);
    return S_OK;
}
inline void STDMETHODCALLTYPE GameInput::StopCallback(gi::GameInputCallbackToken token) {
    std::lock_guard<std::mutex> lock(backend().mu);
    for (CallbackRecord& cb : backend().callbacks)
        if (cb.token == token) cb.active = false;
}
inline bool STDMETHODCALLTYPE GameInput::UnregisterCallback(gi::GameInputCallbackToken token, uint64_t) {
    StopCallback(token);
    return true;
}
inline HRESULT STDMETHODCALLTYPE GameInput::CreateDispatcher(gi::IGameInputDispatcher** dispatcher) {
    if (dispatcher) *dispatcher = nullptr;
    return E_NOTIMPL;  // no dispatcher queue in this backend; callbacks are delivered from the poll (limits in docs)
}
inline HRESULT STDMETHODCALLTYPE GameInput::FindDeviceFromId(const gi::AppLocalDeviceId* value,
                                                             gi::IGameInputDevice** device) {
    if (!device) return E_POINTER;
    *device = nullptr;
    if (!value) return E_INVALIDARG;
    ensure_init();
    std::lock_guard<std::mutex> lock(backend().mu);
    for (uint32_t s = 0; s < kDeviceSlots; ++s) {
        Device* d = backend().devices[s];
        if (!d) continue;
        if (std::memcmp(d->GetDeviceInfo()->deviceId.value, value->value, sizeof(value->value)) == 0) {
            d->AddRef();
            *device = static_cast<gi::IGameInputDevice*>(d);
            return S_OK;
        }
    }
    return gi::GAMEINPUT_E_DEVICE_NOT_FOUND;
}

// ---- test seams (no game, no controller) -------------------------------------------------------------------------

namespace detail {

// Re-reads the environment and rebuilds the backend's devices and copies of the sources. A test calls this before it
// drives the backend, after installing its fake XInput source and keyboard source. No background thread runs.
inline void reset_for_test() {
    Backend& b = backend();
    std::lock_guard<std::mutex> lock(b.mu);
    b.initialised = false;
    b.thread_started = false;
    b.thread_on = false;
    b.callbacks.clear();
    b.next_token = 1;
    b.epoch_us = 0;
    b.last_poll_us = 0;
    b.last_press = static_cast<unsigned>(-1);
    b.active = ActiveInputFeed{};
    if (b.devices[0]) {  // devices are created by the first init; reset them rather than leaking replacements
        for (uint32_t s = 0; s < kDeviceSlots; ++s) {
            b.devices[s]->stream().clear();
            b.devices[s]->status().store(gi::GameInputDeviceNoStatus);
        }
    }
    b.hub = DeviceHub{};
    b.keyboard_pad.reset();
    configure_from_env(&b);
    b.thread_on = false;
    b.xinput = xinput_source();
    b.keyboard_source = default_keyboard_source();
    for (uint32_t s = 0; s < kDeviceSlots; ++s)
        if (!b.devices[s]) b.devices[s] = new Device(s, s == kKeyboardSlot, s == kScriptedSlot, nullptr);
    b.initialised = true;
    b.last_poll_us = now_us();
}

// One explicit poll at `timestamp_us` (a fake clock), delivering callbacks. No background thread runs in a test.
inline void poll_at(uint64_t timestamp_us) { poll_backend(timestamp_us); }

inline Backend& state() { return backend(); }

}  // namespace detail

}  // namespace rt::gameinput::nosdl

// The registry entry: the same three names the SDL shim registers (gameinput.dll by name and by ordinal, and
// xgameruntime.dll), so a title's `GameInput.dll!#1` import binds here when the build has no SDL3.
namespace rt::gameinput::nosdl::detail {

inline HRESULT WINAPI create_game_input(gi::IGameInput** out) {
    void* caller = _ReturnAddress();
    if (!out) {
        trace_call("GameInputCreate", caller, E_POINTER);
        return E_POINTER;
    }
    *out = nullptr;
    ensure_init();
    static GameInput instance;
    instance.AddRef();
    *out = static_cast<gi::IGameInput*>(&instance);
    trace_call("GameInputCreate", caller, S_OK);
    return S_OK;
}

}  // namespace rt::gameinput::nosdl::detail

inline HRESULT WINAPI GameInputCreate(gi::IGameInput** out) {
    return rt::gameinput::nosdl::detail::create_game_input(out);
}

SHIM_TABLE(gameinput_nosdl,
    {"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&GameInputCreate)},
    {"xgameruntime.dll", "GameInputCreate", reinterpret_cast<void*>(&GameInputCreate)},
    // The Xbox build imports GameInputCreate from GameInput.dll by ordinal (measured on the package image: one
    // import, GameInput.dll!#1); the host GameInput.dll exports GameInputCreate at ordinal 1 (verified today).
    {"gameinput.dll", "#1", reinterpret_cast<void*>(&GameInputCreate)},
);
