#pragma once

// The per-submission refresh of a title-written plane texture (docs/peer_port_movie.md, the implemented rank-4 port).
//
// WHAT THIS SERVES. The title decodes and rewrites the bytes of textures it placed itself; the movie's YUV planes are the
// documented case (docs/movie_path.md, docs/peer_port_movie.md section 2.2 item 3). Our layer imports such a texture's
// title pages once into a committed stand-in image behind PALMETTO_TEXTURE_IMPORT (docs/texture_import.md), and a movie
// rewrites its planes every frame, so an import that happens once keeps the first frame. The peers each keep a
// title-written texture current before the submission that samples it, in their own structures: XCL re-uploads a placed
// texture before the next execute when the CPU wrote any of its pages (or skips it on an unchanged content hash),
// VIctory and victory-fork detile the whole texture when its memory changed and hash the memory they cannot watch, and
// Five Star hashes pages on a worker and detiles only the affected subresources. None of that code is copied here; this
// is our own rule, in our own terms, from our own measurement of our own title.
//
// THIS HEADER IS THE PURE HALF and is unit-tested without a GPU (port/test/xmem_test.cpp, test_plane_refresh_plan):
//   - the switch text (PALMETTO_PLANE_REFRESH=hash|watch, PALMETTO_PLANE_REFRESH_TRACE),
//   - the bounded compare budget one submission may spend (round-robin, so no tracked texture starves),
//   - the changed byte range of a texture and the row band that range names (the narrow re-upload),
//   - and the step the device's per-submission walk takes for one tracked texture given the guard-page tracker's
//     verdict (port/shims/xmem.h, docs/card_copies.md).
//
// The impure half is in port/shims/d3d12_vulkan_device.cpp: Resource::import_placed_pages keeps a CPU shadow of the bytes
// it imported, Resource::refresh_placed_pages compares the title's pages against it and re-uploads the changed row band,
// and DeviceState::refresh_placed_textures walks the tracked textures once per submission. With PALMETTO_PLANE_REFRESH
// unset that walk is not entered at all: the default path is the tree's one-shot import, byte for byte.
//
// MEASURED POPULATION (existing boot logs, docs/peer_port_movie.md section 8): the imports that succeed are small
// title-written textures (512 B - 65,024 B: 64x2, 127x128, 64x64, 160x90, 128x72 subresource 0); the sampled placed
// spans a page watch has seen are 16.0-33.2 MB and their heads were all zero in the measured window. A 4K movie plane in
// the import's measured row-major mode is 12.4 MB (R8 tri-planar) to 33.2 MB (RGBA8), which is exactly the population the
// import's 16 MiB default cap and this budget are sized around: compare cost, not upload cost, is what the budget bounds.

#include "../shims/xmem.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace palmetto {

// PALMETTO_PLANE_REFRESH:
//   unset/0/off  the tree before this port: the one-shot import, no shadow, no tracker call, no compare.
//   hash         compare the title's pages against the imported shadow every submission (the fallback that needs no
//                tracker) and re-upload the changed row band.
//   watch        the same compare, but the guard-page tracker is asked first: it is armed on the texture's pages at the
//                import and a poll that answers "unchanged" skips the compare. A tracker that answers "unavailable"
//                (nothing armed, a range refused, the trip record overflowed) falls back to the hash compare, which is
//                the same answer the peers' hash path gives. The tracker is shared with the descriptor-mirror gate, so a
//                mirror write also makes the poll answer "changed": that only costs the compare, never a wrong skip.
//   Any other value is off, logged once by the device (never a guess at a typo).
enum class PlaneRefreshMode : std::uint8_t {
    off = 0,
    hash = 1,
    watch = 2,
};

struct PlaneRefreshPlan {
    PlaneRefreshMode mode = PlaneRefreshMode::off;
    bool trace = false;  // PALMETTO_PLANE_REFRESH_TRACE: the counters only, the path unchanged
    bool enabled() const { return mode != PlaneRefreshMode::off; }
    // The mode also asks the guard-page tracker for a pre-filter. False in hash mode and when off.
    bool arms_tracker() const { return mode == PlaneRefreshMode::watch; }
};

// The pure parse. `refresh` and `trace` are the two switch texts (either may be null); an empty value is unset. The
// parse is case-insensitive only where the switch values are words ("HASH" works); a number other than 0 means hash.
PlaneRefreshPlan plane_refresh_plan_from(const char* refresh, const char* trace);

// What one submission may spend comparing. Sized to the import path's own default cap (16 MiB, Resource::
// import_placed_pages): one 4K movie plane's subresource fits, so the intended population is never deferred.
inline constexpr std::uint64_t plane_refresh_compare_budget = std::uint64_t(16) << 20;

