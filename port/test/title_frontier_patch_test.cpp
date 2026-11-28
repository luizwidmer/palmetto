// Pure synthetic tests for the round-22 frontier instruments (port/src/title_frontier_patch.h).
// No title image, no mapping, no environment mutation, no game data.
#include "../src/title_frontier_patch.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what);
    }
}

std::vector<std::uint8_t> good_image() {
    std::vector<std::uint8_t> image(palmetto::title_frontier::kOutrecordCompareRva + 64, 0);
    std::memcpy(image.data() + palmetto::title_frontier::kOutrecordCompareRva,
                palmetto::title_frontier::kOutrecordBytes, palmetto::title_frontier::kOutrecordBytesCount);
    return image;
}

}  // namespace

int main() {
    using namespace palmetto::title_frontier;
    check(switch_enabled("1"), "exact 1 enables");
    check(!switch_enabled(nullptr), "unset disables");
    check(!switch_enabled(""), "empty disables");
    check(!switch_enabled("0"), "0 disables");
    check(!switch_enabled("01"), "01 disables");
    check(!switch_enabled("1 "), "padded 1 disables");
    check(!switch_enabled("true"), "true disables");

    check(kOutrecordBranchOriginal == 0x74 && kOutrecordBranchPatched == 0xEB, "branch bytes");
    check(kPresentKeyRvas[0] == 0x71B90ADu && kPresentKeyRvas[1] == 0x71B90ACu, "key RVAs");

    check(!check_outrecord_site(nullptr, 0).ok, "null image refused");
    auto small = std::vector<std::uint8_t>(64, 0);
    check(!check_outrecord_site(small.data(), small.size()).ok, "small image refused");

    auto image = good_image();
    check(check_outrecord_site(image.data(), image.size()).ok, "measured site accepted");
    for (std::size_t i = 0; i < kOutrecordBytesCount; ++i) {
        auto mutated = image;
        mutated[kOutrecordCompareRva + i] ^= 0xFFu;
        check(!check_outrecord_site(mutated.data(), mutated.size()).ok, "mutated site refused");
    }

    auto patch_image = image;
    std::uint8_t* branch = patch_image.data() + kOutrecordBranchRva;
    const SiteCheck patched = patch_outrecord_state2(patch_image.data(), patch_image.size(), branch);
    check(patched.ok, "patch accepted on measured site");
    check(*branch == kOutrecordBranchPatched, "branch byte patched");
    check(patch_image[kOutrecordCompareRva + 7] == kOutrecordBranchPatched, "je byte replaced");
    check(*branch != kOutrecordBranchOriginal, "original byte gone");

    auto refuse_image = image;
    refuse_image[kOutrecordCompareRva] = 0x90;
    std::uint8_t saved = refuse_image[kOutrecordBranchRva];
    const SiteCheck refused = patch_outrecord_state2(refuse_image.data(), refuse_image.size(),
                                                     refuse_image.data() + kOutrecordBranchRva);
    check(!refused.ok && std::strcmp(refused.reason, "dispatch site bytes differ") == 0, "bad site refused by name");
    check(refuse_image[kOutrecordBranchRva] == saved, "bad site not patched");

    check(kSession3CompletionRva == 0x78ACB20u, "session-3 completion RVA");
    check(kBootMediaGateRva == 0x71B9048u && kSessionStateRva == 0x98F828Cu, "gate/session RVAs");
    auto gate_image = std::vector<std::uint8_t>(kSessionStateRva + 8, 0);
    check(!present_keys_ready(gate_image.data(), gate_image.size()), "gate not ready at 0");
    gate_image[kBootMediaGateRva] = 2;
    check(!present_keys_ready(gate_image.data(), gate_image.size()), "gate 2 alone not enough (stage init 0)");
    *reinterpret_cast<std::uint32_t*>(gate_image.data() + kInitStateRva) = 2;
    check(!present_keys_ready(gate_image.data(), gate_image.size()), "stage 2 alone not enough (session 0)");
    *reinterpret_cast<std::uint32_t*>(gate_image.data() + kSessionStateRva) = 1;
    check(!present_keys_ready(gate_image.data(), gate_image.size()), "stage 2 + session 1 still not enough");
    *reinterpret_cast<std::uint32_t*>(gate_image.data() + kInitStateRva) = 3;
    check(present_keys_ready(gate_image.data(), gate_image.size()), "gate 2 + stage 3 + session 1 ready");
    gate_image[kBootMediaGateRva] = 1;
    check(!present_keys_ready(gate_image.data(), gate_image.size()), "gate not ready at 1");
    check(!present_keys_ready(nullptr, 0), "gate readiness refuses null");
    auto session_image = std::vector<std::uint8_t>(kSessionStateRva + 8, 0);
    check(!session3_ready(session_image.data(), session_image.size()), "stage2 gate not ready at init 0");
    *reinterpret_cast<std::uint32_t*>(session_image.data() + kInitStateRva) = 2;
    check(session3_ready(session_image.data(), session_image.size()), "stage2 gate ready at init 2");
    *reinterpret_cast<std::uint32_t*>(session_image.data() + kInitStateRva) = 1;
    check(!session3_ready(session_image.data(), session_image.size()), "stage2 gate not ready at init 1");

    // Boot-media producer switch: the event byte writer waits for the gate and sets bit 0 only.
    auto event_image = std::vector<std::uint8_t>(kBootMediaEventByteRva + 8, 0);
    check(!bootmedia_open_ready(event_image.data(), event_image.size()), "boot-media open not ready at gate 0");
    event_image[kBootMediaGateRva] = 2;
    check(!bootmedia_open_ready(event_image.data(), event_image.size()), "gate 2 alone not enough (init 0)");
    *reinterpret_cast<std::uint32_t*>(event_image.data() + kInitStateRva) = 3;
    check(bootmedia_open_ready(event_image.data(), event_image.size()), "boot-media open ready at gate 2 + stage 3");
    event_image[kBootMediaEventByteRva] = 0xFE;
    check(open_bootmedia_event(event_image.data(), event_image.size()), "event byte bit 0 was clear");
    check(event_image[kBootMediaEventByteRva] == 0xFF, "event byte bit 0 set, other bits kept");
    check(!open_bootmedia_event(event_image.data(), event_image.size()), "second set reports no change");
    check(!open_bootmedia_event(nullptr, 0), "event writer refuses null");
    // Frame-request producer: gated on stage 3 + session + the title's own AC, writes only AD.
    auto frame_image = std::vector<std::uint8_t>(kSessionStateRva + 8, 0);
    check(!frame_request_ready(frame_image.data(), frame_image.size()), "frame request not ready at 0");
    frame_image[kBootMediaGateRva] = 2;
    *reinterpret_cast<std::uint32_t*>(frame_image.data() + kInitStateRva) = 3;
    *reinterpret_cast<std::uint32_t*>(frame_image.data() + kSessionStateRva) = 13;
    check(!frame_request_ready(frame_image.data(), frame_image.size()), "AC closed: not ready");
    frame_image[kPresentKeyRvas[1]] = 1;
    check(frame_request_ready(frame_image.data(), frame_image.size()), "stage3 + session + AC ready");
    check(set_frame_request(frame_image.data(), frame_image.size()), "set AD");
    check(frame_image[kPresentKeyRvas[0]] == 1, "AD written to 1");
    check(!set_frame_request(nullptr, 0), "frame request refuses null");
    check(!write_session3_completion(nullptr, 0), "session-3 write refuses null");
    auto too_small = std::vector<std::uint8_t>(64, 0);
    check(!write_session3_completion(too_small.data(), too_small.size()), "session-3 write refuses short image");
    auto completion = std::vector<std::uint8_t>(kSession3CompletionRva + 8, 0);
    check(write_session3_completion(completion.data(), completion.size()), "session-3 write accepts sized image");
    check(completion[kSession3CompletionRva] == 1 && completion[kSession3CompletionRva + 1] == 0, "session-3 dword written alone");

    auto keys = std::vector<std::uint8_t>(kPresentKeyRvas[0] + 16, 0);
    force_present_keys(keys.data());
    check(kPresentKeyMaxWrites == 300 && kPresentKeyWriteIntervalMs == 2, "bounded burst constants");
    check(keys[kPresentKeyRvas[0]] == 1 && keys[kPresentKeyRvas[1]] == 1, "both keys forced to 1");
    // The two key RVAs are adjacent (0x71B90AD and 0x71B90AC); check outside the pair that the writer touched nothing.
    check(keys[kPresentKeyRvas[0] + 1] == 0, "byte after the key pair untouched");
    check(keys[kPresentKeyRvas[1] - 1] == 0, "byte before the key pair untouched");

    std::printf("title_frontier_patch_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}