// bink_witness.h: the measured Bink 2 import/audio surface, the BINK header words, the switch spellings, the installer
// and fast-advance refusal rules, the bounded-log rule and the counter verdict, on synthetic data only. The audio table
// is cross-checked against the measured import table; it adds no guessed callback ABI or export. No game files: the tests
// build header words and slot observations themselves (docs/peer_port_movie.md sections 14-15).
#include "../src/bink_witness.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

namespace bw = palmetto::bink_witness;

void test_header_words() {
    check(bw::kHeaderBytes == 0x24, "the witness reads the measured 0x24 header bytes");
    check(offsetof(bw::HeaderWords, width) == 0x00, "width is at +0x00");
    check(offsetof(bw::HeaderWords, height) == 0x04, "height is at +0x04");
    check(offsetof(bw::HeaderWords, frames) == 0x08, "the frame count is at +0x08");
    check(offsetof(bw::HeaderWords, frame_num) == 0x0c, "the current frame is at +0x0c");
    check(offsetof(bw::HeaderWords, last_frame_num) == 0x10, "the word the title reads is at +0x10");
    check(offsetof(bw::HeaderWords, frame_rate) == 0x14, "the playback rate is at +0x14");
    check(offsetof(bw::HeaderWords, frame_rate_div) == 0x18, "the rate divisor is at +0x18");
    check(offsetof(bw::HeaderWords, read_error) == 0x1c, "the read-error word is at +0x1c");
    check(offsetof(bw::HeaderWords, open_flags) == 0x20, "the open flags are at +0x20");
    check(sizeof(bw::HeaderWords) == bw::kHeaderBytes, "the struct is exactly the measured words");

    // A synthetic decoder block: the witness reads the words from the handle address and nothing else.
    unsigned char block[64] = {};
    const std::uint32_t words[9] = {1920, 1080, 40, 7, 12, 30000, 1001, 0, 0x1000};
    std::memcpy(block, words, sizeof words);
    const bw::HeaderWords read = bw::header_words(block);
    check(read.width == 1920 && read.height == 1080, "the width and height words are read");
    check(read.frames == 40 && read.frame_num == 7, "the frame count and current frame are read");
    check(read.last_frame_num == 12, "the title-read frame word is read");
    check(read.frame_rate == 30000 && read.frame_rate_div == 1001, "the rate words are read");
    check(read.read_error == 0 && read.open_flags == 0x1000, "the read-error and open-flag words are read");
    const bw::HeaderWords none = bw::header_words(nullptr);
    check(none.frames == 0 && none.width == 0, "a null handle reads no words");

    check(bw::kDecodeWrapperRva == 0x2EF0E0, "the decode wrapper RVA is the measured one");
    check(bw::kRegisterFrameBuffersWrapperRva == 0x442F760, "the registration wrapper RVA is the measured one");
    check(bw::kMetadataOpenCallRva == 0x442EBE6, "the metadata open call RVA is the measured one");
    check(bw::kPlaybackOpenCallRva == 0x442F056, "the playback open call RVA is the measured one");
    check(bw::kLastFrameCompareCount == 2, "the title has two measured compares of the header word");
    check(bw::kLastFrameCompareRvas[0] == 0x2EF1E9 && bw::kLastFrameCompareRvas[1] == 0x2EF22E,
          "the compares are at 0x2EF1E9 and 0x2EF22E");
    check(bw::kPlayerHandleOffset == 0x50, "the player's decoder handle is at +0x50");
    check(bw::kPlayerStopFrameOffset == 0x208, "the player's stop frame is at +0x208");
    check(bw::kPlayerFrameNumberOffset == 0x1d0, "the player's frame number is at +0x1d0");
    check(bw::kPeerWillLoopFlag == 0x80, "the peer-measured loop bit is logged as 0x80");
}

