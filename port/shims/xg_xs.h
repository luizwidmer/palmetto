// xg_xs.dll shim: the movie-plane linear upload rule (docs/peer_port_movie.md section 12).
//
// The console's texture-layout library copies a linear subresource into the resource's layout; for the linear
// layout the layout's rows are 256-byte aligned (`port/shims/xg_xs.cpp`, docs/texture_layout.md), so a source row
// pitch that is not 256 bytes wide needs a row-by-row copy. `PALMETTO_XG_MOVIE_PLANES` and
// `PALMETTO_XG_LINEAR_ARRAYS` are separate opt-in switches. Their parsers, read-once getters and test seams let one
// test binary compare both paths on the same synthetic description; outside the tests each switch is fixed on first
// use.
#pragma once

#include <cstdint>

namespace rt::xg_xs {

// Values `on`, `1`, `yes`, `true` (any case) are on; unset, empty, `0`, `off`, `no` and any other word are off -
// an unknown word is off, never a guess.
bool parse_movie_plane_upload(const char* text);

// The switch's value: read from PALMETTO_XG_MOVIE_PLANES on the first call, then fixed. Off by default.
bool movie_plane_upload();

// The test seam: overrides the environment read (the test calls it before the shim's first call, then flips it to
// compare the on and off paths; the switch is process-fixed outside the tests).
void set_movie_plane_upload_for_test(bool enabled);

// A narrow follow-up for the three explicit-linear array descriptors recovered from the XG census. The opt-in is
// separate from the movie row rule and remains off unless PALMETTO_XG_LINEAR_ARRAYS is explicitly enabled.
bool parse_linear_array_coverage(const char* text);
bool linear_array_coverage();
void set_linear_array_coverage_for_test(bool enabled);

// The runtime's bounded trace emits one descriptor prefix per distinct XG shape. This pure parser is also used by the
// GPU-free test against synthetic lines; it stops after `samples`, so interleaved suffix text does not invalidate the
// descriptor fields. The line index is retained so a caller can reject duplicate/out-of-order entries.
struct CensusShape {
    uint32_t index;
    uint32_t dimension;
    uint64_t width;
    uint32_t height;
    uint32_t array_size;
    uint32_t mip_levels;
    uint32_t format;
    uint32_t layout;
    uint32_t flags;
    uint32_t samples;
};

bool parse_census_shape_line(const char* text, CensusShape* out);

enum class CensusClass {
    kThreeDimensional,
    kArray,
    kMipChain,
    kNonLinear,
    kLinearCandidate,
    kOther,
};

// This is an axis classifier for the trace, not a promise that the format/flags/extent can be laid out.
CensusClass census_class(const CensusShape& shape);

}  // namespace rt::xg_xs
