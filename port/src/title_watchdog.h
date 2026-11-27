#pragma once

// Present-side attestation of the media/UI body and an opt-in hold of the title's own hang-watchdog pause switch (P2 and
// P3 of docs/peer_port_first_ui.md; section 13 of that page has the measurement, the design, the A/B and the limits).
// The peer ports were read as evidence only, under docs/upstream_port.md: every constant below is re-measured in this
// image, the rule, thunk, parser and tests are ours, and the credit is in the doc section.
//
// ---- The measured watchdog (image RVAs; the retail image's preferred base is 0x140000000) ----
//
// image+0x2215A60 is the title's own hang watchdog, a thread created by image+0x2215A20 at the image's run-once point.
// Each pass reads QueryPerformanceCounter and sleeps 1000 ms; when the heartbeat qword at image+0x71B6338 has not
// advanced for 0x1E passes it calls the callback pointer at image+0x71B6348 and then executes the deliberate null write
// at image+0x2215C35 (`mov dword ptr [0], 0`) followed by int3.
//
// image+0x71B6330 is the title's own pause switch for that watchdog, one byte at the start of the block:
//   * image+0x2214A90 is its setter: `mov byte ptr [image+0x71B6330], 1` then `ret`, a leaf the title calls directly
//     from its loading/transition code (the two measured call sites are image+0x21B6969 and image+0x2214814).
//   * the watchdog reads it at image+0x2215B50; while bit 0 is set it stores the current counter into the heartbeat and
//     resets the pass budget, so the abort is deferred. The hold is therefore the title's own mechanism, not an
//     invented one; the peers hold the same byte, on the same image revision.
//   * image+0x22161B0 is the title's own release/teardown path: it clears the pause byte and sets the watchdog's stop
//     byte at image+0x71B6340 (whose bit 0 ends the watchdog thread at image+0x2215AB8).
//
// Holding the byte is honest only while a wait the title itself is in is legitimately outstanding. This header's rule
// holds it only while the measured loading state word says a load is running (image+0x7453ED0 in 2..5) and the state
// machine is in one of the boot's load stages (the current u16 at [[image+0x71273C8]+0x10] in 2..3), only after the
// frame heartbeat has been observed advancing at least once, and only for a hard bound of seconds. It releases as soon
// as that wait resolves or the bound expires, and a bound expiry is a named refusal that does not re-arm while the same
// wait is still outstanding: the monitor must never mask a crash indefinitely. The default path never calls any of this.
//
// ---- The measured media/UI present body ----
//
// image+0x22185A0 is the boot-media/UI body the present path waits on. Its measured entry prefix (16 bytes) is
// `push rsi; sub rsp,0x40; cmp byte [image+0x71B90AD],1; jne exit; cmp byte [image+0x71B90AC],1; jne exit; ...`, so the
// body exits on its two present-pass keys until both read 1. A counting thunk on that entry attests that the body ran
// (the body's own words are sampled beside it), while the console queue/allocator proxies attest the presented frames.
// The presented plane's identity and the capture mean stay the existing presenter path's measurements (doc 13.5).
//
// Everything here is off unless a PALMETTO_* switch is set: a default arm writes no title byte, installs no hook and
// starts no thread, so its path is the tree's unchanged one.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include "code_hooks.h"

