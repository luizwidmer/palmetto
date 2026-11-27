// Opt-in Bink 2 import-level call witness and non-looping-movie fast-advance: the measured shapes and the pure rules
// (docs/peer_port_movie.md section 14).
//
// The retail image imports 23 functions from the package's own Bink 2 decoder (`Bink2Scarlett.dll`) and never calls the
// decoder in the measured boot: the player reaches state 0x9 with a non-null handle and `buffers 0`, but frame-buffer
// registration and the per-frame decode wrapper stay at 0 hits (docs/decomp_first_ui.md sections 15-18,
// docs/peer_port_movie.md section 13). This header holds the measured import surface and the rules two opt-in
// instruments use on it:
//
//   PALMETTO_BINK_WITNESS=1      routes the title image's 23 decoder import slots through counted, bounded wrappers that
//                                forward every call to the real decoder export (measured ABI only).
//   PALMETTO_BINK_FASTFORWARD=1  additionally answers the title's advance of a non-looping movie from the measured BINK
//                                header contract, so the movie can reach its end state without decoding frames. It is a
//                                diagnostic: it never claims a decoded frame.
//
// Measured import surface (our own read-only disassembly of the retail image, outside the repository): each import has an
// IAT slot in image+0x67941C0..image+0x6794270 and the title calls it with `call qword ptr [rip+disp]` DIRECTLY at the
// slot, not through a `jmp [rip]` thunk. 17 of the 23 imports have such call sites (table below); the other 6 have none.
// That is why the witness patches the IAT slot rather than a thunk, and it is a disagreement with
// docs/movie_path.md section 2.1's "no direct call sites / function pointers built at run time" reading, which
// tooling/import_call_sites.py cannot see because it looks for thunks only.
//
// The BINK header words below are the measured HBINK layout: width/height/current frame were measured offline by
// port/test/bink_abi_test.cpp (docs/movies.md, "ABI over the archive"); the word at +0x10 is the one the title itself
// reads as a frame number and compares with its stop frame at 0x2EF1E9/0x2EF22E (measured by disassembly). The peers'
// header names for +0x10 and the `BINKWILLLOOP` bit in +0x20 are cross-checked evidence only (section 14.7 credits
// them); the fast-advance decision below uses the loop state the TITLE states through the measured BinkSetWillLoop
// argument, not the peer's flag bit.
//
// No game data is in this header: RVAs, field offsets, counts, constants and names only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace palmetto::bink_witness {

// ---- the measured BINK header words ------------------------------------------------------------------------------
// The decoder's HBINK is readable for at least 256 bytes (docs/movies.md); these are the words the witness logs and the
// fast-advance rule validates and writes.
struct HeaderWords {
    std::uint32_t width;           // +0x00  measured offline over every stream
    std::uint32_t height;          // +0x04  measured offline over every stream
    std::uint32_t frames;          // +0x08  frame count, measured offline
    std::uint32_t frame_num;       // +0x0c  current frame, 1-based, measured offline
    std::uint32_t last_frame_num;  // +0x10  the word the title compares with its stop frame (0x2EF1E9/0x2EF22E)
    std::uint32_t frame_rate;      // +0x14  measured offline under a forced rate
    std::uint32_t frame_rate_div;  // +0x18  measured offline under a forced rate
    std::uint32_t read_error;      // +0x1c  the decoder's read-error word
    std::uint32_t open_flags;      // +0x20  open flags; logged as evidence, not used by the fast-advance decision
};

inline constexpr std::size_t kHeaderBytes = 0x24;

inline constexpr std::uint32_t kWidthOffset = 0x00;
inline constexpr std::uint32_t kHeightOffset = 0x04;
inline constexpr std::uint32_t kFramesOffset = 0x08;
inline constexpr std::uint32_t kFrameNumOffset = 0x0c;
inline constexpr std::uint32_t kLastFrameNumOffset = 0x10;
inline constexpr std::uint32_t kFrameRateOffset = 0x14;
inline constexpr std::uint32_t kFrameRateDivOffset = 0x18;
inline constexpr std::uint32_t kReadErrorOffset = 0x1c;
inline constexpr std::uint32_t kOpenFlagsOffset = 0x20;

