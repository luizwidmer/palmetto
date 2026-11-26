// The always-built no-SDL GameInput backend (port/shims/gameinput_nosdl.h, docs/input_backend.md) without a controller
// and without game data: the XInput-state to GameInput-pad mapping, the hot-plug poll and device pairing, the reading
// stream, the keyboard/mouse-as-pad mapping with its own keys and cursor, the scripted first-run press schedule, and
// the same over the v0 GameInput ABI through GameInputCreate with a fake XInput source.
#include "../shims/gameinput_nosdl.h"

#include <cstdio>
#include <iterator>
#include <vector>

using namespace rt::gameinput::nosdl;

#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

// ---- a fake XInput source: no controller, no OS input -------------------------------------------------------------

struct FakePad {
    bool present = false;
    XInputPad pad{};
};
FakePad g_fake[kXInputSlots];

bool fake_xinput(uint32_t slot, XInputPad* out) {
    if (slot >= kXInputSlots || !out || !g_fake[slot].present) return false;
    *out = g_fake[slot].pad;
    return true;
}
const XInputSource kFakeXInput{&fake_xinput, "fake"};

// ---- a fake keyboard/mouse source ---------------------------------------------------------------------------------

bool g_keys[256] = {};
bool g_mouse[2] = {};
int g_cursor_x = 0, g_cursor_y = 0;
bool g_cursor_ok = true;
bool g_focused = true;

bool fake_key(int vk) { return vk >= 0 && vk < 256 && g_keys[vk]; }
bool fake_mouse(int button) { return button == 0 || button == 1 ? g_mouse[button] : false; }
bool fake_cursor(int* x, int* y) {
    if (!g_cursor_ok) return false;
    *x = g_cursor_x;
    *y = g_cursor_y;
    return true;
}
bool fake_focused() { return g_focused; }
const KeyboardSource kFakeKeyboard{fake_key, fake_mouse, fake_cursor, fake_focused};

void clear_sources() {
    for (FakePad& p : g_fake) p = FakePad{};
    for (bool& k : g_keys) k = false;
    g_mouse[0] = g_mouse[1] = false;
    g_cursor_x = g_cursor_y = 0;
    g_cursor_ok = true;
    g_focused = true;
    SetEnvironmentVariableA("PALMETTO_KEYBOARD_PAD", nullptr);
    SetEnvironmentVariableA("PALMETTO_FAKE_INPUT", nullptr);
    // The backend presents one neutral pad by default (docs/decomp_first_ui.md 12); the ABI tests that count devices
    // arm each source explicitly, so the shared reset removes the default pad first.
    SetEnvironmentVariableA("PALMETTO_PAD_PRESENT", "0");
    SetEnvironmentVariableA("PALMETTO_GAMEINPUT_THREAD", "0");
    SetEnvironmentVariableA("PALMETTO_INPUT_TRACE", nullptr);
    set_xinput_source(&kFakeXInput);
    set_keyboard_source(&kFakeKeyboard);
}

// ---- 1. the XInput mapping ------------------------------------------------------------------------------------------

int test_xinput_mapping() {
    XInputPad raw;
    raw.buttons = XiA | XiB | XiX | XiY | XiStart | XiBack | XiDPadUp | XiRightShoulder;
    raw.left_trigger = 255;
    raw.right_trigger = 51;  // 0.2
    raw.left_x = 32767;
    raw.left_y = -32768;
    raw.right_x = -16384;
    raw.right_y = 0;
    const PadState pad = translate_xinput(raw);
    CHECK(pad.buttons & PadA);
    CHECK(pad.buttons & PadB);
    CHECK(pad.buttons & PadX);
    CHECK(pad.buttons & PadY);
    CHECK(pad.buttons & PadMenu);
    CHECK(pad.buttons & PadView);
    CHECK(pad.buttons & PadDPadUp);
    CHECK(pad.buttons & PadRightShoulder);
    CHECK(!(pad.buttons & PadDPadDown));
    CHECK(pad.left_trigger == 1.0f);
    CHECK(pad.right_trigger > 0.19f && pad.right_trigger < 0.21f);
    CHECK(pad.left_x == 1.0f);
    CHECK(pad.left_y == -1.0f);  // XInput +Y is up, GameInput +Y is up: the sign is kept
    CHECK(pad.right_x < -0.49f && pad.right_x > -0.51f);
    CHECK(pad.right_y == 0.0f);
    // The button values are GameInput's own, so a translated pad state copies straight into the ABI state.
    CHECK((gamepad_button_bits(pad.buttons) & gi::GameInputGamepadA) != 0);
    CHECK((gamepad_button_bits(pad.buttons) & 0x4000) == 0);  // PadGuide has no GameInput gamepad bit
    CHECK(PadA == static_cast<uint32_t>(gi::GameInputGamepadA));
    CHECK(PadMenu == static_cast<uint32_t>(gi::GameInputGamepadMenu));
    return 0;
}

