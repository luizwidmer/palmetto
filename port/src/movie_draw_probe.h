// The measured movie draw path of the retail image as RVAs and instruction shapes, and the pure rules the
// PALMETTO_MOVIE_PROBE=draw instrument uses (docs/peer_port_movie.md section 13).
//
// The movie player reaches the middleware's playing state 0x9 with a non-null Bink handle and frame buffers 0, while
// frame-buffer registration 0x442F760 and per-frame decode 0x2EF0E0 never run (docs/decomp_first_ui.md sections
// 15-18). The open question this table answers is where the draw path stops: never issued, issued and skipped at a
// flag, or blocked before the layer. Four measured sites answer it, plus two inside-gate controls:
//
//   0x23495A0  the movie layer entry. rcx = the layer object; the first instructions gate on [rcx+0x30] != -1,
//              [rcx+0x68] != 0 and [rcx+0x90] & 1, then take the fill path ([rcx+0x90] & 1) or the player path.
//   0x2349D80  the player draw the layer runs. rcx = the layer object; [rcx+0xc] must be > 0, and the draw resolves the
//              player and gates on [player+0x1c7] (frame ready) before it calls the decode wrapper 0x2EF0E0 at
//              0x234A117.
//   0x2349DD4  the frame-ready compare inside the player draw; rax = the resolved player, [rax+0x1c7] is the word the
//              draw tests - the exact skip decision.
//   0x51B45B0  the second, independent per-frame advance. rcx = the movie component; [rcx+8] is the player entity.
//   0x51B4646  the advance's decode preparation; rsi = the movie component, the player entity is [rsi+8].
//   0x2EF0E0   the per-frame decode wrapper as the same-boot control (state 9 gate, then the registration call at
//              0x2EF130).
//
// The signature bytes are this image build's own instruction shapes; the installer refuses a site whose bytes do not
// match rather than patch an instruction whose meaning was not measured. No game data is in this header: RVAs,
// instruction bytes, field offsets and counters only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace palmetto::movie_draw {

inline constexpr unsigned kSiteCount = 6;

enum SiteIndex : unsigned {
    kLayerDraw = 0,   // 0x23495A0
    kPlayerDraw,      // 0x2349D80
    kDrawReady,       // 0x2349DD4
    kAdvance,         // 0x51B45B0
    kAdvanceDecode,   // 0x51B4646
    kDecode,          // 0x2EF0E0
};

// Register indexes for ControlWord::reg, in the order the probe's log prints them.
enum RegisterIndex : unsigned {
    kRcx = 0,
    kRdx,
    kRax,
    kRsi,
    kRdi,
    kR8,
    kR9,
    kRbx,
    kRbp,
    kRegisterCount,
};

inline constexpr const char* kRegisterNames[kRegisterCount] = {"rcx", "rdx", "rax", "rsi", "rdi",
                                                               "r8",  "r9",  "rbx", "rbp"};

// One control word read off a register at a probe hit. `base == 0`: the register holds the object and the address is
// register + offset. `base == 1`: the register holds a pointer whose qword at `deref` is the object (the movie
// component names its player entity at [component+8]) and the address is that object + offset. `width` is 1, 4 or 8
// bytes; `label` asks the log to render a pointer into the title image as an image RVA.
struct ControlWord {
    const char* name;
    unsigned char base;
    unsigned char reg;
    std::uint32_t deref;
    std::uint32_t offset;
    unsigned char width;
    bool label;
};

struct SiteShape {
    const char* name;
    std::uint32_t rva;
    const unsigned char* signature;
    unsigned signature_length;
    const ControlWord* words;
    unsigned word_count;
};

// 0x23495A0: push rsi / push rdi / sub rsp,0x58 / cmp dword [rcx+0x30],-1 ...
inline constexpr unsigned char kLayerDrawSignature[] = {0x56, 0x57, 0x48, 0x83, 0xEC,
                                                        0x58, 0x83, 0x79, 0x30, 0xFF};
// 0x2349D80: push r15 / push r14 / push r12 / push rsi / push rdi / push rbp / push rbx ...
inline constexpr unsigned char kPlayerDrawSignature[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                                         0x56, 0x57, 0x55, 0x53};
// 0x2349DD4: cmp byte [rax+0x1c7],0 - the frame-ready gate.
inline constexpr unsigned char kDrawReadySignature[] = {0x80, 0xB8, 0xC7, 0x01, 0x00, 0x00, 0x00};
// 0x51B45B0: push rsi / sub rsp,0x20 / cmp byte [rcx+0x511],0 ...
inline constexpr unsigned char kAdvanceSignature[] = {0x56, 0x48, 0x83, 0xEC, 0x20,
                                                       0x80, 0xB9, 0x11, 0x05, 0x00};