void test_import_table() {
    check(bw::kImportCount == 23, "the measured import surface is 23 names");
    check(std::strcmp(bw::kDecoderDllName, "bink2scarlett.dll") == 0, "the decoder DLL name is measured");
    std::set<std::string> names;
    std::set<std::uint32_t> slots;
    std::set<unsigned> wrappers;
    unsigned wrapped = 0, no_call_site = 0, no_measured_abi = 0;
    for (unsigned i = 0; i < bw::kImportCount; ++i) {
        const bw::ImportShape& shape = bw::kImports[i];
        check(shape.name && *shape.name, "every import is named");
        check(names.insert(shape.name).second, "the import names are unique");
        check(slots.insert(shape.iat_rva).second, "the measured IAT slot RVAs are unique");
        check(shape.iat_rva + sizeof(void*) <= 0x6A00000,
              "the IAT slot is inside the mapped image's import address table range");
        check(static_cast<unsigned>(shape.kind) <= static_cast<unsigned>(bw::Kind::kConfig),
              "every import has a measured category");
        if (shape.wrapper != bw::Wrapper::kNone) {
            ++wrapped;
            check(wrappers.insert(static_cast<unsigned>(shape.wrapper)).second,
                  "each wrapper is installed on one import only");
            check(shape.arg_count >= 1 && shape.arg_count <= 4,
                  "a wrapped import has a measured register-argument count of 1 to 4");
            check(shape.unwrapped == bw::UnwrappedReason::kNone, "a wrapped import has no unwrapped reason");
        } else {
            check(shape.unwrapped != bw::UnwrappedReason::kNone, "an unwrapped import names why");
            check(shape.arg_count == 0, "an unwrapped import claims no measured argument shape");
            if (shape.unwrapped == bw::UnwrappedReason::kNoCallSite) {
                ++no_call_site;
                check(shape.call_site_count == 0, "an import with no call site lists no site");
            } else {
                ++no_measured_abi;
                check(shape.call_site_count >= 1, "an import refused for its ABI has a measured call site");
            }
        }
        for (unsigned site = 0; site < shape.call_site_count; ++site) {
            check(shape.call_sites != nullptr, "a listed call site has a table");
            check(shape.call_sites[site] > 0x1000, "every measured call site is an image RVA");
            check(shape.call_sites[site] < 0x6000000, "every measured call site is inside the image text");
            if (site != 0) check(shape.call_sites[site] > shape.call_sites[site - 1], "call sites are ascending");
        }
    }
    check(wrapped == 16, "16 imports have a wrapper with a measured ABI");
    check(no_call_site == 5, "5 imports have no measured call site");
    check(no_measured_abi == 2, "2 imports have a measured call site but no measured ABI");
    check(wrapped + no_call_site + no_measured_abi == bw::kImportCount, "every import is accounted for");

    // The IAT slots are the measured 8-byte stride from the first to the last name.
    check(bw::kImports[0].iat_rva == 0x67941C0, "the first IAT slot is the measured BinkClose slot");
    check(bw::kImports[bw::kImportCount - 1].iat_rva == 0x6794270,
          "the last IAT slot is the measured BinkWaitStopAsyncThreadsMulti slot");

    // The measured argument counts the wrappers rely on, from the title's own call sites.
    const auto named = [](const char* name) -> const bw::ImportShape& {
        for (const auto& shape : bw::kImports)
            if (std::strcmp(shape.name, name) == 0) return shape;
        return bw::kImports[0];
    };
    check(named("BinkOpenWithOptions").arg_count == 3, "BinkOpenWithOptions takes the measured 3 register arguments");
    check(named("BinkGetFrameBuffersInfo").arg_count == 2, "BinkGetFrameBuffersInfo takes 2");
    check(named("BinkRegisterFrameBuffers").arg_count == 2, "BinkRegisterFrameBuffers takes 2");
    check(named("BinkSetWillLoop").arg_count == 2, "BinkSetWillLoop takes the measured 2");
    check(named("BinkNextFrame").arg_count == 1, "BinkNextFrame takes 1");
    check(named("BinkPause").arg_count == 2, "BinkPause takes 2");
    check(named("BinkDoFrameAsyncMulti").arg_count == 3, "BinkDoFrameAsyncMulti takes the measured 3");
    check(named("BinkDoFrameAsyncWait").arg_count == 2, "BinkDoFrameAsyncWait takes the measured 2");
    check(named("BinkSetOSFileCallbacks").arg_count == 4, "BinkSetOSFileCallbacks takes the measured 4");
    check(named("BinkSetMemory").arg_count == 2, "BinkSetMemory takes 2");
    check(named("BinkClose").arg_count == 1, "BinkClose takes 1");
    check(named("BinkRequestStopAsyncThreadsMulti").arg_count == 1, "BinkRequestStopAsyncThreadsMulti takes 1");
    check(named("BinkWaitStopAsyncThreadsMulti").arg_count == 1, "BinkWaitStopAsyncThreadsMulti takes 1");
    check(named("BinkOpenWithOptions").call_site_count == 2, "both measured open call sites are in the table");
    check(named("BinkClose").call_site_count == 2, "both measured close call sites are in the table");
    check(named("BinkNextFrame").call_site_count == 2, "both measured advance call sites are in the table");
    check(named("BinkPause").call_site_count == 6, "all six measured pause call sites are in the table");
    check(named("BinkGoto").wrapper == bw::Wrapper::kNone, "BinkGoto has no measured call site and is not wrapped");
    check(named("BinkSetSoundSystem").kind == bw::Kind::kConfig, "the sound system is a configuration import");
}