namespace palmetto::title_watchdog {

// ---- The measured image shape ---------------------------------------------------------------------------------------

// The title's watchdog pause byte: bit 0 = hold (the watchdog defers its abort while set).
inline constexpr std::uint32_t kPauseByteRva = 0x71B6330u;
// The title's own setter for that byte, a 7-byte leaf: mov byte ptr [rip+disp32], 1 ; ret.
inline constexpr std::uint32_t kPauseSetterRva = 0x2214A90u;
inline constexpr std::uint8_t kPauseSetterBytes[7] = {0xC6, 0x05, 0x99, 0x18, 0xFA, 0x04, 0x01};
// The watchdog's own read of the byte: mov cl, byte ptr [rip+disp32] (the defer branch follows the test).
inline constexpr std::uint32_t kWatchdogPauseReadRva = 0x2215B50u;
inline constexpr std::uint8_t kWatchdogPauseReadBytes[6] = {0x8A, 0x0D, 0xDA, 0x07, 0xFA, 0x04};
// The watchdog's stop byte: bit 0 set ends the watchdog thread instead of the defer branch.
inline constexpr std::uint32_t kWatchdogStopByteRva = 0x71B6340u;
// The heartbeat qword the per-frame tick pets (image+0x22159F0 stores QueryPerformanceCounter there).
inline constexpr std::uint32_t kHeartbeatRva = 0x71B6338u;
// The deliberate null write: mov dword ptr [0], 0 (the abort this hold defers).
inline constexpr std::uint32_t kWatchdogAbortRva = 0x2215C35u;
inline constexpr std::uint8_t kWatchdogAbortBytes[7] = {0xC7, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
// The pass budget and the pass sleep that set the measured ~30 s window (0x1E one-second passes).
inline constexpr std::uint32_t kWatchdogBudgetPasses = 0x1Eu;
inline constexpr std::uint32_t kWatchdogPassMs = 1000u;
// The pause byte, the stop byte and the heartbeat qword are one 0x18-byte block starting at the pause byte; checking
// that range covers all three bounds (0x71B6330..0x71B6347).
inline constexpr std::uint32_t kWatchdogBlockBytes = 0x18u;

// The measured loading/stage window a hold may cover: the loading state word's "a load is running" band and the boot's
// load stages (the state trace's own meanings, docs/decomp_first_ui.md sections 11-12 and 17).
inline constexpr std::uint32_t kLoadingStateRva = 0x7453ED0u;
inline constexpr std::uint32_t kLoadingRunningFirst = 2u;
inline constexpr std::uint32_t kLoadingRunningLast = 5u;
inline constexpr std::uint32_t kStateMachinePointerRva = 0x71273C8u;
inline constexpr std::uint32_t kStateMachineCurrentStageOffset = 0x10u;
inline constexpr std::uint16_t kHoldStageFirst = 2u;
inline constexpr std::uint16_t kHoldStageLast = 3u;
// The hard bound the switch's value names, in seconds (a whole number, 1..600). The measured watchdog window is 30 s,
// so a bound in minutes covers many windows; the bound is what keeps the monitor from masking a truly hung title.
inline constexpr unsigned kHoldBoundMinSeconds = 1u;
inline constexpr unsigned kHoldBoundMaxSeconds = 600u;

// The measured media/UI present body's entry, its two present-pass keys and the gate the stage-2 wait releases it on.
inline constexpr std::uint32_t kBodyEntryRva = 0x22185A0u;
// The first 16 bytes of the entry: push rsi; sub rsp,0x40; cmp byte [image+0x71B90AD],1; jne +0xE8; cmp byte
// [image+0x71B90AC],1; ... The disp32 in bytes 7..10 is the check that this is the measured body and not a same-shaped
// function: it must equal kBootMediaActiveRva - (kBodyEntryRva + 11).
inline constexpr std::uint8_t kBodyEntryPrefix[16] = {0x56, 0x48, 0x83, 0xEC, 0x40, 0x80, 0x3D, 0x01,
                                                      0x0B, 0xFA, 0x04, 0x01, 0x0F, 0x85, 0xE8, 0x00};
inline constexpr std::uint32_t kBootMediaActiveRva = 0x71B90ADu;  // the first key the body compares
inline constexpr std::uint32_t kBootMediaFlagRva = 0x71B90ACu;    // the second key the body compares
inline constexpr std::uint32_t kBootMediaGateRva = 0x71B9048u;    // 2 = the boot-media stage the stage-2 wait wants
inline constexpr std::uint32_t kBootMediaSemaphoreARva = 0x71B9078u;
inline constexpr std::uint32_t kBootMediaSemaphoreBRva = 0x71B9088u;

// ---- Named refusals and image-shape validation -----------------------------------------------------------------------

enum class ImageShape {
    Ok,
    NotMapped,             // a null image or a zero size
    TooSmall,              // a measured RVA is outside the mapped image
    PauseSetterMismatch,   // image+kPauseSetterRva is not the measured setter bytes
    WatchdogReadMismatch,  // image+kWatchdogPauseReadRva is not the measured read of the pause byte
    AbortSiteMismatch,     // image+kWatchdogAbortRva is not the measured null write
    BodyEntryMismatch,     // image+kBodyEntryRva is not the measured body entry (only when asked for)
};

inline const char* image_shape_name(ImageShape shape) {
    switch (shape) {
        case ImageShape::Ok: return "the measured image shape";
        case ImageShape::NotMapped: return "the image is not mapped";
        case ImageShape::TooSmall: return "a measured RVA is outside the mapped image";
        case ImageShape::PauseSetterMismatch: return "image+0x2214A90 is not the measured pause setter";
        case ImageShape::WatchdogReadMismatch: return "the watchdog does not read the measured pause byte";
        case ImageShape::AbortSiteMismatch: return "image+0x2215C35 is not the measured watchdog abort";
        case ImageShape::BodyEntryMismatch: return "image+0x22185A0 is not the measured present body";
    }
    return "unknown";
}

namespace detail {

inline bool range_inside(std::uint32_t rva, std::size_t bytes, std::uint32_t image_size) {
    return bytes <= image_size && rva <= image_size - static_cast<std::uint32_t>(bytes);
}

inline bool bytes_are(const std::uint8_t* at, const std::uint8_t* expected, std::size_t count) {
    return at != nullptr && std::memcmp(at, expected, count) == 0;
}

}  // namespace detail

// Validates the measured constants against the mapped image. `need_body_entry` also checks the present body's entry.
// Refuses by name, never guesses.
inline ImageShape validate_image(const std::uint8_t* image, std::uint32_t image_size, bool need_body_entry) {
    if (image == nullptr || image_size == 0) return ImageShape::NotMapped;
    if (!detail::range_inside(kPauseSetterRva, sizeof kPauseSetterBytes, image_size) ||
        !detail::range_inside(kPauseByteRva, kWatchdogBlockBytes, image_size) ||
        !detail::range_inside(kWatchdogPauseReadRva, sizeof kWatchdogPauseReadBytes, image_size) ||
        !detail::range_inside(kWatchdogAbortRva, sizeof kWatchdogAbortBytes, image_size))
        return ImageShape::TooSmall;
    if (!detail::bytes_are(image + kPauseSetterRva, kPauseSetterBytes, sizeof kPauseSetterBytes))
        return ImageShape::PauseSetterMismatch;
    if (!detail::bytes_are(image + kWatchdogPauseReadRva, kWatchdogPauseReadBytes, sizeof kWatchdogPauseReadBytes))
        return ImageShape::WatchdogReadMismatch;
    if (!detail::bytes_are(image + kWatchdogAbortRva, kWatchdogAbortBytes, sizeof kWatchdogAbortBytes))
        return ImageShape::AbortSiteMismatch;
    if (need_body_entry) {
        if (!detail::range_inside(kBodyEntryRva, sizeof kBodyEntryPrefix, image_size)) return ImageShape::TooSmall;
        if (!detail::bytes_are(image + kBodyEntryRva, kBodyEntryPrefix, sizeof kBodyEntryPrefix))
            return ImageShape::BodyEntryMismatch;
    }
    return ImageShape::Ok;
}

// ---- The switch parsers -----------------------------------------------------------------------------------------------

// PALMETTO_PRESENT_ATTEST's value: set and not 0/off/false/no (case-insensitive, surrounding blanks ignored). Unset or
// any of the off spellings keeps the default path, which is every current arm's path.
inline bool flag_enabled(const char* value) {
    if (value == nullptr) return false;
    while (*value == ' ' || *value == '\t') ++value;
    const char* end = value;
    while (*end != '\0') ++end;
    while (end != value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    const std::size_t size = static_cast<std::size_t>(end - value);
    if (size == 0) return false;
    if (size == 1 && value[0] == '0') return false;
    const auto is = [&](const char* word) {
        const std::size_t length = std::strlen(word);
        if (length != size) return false;
        for (std::size_t i = 0; i < size; ++i) {
            char c = value[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != word[i]) return false;
        }
        return true;
    };
    return !(is("off") || is("false") || is("no"));
}

// PALMETTO_TITLE_WATCHDOG_HOLD's value: unset/empty/0/off/false/no = off; "watch" = read the words and log, never write;
// a whole number of seconds in 1..600 = hold with that hard bound. Anything else is refused by name, so a typo cannot
// silently turn a hold on with a bound nobody chose.
enum class HoldMode { Off, Watch, Hold };

struct HoldSpec {
    HoldMode mode = HoldMode::Off;
    double bound_seconds = 0.0;
    const char* refusal = nullptr;  // non-null when the value was not understood
};

inline HoldSpec parse_hold_spec(const char* value) {
    HoldSpec spec;
    if (value == nullptr) return spec;
    while (*value == ' ' || *value == '\t') ++value;
    const char* end = value;
    while (*end != '\0') ++end;
    while (end != value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    const std::size_t size = static_cast<std::size_t>(end - value);
    if (size == 0 || (size == 1 && value[0] == '0')) return spec;
    const auto equals = [&](const char* word) {
        const std::size_t length = std::strlen(word);
        if (length != size) return false;
        for (std::size_t i = 0; i < size; ++i) {
            char c = value[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != word[i]) return false;
        }
        return true;
    };
    if (equals("off") || equals("false") || equals("no")) return spec;
    if (equals("watch")) {
        spec.mode = HoldMode::Watch;
        return spec;
    }
    unsigned seconds = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            spec.refusal = "PALMETTO_TITLE_WATCHDOG_HOLD is not off, watch or a whole number of seconds in 1..600";
            return spec;
        }
        seconds = seconds * 10u + static_cast<unsigned>(value[i] - '0');
        if (seconds > kHoldBoundMaxSeconds) {
            spec.refusal = "PALMETTO_TITLE_WATCHDOG_HOLD is above the 600 s hard-bound maximum";
            return spec;
        }
    }
    if (seconds < kHoldBoundMinSeconds) {
        spec.refusal = "PALMETTO_TITLE_WATCHDOG_HOLD is below the 1 s hard-bound minimum";
        return spec;
    }
    spec.mode = HoldMode::Hold;
    spec.bound_seconds = static_cast<double>(seconds);
    return spec;
}

// ---- The pause-hold rule (pure: no Win32, no clock, no title writes) -------------------------------------------------

struct HoldConfig {
    HoldMode mode = HoldMode::Off;
    double bound_seconds = 0.0;
};

// One observation of the title, taken by the caller from the measured words.
struct HoldSample {
    double now_seconds = 0.0;
    std::uint32_t loading = 0;        // image+0x7453ED0 (0 idle, 1 subsystem initialised, 2..5 a load is running)
    std::uint16_t stage_current = 0;  // [[image+0x71273C8]+0x10]
    std::uint64_t heartbeat = 0;      // image+0x71B6338
    bool heartbeat_advanced = false;  // the monitor saw the heartbeat change at least once
    bool watchdog_stopped = false;    // image+0x71B6340 bit 0
    std::uint8_t pause_byte = 0;      // image+0x71B6330, the value observed before any write
};

enum class HoldPhase {
    Idle,                    // no load wait, or none has been seen yet
    Waiting,                 // a load wait is outstanding but the title's heartbeat has not been seen yet
    Holding,                 // the monitor is writing the pause byte
    ReleasedResolved,        // the earlier hold was released because the wait resolved
    ReleasedBound,           // the earlier hold was released because the hard bound expired; no re-arm for that wait
    RefusedWatchdogStopped,  // the title's own stop byte is set: the watchdog is already gone
};

enum class HoldAction { None, WriteOne, Restore };

struct HoldState {
    HoldPhase phase = HoldPhase::Idle;
    const char* why = "no sample yet";
    double hold_started = 0.0;
    double held_seconds = 0.0;
    std::uint64_t writes = 0;
    unsigned holds = 0;
    std::uint8_t restore_value = 0;
    bool restore_taken = false;
    bool bound_exhausted_for_wait = false;
};

// Whether the measured words say a load the title is waiting for is running now.
inline bool load_wait_outstanding(const HoldSample& sample) {
    return sample.loading >= kLoadingRunningFirst && sample.loading <= kLoadingRunningLast &&
           sample.stage_current >= kHoldStageFirst && sample.stage_current <= kHoldStageLast;
}

// The rule. Returns what the caller should do to the pause byte this tick and sets `state.why` to the named reason.
// Order: watch/off never write; a stopped watchdog is refused; then a resolved wait releases; then an outstanding wait
// holds, but only once the heartbeat has been seen and only inside the hard bound. A bound expiry is sticky while the
// same wait is still outstanding.
inline HoldAction hold_step(const HoldConfig& config, HoldState& state, const HoldSample& sample) {
    if (config.mode != HoldMode::Hold) {
        state.bound_exhausted_for_wait = false;
        const bool outstanding = load_wait_outstanding(sample);
        if (sample.watchdog_stopped) {
            state.phase = HoldPhase::RefusedWatchdogStopped;
            state.why = "watch only: the title's watchdog stop byte is set (image+0x71B6340 bit 0)";
        } else if (outstanding) {
            state.phase = HoldPhase::Waiting;
            state.why = config.mode == HoldMode::Watch ? "watch only: a loading wait is outstanding; no byte is written"
                                                       : "the switch is off";
        } else {
            state.phase = HoldPhase::Idle;
            state.why = config.mode == HoldMode::Watch ? "watch only: no loading wait is outstanding"
                                                       : "the switch is off";
        }
        return HoldAction::None;
    }
    if (sample.watchdog_stopped) {
        state.bound_exhausted_for_wait = false;
        state.phase = HoldPhase::RefusedWatchdogStopped;
        state.why = "the title's watchdog stop byte is set (image+0x71B6340 bit 0): the watchdog is already gone";
        return state.restore_taken ? HoldAction::Restore : HoldAction::None;
    }
    if (!load_wait_outstanding(sample)) {
        state.bound_exhausted_for_wait = false;
        if (state.phase == HoldPhase::Holding) {
            state.held_seconds += sample.now_seconds - state.hold_started;
            state.phase = HoldPhase::ReleasedResolved;
            state.why = "the measured loading wait resolved (loading/stage left the 2..5 / 2..3 window)";
            return state.restore_taken ? HoldAction::Restore : HoldAction::None;
        }
        state.phase = HoldPhase::Idle;
        state.why = "no load the title is waiting for is outstanding";
        return HoldAction::None;
    }
    // A load wait is outstanding.
    if (state.phase == HoldPhase::Holding) {
        if (sample.now_seconds - state.hold_started > config.bound_seconds) {
            state.held_seconds += sample.now_seconds - state.hold_started;
            state.bound_exhausted_for_wait = true;
            state.phase = HoldPhase::ReleasedBound;
            state.why = "the hard bound expired while the loading wait is still outstanding: not re-armed for this wait";
            return state.restore_taken ? HoldAction::Restore : HoldAction::None;
        }
        state.why = "holding: the measured loading wait is outstanding";
        ++state.writes;
        return HoldAction::WriteOne;
    }
    if (state.bound_exhausted_for_wait) {
        state.phase = HoldPhase::ReleasedBound;
        state.why = "the hard bound expired for this wait earlier: not re-armed until it resolves";
        return HoldAction::None;
    }
    if (!sample.heartbeat_advanced) {
        state.phase = HoldPhase::Waiting;
        state.why = "the loading wait is outstanding but the title's heartbeat has not been seen advancing yet";
        return HoldAction::None;
    }
    state.phase = HoldPhase::Holding;
    state.hold_started = sample.now_seconds;
    state.restore_value = sample.pause_byte;
    state.restore_taken = true;
    ++state.holds;
    state.why = "holding: the measured loading wait is outstanding";
    ++state.writes;
    return HoldAction::WriteOne;
}

inline const char* hold_phase_name(HoldPhase phase) {
    switch (phase) {
        case HoldPhase::Idle: return "idle";
        case HoldPhase::Waiting: return "waiting";
        case HoldPhase::Holding: return "holding";
        case HoldPhase::ReleasedResolved: return "released-resolved";
        case HoldPhase::ReleasedBound: return "released-bound";
        case HoldPhase::RefusedWatchdogStopped: return "refused-watchdog-stopped";
    }
    return "unknown";
}

// ---- Present-side attestation (pure classification of counters + the body's own words) -------------------------------

struct PresentCounters {
    std::uint64_t presents = 0;          // console queue proxy slot 32 calls (the layer's present path)
    std::uint64_t submits = 0;           // console queue proxy slot 35 calls
    std::uint64_t frame_callbacks = 0;   // console allocator proxy slot 9 calls (once per presented frame here)
    std::uint64_t body_entries = 0;      // entries of image+0x22185A0 counted by the counting thunk
    std::uint32_t gate = 0;              // image+0x71B9048 (2 = the boot-media stage)
    std::uint32_t active = 0;            // image+0x71B90AD, the body's first key
    std::uint32_t flag = 0;              // image+0x71B90AC, the body's second key
    std::uint64_t semaphore_a = 0;       // the body's release sites, image+0x71B9078 / image+0x71B9088
    std::uint64_t semaphore_b = 0;
};

// The three states the present side can name, plus "nothing yet" for the window before the first present.
enum class PresentBodyState {
    NothingYet,          // no present and no body entry observed yet
    PresentWithoutBody,  // the layer presented and the body's entry was never counted
    BodyExitsOnKeys,     // the body ran, but a present-pass key still reads 0: it exits at its first compares
    BodyPresentPass,     // the body ran and both present-pass keys read 1: the present pass was entered
};

inline PresentBodyState present_body_state(const PresentCounters& counters) {
    if (counters.body_entries == 0)
        return counters.presents == 0 ? PresentBodyState::NothingYet : PresentBodyState::PresentWithoutBody;
    if (counters.active == 0 || counters.flag == 0) return PresentBodyState::BodyExitsOnKeys;
    return PresentBodyState::BodyPresentPass;
}

inline const char* present_body_state_name(PresentBodyState state) {
    switch (state) {
        case PresentBodyState::NothingYet: return "nothing-yet";
        case PresentBodyState::PresentWithoutBody: return "present-without-body";
        case PresentBodyState::BodyExitsOnKeys: return "body-exits-on-keys";
        case PresentBodyState::BodyPresentPass: return "body-present-pass";
    }
    return "unknown";
}

// The bounded per-present line's counters: a change in this key is what the caller logs beyond the line cap.
struct AttestLine {
    PresentBodyState state = PresentBodyState::NothingYet;
    std::uint64_t presents = 0;
    std::uint64_t body_entries = 0;
    std::uint32_t active = 0;
    std::uint32_t flag = 0;
};

inline bool attest_changed(const AttestLine& before, const AttestLine& after) {
    return before.state != after.state || before.active != after.active || before.flag != after.flag;
}

// ---- The counting thunk -----------------------------------------------------------------------------------------------

// mov rax, <counter>; lock inc qword ptr [rax]; mov rax, <original>; jmp rax. It uses no stack, calls nothing and
// changes no argument register, so it can run at a function entry: the caller's return address stays on the stack and
// the Windows unwinder treats the jmp-only trampoline as a leaf (which is what it is).
inline constexpr std::size_t kCountThunkBytes = 26;

inline std::size_t build_count_thunk(std::uint8_t* out, std::size_t capacity,
                                     const std::atomic<std::uint64_t>* counter, const void* original) {
    if (out == nullptr || counter == nullptr || original == nullptr || capacity < kCountThunkBytes) return 0;
    std::uint8_t* p = out;
    p[0] = 0x48;  // mov rax, imm64
    p[1] = 0xB8;
    const std::uint64_t counter_address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(counter));
    std::memcpy(p + 2, &counter_address, sizeof(counter_address));
    p += 10;
    p[0] = 0xF0;  // lock inc qword ptr [rax]
    p[1] = 0x48;
    p[2] = 0xFF;
    p[3] = 0x00;
    p += 4;
    p[0] = 0x48;  // mov rax, imm64
    p[1] = 0xB8;
    const std::uint64_t original_address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(original));
    std::memcpy(p + 2, &original_address, sizeof(original_address));
    p += 10;
    p[0] = 0xFF;  // jmp rax
    p[1] = 0xE0;
    return kCountThunkBytes;
}