// The guard-page tracker arm's measured cost bound (docs/card_copies.md section 2.3): arming a range is one
// VirtualProtect per 64 KiB view (~1 us each), but every write to a guarded page pays one exception (~10.3 us). A
// title-written texture a frame rewrites would pay that per written page, so watch mode only arms spans at or below this
// size; a larger texture uses the hash compare (the peers' "hash what you cannot watch" answer). 1 MiB = 256 pages, so a
// fully rewritten span of this size costs ~2.6 ms of exceptions at the measured rate - the bound this choice states.
inline constexpr std::uint64_t plane_refresh_watch_max_bytes = std::uint64_t(1) << 20;

// The bounded compare budget: textures are admitted whole in round-robin order, so a later texture is deferred only
// while an earlier one has been admitted this submission, and an oversized texture (larger than the whole budget) is
// admitted alone when it reaches the front - it is never starved, and neither is anything else.
class RefreshBudget {
public:
    explicit RefreshBudget(std::uint64_t bytes = plane_refresh_compare_budget) : remaining_(bytes), total_(bytes) {}
    bool take(std::uint64_t bytes) {
        if (!admitted_ && total_ != 0 && bytes > total_) {  // larger than the whole budget: admitted alone
            admitted_ = true;
            remaining_ = 0;
            oversized_ = true;
            return true;
        }
        if (bytes > remaining_) return false;
        remaining_ -= bytes;
        admitted_ = true;
        return true;
    }
    bool admitted() const { return admitted_; }
    bool oversized() const { return oversized_; }
    std::uint64_t remaining() const { return remaining_; }
    std::uint64_t total() const { return total_; }

private:
    std::uint64_t remaining_;
    std::uint64_t total_;
    bool admitted_ = false;
    bool oversized_ = false;
};

// The first byte and one past the last byte that differ between two spans of `bytes`, or false when they are equal (the
// texture did not change and nothing is uploaded). `bytes` is what the budget admitted, so the scan is bounded.
bool changed_byte_range(const std::uint8_t* before, const std::uint8_t* after, std::uint64_t bytes,
                        std::uint64_t& first, std::uint64_t& last_exclusive);

// The copy-unit rows [top, bottom) that a changed byte range touches in a row-major subresource of `rows` rows of
// `row_bytes` bytes, or false when the range cannot be narrowed to rows (no rows, no row bytes, or byte offsets outside
// the subresource) - the caller then re-uploads the subresource whole. A range that covers every row is reported as
// such (top 0, bottom rows); the caller decides whether the narrow box is worth recording.
bool changed_rows(std::uint64_t row_bytes, std::uint32_t rows, std::uint64_t first, std::uint64_t last_exclusive,
                  std::uint32_t& top, std::uint32_t& bottom);

// What the walk does for one tracked texture in one submission. The rule, in order:
//   - the tracker answered "unchanged" in watch mode: no compare, no upload (the saving the tracker buys);
//   - otherwise, if the budget admitted the texture: compare and, if it changed, upload the changed row band;
//   - otherwise: deferred to the next submission (the round-robin order moves the start by one each submission).
enum class TextureRefreshStep : std::uint8_t {
    compare = 0,
    skip_tracker_unchanged = 1,
    defer_budget = 2,
};

TextureRefreshStep texture_refresh_step(PlaneRefreshMode mode, rt::xmem::WriteWatchPoll tracker, bool fits_budget);

// The boot's counters (one line, the style of the card-copy report). Sums are 64-bit: a 300 s boot submits tens of
// thousands of times and a movie's plane would change on many of them.
struct PlaneRefreshCounters {
    std::uint64_t submissions = 0;        // per-submission walks (with the switch on)
    std::uint64_t tracked = 0;            // texture-submission pairs walked
    std::uint64_t watched = 0;            // tracked textures with the tracker armed
    std::uint64_t compared = 0;           // textures whose bytes were compared
    std::uint64_t compare_bytes = 0;      // bytes compared
    std::uint64_t changed = 0;            // compares that found a change
    std::uint64_t unchanged = 0;          // compares that found none
    std::uint64_t uploaded = 0;           // re-uploads that reached the image
    std::uint64_t upload_bytes = 0;       // bytes uploaded
    std::uint64_t narrow_uploads = 0;     // uploads by row band (only the changed rows)
    std::uint64_t full_uploads = 0;       // uploads of the whole subresource (not narrowable)
    std::uint64_t tracker_skips = 0;      // compares the tracker's "unchanged" verdict skipped
    std::uint64_t deferred = 0;           // walked textures deferred by the budget
    std::uint64_t unreadable = 0;         // title pages not readable at compare time (left as they were)
};