void test_audio_import_table() {
    check(bw::kAudioImportCount == 2, "the audio metadata names only measured audio-related title imports");
    const struct {
        const char* name;
        bw::AudioImportRole role;
        std::uint32_t slot_rva;
        const char* role_name;
    } expected[] = {
        {"BinkSetSoundSystem", bw::AudioImportRole::kSoundSystemRegistration, 0x6794240,
         "sound-system-registration"},
        {"BinkSetVolume", bw::AudioImportRole::kVolumeControl, 0x6794248, "volume-control"},
    };
    std::set<std::string> names;
    for (unsigned audio_index = 0; audio_index < bw::kAudioImportCount; ++audio_index) {
        const bw::AudioImportShape& audio = bw::kAudioImports[audio_index];
        check(std::strcmp(audio.name, expected[audio_index].name) == 0,
              "the audio row is one of the two names measured in the title import table");
        check(audio.role == expected[audio_index].role, "the audio row has its measured classification");
        check(names.insert(audio.name).second, "audio import names are unique");
        check(std::strcmp(bw::audio_import_role_name(audio.role), expected[audio_index].role_name) == 0,
              "each audio role has a stable diagnostic name");
        check(bw::find_audio_import(audio.name) == &audio, "an exact audio import name resolves to its row");

        unsigned matches = 0;
        unsigned import_index = bw::kImportCount;
        for (unsigned i = 0; i < bw::kImportCount; ++i) {
            if (std::strcmp(bw::kImports[i].name, audio.name) != 0) continue;
            ++matches;
            import_index = i;
        }
        check(matches == 1, "each audio metadata row maps to exactly one measured import row");
        if (import_index == bw::kImportCount) continue;

        const bw::ImportShape& import = bw::kImports[import_index];
        check(import.iat_rva == expected[audio_index].slot_rva, "the audio row uses its measured IAT slot");
        check(import.kind == bw::Kind::kConfig, "the two audio names are measured configuration imports");
        check(import.wrapper == bw::Wrapper::kNone, "no audio wrapper is invented without a measured ABI");
        check(import.unwrapped == bw::UnwrappedReason::kNoCallSite,
              "the audio import is refused by the measured no-call-site result");
        check(import.arg_count == 0 && import.call_site_count == 0 && import.call_sites == nullptr,
              "no argument count or callback site is inferred for an uncalled audio import");
        check(bw::check_slot(import_index, bw::SlotObservation{}) == bw::InstallRefusal::kUnwrappedNoCallSite,
              "the generic installer refuses the audio import by its measured reason");
    }
    check(names.size() == bw::kAudioImportCount, "the audio metadata table has no duplicate rows");
    check(bw::find_audio_import("binksetsoundsystem") == &bw::kAudioImports[0],
          "audio import lookup is case-insensitive like the existing switch/parser helpers");
    check(bw::find_audio_import("BINKSETVOLUME") == &bw::kAudioImports[1], "volume lookup is case-insensitive");
    check(bw::find_audio_import(nullptr) == nullptr && bw::find_audio_import("") == nullptr,
          "null and empty names do not resolve to audio rows");
    check(bw::find_audio_import("BinkSetSoundSystem2") == nullptr,
          "audio import lookup does not accept a longer export-name prefix");

    const char* absent_track_imports[] = {"BinkOpenTrack", "BinkGetTrackType", "BinkGetTrackData", "BinkSetSoundTrack"};
    for (const char* name : absent_track_imports) {
        bool found = false;
        for (const bw::ImportShape& import : bw::kImports)
            if (std::strcmp(import.name, name) == 0) found = true;
        check(!found, "an unmeasured track export is not invented as a title import row");
        check(bw::find_audio_import(name) == nullptr, "an unimported track name is not a reachable audio witness row");
    }
}