static_assert(offsetof(HeaderWords, width) == kWidthOffset, "the measured width word is at +0x00");
static_assert(offsetof(HeaderWords, height) == kHeightOffset, "the measured height word is at +0x04");
static_assert(offsetof(HeaderWords, frames) == kFramesOffset, "the measured frame count is at +0x08");
static_assert(offsetof(HeaderWords, frame_num) == kFrameNumOffset, "the measured current frame is at +0x0c");
static_assert(offsetof(HeaderWords, last_frame_num) == kLastFrameNumOffset, "the word the title reads is at +0x10");
static_assert(offsetof(HeaderWords, frame_rate) == kFrameRateOffset, "the measured rate is at +0x14");
static_assert(offsetof(HeaderWords, frame_rate_div) == kFrameRateDivOffset, "the measured rate divisor is at +0x18");
static_assert(offsetof(HeaderWords, read_error) == kReadErrorOffset, "the read-error word is at +0x1c");
static_assert(offsetof(HeaderWords, open_flags) == kOpenFlagsOffset, "the open flags are at +0x20");
static_assert(sizeof(HeaderWords) == kHeaderBytes, "the header words the witness reads are the measured 0x24 bytes");

// The peer-measured loop flag, logged as a cross-check word only (section 14.7). The fast-advance rule uses the loop
// state the title passes to BinkSetWillLoop, which this tree measures directly at 0x2EF109/0x442F3B9/0x5536EB7.
inline constexpr std::uint32_t kPeerWillLoopFlag = 0x00000080u;

// Reads the header words. The caller must have validated that the handle is a decoder handle the witness itself opened
// and that the memory is readable (both are done in main.cpp); this function does no validation of its own.
inline HeaderWords header_words(const void* handle) {
    HeaderWords words{};
    if (handle) std::memcpy(&words, handle, kHeaderBytes);
    return words;
}

// ---- the measured 23-import surface ------------------------------------------------------------------------------
// `iat_rva` is the measured address-table slot the title's `call qword ptr [rip+disp]` reads. `call_sites` are the
// measured direct call instructions that read that slot (a subset: the sites this tree's disassembly found, not a
// claim about every byte of the image). `arg_count` is the measured number of register arguments the title passes:
// the decoder is a title-side import, so the call sites fix its arity. `wrapper` is the wrapper the witness installs;
// `UnwrappedReason` names why an import has none.
enum class Kind : unsigned char {
    kOpen = 0,      // opens a movie and returns the HBINK
    kRegister,      // frame-buffer information and registration
    kDecode,        // asks the decoder for a frame / waits for it
    kAdvance,       // advances or pauses the movie
    kClose,         // releases the movie or its async threads
    kConfig,        // installs callbacks / memory / sound configuration
};

enum class Wrapper : unsigned char {
    kNone = 0,
    kOpen,
    kGetFrameBuffersInfo,
    kRegisterFrameBuffers,
    kSetWillLoop,
    kNextFrame,
    kPause,
    kDoFrame,
    kDoFrameAsyncMulti,
    kDoFrameAsyncWait,
    kWait,
    kShouldSkip,
    kClose,
    kRequestStopAsync,
    kWaitStopAsync,
    kSetMemory,
    kSetOsFileCallbacks,
    kCount,
};

enum class UnwrappedReason : unsigned char {
    kNone = 0,         // wrapped
    kNoCallSite,       // the import is bound but the measured disassembly finds no instruction that calls it
    kNoMeasuredAbi,    // the call site is measured but does not fix the full argument/return shape
};

struct ImportShape {
    const char* name;
    Kind kind;
    std::uint32_t iat_rva;
    Wrapper wrapper;
    UnwrappedReason unwrapped;
    unsigned arg_count;
    const std::uint32_t* call_sites;
    unsigned call_site_count;
};