// The one-line report (the format the boot's milestone lines use), written into `out`; empty text when no counter moved.
void write_plane_refresh_report(const PlaneRefreshCounters& counters, char* out, std::size_t bytes);

// ---- implementation -----------------------------------------------------------------------------------------------

inline bool plane_refresh_ascii_equal(const char* text, const char* word) {
    if (!text || !word) return false;
    for (; *word; ++text, ++word) {
        char a = *text, b = *word;
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (a != b) return false;
    }
    return *text == '\0';
}

inline PlaneRefreshPlan plane_refresh_plan_from(const char* refresh, const char* trace) {
    PlaneRefreshPlan plan;
    if (refresh && *refresh) {
        if (plane_refresh_ascii_equal(refresh, "hash")) {
            plan.mode = PlaneRefreshMode::hash;
        } else if (plane_refresh_ascii_equal(refresh, "watch")) {
            plan.mode = PlaneRefreshMode::watch;
        } else if (plane_refresh_ascii_equal(refresh, "off") || plane_refresh_ascii_equal(refresh, "0")) {
            plan.mode = PlaneRefreshMode::off;
        } else if (refresh[0] >= '1' && refresh[0] <= '9') {
            plan.mode = PlaneRefreshMode::hash;  // "1" and any other leading digit: the default implementation
        }
        // Anything else is off: a typo must not enable a path the measurement did not choose.
    }
    plan.trace = trace && *trace && !(trace[0] == '0' && trace[1] == '\0');
    return plan;
}

inline bool changed_byte_range(const std::uint8_t* before, const std::uint8_t* after, std::uint64_t bytes,
                               std::uint64_t& first, std::uint64_t& last_exclusive) {
    if (!before || !after) return false;
    if (!bytes) return false;
    // The first differing byte from the front.
    std::uint64_t head = 0;
    while (head < bytes && before[head] == after[head]) ++head;
    if (head == bytes) return false;
    // The last differing byte from the back, never at or before `head`.
    std::uint64_t tail = bytes;
    while (tail > head + 1 && before[tail - 1] == after[tail - 1]) --tail;
    first = head;
    last_exclusive = tail;
    return true;
}

inline bool changed_rows(std::uint64_t row_bytes, std::uint32_t rows, std::uint64_t first,
                         std::uint64_t last_exclusive, std::uint32_t& top, std::uint32_t& bottom) {
    if (!row_bytes || !rows || first >= last_exclusive) return false;
    const std::uint64_t total = row_bytes * rows;
    if (last_exclusive > total) return false;
    const std::uint64_t first_row = first / row_bytes;
    const std::uint64_t last_row = (last_exclusive - 1) / row_bytes;
    if (first_row >= rows || last_row >= rows || last_row < first_row) return false;
    top = static_cast<std::uint32_t>(first_row);
    bottom = static_cast<std::uint32_t>(last_row + 1);
    return true;
}

inline TextureRefreshStep texture_refresh_step(PlaneRefreshMode mode, rt::xmem::WriteWatchPoll tracker, bool fits_budget) {
    if (mode == PlaneRefreshMode::watch && tracker == rt::xmem::WriteWatchPoll::unchanged)
        return TextureRefreshStep::skip_tracker_unchanged;
    if (!fits_budget) return TextureRefreshStep::defer_budget;
    return TextureRefreshStep::compare;
}

inline void write_plane_refresh_report(const PlaneRefreshCounters& c, char* out, std::size_t bytes) {
    if (!out || !bytes) return;
    std::snprintf(out, bytes,
                  "plane refresh: %llu tracked texture-submission(s), %llu watched | compared %llu (%.1f MB), changed "
                  "%llu, unchanged %llu | uploaded %llu (%.1f MB; narrow %llu, full %llu) | tracker skips %llu, "
                  "deferred %llu, unreadable %llu",
                  static_cast<unsigned long long>(c.tracked), static_cast<unsigned long long>(c.watched),
                  static_cast<unsigned long long>(c.compared), double(c.compare_bytes) / (1 << 20),
                  static_cast<unsigned long long>(c.changed), static_cast<unsigned long long>(c.unchanged),
                  static_cast<unsigned long long>(c.uploaded), double(c.upload_bytes) / (1 << 20),
                  static_cast<unsigned long long>(c.narrow_uploads), static_cast<unsigned long long>(c.full_uploads),
                  static_cast<unsigned long long>(c.tracker_skips), static_cast<unsigned long long>(c.deferred),
                  static_cast<unsigned long long>(c.unreadable));
}

}  // namespace palmetto