void test_check_slot() {
    bw::SlotObservation good;
    good.found = true;
    good.export_found = true;
    good.slot_value = 0x12345678;
    good.export_address = 0x12345678;
    std::set<std::string> refusal_names;
    for (unsigned i = 0; i < bw::kImportCount; ++i) {
        good.found_slot_rva = bw::kImports[i].iat_rva;
        const bw::ImportShape& shape = bw::kImports[i];
        if (shape.wrapper == bw::Wrapper::kNone) {
            const bw::InstallRefusal expected = shape.unwrapped == bw::UnwrappedReason::kNoCallSite
                                                   ? bw::InstallRefusal::kUnwrappedNoCallSite
                                                   : bw::InstallRefusal::kUnwrappedNoMeasuredAbi;
            check(bw::check_slot(i, good) == expected, "an unwrapped import is refused with its named reason");
            continue;
        }
        check(bw::check_slot(i, good) == bw::InstallRefusal::kNone, "a measured slot validates");
    }
    // Mutating one observation at a time names exactly the failing step; the first wrapped import is enough.
    unsigned at = 0;
    while (bw::kImports[at].wrapper == bw::Wrapper::kNone) ++at;
    good.found_slot_rva = bw::kImports[at].iat_rva;
    bw::SlotObservation missing = good;
    missing.found = false;
    check(bw::check_slot(at, missing) == bw::InstallRefusal::kNotInImportTable, "a missing import is refused");
    bw::SlotObservation shifted = good;
    shifted.found_slot_rva += 8;
    check(bw::check_slot(at, shifted) == bw::InstallRefusal::kSlotRvaMismatch,
          "a slot at a different RVA is a different image build and is refused");
    bw::SlotObservation absent = good;
    absent.export_found = false;
    check(bw::check_slot(at, absent) == bw::InstallRefusal::kExportNotFound, "a decoder without the export is refused");
    bw::SlotObservation elsewhere = good;
    elsewhere.slot_value = 0x87654321;
    check(bw::check_slot(at, elsewhere) == bw::InstallRefusal::kBoundElsewhere,
          "a slot bound to anything but the mapped export is refused");
    check(bw::check_slot(bw::kImportCount, good) == bw::InstallRefusal::kNotInImportTable,
          "an index past the table is refused");
    for (unsigned r = 0; r < static_cast<unsigned>(bw::InstallRefusal::kCount); ++r) {
        const char* name = bw::install_refusal_name(static_cast<bw::InstallRefusal>(r));
        check(name != nullptr && std::strcmp(name, "?") != 0, "every install refusal has a name");
        refusal_names.insert(name);
    }
    check(refusal_names.size() == static_cast<unsigned>(bw::InstallRefusal::kCount), "install refusal names are unique");
}

void test_switches() {
    const struct {
        const char* text;
        bw::SwitchState state;
    } cases[] = {
        {nullptr, bw::SwitchState::kOff}, {"", bw::SwitchState::kOff},        {"0", bw::SwitchState::kOff},
        {"off", bw::SwitchState::kOff},   {"false", bw::SwitchState::kOff},   {"no", bw::SwitchState::kOff},
        {"OFF", bw::SwitchState::kOff},   {"1", bw::SwitchState::kOn},        {"on", bw::SwitchState::kOn},
        {"true", bw::SwitchState::kOn},   {"yes", bw::SwitchState::kOn},      {"ON", bw::SwitchState::kOn},
        {"True", bw::SwitchState::kOn},   {"2", bw::SwitchState::kRefused},   {"fast", bw::SwitchState::kRefused},
        {"all", bw::SwitchState::kRefused}, {" 1", bw::SwitchState::kRefused},
    };
    for (const auto& item : cases) {
        const bw::Switch parsed = bw::parse_switch(item.text);
        check(parsed.state == item.state, "the switch answers the measured spelling set");
        if (parsed.state == bw::SwitchState::kRefused)
            check(parsed.refusal != nullptr && *parsed.refusal, "a refused spelling names the refusal");
        else
            check(parsed.refusal == nullptr, "an off/on spelling carries no refusal");
    }
    check(std::strcmp(bw::switch_state_name(bw::SwitchState::kOff), "off") == 0, "the off state is named");
    check(std::strcmp(bw::switch_state_name(bw::SwitchState::kOn), "on") == 0, "the on state is named");
    check(std::strcmp(bw::switch_state_name(bw::SwitchState::kRefused), "refused") == 0, "the refused state is named");
}

bw::HeaderWords valid_header() {
    bw::HeaderWords header{};
    header.width = 1920;
    header.height = 1080;
    header.frames = 150;
    header.frame_num = 1;
    header.last_frame_num = 0;
    header.frame_rate = 30000;
    header.frame_rate_div = 1001;
    header.read_error = 0;
    header.open_flags = 0;
    return header;
}