// Builds a counting thunk on an executable page within +-2 GiB of `close_to` so a rel32 branch in the image reaches it.
// The page is registered as a leaf function (no prolog, no unwind codes), so the unwinder can walk over it and the
// test can free it. Returns null when no page can be found. In the runtime the page is not freed at exit: the sites
// point at it and the harness exits with ExitProcess, and the mapped image is this process's private copy.
inline std::uint8_t* make_count_thunk(const void* close_to, std::atomic<std::uint64_t>* counter, const void* original) {
    std::uint8_t* const thunk = palmetto::hooks::alloc_near(close_to, 0x1000);
    if (thunk == nullptr) return nullptr;
    if (build_count_thunk(thunk, 0x1000, counter, original) == 0) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return nullptr;
    }
    return palmetto::hooks::finish_thunk(thunk, kCountThunkBytes, {0x01, 0, 0});
}

// ---- Installing the attestation's counting hook ----------------------------------------------------------------------

// Counts entries of the measured media/UI present body by redirecting its direct callers to a counting thunk. The
// counter is a plain atomic the sampler thread reads; nothing in the title sees a changed answer.
struct BodyCountHook {
    std::atomic<std::uint64_t> entries{0};
    std::uint8_t* thunk = nullptr;
    std::size_t redirected = 0;
    const char* error = "";
    bool ok = false;
};

