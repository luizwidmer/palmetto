// xg_xs.dll shim: the movie-plane linear upload rule (docs/peer_port_movie.md section 12).
//
// The console's texture-layout library copies a linear subresource into the resource's layout; for the linear
// layout the layout's rows are 256-byte aligned (`port/shims/xg_xs.cpp`, docs/texture_layout.md), so a source row
// pitch that is not 256 bytes wide needs a row-by-row copy. `PALMETTO_XG_MOVIE_PLANES` is the opt-in switch for
// that rule; the functions here are the switch read, its pure parser and the test seam (the shim reads the switch
// once, on its first call, so one test binary compares both paths on the same description).
#pragma once

namespace rt::xg_xs {

// Values `on`, `1`, `yes`, `true` (any case) are on; unset, empty, `0`, `off`, `no` and any other word are off -
// an unknown word is off, never a guess.
bool parse_movie_plane_upload(const char* text);

// The switch's value: read from PALMETTO_XG_MOVIE_PLANES on the first call, then fixed. Off by default.
bool movie_plane_upload();

// The test seam: overrides the environment read (the test calls it before the shim's first call, then flips it to
// compare the on and off paths; the switch is process-fixed outside the tests).
void set_movie_plane_upload_for_test(bool enabled);

}  // namespace rt::xg_xs