bw::AdvanceInputs ready_inputs() {
    bw::AdvanceInputs inputs;
    inputs.fast_advance = true;
    inputs.handle_read = true;
    inputs.handle_opened = true;
    inputs.loop_state_known = true;
    inputs.will_loop = false;
    inputs.header = valid_header();
    return inputs;
}

void test_advance_rule() {
    const bw::AdvanceDecision applied = bw::decide_advance(ready_inputs());
    check(applied.apply && applied.refusal == bw::AdvanceRefusal::kNone, "a valid non-looping movie is advanced");
    check(applied.target_frame == 150, "the write target is the header's own frame count");
    check(bw::decide_advance(ready_inputs()).target_frame == applied.target_frame, "the rule is a pure function");

    bw::AdvanceInputs off = ready_inputs();
    off.fast_advance = false;
    check(bw::decide_advance(off).refusal == bw::AdvanceRefusal::kFastAdvanceOff, "the switch off forwards untouched");

    bw::AdvanceInputs unread = ready_inputs();
    unread.handle_read = false;
    check(bw::decide_advance(unread).refusal == bw::AdvanceRefusal::kHandleNotRead, "an unread handle is refused");
    bw::AdvanceInputs unknown = ready_inputs();
    unknown.handle_opened = false;
    check(bw::decide_advance(unknown).refusal == bw::AdvanceRefusal::kHandleNotOpened,
          "a handle the witness did not open is refused");
    bw::AdvanceInputs no_loop_state = ready_inputs();
    no_loop_state.loop_state_known = false;
    check(bw::decide_advance(no_loop_state).refusal == bw::AdvanceRefusal::kLoopStateUnmeasured,
          "a handle whose loop state the title never stated is refused");
    bw::AdvanceInputs looping = ready_inputs();
    looping.will_loop = true;
    check(bw::decide_advance(looping).refusal == bw::AdvanceRefusal::kLooping,
          "a movie the title said loops is never advanced");

    struct BadHeader {
        const char* what;
        bw::AdvanceRefusal refusal;
        bw::HeaderWords header;
    };
    BadHeader bad[] = {
        {"a zero width", bw::AdvanceRefusal::kWidthOutOfRange, valid_header()},
        {"a width over the bound", bw::AdvanceRefusal::kWidthOutOfRange, valid_header()},
        {"a zero height", bw::AdvanceRefusal::kHeightOutOfRange, valid_header()},
        {"a zero frame count", bw::AdvanceRefusal::kFramesZero, valid_header()},
        {"a frame count over the bound", bw::AdvanceRefusal::kFramesOutOfRange, valid_header()},
        {"a zero current frame", bw::AdvanceRefusal::kFrameNumOutOfRange, valid_header()},
        {"a current frame past the end", bw::AdvanceRefusal::kFrameNumOutOfRange, valid_header()},
        {"a last-frame word past the end", bw::AdvanceRefusal::kLastFrameNumOutOfRange, valid_header()},
        {"a set read-error word", bw::AdvanceRefusal::kReadErrorSet, valid_header()},
        {"both words already at the end", bw::AdvanceRefusal::kAlreadyLastFrame, valid_header()},
    };
    bad[0].header.width = 0;
    bad[1].header.width = bw::kMaxDimension + 1;
    bad[2].header.height = 0;
    bad[3].header.frames = 0;
    bad[4].header.frames = bw::kMaxFrames + 1;
    bad[5].header.frame_num = 0;
    bad[6].header.frame_num = bad[6].header.frames + 1;
    bad[7].header.last_frame_num = bad[7].header.frames + 1;
    bad[8].header.read_error = 1;
    bad[9].header.frame_num = bad[9].header.frames;
    bad[9].header.last_frame_num = bad[9].header.frames;
    for (const auto& item : bad) {
        bw::AdvanceInputs inputs = ready_inputs();
        inputs.header = item.header;
        const bw::AdvanceDecision decision = bw::decide_advance(inputs);
        check(!decision.apply && decision.refusal == item.refusal, item.what);
    }

    // The loop state wins over the header bounds: a looping movie is never advanced even from a corrupt header.
    bw::AdvanceInputs precedence = ready_inputs();
    precedence.will_loop = true;
    precedence.header.frames = 0;
    check(bw::decide_advance(precedence).refusal == bw::AdvanceRefusal::kLooping,
          "the loop statement is checked before the header bounds");

    // The +0x10 word is the decoder's -1 sentinel until its first advance: the measured metadata open of a 450-frame
    // stream reads 0xFFFFFFFF there, so the rule must accept it and still advance (the write replaces it).
    check(bw::kLastFrameSentinel == 0xFFFFFFFFu, "the last-frame sentinel is the measured -1");
    bw::AdvanceInputs fresh_open = ready_inputs();
    fresh_open.header.last_frame_num = bw::kLastFrameSentinel;
    const bw::AdvanceDecision fresh = bw::decide_advance(fresh_open);
    check(fresh.apply && fresh.target_frame == fresh_open.header.frames,
          "the measured -1 sentinel at +0x10 is accepted and advanced to the frame count");

    std::set<std::string> names;
    for (unsigned r = 0; r < static_cast<unsigned>(bw::AdvanceRefusal::kCount); ++r) {
        const char* name = bw::advance_refusal_name(static_cast<bw::AdvanceRefusal>(r));
        check(name != nullptr && std::strcmp(name, "?") != 0, "every advance refusal has a name");
        names.insert(name);
    }
    check(names.size() == static_cast<unsigned>(bw::AdvanceRefusal::kCount), "advance refusal names are unique");
}