namespace detail {

// The effective executable span of a mapped PE64 image's section, the same rule pe_loader and code_hooks use.
inline bool executable_span(const IMAGE_SECTION_HEADER& section, std::uint32_t image_size, std::uint64_t& va,
                            std::uint64_t& len) {
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) return false;
    va = section.VirtualAddress;
    const std::uint64_t raw =
        section.Misc.VirtualSize ? static_cast<std::uint64_t>(section.Misc.VirtualSize)
                                 : static_cast<std::uint64_t>(section.SizeOfRawData);
    if (raw == 0 || va >= image_size) return false;
    len = raw > image_size - va ? image_size - va : raw;
    return len > 0;
}

}  // namespace detail

// Points every direct E8/E9 rel32 in the image's executable sections whose target is image+rva at `thunk`, restoring the
// section protection afterwards. Returns the number of rewritten sites; sets `error` on a failed protection call.
inline std::size_t redirect_direct_calls_to(std::uint8_t* image, std::uint32_t image_size, std::uint32_t rva,
                                            const std::uint8_t* thunk, const char** error) {
    *error = "";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        *error = "not a mapped PE image";
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        *error = "not a mapped PE64 image";
        return 0;
    }
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* const target = image + rva;
    std::size_t redirected = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        std::uint64_t va = 0, len = 0;
        if (!detail::executable_span(sections[i], image_size, va, len)) continue;
        std::uint8_t* const begin = image + va;
        DWORD old = 0;
        if (!VirtualProtect(begin, static_cast<SIZE_T>(len), PAGE_EXECUTE_READWRITE, &old)) {
            *error = "VirtualProtect failed on an executable section";
            break;
        }
        redirected += palmetto::hooks::redirect_direct_branches(begin, static_cast<std::size_t>(len), target, thunk);
        DWORD ignored = 0;
        VirtualProtect(begin, static_cast<SIZE_T>(len), old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), begin, static_cast<SIZE_T>(len));
    }
    return redirected;
}