// ---- 2. hot-plug, pairing and the scripted pad ----------------------------------------------------------------------

int test_hotplug_and_pairing() {
    clear_sources();
    detail::reset_for_test();
    DeviceHub hub;
    const ScriptedPresses none(0);
    (void)none;
    KeyboardPad keys;
    hub.poll(0, kFakeXInput, nullptr, KeyboardPadMode::Off, &keys, nullptr);
    CHECK(hub.connected(0) == false);
    CHECK(hub.events().empty());

    g_fake[0].present = true;
    g_fake[0].pad.buttons = XiA;
    hub.poll(1'000, kFakeXInput, nullptr, KeyboardPadMode::Off, &keys, nullptr);
    CHECK(hub.connected(0));
    CHECK(hub.slot(0).pad.buttons & PadA);
    CHECK(hub.events().size() == 1);
    CHECK(hub.events()[0].kind == DeviceEventKind::Connected && hub.events()[0].slot == 0);

    hub.poll(2'000, kFakeXInput, nullptr, KeyboardPadMode::Off, &keys, nullptr);
    CHECK(hub.events().empty());  // a steady pad is not an event

    g_fake[0].present = false;
    hub.poll(3'000, kFakeXInput, nullptr, KeyboardPadMode::Off, &keys, nullptr);
    CHECK(!hub.connected(0));
    CHECK(hub.events().size() == 1 && hub.events()[0].kind == DeviceEventKind::Disconnected);

    // Hot-plug: the same slot comes back, and the keyboard pad and the scripted pad are separate devices.
    g_fake[0].present = true;
    g_fake[0].pad = XInputPad{};
    ScriptedPresses schedule(500'000);
    hub.poll(4'000, kFakeXInput, &kFakeKeyboard, KeyboardPadMode::Always, &keys, &schedule);
    CHECK(hub.connected(0) && hub.connected(kKeyboardSlot) && hub.connected(kScriptedSlot));
    CHECK(hub.slot(0).pad.buttons == 0);
    CHECK(hub.slot(kKeyboardSlot).keyboard);
    CHECK(hub.slot(kScriptedSlot).scripted);
    CHECK(!hub.connected(1));

    // The scripted pad holds confirm on press 0 and start on press 1, and leaves afterwards (the schedule is bounded).
    hub.poll(600'000, kFakeXInput, &kFakeKeyboard, KeyboardPadMode::Always, &keys, &schedule);
    CHECK(hub.slot(kScriptedSlot).pad.buttons == PadA);
    hub.poll(1'600'000, kFakeXInput, &kFakeKeyboard, KeyboardPadMode::Always, &keys, &schedule);
    CHECK(hub.slot(kScriptedSlot).pad.buttons == PadMenu);
    hub.poll(2'600'000, kFakeXInput, &kFakeKeyboard, KeyboardPadMode::Always, &keys, &schedule);
    CHECK(hub.slot(kScriptedSlot).pad.buttons == PadA);
    hub.poll(40'000'000, kFakeXInput, &kFakeKeyboard, KeyboardPadMode::Always, &keys, &schedule);
    CHECK(hub.connected(kScriptedSlot));  // the pad stays paired, neutral, after the schedule ends
    CHECK(hub.slot(kScriptedSlot).pad.buttons == 0);
    return 0;
}

// ---- 3. readings ----------------------------------------------------------------------------------------------------

int test_reading_stream() {
    ReadingStream stream;
    PadState rest;
    CHECK(stream.size() == 0);
    CHECK(stream.update(rest, 1'000, true));
    CHECK(stream.size() == 1);
    CHECK(!stream.update(rest, 2'000, false));  // unchanged: no reading
    PadState pressed = rest;
    pressed.buttons = PadA;
    CHECK(stream.update(pressed, 3'000, false));
    CHECK(stream.size() == 2);
    ReadingData current;
    CHECK(stream.current(&current));
    CHECK(current.sequence == 2 && current.pad.buttons == PadA && current.timestamp_us == 3'000);
    ReadingData previous;
    CHECK(stream.previous(current, &previous));
    CHECK(previous.sequence == 1 && previous.timestamp_us == 1'000);
    ReadingData next;
    CHECK(stream.previous(previous, &next) == false);
    CHECK(stream.find(1, &next) && next.timestamp_us == 1'000);
    // next() from the first reading answers the second; from the newest it answers nothing.
    CHECK(stream.next(previous, &next) && next.sequence == 2);
    CHECK(!stream.next(current, &next));
    // The ring keeps the newest 128; sequences still order the stream.
    for (int i = 0; i < 200; ++i) {
        PadState p;
        p.buttons = static_cast<uint32_t>(i + 1);
        stream.update(p, 10'000 + i, false);
    }
    CHECK(stream.size() == ReadingStream::kCap);
    CHECK(stream.current(&current));
    CHECK(current.sequence == 202);
    // The UI navigation state a first-run screen would read.
    PadState ui;
    ui.buttons = PadA | PadDPadDown;
    ui.left_trigger = 0.9f;
    const uint32_t bits = ui_navigation_bits(ui);
    CHECK(bits & 0x00000004);  // Accept
    CHECK(bits & 0x00000020);  // Down
    CHECK(bits & 0x00001000);  // PageUp
    CHECK(!(bits & 0x00000001));  // Menu, not pressed here
    CHECK(!(bits & 0x00000002));
    return 0;
}

// ---- 4. the keyboard/mouse pad --------------------------------------------------------------------------------------

int test_keyboard_pad() {
    clear_sources();
    KeyboardPad pad;
    // Confirm/cancel/start/menu and the face buttons.
    g_keys[kVkReturn] = true;
    PadState s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.buttons & PadA);
    g_keys[kVkReturn] = false;
    g_keys[kVkSpace] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.buttons & PadA);
    g_keys[kVkSpace] = false;
    g_keys[kVkBack] = true;
    CHECK(pad.poll(kFakeKeyboard, KeyboardPadMode::Always).buttons & PadB);
    g_keys[kVkBack] = false;
    g_keys[kVkEscape] = true;
    CHECK(pad.poll(kFakeKeyboard, KeyboardPadMode::Always).buttons & PadMenu);
    g_keys[kVkEscape] = false;
    g_keys[kVkTab] = true;
    CHECK(pad.poll(kFakeKeyboard, KeyboardPadMode::Always).buttons & PadView);
    g_keys[kVkTab] = false;
    g_keys[kVkX] = true;
    g_keys[kVkY] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK((s.buttons & PadX) && (s.buttons & PadY));
    g_keys[kVkX] = g_keys[kVkY] = false;
    g_keys[kVkQ] = g_keys[kVkE] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK((s.buttons & PadLeftShoulder) && (s.buttons & PadRightShoulder));
    g_keys[kVkQ] = g_keys[kVkE] = false;

    // WASD and the arrows push the left stick; a diagonal sits on the round gate; opposite keys cancel.
    g_keys[kVkW] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.left_y == 1.0f && s.left_x == 0.0f);
    g_keys[kVkW] = false;
    g_keys[kVkUp] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.left_y == 1.0f);
    g_keys[kVkUp] = false;
    g_keys[kVkD] = true;
    CHECK(pad.poll(kFakeKeyboard, KeyboardPadMode::Always).left_x == 1.0f);
    g_keys[kVkW] = true;  // D + W: diagonal
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.left_x > 0.70f && s.left_x < 0.71f);
    CHECK(s.left_y > 0.70f && s.left_y < 0.71f);
    g_keys[kVkW] = false;
    g_keys[kVkA] = true;  // D + A cancel
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.left_x == 0.0f);
    g_keys[kVkD] = g_keys[kVkA] = false;

    // The mouse: the buttons are the triggers; movement pushes the right stick and it decays to zero.
    g_mouse[0] = true;
    g_mouse[1] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.right_trigger == 1.0f && s.left_trigger == 1.0f);
    g_mouse[0] = g_mouse[1] = false;
    pad = KeyboardPad{};  // a fresh stick and cursor baseline
    pad.poll(kFakeKeyboard, KeyboardPadMode::Always);  // baseline cursor
    g_cursor_x = 64;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.right_x == 1.0f);  // 64 px * 1/32, clamped to full
    int polls = 1;
    while (pad.right_x() != 0.0f && polls < 200) {
        pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
        ++polls;
    }
    CHECK(pad.right_x() == 0.0f);
    CHECK(polls <= 70);  // full to zero in ~64 polls (128 ms at the 2 ms poll)
    g_cursor_y = 64;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.right_y == -1.0f);  // moving the mouse down pushes the stick down (-Y)

    // Focus: in Focused mode losing focus releases everything and drops the cursor baseline.
    g_focused = false;
    g_keys[kVkA] = true;
    g_mouse[0] = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Focused);
    CHECK(s.buttons == 0 && s.left_x == 0 && s.right_trigger == 0);
    CHECK(pad.right_x() == 0.0f && pad.right_y() == 0.0f);
    g_focused = true;
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Always);
    CHECK(s.left_x == -1.0f);  // Always reads whatever has focus
    g_keys[kVkA] = false;
    g_mouse[0] = false;
    // Off is neutral and resets.
    s = pad.poll(kFakeKeyboard, KeyboardPadMode::Off);
    CHECK(s.buttons == 0 && s.right_x == 0.0f);
    return 0;
}

// ---- 5. the scripted schedule ---------------------------------------------------------------------------------------

int test_scripted_schedule() {
    CHECK(ScriptedPresses::parse(nullptr) == std::nullopt);
    CHECK(ScriptedPresses::parse("") == std::nullopt);
    CHECK(ScriptedPresses::parse("0x10") == std::nullopt);
    CHECK(ScriptedPresses::parse("1e3") == std::nullopt);
    CHECK(ScriptedPresses::parse("-1") == std::nullopt);
    CHECK(ScriptedPresses::parse("1.5s") == std::nullopt);
    const auto parsed = ScriptedPresses::parse("2.5");
    CHECK(parsed.has_value());
    CHECK(parsed->first_press_us() == 2'500'000);

    ScriptedPresses schedule(1'000'000);
    CHECK(schedule.presses() == ScriptedPresses::kPresses);
    CHECK(schedule.buttons_at(999'999) == 0);
    CHECK(schedule.buttons_at(1'000'000) == PadA);     // confirm, press 0
    CHECK(schedule.buttons_at(1'149'999) == PadA);
    CHECK(schedule.buttons_at(1'150'000) == 0);        // released
    CHECK(schedule.buttons_at(2'000'000) == PadMenu);   // start, press 1
    CHECK(schedule.buttons_at(3'000'000) == PadA);      // press 2
    CHECK(schedule.buttons_at(30'000'000) == PadMenu);  // press 29, the last
    CHECK(schedule.finished_at(30'149'999) == false);
    CHECK(schedule.finished_at(30'150'000) == true);
    CHECK(schedule.ends_at_us() == 30'150'000);
    // A zero delay starts the first press at the epoch; a refused value never becomes a schedule.
    ScriptedPresses immediate(0);
    CHECK(immediate.buttons_at(0) == PadA);
    CHECK(ScriptedPresses::parse("soon") == std::nullopt);
    return 0;
}

// ---- 6. the active-input record -------------------------------------------------------------------------------------

int test_active_input_feed() {
    ActiveInputFeed active;
    CHECK(active.family() == palmetto::InputFamily::Xbox);
    PadState rest;
    active.observe(0, palmetto::InputFamily::Xbox, rest, 0);  // first sample of device 0: baseline, no change
    active.observe(4, palmetto::InputFamily::Keyboard, rest, 500'000);  // first sample of device 4: baseline
    CHECK(active.family() == palmetto::InputFamily::Xbox);
    PadState pressed = rest;
    pressed.buttons = PadA;
    active.observe(4, palmetto::InputFamily::Keyboard, pressed, 1'000'000);
    CHECK(active.family() == palmetto::InputFamily::Keyboard);
    CHECK(active.generation() == 1);
    // An Xbox pad used inside the 300 ms window is ignored, not queued.
    const uint64_t before = active.generation();
    active.observe(0, palmetto::InputFamily::Xbox, pressed, 1'100'000);
    CHECK(active.family() == palmetto::InputFamily::Keyboard);
    CHECK(active.generation() == before);
    // After the window, a deliberate edge from the pad takes over.
    PadState pad_edge = rest;
    pad_edge.buttons = PadB;
    active.observe(0, palmetto::InputFamily::Xbox, pad_edge, 1'500'000);
    CHECK(active.family() == palmetto::InputFamily::Xbox);
    return 0;
}

// ---- 7. the v0 ABI over the fake source -----------------------------------------------------------------------------

struct CallbackLog {
    std::vector<gi::IGameInputDevice*> devices;
    std::vector<uint32_t> statuses;
    std::vector<uint32_t> previous;
};

void CALLBACK on_device(gi::GameInputCallbackToken, void* context, gi::IGameInputDevice* device, uint64_t,
                        uint32_t status, uint32_t previous) {
    auto* log = static_cast<CallbackLog*>(context);
    log->devices.push_back(device);
    log->statuses.push_back(status);
    log->previous.push_back(previous);
}

int test_abi_over_fake_source() {
    clear_sources();
    g_fake[0].present = true;
    g_fake[0].pad.buttons = XiA;
    g_fake[0].pad.left_x = 16384;
    detail::reset_for_test();
    detail::poll_at(1'000);

    gi::IGameInput* input = nullptr;
    CHECK(GameInputCreate(&input) == S_OK);
    CHECK(input != nullptr);

    // One gamepad is present and pairs: a blocking-enumeration device callback hears it during registration.
    CallbackLog log;
    gi::GameInputCallbackToken token = 0;
    CHECK(input->RegisterDeviceCallback(nullptr, gi::GameInputKindGamepad, gi::GameInputDeviceAnyStatus,
                                        gi::GameInputBlockingEnumeration, &log, &on_device, &token) == S_OK);
    CHECK(token != 0);
    CHECK(log.devices.size() == 1);
    CHECK(log.statuses.size() == 1 && (log.statuses[0] & gi::GameInputDeviceConnected) != 0);
    gi::IGameInputDevice* device = log.devices[0];
    CHECK(device != nullptr);
    const gi::GameInputDeviceInfo* info = device->GetDeviceInfo();
    CHECK(info != nullptr);
    CHECK(info->infoSize == sizeof(gi::GameInputDeviceInfo));
    CHECK(info->deviceFamily == gi::GameInputFamilyConsoleVariant2);
    CHECK(info->supportedInput & gi::GameInputKindGamepad);
    CHECK(info->controllerAxisCount == 6 && info->controllerButtonCount == 14);
    CHECK(info->gamepadInfo != nullptr && info->gamepadInfo->aButtonLabel == gi::GameInputLabelXboxA);
    CHECK(info->uiNavigationInfo != nullptr && info->uiNavigationInfo->acceptButtonLabel == gi::GameInputLabelXboxA);
    CHECK(device->GetDeviceStatus() & gi::GameInputDeviceConnected);

    // GetCurrentReading answers the pad state, and the reading's gamepad and UI-navigation shapes are GameInput's.
    gi::IGameInputReading* current = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, device, &current) == S_OK);
    CHECK(current != nullptr);
    gi::GameInputGamepadState state{};
    CHECK(current->GetGamepadState(&state));
    CHECK(state.buttons == gi::GameInputGamepadA);
    CHECK(state.leftThumbstickX > 0.49f && state.leftThumbstickX < 0.51f);
    gi::GameInputUiNavigationState ui{};
    CHECK(current->GetUiNavigationState(&ui));
    CHECK(ui.buttons & 0x00000004);  // Accept
    CHECK(current->GetInputKind() & gi::GameInputKindGamepad);
    CHECK(current->GetSequenceNumber(gi::GameInputKindGamepad) > 0);
    uint32_t axes_count = 0;
    float axes[6] = {};
    axes_count = current->GetControllerAxisState(6, axes);
    CHECK(axes_count == 6 && axes[0] > 0.74f && axes[0] < 0.76f);  // 0.5 + 0.5 * 0.5
    bool buttons[14] = {};
    CHECK(current->GetControllerButtonState(14, buttons) == 14);
    CHECK(buttons[2] == true);  // A is bit 2 in GameInput's order

    // A change makes a new reading; the old one is still reachable backwards, the new one forwards.
    g_fake[0].pad.buttons = XiB;
    detail::poll_at(17'000);
    const uint64_t first_sequence =
        current->GetSequenceNumber(gi::GameInputKindGamepad);
    gi::IGameInputReading* newer = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, device, &newer) == S_OK);
    CHECK(newer != nullptr);
    CHECK(newer->GetSequenceNumber(gi::GameInputKindGamepad) > first_sequence);
    gi::IGameInputReading* forward = nullptr;
    CHECK(input->GetNextReading(current, gi::GameInputKindGamepad, device, &forward) == S_OK);
    gi::IGameInputReading* backward = nullptr;
    CHECK(input->GetPreviousReading(newer, gi::GameInputKindGamepad, device, &backward) == S_OK);
    gi::GameInputGamepadState b_state{};
    CHECK(forward->GetGamepadState(&b_state) && b_state.buttons == gi::GameInputGamepadB);
    CHECK(backward->GetGamepadState(&b_state) && b_state.buttons == gi::GameInputGamepadA);

    // Unplug: the device callback hears the stop, and a reading query answers not-found.
    g_fake[0].present = false;
    detail::poll_at(33'000);
    CHECK(log.devices.size() == 2);
    CHECK(log.statuses[1] == gi::GameInputDeviceNoStatus);
    CHECK(log.previous[1] & gi::GameInputDeviceConnected);
    gi::IGameInputReading* gone = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, device, &gone) == gi::GAMEINPUT_E_READING_NOT_FOUND);
    CHECK(gone == nullptr);

    // Replug: the same device object comes back, connected, and a new reading exists.
    g_fake[0].present = true;
    g_fake[0].pad.buttons = XiX;
    detail::poll_at(49'000);
    CHECK(log.devices.size() == 3);
    CHECK(log.devices[2] == log.devices[0]);
    CHECK(log.statuses[2] & gi::GameInputDeviceConnected);
    gi::IGameInputReading* back = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, device, &back) == S_OK);
    CHECK(back->GetGamepadState(&b_state) && b_state.buttons == gi::GameInputGamepadX);

    // The scripted first-run pad and the keyboard pad are devices too, and the keyboard pad reports the
    // keyboard-and-mouse family to the runtime's active-input record.
    CHECK(input->UnregisterCallback(token, 0));
    current->Release();
    newer->Release();
    forward->Release();
    backward->Release();
    back->Release();
    input->Release();
    return 0;
}

// ---- 8. the scripted pad through the ABI, on an absolute poll clock --------------------------------------------------

int test_scripted_over_abi() {
    clear_sources();
    SetEnvironmentVariableA("PALMETTO_FAKE_INPUT", "1");
    detail::reset_for_test();
    // The schedule is relative to the first GameInputCreate (the backend's epoch), while the poll clock is absolute
    // (QPC-derived). The backend must subtract its epoch: arm the epoch so the real clock is 0.5 s into the schedule,
    // then re-arm it for the 1 s and 2 s press windows, and the presses must land on those windows.
    detail::state().epoch_us = detail::now_us() - 500'000;
    detail::poll_at(detail::now_us());
    gi::IGameInput* input = nullptr;
    CHECK(GameInputCreate(&input) == S_OK);
    CallbackLog log;
    gi::GameInputCallbackToken token = 0;
    CHECK(input->RegisterDeviceCallback(nullptr, gi::GameInputKindGamepad, gi::GameInputDeviceAnyStatus,
                                        gi::GameInputBlockingEnumeration, &log, &on_device, &token) == S_OK);
    CHECK(log.devices.size() == 1);  // the scripted pad is the only one connected
    gi::IGameInputReading* reading = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, log.devices[0], &reading) == S_OK);
    gi::GameInputGamepadState state{};
    CHECK(reading->GetGamepadState(&state));
    CHECK(state.buttons == 0);
    reading->Release();
    detail::state().epoch_us = detail::now_us() - 1'050'000;  // 1.05 s in: confirm (A) held
    detail::poll_at(detail::now_us());
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, log.devices[0], &reading) == S_OK);
    CHECK(reading->GetGamepadState(&state) && state.buttons == gi::GameInputGamepadA);
    reading->Release();
    detail::state().epoch_us = detail::now_us() - 2'050'000;  // 2.05 s in: start (Menu) held
    detail::poll_at(detail::now_us());
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, log.devices[0], &reading) == S_OK);
    CHECK(reading->GetGamepadState(&state) && state.buttons == gi::GameInputGamepadMenu);
    reading->Release();
    input->Release();
    SetEnvironmentVariableA("PALMETTO_FAKE_INPUT", nullptr);
    return 0;
}

