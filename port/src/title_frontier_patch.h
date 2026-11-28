// Round-22 frontier instruments for the first menus push (F-01 / media-UI present keys).
//
// Two opt-in diagnostic redirects, both inert unless their exact environment switch is "1":
//
//   PALMETTO_TITLE_OUTRECORD_STATE2=1
//     The session-3 completion dispatch at RVA 0x402E8A0 checks its engine out record's state at +0xac
//     (docs/decomp_stage.md 15.3-15.6). The measured out records all read state 0, so the dispatch takes the
//     re-register path (0x402D010) and the state-2 completion path (0x402E050 -> 0x4043390) never runs. This is the
//     bounded one-byte conditional redirect the section 15.6 falsifier asks for: at RVA 0x402E987 the `je +0x24`
//     (0x74 0x24) becomes `jmp +0x24` (0xEB 0x24) after the `cmp dword ptr [rdi+0xac], 2` at RVA 0x402E980, so an
//     engine out record that reads 0 still takes the state-2 completion branch. The site is validated byte-for-byte
//     before the write; a different image refuses by name. This is a diagnostic, not console-faithful behavior.
//
//   PALMETTO_TITLE_PRESENT_KEYS=1
//     The boot-media/UI present body at image+0x22185A0 exits unless both present-pass keys read 1: image+0x71B90AD
//     (frame-request flag) and image+0x71B90AC (present-side setter flag). Their only common producer is the type-0x24
//     app-state callback path, which measured boots never exercised (docs/decomp_first_ui.md section 11). This writer
//     forces both one-byte flags to 1 on a bounded interval (default 2 ms) for the process lifetime, so the body's
//     present pass can be measured without synthesizing a title event. It writes only those two bytes.
//
// The header is pure data/decision logic so it can be tested without mapping the title; main.cpp performs the
// VirtualProtect/patch and starts the writer thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace palmetto::title_frontier {

// The exact-value parser used by both switches: only the literal string "1" enables a path.
inline bool switch_enabled(const char* value) noexcept {
    return value && value[0] == '1' && value[1] == '\0';
}

// The measured completion-dispatch site (image-relative RVAs).
inline constexpr std::uint32_t kOutrecordCompareRva = 0x402E980u;
inline constexpr std::uint32_t kOutrecordBranchRva = 0x402E987u;
inline constexpr std::uint8_t kOutrecordBranchOriginal = 0x74u;  // je rel8
inline constexpr std::uint8_t kOutrecordBranchPatched = 0xEBu;   // jmp rel8
inline constexpr std::uint8_t kOutrecordBytes[9] = {0x83, 0xBF, 0xAC, 0x00, 0x00, 0x00, 0x02, 0x74, 0x24};
inline constexpr std::size_t kOutrecordBytesCount = sizeof(kOutrecordBytes);

// The two media/UI present-pass key bytes (image-relative RVAs).
inline constexpr std::uint32_t kPresentKeyRvas[2] = {0x71B90ADu, 0x71B90ACu};
inline constexpr std::uint32_t kPresentKeyWriteIntervalMs = 2u;
// A bounded burst, not a permanent override: 300 writes at 2 ms = 0.6 s. The first arm that forced the keys every 2 ms for
// the whole run made the body take its present pass but then froze presentation (presents/submits stopped at 1323/2646).
// The burst only triggers the pass; the title's own state machine clears the keys afterwards.
inline constexpr std::uint32_t kPresentKeyMaxWrites = 300u;

// The session-3 loading record's slot-4 completion dword (docs/decomp_stage.md 15.4-15.5; docs/decomp_first_ui.md
// section 17: the frame thread's 33 ms poll 0x21B7070 checks 0x41E8C0 on image+0x78ACB20, and nothing measured writes
// it). PALMETTO_TITLE_SESSION3_COMPLETE=1 writes 1 there on a bounded interval, which is the direct diagnostic for the
// post-session-3/menu path: session 3 -> 4, media start 0x1D24C80, and the frame pump that can issue the first movie/
// UI draw (docs/decomp_first_ui.md sections 14-18; T12 analysis).
inline constexpr std::uint32_t kSession3CompletionRva = 0x78ACB20u;
inline constexpr std::uint32_t kSession3CompletionValue = 1u;
inline constexpr std::uint32_t kSession3CompletionIntervalMs = 100u;
inline constexpr std::uint32_t kSession3CompletionMaxWrites = 2400u;  // 240 s at 100 ms