// Validates the measured body entry, then redirects every direct call to it at the counting thunk. Refuses by name
// (ImageShape) and leaves the image untouched on refusal; an installed hook reports `ok` and its covered site count.
inline ImageShape install_body_count_hook(std::uint8_t* image, std::uint32_t image_size, BodyCountHook& out_hook) {
    const ImageShape shape = validate_image(image, image_size, /*need_body_entry=*/true);
    if (shape != ImageShape::Ok) return shape;
    const std::uint8_t* const target = image + kBodyEntryRva;
    out_hook.thunk = make_count_thunk(target, &out_hook.entries, target);
    if (out_hook.thunk == nullptr) {
        out_hook.error = "no executable page within 2 GiB of the image for the counting thunk";
        return ImageShape::Ok;
    }
    out_hook.redirected = redirect_direct_calls_to(image, image_size, kBodyEntryRva, out_hook.thunk, &out_hook.error);
    out_hook.ok = out_hook.redirected > 0;
    if (!out_hook.ok) {
        // No site points at the thunk, so its page can be released. The image is untouched either way.
        palmetto::hooks::free_thunk(out_hook.thunk);
        out_hook.thunk = nullptr;
        if (out_hook.error[0] == '\0') out_hook.error = "no direct call site of image+0x22185A0 could be redirected";
    }
    return ImageShape::Ok;
}

}  // namespace palmetto::title_watchdog