// 0x51B4646: mov rcx,[rsi+8] / call 0x2ef0e0 - the advance's decode preparation.
inline constexpr unsigned char kAdvanceDecodeSignature[] = {0x48, 0x8B, 0x4E, 0x08, 0xE8, 0x91, 0xAA, 0x13};
// 0x2EF0E0: push r15 / push r14 / push r12 / push rsi / push rdi / push rbx / sub rsp,0x28 ...
inline constexpr unsigned char kDecodeSignature[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                                     0x56, 0x57, 0x53};

// The gate words the layer entry and the player draw read, all relative to the layer object in rcx.
inline constexpr ControlWord kLayerWords[] = {
    {"layer+0x30", 0, kRcx, 0, 0x30, 4, false},   // dword compared with -1: != -1 is one way into the draw
    {"layer+0x68", 0, kRcx, 0, 0x68, 8, true},    // qword compared with 0: non-null is the other
    {"layer+0x90", 0, kRcx, 0, 0x90, 1, false},   // visibility flag: bit 0 takes the fill path
    {"layer+0xc", 0, kRcx, 0, 0xc, 4, false},     // float the player draw compares with 0 before it resolves the player
    {"layer+0x34", 0, kRcx, 0, 0x34, 4, false},   // height: non-zero is the filled-rectangle condition
    {"layer+0x88", 0, kRcx, 0, 0x88, 8, true},    // the pointer the player path tests before its batch helper
    {"layer+0x2c", 0, kRcx, 0, 0x2c, 4, false},   // the id the player lookup takes
};
inline constexpr ControlWord kPlayerDrawWords[] = {
    {"layer+0xc", 0, kRcx, 0, 0xc, 4, false},     // > 0 gate (vucomiss against 0)
    {"layer+0x2c", 0, kRcx, 0, 0x2c, 4, false},   // the id the draw resolves the player with
    {"layer+0x30", 0, kRcx, 0, 0x30, 4, false},
    {"layer+0x68", 0, kRcx, 0, 0x68, 8, true},
    {"layer+0x88", 0, kRcx, 0, 0x88, 8, true},
    {"layer+0x90", 0, kRcx, 0, 0x90, 1, false},
};
// The frame-ready compare reads the player the draw resolved into rax: this is the skip decision itself.
inline constexpr ControlWord kDrawReadyWords[] = {
    {"player+0x1c4", 0, kRax, 0, 0x1c4, 1, false},   // middleware player state; 9 is the playing state the decode gates on
    {"player+0x1c7", 0, kRax, 0, 0x1c7, 1, false},   // frame ready: non-zero lets the decoded frame be drawn
    {"player+0x1b8", 0, kRax, 0, 0x1b8, 1, false},   // frame-buffer count / decode gate (buffers 0 in every measured arm)
    {"player+0x50", 0, kRax, 0, 0x50, 8, true},      // the Bink handle
    {"player+0x1e5", 0, kRax, 0, 0x1e5, 1, false},   // the byte the decode wrapper reads before its first Bink call
};
inline constexpr ControlWord kAdvanceWords[] = {
    {"component+0x8", 0, kRcx, 0, 0x8, 8, true},      // the player entity the advance calls the decode with
    {"component+0x511", 0, kRcx, 0, 0x511, 1, false}, // non-zero returns before the set/advance path
    {"component+0x550", 0, kRcx, 0, 0x550, 4, false}, // non-zero takes the early branch
    {"component+0x560", 0, kRcx, 0, 0x560, 4, false}, // non-zero takes the second early branch
    {"component+0x4f8", 0, kRcx, 0, 0x4f8, 1, false}, // flags: bit 0x40 skips the advance
    {"component+0x5f2", 0, kRcx, 0, 0x5f2, 1, false}, // == 1 is the advance's own gate
    {"component+0x8ae", 0, kRcx, 0, 0x8ae, 1, false}, // the state byte that selects the decode branch
    {"component+0x50c", 0, kRcx, 0, 0x50c, 1, false}, // the byte that selects between the advance and its helper
    {"player+0x1c4", 1, kRcx, 0x8, 0x1c4, 1, false},
    {"player+0x1c7", 1, kRcx, 0x8, 0x1c7, 1, false},
    {"player+0x1b8", 1, kRcx, 0x8, 0x1b8, 1, false},
    {"player+0x50", 1, kRcx, 0x8, 0x50, 8, true},
};
inline constexpr ControlWord kAdvanceDecodeWords[] = {
    {"component+0x8", 0, kRsi, 0, 0x8, 8, true},
    {"component+0x50c", 0, kRsi, 0, 0x50c, 1, false},
    {"player+0x1c4", 1, kRsi, 0x8, 0x1c4, 1, false},
    {"player+0x1c7", 1, kRsi, 0x8, 0x1c7, 1, false},
    {"player+0x1b8", 1, kRsi, 0x8, 0x1b8, 1, false},
    {"player+0x50", 1, kRsi, 0x8, 0x50, 8, true},
};
// The decode wrapper's own control: rcx = the player entity (state byte at +0x1c4, handle at +0x50).
inline constexpr ControlWord kDecodeWords[] = {
    {"player+0x1c4", 0, kRcx, 0, 0x1c4, 1, false},
    {"player+0x1c7", 0, kRcx, 0, 0x1c7, 1, false},
    {"player+0x1b8", 0, kRcx, 0, 0x1b8, 1, false},
    {"player+0x50", 0, kRcx, 0, 0x50, 8, true},
    {"player+0x1e5", 0, kRcx, 0, 0x1e5, 1, false},
    {"player+0x1b6", 0, kRcx, 0, 0x1b6, 1, false},
    {"player+0x198", 0, kRcx, 0, 0x198, 1, false},
};