inline constexpr std::uint32_t kBinkCloseSites[] = {0x442E8F5, 0x442EC52};
inline constexpr std::uint32_t kBinkDoFrameSites[] = {0x2EF309};
inline constexpr std::uint32_t kBinkDoFrameAsyncMultiSites[] = {0x2EF265, 0x2EF3BE};
inline constexpr std::uint32_t kBinkDoFrameAsyncWaitSites[] = {0x2EF274, 0x2EF3CD};
inline constexpr std::uint32_t kBinkFreeGlobalsSites[] = {0x442FDFF};
inline constexpr std::uint32_t kBinkGetFrameBuffersInfoSites[] = {0x199F066, 0x20EEDAA, 0x442EBFC};
inline constexpr std::uint32_t kBinkGetKeyFrameSites[] = {0x442F5D7};
inline constexpr std::uint32_t kBinkNextFrameSites[] = {0x2EF253, 0x2EF2FF};
inline constexpr std::uint32_t kBinkOpenWithOptionsSites[] = {0x442EBE6, 0x442F056};
inline constexpr std::uint32_t kBinkPauseSites[] = {0x11D8BD1, 0x442F30C, 0x442F398, 0x442F400, 0x442F42A,
                                                    0x5536EA6};
inline constexpr std::uint32_t kBinkRegisterFrameBuffersSites[] = {0x442F9B8};
inline constexpr std::uint32_t kBinkRequestStopAsyncSites[] = {0x442FDDE};
inline constexpr std::uint32_t kBinkSetMemorySites[] = {0x15E3C4F};
inline constexpr std::uint32_t kBinkSetOsFileCallbacksSites[] = {0x15E3C3B};
inline constexpr std::uint32_t kBinkSetWillLoopSites[] = {0x2EF109, 0x442F3B9, 0x5536EB7};
inline constexpr std::uint32_t kBinkShouldSkipSites[] = {0x442FA4E};
inline constexpr std::uint32_t kBinkWaitSites[] = {0x442F3DF, 0x442F9F1};
inline constexpr std::uint32_t kBinkWaitStopAsyncSites[] = {0x442FDED};

inline constexpr ImportShape kImports[] = {
    {"BinkClose", Kind::kClose, 0x67941C0, Wrapper::kClose, UnwrappedReason::kNone, 1, kBinkCloseSites, 2},
    {"BinkDoFrame", Kind::kDecode, 0x67941C8, Wrapper::kDoFrame, UnwrappedReason::kNone, 1, kBinkDoFrameSites, 1},
    {"BinkDoFrameAsyncMulti", Kind::kDecode, 0x67941D0, Wrapper::kDoFrameAsyncMulti, UnwrappedReason::kNone, 3,
     kBinkDoFrameAsyncMultiSites, 2},
    {"BinkDoFrameAsyncWait", Kind::kDecode, 0x67941D8, Wrapper::kDoFrameAsyncWait, UnwrappedReason::kNone, 2,
     kBinkDoFrameAsyncWaitSites, 2},
    {"BinkFreeGlobals", Kind::kClose, 0x67941E0, Wrapper::kNone, UnwrappedReason::kNoMeasuredAbi, 0,
     kBinkFreeGlobalsSites, 1},
    {"BinkGetFrameBuffersInfo", Kind::kRegister, 0x67941E8, Wrapper::kGetFrameBuffersInfo, UnwrappedReason::kNone, 2,
     kBinkGetFrameBuffersInfoSites, 3},
    {"BinkGetKeyFrame", Kind::kConfig, 0x67941F0, Wrapper::kNone, UnwrappedReason::kNoMeasuredAbi, 0,
     kBinkGetKeyFrameSites, 1},
    {"BinkGetPlatformInfo", Kind::kConfig, 0x67941F8, Wrapper::kNone, UnwrappedReason::kNoCallSite, 0, nullptr, 0},
    {"BinkGoto", Kind::kAdvance, 0x6794200, Wrapper::kNone, UnwrappedReason::kNoCallSite, 0, nullptr, 0},
    {"BinkNextFrame", Kind::kAdvance, 0x6794208, Wrapper::kNextFrame, UnwrappedReason::kNone, 1, kBinkNextFrameSites,
     2},
    {"BinkOpenWithOptions", Kind::kOpen, 0x6794210, Wrapper::kOpen, UnwrappedReason::kNone, 3,
     kBinkOpenWithOptionsSites, 2},
    {"BinkPause", Kind::kAdvance, 0x6794218, Wrapper::kPause, UnwrappedReason::kNone, 2, kBinkPauseSites, 6},
    {"BinkRegisterFrameBuffers", Kind::kRegister, 0x6794220, Wrapper::kRegisterFrameBuffers, UnwrappedReason::kNone, 2,
     kBinkRegisterFrameBuffersSites, 1},
    {"BinkRequestStopAsyncThreadsMulti", Kind::kClose, 0x6794228, Wrapper::kRequestStopAsync, UnwrappedReason::kNone,
     1, kBinkRequestStopAsyncSites, 1},
    {"BinkSetMemory", Kind::kConfig, 0x6794230, Wrapper::kSetMemory, UnwrappedReason::kNone, 2, kBinkSetMemorySites,
     1},
    {"BinkSetOSFileCallbacks", Kind::kConfig, 0x6794238, Wrapper::kSetOsFileCallbacks, UnwrappedReason::kNone, 4,
     kBinkSetOsFileCallbacksSites, 1},
    {"BinkSetSoundSystem", Kind::kConfig, 0x6794240, Wrapper::kNone, UnwrappedReason::kNoCallSite, 0, nullptr, 0},
    {"BinkSetVolume", Kind::kConfig, 0x6794248, Wrapper::kNone, UnwrappedReason::kNoCallSite, 0, nullptr, 0},
    {"BinkSetWillLoop", Kind::kAdvance, 0x6794250, Wrapper::kSetWillLoop, UnwrappedReason::kNone, 2,
     kBinkSetWillLoopSites, 3},
    {"BinkShouldSkip", Kind::kDecode, 0x6794258, Wrapper::kShouldSkip, UnwrappedReason::kNone, 1, kBinkShouldSkipSites,
     1},
    {"BinkStartAsyncThread", Kind::kConfig, 0x6794260, Wrapper::kNone, UnwrappedReason::kNoCallSite, 0, nullptr, 0},
    {"BinkWait", Kind::kDecode, 0x6794268, Wrapper::kWait, UnwrappedReason::kNone, 1, kBinkWaitSites, 2},
    {"BinkWaitStopAsyncThreadsMulti", Kind::kClose, 0x6794270, Wrapper::kWaitStopAsync, UnwrappedReason::kNone, 1,
     kBinkWaitStopAsyncSites, 1},
};

