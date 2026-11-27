#pragma once

// The measured rule for PresentX's second display plane (docs/peer_port_movie.md, the implemented rank-4 port, item 6).
//
// THE INVENTED GATE THIS REPLACES. The present path has composed a second plane since docs/d3d12_vulkan.md section 4.5,
// but it shows it only when the plane carries `xbox_plane_flag_overlay` (port/shims/xbox_present.h). That bit value is
// our own placeholder: the plane-flag enum was never measured (the public Xbox samples give the 0x48-byte plane layout
// and the field offsets, not the flag values). So the gate that decides whether the title's second plane is shown has
// been a hypothesis, and `ResourceCount != 1` has been refused as unmeasured (docs/present_wiring.md).
//
// WHAT THE PEERS DO. VIctory's host compositor draws plane 0 opaque and every later plane with an overlay blend; XCL
// draws every plane the same way; PercyScarlett names the Series S case - the interface/movie overlay plane,
// premultiplied, blended over the game plane. Their evidence is about a different runtime's field set, so it cannot be a
// measurement of our title's plane words (docs/peer_port_movie.md section 5, item 6: "measure first, then relax").
//
// WHAT WE MEASURED, AND WHAT THE RULE IS. Every sampled PresentX of this title (boot logs, docs/peer_port_movie.md
// section 8) presents ONE plane whose words are ResourceCount 1, ColorSpace 0 (SDR G22/P709), ScaleFilter 0 and
// Flags 0x0, over the same resource every frame: in the measured window a second plane has never been presented, so the
// invented flag has never been exercised and the GameDVR-copy question has no population here. The rule below therefore
// composes the second plane only when the title's own descriptor asks for it through a field whose meaning our present
// path already measured for plane 0:
//   - a sane pDestPlacementBase on plane 1 (the plane is placed into the output; the same field is what makes plane 0
//     compose today), or
//   - a point or linear ScaleFilter on plane 1 (the plane asks for its own filtering).
// Anything else - no placement, the default filter - is the unmeasured case (it is the public HDR sample's GameDVR copy,
// which is not shown), and the rule refuses it by name rather than showing a frame twice. ResourceCount above 1 stays
// refused: it has no measured population and the composition reads one resource per plane. The blend stays what the
// compose pass already measured and implements: plane 1 is composited over plane 0 premultiplied (unless the title sets
// the straight-alpha bit, whose value is still a hypothesis and is documented as such).
//
// The rule is behind PALMETTO_PRESENT_PLANE_RULE=measured (off by default): with it unset the present path keeps the
// invented flag gate byte for byte, and with it set the gate above replaces the flag. The pure decision below is
// unit-tested in port/test/xbox_present_test.cpp; the caller (xbox_present.cpp) only passes it the plane words it has
// already validated.

#include <cstdint>

namespace palmetto {

// The plane-1 words the decision reads. They are the console layout's fields after the caller's own validation (the
// caller resolves `has_placement` from a sane sub-rectangle and `scale_filter` from a known value), so the rule is pure.
struct SecondPlaneWords {
    unsigned plane_count = 1;
    std::uint32_t resource_count = 1;
    bool has_placement = false;
    int scale_filter = 0;  // xbox_scale_filter_*: 0 default, 1 point, 2 linear
};

enum class SecondPlaneDecision : std::uint8_t {
    not_requested = 0,       // one plane: the present path is the tree's
    compose = 1,             // the title's descriptor asks for the plane: compose it over plane 0
    refuse_unmeasured = 2,   // a second plane with no measured display field: not shown (the GameDVR-copy case)
    refuse_resource_count = 3,  // a plane spanning more than one resource: unmeasured, refused
    refuse_plane_count = 4,  // more than two planes: the UMD limit, refused elsewhere
};

struct SecondPlanePlan {
    SecondPlaneDecision decision = SecondPlaneDecision::not_requested;
    const char* reason = "one plane";
};

// The rule. Order: plane count, resource count, then the measured display fields.
inline SecondPlanePlan compose_second_plane(const SecondPlaneWords& words) {
    if (words.plane_count < 2)
        return {SecondPlaneDecision::not_requested, "one plane"};
    if (words.plane_count > 2)
        return {SecondPlaneDecision::refuse_plane_count, "more than two planes"};
    if (words.resource_count != 1)
        return {SecondPlaneDecision::refuse_resource_count, "a plane with ResourceCount other than 1"};
    if (words.has_placement)
        return {SecondPlaneDecision::compose, "plane 1 names a destination placement"};
    if (words.scale_filter == 1 || words.scale_filter == 2)
        return {SecondPlaneDecision::compose, "plane 1 asks for point or linear filtering"};
    return {SecondPlaneDecision::refuse_unmeasured,
            "plane 1 has no placement and the default filter: the fields that would say it is shown are unmeasured"};
}

// The switch text: false unless it names "measured" (unset, empty, "0" and any other value keep the invented flag gate).
inline bool present_plane_rule_measured_from(const char* value) {
    if (!value || !*value) return false;
    const auto equal = [](const char* text, const char* word) {
        for (; *word; ++text, ++word) {
            char a = *text;
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (a != *word) return false;
        }
        return *text == '\0';
    };
    return equal(value, "measured");
}

}  // namespace palmetto