void test_log_budget() {
    check(!bw::log_wanted(0), "call 0 is no call and is not logged");
    check(bw::log_wanted(1) && bw::log_wanted(2) && bw::log_wanted(3), "the first three calls are logged in full");
    check(!bw::log_wanted(4), "the fourth call is not logged");
    check(!bw::log_wanted(bw::kLogEvery - 1), "a call just before the interval is not logged");
    check(bw::log_wanted(bw::kLogEvery), "every 256th call is logged");
    check(bw::log_wanted(bw::kLogUntil), "the last bounded line is logged");
    check(!bw::log_wanted(bw::kLogUntil + bw::kLogEvery), "a call past the bound is not logged");
    unsigned logged = 0;
    for (unsigned hit = 1; hit <= bw::kLogUntil; ++hit)
        if (bw::log_wanted(hit)) ++logged;
    check(logged <= bw::kMaxLogLinesPerExport, "a hot export is logged at most the bounded number of lines");
}

void test_verdict() {
    bw::Counters none;
    check(bw::verdict_of(none) == bw::Verdict::kNoDecoderCall, "no counters is no-decoder-call");
    bw::Counters open;
    open.open = 1;
    check(bw::verdict_of(open) == bw::Verdict::kOpenOnly, "an open alone is open-only");
    bw::Counters closed = open;
    closed.close = 1;
    check(bw::verdict_of(closed) == bw::Verdict::kOpenedAndClosed, "an open and close is opened-and-closed");
    bw::Counters registered = closed;
    registered.registration = 2;
    check(bw::verdict_of(registered) == bw::Verdict::kRegisteredNoDecode,
          "registration without decode is registered-no-decode");
    bw::Counters decoded = registered;
    decoded.decode = 5;
    check(bw::verdict_of(decoded) == bw::Verdict::kDecodedNoAdvance, "decode without an advance is decoded-no-advance");
    bw::Counters requested = decoded;
    requested.advance_requested = 3;
    check(bw::verdict_of(requested) == bw::Verdict::kAdvanceRequested,
          "an advance the rule refused is advance-requested");
    bw::Counters suppressed = requested;
    suppressed.decode_suppressed = 2;
    check(bw::verdict_of(suppressed) == bw::Verdict::kDecodeSuppressed,
          "suppressed decode on a fast-advanced handle is decode-suppressed");
    bw::Counters applied = suppressed;
    applied.advance_applied = 1;
    check(bw::verdict_of(applied) == bw::Verdict::kAdvanceApplied, "an applied advance wins over every shallower step");
    std::set<std::string> names;
    for (unsigned v = 0; v < static_cast<unsigned>(bw::Verdict::kCount); ++v) {
        const char* name = bw::verdict_name(static_cast<bw::Verdict>(v));
        check(name != nullptr && std::strcmp(name, "?") != 0, "every verdict has a name");
        names.insert(name);
    }
    check(names.size() == static_cast<unsigned>(bw::Verdict::kCount), "verdict names are unique");
}

}  // namespace

int main() {
    test_header_words();
    test_import_table();
    test_audio_import_table();
    test_check_slot();
    test_switches();
    test_advance_rule();
    test_log_budget();
    test_verdict();
    std::printf("bink_witness_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