inline constexpr unsigned kImportCount = sizeof kImports / sizeof kImports[0];
static_assert(kImportCount == 23, "the retail image's measured Bink2Scarlett import surface is 23 names");

inline constexpr const char* kDecoderDllName = "bink2scarlett.dll";

// The measured title-side consumers of the header words, as evidence for the fast-advance lever:
//   the per-frame decode wrapper 0x2EF0E0 calls BinkSetWillLoop, BinkNextFrame and the async decode exports;
//   0x2EF1E9 and 0x2EF22E compare [HBINK+0x10] with [player+0x208] and skip the async decode loop while the header's
//   word is not smaller. Writing that word is therefore the measured way to make the title skip decoding frames.
inline constexpr std::uint32_t kDecodeWrapperRva = 0x2EF0E0;
inline constexpr std::uint32_t kRegisterFrameBuffersWrapperRva = 0x442F760;
inline constexpr std::uint32_t kMetadataOpenCallRva = 0x442EBE6;
inline constexpr std::uint32_t kPlaybackOpenCallRva = 0x442F056;
inline constexpr std::uint32_t kLastFrameCompareRvas[] = {0x2EF1E9, 0x2EF22E};
inline constexpr unsigned kLastFrameCompareCount = sizeof kLastFrameCompareRvas / sizeof kLastFrameCompareRvas[0];
inline constexpr std::uint32_t kPlayerHandleOffset = 0x50;
inline constexpr std::uint32_t kPlayerStopFrameOffset = 0x208;
inline constexpr std::uint32_t kPlayerFrameNumberOffset = 0x1d0;

// ---- switches ------------------------------------------------------------------------------------------------------
// Both switches are off by default. An unrecognised spelling does not turn anything on: the decision names the refusal
// and the caller logs it, so a typo cannot silently change the path.
enum class SwitchState : unsigned char {
    kOff = 0,
    kOn,
    kRefused,  // a spelling this switch does not answer; the instrument stays off
};

struct Switch {
    SwitchState state = SwitchState::kOff;
    const char* refusal = nullptr;  // named reason, set only for kRefused
};