// Preconditions learned from the first unlocked arms: forcing either instrument from process start disturbs the title's
// early boot (the present-key arm stayed at init 1 / loading 0 / session 0 for 180 s, and the always-on completion
// writer stopped the title at an illegal instruction). Both writers therefore wait for the title's own measured gate
// first: the boot-media gate byte reaches 2 (stage 2 prepared) before the present-pass keys are forced, and the
// session state word reads 3 (the session-3 poll is running) before the record completion dword is forced.
inline constexpr std::uint32_t kBootMediaGateRva = 0x71B9048u;    // byte: 2 = the boot-media stage the stage-2 wait wants
inline constexpr std::uint8_t kBootMediaGateReady = 2u;
inline constexpr std::uint32_t kSessionStateRva = 0x98F828Cu;     // dword: the title's session state (3 = session-3 poll)
inline constexpr std::uint32_t kSessionStateThree = 3u;
inline constexpr std::uint32_t kInitStateRva = 0x7446510u;       // dword: 0..6, 2 = stage 2 prepared, 3 = stage 3 running
inline constexpr std::uint32_t kInitStateStage2 = 2u;
inline constexpr std::uint32_t kInitStateStage3 = 3u;   // stage 3 running (the media/UI present window)

struct SiteCheck {
    bool ok = false;
    const char* reason = "";
};

// True when the image-relative bytes at the dispatch site are exactly the measured ones. `image` must point at the
// mapped image base and `image_size` is SizeOfImage; the check never reads past image_size.
inline SiteCheck check_outrecord_site(const std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size < kOutrecordCompareRva + kOutrecordBytesCount) return {false, "image too small"};
    const std::uint8_t* site = image + kOutrecordCompareRva;
    for (std::size_t i = 0; i < kOutrecordBytesCount; ++i)
        if (site[i] != kOutrecordBytes[i]) return {false, "dispatch site bytes differ"};
    return {true, ""};
}

// Writes the patched branch byte only after the site check passes. Returns the check result.
inline SiteCheck patch_outrecord_state2(std::uint8_t* image, std::size_t image_size, std::uint8_t* branch_byte) noexcept {
    const SiteCheck check = check_outrecord_site(image, image_size);
    if (!check.ok) return check;
    *branch_byte = kOutrecordBranchPatched;
    return check;
}

// Sets both present-pass key bytes to 1. Pure byte writes; no allocation, no I/O.
inline void force_present_keys(std::uint8_t* image) noexcept {
    for (const std::uint32_t rva : kPresentKeyRvas) image[rva] = 1;
}

// Writes the slot-4 completion dword the session-3 poll reads. Returns false when the RVA cannot hold a u32 inside
// image_size (the caller still validates the mapped image first).
inline bool write_session3_completion(std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size < kSession3CompletionRva + sizeof(std::uint32_t)) return false;
    auto* slot = reinterpret_cast<std::uint32_t*>(image + kSession3CompletionRva);
    *slot = kSession3CompletionValue;
    return true;
}

// True when the title is past the stage-2 wait for the media handshake: the boot-media gate byte reads 2 and the
// session state is non-zero (the stage-3 window has begun). Forcing the keys at gate==2 alone still stalled the boot at
// init 1 in the first gated arm, so the writer waits for both.
inline bool present_keys_ready(const std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size <= kBootMediaGateRva || image[kBootMediaGateRva] != kBootMediaGateReady) return false;
    if (image_size < kSessionStateRva + sizeof(std::uint32_t) || image_size < kInitStateRva + sizeof(std::uint32_t))
        return false;
    // Stage 3 (init == 3) is required: forcing the keys at stage 2/session 1 froze the loading tick and held the title in
    // session 1. The keys only matter for the media/UI present body, which runs in the stage-3 window.
    const bool stage3 = *reinterpret_cast<const std::uint32_t*>(image + kInitStateRva) >= kInitStateStage3;
    const bool session = *reinterpret_cast<const std::uint32_t*>(image + kSessionStateRva) != 0;
    return stage3 && session;
}

// True when the title has reached stage 2 (init state >= 2); the completion writer waits for this: writing the
// completion dword during init 1 disturbed early boot, while writing it from stage 2 on matches the old arm that reached
// session 13.
inline bool session3_ready(const std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size < kInitStateRva + sizeof(std::uint32_t)) return false;
    return *reinterpret_cast<const std::uint32_t*>(image + kInitStateRva) >= kInitStateStage2;
}