inline constexpr SiteShape kSites[kSiteCount] = {
    {"layer-draw", 0x23495A0, kLayerDrawSignature, sizeof kLayerDrawSignature, kLayerWords,
     sizeof kLayerWords / sizeof kLayerWords[0]},
    {"player-draw", 0x2349D80, kPlayerDrawSignature, sizeof kPlayerDrawSignature, kPlayerDrawWords,
     sizeof kPlayerDrawWords / sizeof kPlayerDrawWords[0]},
    {"draw-ready", 0x2349DD4, kDrawReadySignature, sizeof kDrawReadySignature, kDrawReadyWords,
     sizeof kDrawReadyWords / sizeof kDrawReadyWords[0]},
    {"advance", 0x51B45B0, kAdvanceSignature, sizeof kAdvanceSignature, kAdvanceWords,
     sizeof kAdvanceWords / sizeof kAdvanceWords[0]},
    {"advance-decode", 0x51B4646, kAdvanceDecodeSignature, sizeof kAdvanceDecodeSignature, kAdvanceDecodeWords,
     sizeof kAdvanceDecodeWords / sizeof kAdvanceDecodeWords[0]},
    {"decode", 0x2EF0E0, kDecodeSignature, sizeof kDecodeSignature, kDecodeWords,
     sizeof kDecodeWords / sizeof kDecodeWords[0]},
};
static_assert(sizeof kSites / sizeof kSites[0] == kSiteCount, "every measured movie draw site is in the table");

// The byte-shape check the installer runs before it writes an int3: the bytes at the measured RVA must be the measured
// instruction shape, otherwise the probe would patch an instruction whose meaning was not measured and the site is
// refused by name instead.
inline bool shape_matches(unsigned site, const unsigned char* bytes) {
    if (site >= kSiteCount || !bytes) return false;
    const SiteShape& shape = kSites[site];
    if (!shape.signature || shape.signature_length == 0) return false;
    return std::memcmp(bytes, shape.signature, shape.signature_length) == 0;
}

// Which step the four draw-path counters say the boot reached. `decode` is the deepest step and wins over the others
// because the second advance calls the same wrapper without the layer draw; the advance counters are reported
// separately, not folded in here.
enum class Verdict : unsigned {
    kNeverIssued = 0,     // no layer entry, no player draw, no decode
    kLayerOnly,           // the layer entry ran, the player draw did not
    kDrawNotReady,        // the player draw ran, the frame-ready compare was not reached
    kReadyNotDecoded,     // the frame-ready compare was reached, the decode was not called
    kDecodeReached,       // the decode wrapper entered
};

inline constexpr const char* kVerdictNames[] = {
    "never-issued", "layer-only", "draw-not-ready", "ready-not-decoded", "decode-reached",
};
static_assert(sizeof kVerdictNames / sizeof kVerdictNames[0] == static_cast<unsigned>(Verdict::kDecodeReached) + 1,
              "every verdict has a name");

inline const char* verdict_name(Verdict verdict) {
    const auto index = static_cast<unsigned>(verdict);
    return index < sizeof kVerdictNames / sizeof kVerdictNames[0] ? kVerdictNames[index] : "?";
}

inline Verdict verdict_of(unsigned layer, unsigned draw, unsigned ready, unsigned decode) {
    if (decode != 0) return Verdict::kDecodeReached;
    if (draw != 0) return ready != 0 ? Verdict::kReadyNotDecoded : Verdict::kDrawNotReady;
    if (layer != 0) return Verdict::kLayerOnly;
    return Verdict::kNeverIssued;
}

}  // namespace palmetto::movie_draw