inline bool same_text(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

inline const char* switch_state_name(SwitchState state) {
    switch (state) {
        case SwitchState::kOff: return "off";
        case SwitchState::kOn: return "on";
        case SwitchState::kRefused: return "refused";
    }
    return "?";
}

// unset, empty, "0", "off", "false" and "no" are off; "1", "on", "true" and "yes" are on; every other spelling is
// refused by name and leaves the instrument off.
inline Switch parse_switch(const char* text) {
    if (!text || !*text) return Switch{SwitchState::kOff, nullptr};
    if (same_text(text, "1") || same_text(text, "on") || same_text(text, "true") || same_text(text, "yes"))
        return Switch{SwitchState::kOn, nullptr};
    if (same_text(text, "0") || same_text(text, "off") || same_text(text, "false") || same_text(text, "no"))
        return Switch{SwitchState::kOff, nullptr};
    return Switch{SwitchState::kRefused, "not a spelling this switch answers (use 1 or off)"};
}

// ---- the fast-advance decision -------------------------------------------------------------------------------------
// Bounds chosen to refuse a header that cannot be the decoded movie this tree's archive produces (measured frames
// 3-2395, dimensions 512x256-3840x2160) rather than to describe the format's full range.
inline constexpr std::uint32_t kMaxFrames = 100000;
inline constexpr std::uint32_t kMaxDimension = 8192;
// The +0x10 word is the decoder's -1 sentinel until its first advance computes it; the measured metadata open reads
// 0xFFFFFFFF there on a real 450-frame stream (section 14.4). The sentinel is accepted as "not computed yet"; any
// other value must be inside the frame count.
inline constexpr std::uint32_t kLastFrameSentinel = 0xFFFFFFFFu;

enum class AdvanceRefusal : unsigned char {
    kNone = 0,
    kFastAdvanceOff,          // the switch is off: the advance is forwarded untouched
    kHandleNotRead,           // the header words could not be read
    kHandleNotOpened,         // the handle was not opened while the witness was installed
    kLoopStateUnmeasured,     // the title did not state the loop state through BinkSetWillLoop before the advance
    kLooping,                 // the title stated the movie loops: never advanced
    kWidthOutOfRange,
    kHeightOutOfRange,
    kFramesZero,
    kFramesOutOfRange,
    kFrameNumOutOfRange,
    kLastFrameNumOutOfRange,
    kReadErrorSet,
    kAlreadyLastFrame,        // both header words already name the last frame: nothing to write
    kCount,
};

inline constexpr const char* kAdvanceRefusalNames[] = {
    "none",
    "fast-advance-off",
    "handle-not-read",
    "handle-not-opened",
    "loop-state-unmeasured",
    "looping",
    "width-out-of-range",
    "height-out-of-range",
    "frames-zero",
    "frames-out-of-range",
    "frame-num-out-of-range",
    "last-frame-num-out-of-range",
    "read-error-set",
    "already-last-frame",
};
static_assert(sizeof kAdvanceRefusalNames / sizeof kAdvanceRefusalNames[0] ==
                  static_cast<unsigned>(AdvanceRefusal::kCount),
              "every advance refusal has a name");

inline const char* advance_refusal_name(AdvanceRefusal refusal) {
    const auto index = static_cast<unsigned>(refusal);
    return index < sizeof kAdvanceRefusalNames / sizeof kAdvanceRefusalNames[0] ? kAdvanceRefusalNames[index] : "?";
}

struct AdvanceInputs {
    bool fast_advance = false;    // PALMETTO_BINK_FASTFORWARD=1
    bool handle_read = false;     // the header words were read from the handle
    bool handle_opened = false;   // the witness opened this handle itself
    bool loop_state_known = false;  // the title called BinkSetWillLoop for this handle
    bool will_loop = false;       // ...and its argument was non-zero
    HeaderWords header{};         // the words read from the handle
};

struct AdvanceDecision {
    bool apply = false;
    AdvanceRefusal refusal = AdvanceRefusal::kNone;
    std::uint32_t target_frame = 0;  // the value written to BOTH +0x0c and +0x10 when apply
};

// The rule, in order: switch off; handle unread or not opened by the witness; loop state unmeasured or looping; the
// bounded header validation; already at the last frame; otherwise apply. The written value is the header's own frame
// count, so the write is a function of the measured header alone.
inline AdvanceDecision decide_advance(const AdvanceInputs& in) {
    AdvanceDecision decision;
    if (!in.fast_advance) return decision.refusal = AdvanceRefusal::kFastAdvanceOff, decision;
    if (!in.handle_read) return decision.refusal = AdvanceRefusal::kHandleNotRead, decision;
    if (!in.handle_opened) return decision.refusal = AdvanceRefusal::kHandleNotOpened, decision;
    if (!in.loop_state_known) return decision.refusal = AdvanceRefusal::kLoopStateUnmeasured, decision;
    if (in.will_loop) return decision.refusal = AdvanceRefusal::kLooping, decision;
    const HeaderWords& h = in.header;
    if (h.width == 0 || h.width > kMaxDimension) return decision.refusal = AdvanceRefusal::kWidthOutOfRange, decision;
    if (h.height == 0 || h.height > kMaxDimension)
        return decision.refusal = AdvanceRefusal::kHeightOutOfRange, decision;
    if (h.frames == 0) return decision.refusal = AdvanceRefusal::kFramesZero, decision;
    if (h.frames > kMaxFrames) return decision.refusal = AdvanceRefusal::kFramesOutOfRange, decision;
    if (h.frame_num == 0 || h.frame_num > h.frames)
        return decision.refusal = AdvanceRefusal::kFrameNumOutOfRange, decision;
    if (h.last_frame_num != kLastFrameSentinel && h.last_frame_num > h.frames)
        return decision.refusal = AdvanceRefusal::kLastFrameNumOutOfRange, decision;
    if (h.read_error != 0) return decision.refusal = AdvanceRefusal::kReadErrorSet, decision;
    if (h.frame_num == h.frames && h.last_frame_num == h.frames)
        return decision.refusal = AdvanceRefusal::kAlreadyLastFrame, decision;
    decision.apply = true;
    decision.target_frame = h.frames;
    return decision;
}

// ---- bounded logging -----------------------------------------------------------------------------------------------
// Per export: the first three calls in full, then every 256th up to 64 lines' worth, so a hot export cannot make the
// instrument the measurement. Counters are always exact.
inline constexpr unsigned kFullLogLines = 3;
inline constexpr unsigned kLogEvery = 256;
inline constexpr unsigned kLogUntil = 64 * 256;
inline constexpr unsigned kMaxLogLinesPerExport = kFullLogLines + kLogUntil / kLogEvery;

inline bool log_wanted(unsigned hit) {
    if (hit == 0) return false;
    return hit <= kFullLogLines || (hit % kLogEvery == 0 && hit <= kLogUntil);
}

// ---- installer validation ------------------------------------------------------------------------------------------
// The installer looks the import up in the title image's own import table, compares the slot it found with the measured
// slot RVA, and compares the bound value with the mapped decoder's export. Every mismatch is a named refusal and the
// slot is left untouched, so the witness cannot redirect a slot whose meaning this image was not measured to have.
enum class InstallRefusal : unsigned char {
    kNone = 0,
    kUnwrappedNoCallSite,    // the import has no measured call site: there is nothing to witness by design
    kUnwrappedNoMeasuredAbi, // the call site does not fix the argument/return shape: refusing to invent one
    kNotInImportTable,       // the title image's import table has no slot for the name
    kSlotRvaMismatch,        // the slot is not at the measured address: a different image build
    kExportNotFound,         // the mapped decoder does not export the name
    kBoundElsewhere,         // the slot does not point at the mapped decoder's export (a compat module or a hook)
    kNotWritable,            // the slot's page could not be made writable
    kCount,
};

inline constexpr const char* kInstallRefusalNames[] = {
    "none",
    "unwrapped-no-call-site",
    "unwrapped-no-measured-abi",
    "not-in-import-table",
    "slot-rva-mismatch",
    "export-not-found",
    "bound-elsewhere",
    "not-writable",
};
static_assert(sizeof kInstallRefusalNames / sizeof kInstallRefusalNames[0] ==
                  static_cast<unsigned>(InstallRefusal::kCount),
              "every install refusal has a name");

inline const char* install_refusal_name(InstallRefusal refusal) {
    const auto index = static_cast<unsigned>(refusal);
    return index < sizeof kInstallRefusalNames / sizeof kInstallRefusalNames[0] ? kInstallRefusalNames[index] : "?";
}

// What the installer read for one import, before it writes the slot.
struct SlotObservation {
    bool found = false;                // the title image's import table has this import
    std::uint32_t found_slot_rva = 0;  // the IAT slot RVA the import table gave
    bool export_found = false;         // the mapped decoder exports the name
    std::uintptr_t slot_value = 0;     // the pointer currently in the slot
    std::uintptr_t export_address = 0; // the mapped decoder's export address
};

inline InstallRefusal check_slot(unsigned index, const SlotObservation& observed) {
    if (index >= kImportCount) return InstallRefusal::kNotInImportTable;
    const ImportShape& shape = kImports[index];
    if (shape.wrapper == Wrapper::kNone)
        return shape.unwrapped == UnwrappedReason::kNoCallSite ? InstallRefusal::kUnwrappedNoCallSite
                                                               : InstallRefusal::kUnwrappedNoMeasuredAbi;
    if (!observed.found) return InstallRefusal::kNotInImportTable;
    if (observed.found_slot_rva != shape.iat_rva) return InstallRefusal::kSlotRvaMismatch;
    if (!observed.export_found) return InstallRefusal::kExportNotFound;
    if (observed.slot_value != observed.export_address) return InstallRefusal::kBoundElsewhere;
    return InstallRefusal::kNone;
}

// ---- the counter verdict -------------------------------------------------------------------------------------------
struct Counters {
    unsigned open = 0;               // BinkOpenWithOptions calls
    unsigned close = 0;              // BinkClose calls
    unsigned registration = 0;       // BinkGetFrameBuffersInfo + BinkRegisterFrameBuffers calls
    unsigned decode = 0;             // BinkDoFrame/AsyncMulti/AsyncWait/Wait/ShouldSkip calls
    unsigned advance_requested = 0;  // BinkNextFrame calls
    unsigned advance_applied = 0;    // BinkNextFrame calls answered by the fast-advance rule
    unsigned decode_suppressed = 0;  // decode calls answered without decoding for a fast-advanced handle
};

enum class Verdict : unsigned char {
    kNoDecoderCall = 0,   // no wrapped export called
    kOpenOnly,            // opened a movie, never closed or registered it
    kOpenedAndClosed,     // opened and closed, no registration or decode
    kRegisteredNoDecode,  // frame-buffer information/registration reached, decode did not
    kDecodedNoAdvance,    // decode exports reached, advance was never asked
    kAdvanceRequested,    // the title asked to advance but the rule refused every time
    kAdvanceApplied,      // the rule advanced a non-looping movie
    kDecodeSuppressed,    // a fast-advanced handle's decode calls were answered without decoding
    kCount,
};

inline constexpr const char* kVerdictNames[] = {
    "no-decoder-call",
    "open-only",
    "opened-and-closed",
    "registered-no-decode",
    "decoded-no-advance",
    "advance-requested",
    "advance-applied",
    "decode-suppressed",
};
static_assert(sizeof kVerdictNames / sizeof kVerdictNames[0] == static_cast<unsigned>(Verdict::kCount),
              "every verdict has a name");

inline const char* verdict_name(Verdict verdict) {
    const auto index = static_cast<unsigned>(verdict);
    return index < sizeof kVerdictNames / sizeof kVerdictNames[0] ? kVerdictNames[index] : "?";
}

// Deepest step wins, in the order the instrument can have affected the boot: applied/suppressed (the fast-advance ran)
// before requested, decode, registration, open. A refused advance is the shallowest of the three advance states only
// because nothing was written.
inline Verdict verdict_of(const Counters& c) {
    if (c.advance_applied != 0) return Verdict::kAdvanceApplied;
    if (c.decode_suppressed != 0) return Verdict::kDecodeSuppressed;
    if (c.advance_requested != 0) return Verdict::kAdvanceRequested;
    if (c.decode != 0) return Verdict::kDecodedNoAdvance;
    if (c.registration != 0) return Verdict::kRegisteredNoDecode;
    if (c.open == 0) return Verdict::kNoDecoderCall;
    if (c.close != 0) return Verdict::kOpenedAndClosed;
    return Verdict::kOpenOnly;
}

}  // namespace palmetto::bink_witness