// PALMETTO_TITLE_BOOTMEDIA_OPEN=1: write bit 0 of the boot-media event byte image+0x75DF0D8 once the boot-media gate
// reads 2. This is the *producer* side of the present handshake instead of the forced consumer keys: the handler
// 0x247B500 normally sets this byte on event type 0x24 (the PLM app-state callback producer is unresolved statically;
// measured boots never deliver it). With the bit set, the title's own media tick 0x24846D0 calls opener 0x2484A90 ->
// 0x2218900/0x2218910, which sets the present handshake AC (image+0x71B90AC) and lets the body take its real present
// pass, with the title's own frame-consumption side clearing it again (docs/decomp_stage.md 6 milestone 6 and
// docs/decomp_first_ui.md sections 11/17). A diagnostic, not console-faithful behavior: it synthesizes the missing
// app-state edge rather than delivering the event.
inline constexpr std::uint32_t kBootMediaEventByteRva = 0x75DF0D8u;
inline constexpr std::uint8_t kBootMediaEventOpenValue = 1u;
inline constexpr std::uint32_t kBootMediaEventPollMs = 100u;
inline constexpr std::uint32_t kBootMediaEventMaxWaitTicks = 2400u;

// True when the boot-media gate reads 2 and stage 3 is running. The first producer arm wrote the event byte at gate==2
// (4 s) and the title then never left init 2 / session 0 (presents 1, body ticking but the frame loop stalled); the
// event must be synthesized in the stage-3/menu window instead.
inline bool bootmedia_open_ready(const std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size <= kBootMediaGateRva || image[kBootMediaGateRva] != kBootMediaGateReady) return false;
    if (image_size < kInitStateRva + sizeof(std::uint32_t)) return false;
    return *reinterpret_cast<const std::uint32_t*>(image + kInitStateRva) >= kInitStateStage3;
}

// Set bit 0 of the event byte; returns true when the byte changed. Only bytes inside a non-null image are touched.
inline bool open_bootmedia_event(std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size <= kBootMediaEventByteRva) return false;
    std::uint8_t& event_byte = image[kBootMediaEventByteRva];
    const bool changed = (event_byte & kBootMediaEventOpenValue) == 0;
    event_byte = static_cast<std::uint8_t>(event_byte | kBootMediaEventOpenValue);
    return changed;
}


// PALMETTO_TITLE_FRAME_REQUEST=1: the last missing producer edge for the media/UI present body. With
// PALMETTO_TITLE_BOOTMEDIA_OPEN=1 the title's own tick opens the present handshake (AC image+0x71B90AC = 1, measured),
// but the body still exits on the frame-request flag AD image+0x71B90AD == 0 because the request ring never got its
// state-2 media item (docs/decomp_first_ui.md sections 11/17). This writer sets only AD (the title's own present side
// clears it after each frame at 0x2218689) while AC stays title-owned, so the body runs its real present pass per tick
// instead of having both keys forced. Requires init>=3, session!=0 and AC already 1.
inline constexpr std::uint32_t kFrameRequestRva = kPresentKeyRvas[0];  // image+0x71B90AD
inline constexpr std::uint32_t kFrameRequestIntervalMs = 2u;
inline constexpr std::uint32_t kFrameRequestMaxWrites = 120000u;  // 240 s at 2 ms

// True when the frame-request writer may run: stage 3, a session, and the title's own present handshake is open.
inline bool frame_request_ready(const std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size <= kBootMediaGateRva || image[kBootMediaGateRva] != kBootMediaGateReady) return false;
    if (image_size < kInitStateRva + sizeof(std::uint32_t) || image_size < kSessionStateRva + sizeof(std::uint32_t))
        return false;
    const bool stage3 = *reinterpret_cast<const std::uint32_t*>(image + kInitStateRva) >= kInitStateStage3;
    const bool session = *reinterpret_cast<const std::uint32_t*>(image + kSessionStateRva) != 0;
    return stage3 && session && image[kPresentKeyRvas[1]] == 1;  // AC open by the title
}

inline bool set_frame_request(std::uint8_t* image, std::size_t image_size) noexcept {
    if (!image || image_size <= kFrameRequestRva) return false;
    image[kFrameRequestRva] = 1;
    return true;
}
}  // namespace palmetto::title_frontier