// ---- 9. the default paired pad and its fallback (docs/decomp_first_ui.md 12) ---------------------------------------
//
// A console boot always has a pad; the always-built backend now presents one neutral pad by default, so a title's
// first-run code that reads a device finds one. PALMETTO_PAD_PRESENT=0 is the fallback (no device, the pre-default
// host state), and a refused PALMETTO_FAKE_INPUT value still leaves the pad present and neutral (it presses nothing).
int test_default_pad_presence() {
    clear_sources();
    SetEnvironmentVariableA("PALMETTO_PAD_PRESENT", nullptr);
    detail::reset_for_test();
    detail::poll_at(1'000);

    gi::IGameInput* input = nullptr;
    CHECK(GameInputCreate(&input) == S_OK);
    CallbackLog log;
    gi::GameInputCallbackToken token = 0;
    CHECK(input->RegisterDeviceCallback(nullptr, gi::GameInputKindGamepad, gi::GameInputDeviceAnyStatus,
                                        gi::GameInputBlockingEnumeration, &log, &on_device, &token) == S_OK);
    CHECK(log.devices.size() == 1);  // the default pad is the only device: no physical XInput pad in the fake source
    CHECK(log.statuses.size() == 1 && (log.statuses[0] & gi::GameInputDeviceConnected) != 0);
    gi::IGameInputReading* reading = nullptr;
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, log.devices[0], &reading) == S_OK);
    gi::GameInputGamepadState state{};
    CHECK(reading->GetGamepadState(&state));
    CHECK(state.buttons == 0);  // present and neutral: presence is the runtime answer, presses are a stimulus
    reading->Release();
    CHECK(input->UnregisterCallback(token, 0));

    // A refused press schedule still leaves the default pad present and neutral.
    SetEnvironmentVariableA("PALMETTO_FAKE_INPUT", "not-a-number");
    detail::reset_for_test();
    detail::poll_at(2'000);
    CallbackLog invalid_log;
    CHECK(input->RegisterDeviceCallback(nullptr, gi::GameInputKindGamepad, gi::GameInputDeviceAnyStatus,
                                        gi::GameInputBlockingEnumeration, &invalid_log, &on_device, &token) == S_OK);
    CHECK(invalid_log.devices.size() == 1);
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, invalid_log.devices[0], &reading) == S_OK);
    CHECK(reading->GetGamepadState(&state) && state.buttons == 0);
    reading->Release();
    CHECK(input->UnregisterCallback(token, 0));
    SetEnvironmentVariableA("PALMETTO_FAKE_INPUT", nullptr);

    // Fallback: PALMETTO_PAD_PRESENT=0 removes the pad, so the title finds no device and no reading.
    SetEnvironmentVariableA("PALMETTO_PAD_PRESENT", "0");
    detail::reset_for_test();
    detail::poll_at(3'000);
    gi::IGameInputReading* none = reinterpret_cast<gi::IGameInputReading*>(1);
    CHECK(input->GetCurrentReading(gi::GameInputKindGamepad, nullptr, &none) == gi::GAMEINPUT_E_READING_NOT_FOUND);
    CHECK(none == nullptr);
    SetEnvironmentVariableA("PALMETTO_PAD_PRESENT", nullptr);
    input->Release();
    return 0;
}

}  // namespace

int main() {
    struct Case {
        const char* name;
        int (*run)();
    };
    const Case cases[] = {
        {"XInput mapping", test_xinput_mapping},
        {"hot-plug and pairing", test_hotplug_and_pairing},
        {"readings", test_reading_stream},
        {"keyboard/mouse pad", test_keyboard_pad},
        {"scripted schedule", test_scripted_schedule},
        {"active input record", test_active_input_feed},
        {"v0 ABI over a fake source", test_abi_over_fake_source},
        {"scripted pad over the ABI", test_scripted_over_abi},
        {"default pad presence", test_default_pad_presence},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const int r = c.run();
        std::printf("%s: %s\n", c.name, r ? "FAIL" : "ok");
        failures += r;
    }
    const int total = static_cast<int>(std::size(cases));
    std::printf("no-SDL input backend: %d of %d cases pass\n", total - failures, total);
    return failures ? 1 : 0;
}
