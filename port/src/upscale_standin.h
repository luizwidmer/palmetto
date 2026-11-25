#pragma once

// The upscale stand-in (docs/upscale_standin.md): when the console upscaler's own pipelines cannot be made, the compose
// (upscale) pass is replaced by this project's own stretch of the colour image the pass samples, written into the
// presented target. This header holds the decision, the measured geometry, the push-constant block and the refusal
// names - all pure, so the unit tests can exercise every arm without a GPU, a title or game data. The Vulkan side
// (pipeline, descriptor set, draw) lives in vulkan_backend.h; the shader is upscale_standin.frag/vert.
//
// Measured shape (docs/upscale_standin.md section 2): the title's upscale pass is a graphics draw whose pixel stage
// samples one 4K colour image through six image instructions (the 6-image `fspx/lib_ps_00822` compose pixel shader),
// and writes the presenter's 4K plane-0 target. Its own pipeline is created on this tree, so this stand-in is opt-in
// (PALMETTO_UPSCALE_STANDIN) until a boot shows the pipeline cannot be made; the peer claim it implements is the
// public "the upscale is now a stretch of the colour image" (docs/cleanroom_peers.md).
//
// The stand-in never invents colour in its shipping mode: it texel-fetches the resolved colour image, nearest-filtered,
// from the image's whole texel rectangle into the presented target's whole pixel rectangle. The only non-sampling arm
// is the project's mapping control (PALMETTO_UPSCALE_STANDIN_CONTROL), a diagnostic that writes the mapped source
// coordinate instead of a colour so a boot can prove the rectangle arithmetic when the title's colour image is black
// by design (docs/colour_path.md). It is never the shipping plan.

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace palmetto {
namespace upscale_standin {

// The measured upscale pass: its pixel stage takes the colour image through six image instructions (six samples of the
// one sibling target, docs/frame_capture.md; the readback prints "ps present 6 image(s)"). The constant/background
// pass's pixel stage samples none. A draw whose pixel stage samples this many images is the measured upscale pass.
inline constexpr std::uint32_t compose_image_samples = 6;

// The opaque pipeline id the stand-in's own draw names; 3 is never a PSO pointer and never a registered pipeline id
// (1 and 2 are the frame dump's two control pipelines).
inline constexpr std::uintptr_t pipeline_id = 3;

inline constexpr std::uint32_t mode_sample_image = 0;    // shipping: texel-fetch the colour image
inline constexpr std::uint32_t mode_mapping_control = 1; // diagnostic: the mapped source coordinate, never a colour

// The stand-in's push-constant block: the first 48 bytes of the 144-byte ShaderUserData block the replay pushes for
// every graphics draw, so the layer's own draw needs no new push path (vulkan_replay.h, ShaderUserData).
struct Push {
    float src[4]{};  // the colour image's texel rectangle: left, top, right, bottom
    float dst[4]{};  // the presented target's pixel rectangle: left, top, right, bottom
    std::uint32_t mode = mode_sample_image;
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(Push) == 48, "the stand-in's push block is the first 48 bytes of ShaderUserData");

// Why no stand-in draw is recorded. Named so a boot log and the test pin the arm, not just "nothing happened".
enum class Refusal : std::uint32_t {
    none = 0,
    switch_off,                // PALMETTO_UPSCALE_STANDIN is unset/off
    not_compose_pass,          // the armed draw's pixel stage is not the measured six-sample upscale pass
    no_colour_image,           // no resolved T# image on the upscale draw's descriptor tables
    colour_extent_degenerate,  // the resolved colour image has a zero extent
    target_extent_degenerate,  // the presented target has a zero extent
};

inline const char* refusal_name(Refusal refusal) {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::switch_off: return "switch off";
    case Refusal::not_compose_pass: return "the armed draw is not the measured upscale pass";
    case Refusal::no_colour_image: return "no resolved colour image on the upscale draw's tables";
    case Refusal::colour_extent_degenerate: return "the colour image has a zero extent";
    case Refusal::target_extent_degenerate: return "the presented target has a zero extent";
    }
    return "unknown";
}

// The measured predicate: the draw is the upscale pass when its pixel stage samples the colour image through the six
// measured image instructions. A depth-only or constant pass is not it.
inline bool is_compose_pass(bool has_pixel_stage, std::uint32_t pixel_image_samples) {
    return has_pixel_stage && pixel_image_samples == compose_image_samples;
}

// The inputs one armed draw gives the decision. Pure.
struct Inputs {
    bool switch_on = false;
    bool has_pixel_stage = false;
    std::uint32_t pixel_image_samples = 0;
    bool colour_resolved = false;       // a T# the shim resolved to a layer image
    std::uint32_t colour_width = 0;
    std::uint32_t colour_height = 0;
    std::uint32_t target_width = 0;     // the presented (armed draw's) colour target
    std::uint32_t target_height = 0;
};

// The decision and the geometry a stand-in draw would use. `push` is only meaningful when `draw` is true.
struct Plan {
    bool draw = false;
    Refusal refusal = Refusal::switch_off;
    Push push{};
};

// The stand-in's shipping plan: switch on, the measured upscale pass, a resolved colour image and two non-empty
// extents. The geometry is the whole colour image stretched over the whole presented target (the measured 4K-to-4K
// case makes it a 1:1 stretch); nothing is cropped, offset or colourised. Pure; the tests pin every refusal.
inline Plan plan(const Inputs& in) {
    Plan out;
    if (!in.switch_on) {
        out.refusal = Refusal::switch_off;
        return out;
    }
    if (!is_compose_pass(in.has_pixel_stage, in.pixel_image_samples)) {
        out.refusal = Refusal::not_compose_pass;
        return out;
    }
    if (!in.colour_resolved) {
        out.refusal = Refusal::no_colour_image;
        return out;
    }
    if (!in.colour_width || !in.colour_height) {
        out.refusal = Refusal::colour_extent_degenerate;
        return out;
    }
    if (!in.target_width || !in.target_height) {
        out.refusal = Refusal::target_extent_degenerate;
        return out;
    }
    out.draw = true;
    out.refusal = Refusal::none;
    out.push.src[0] = 0.0f;
    out.push.src[1] = 0.0f;
    out.push.src[2] = static_cast<float>(in.colour_width);
    out.push.src[3] = static_cast<float>(in.colour_height);
    out.push.dst[0] = 0.0f;
    out.push.dst[1] = 0.0f;
    out.push.dst[2] = static_cast<float>(in.target_width);
    out.push.dst[3] = static_cast<float>(in.target_height);
    out.push.mode = mode_sample_image;
    out.push.target_width = in.target_width;
    out.push.target_height = in.target_height;
    return out;
}

// The project's mapping control: the same geometry, but the pixel stage writes the mapped source coordinate instead of
// sampling. Diagnostic only - the shipping plan above is always the sampling one; the tests check they cannot be
// confused. Pure.
inline Plan mapping_control(const Plan& sampled) {
    Plan out = sampled;
    out.push.mode = mode_mapping_control;
    return out;
}

// Whether a plan samples a colour image (the shipping mode) or is the mapping control. Pure.
inline bool samples_image(const Plan& p) { return p.draw && p.push.mode == mode_sample_image; }

// The continuous source texel coordinate of a destination pixel's centre, and the nearest texel inside `src`. These are
// exactly the shader's formulas (upscale_standin.frag), so the unit test measures the stretch geometry the boot's
// control draw writes. Pure.
inline void map_dest_pixel(const Push& push, std::uint32_t x, std::uint32_t y, float& sx, float& sy) {
    const float u = (static_cast<float>(x) + 0.5f - push.dst[0]) / (push.dst[2] - push.dst[0]);
    const float v = (static_cast<float>(y) + 0.5f - push.dst[1]) / (push.dst[3] - push.dst[1]);
    sx = push.src[0] + u * (push.src[2] - push.src[0]);
    sy = push.src[1] + v * (push.src[3] - push.src[1]);
}

inline void sample_texel(const Push& push, std::uint32_t x, std::uint32_t y, std::int32_t& tx, std::int32_t& ty) {
    float sx = 0.0f, sy = 0.0f;
    map_dest_pixel(push, x, y, sx, sy);
    tx = static_cast<std::int32_t>(sx);  // sx >= src.left >= 0
    ty = static_cast<std::int32_t>(sy);
    if (tx > static_cast<std::int32_t>(push.src[2]) - 1) tx = static_cast<std::int32_t>(push.src[2]) - 1;
    if (ty > static_cast<std::int32_t>(push.src[3]) - 1) ty = static_cast<std::int32_t>(push.src[3]) - 1;
    if (tx < static_cast<std::int32_t>(push.src[0])) tx = static_cast<std::int32_t>(push.src[0]);
    if (ty < static_cast<std::int32_t>(push.src[1])) ty = static_cast<std::int32_t>(push.src[1]);
}

// The mapping control's pixel value: the mapped source coordinate in the first two channels, alpha 1. The boot's
// readback of a full-rect 1:1 stretch therefore reads a horizontal/vertical ramp (mean about 128 128 0), which no
// black frame and no constant control colour produces. Pure.
inline void mapping_control_colour(const Push& push, std::uint32_t x, std::uint32_t y, float out[4]) {
    out[0] = (static_cast<float>(x) + 0.5f - push.dst[0]) / (push.dst[2] - push.dst[0]);
    out[1] = (static_cast<float>(y) + 0.5f - push.dst[1]) / (push.dst[3] - push.dst[1]);
    out[2] = 0.0f;
    out[3] = 1.0f;
}

// The push dwords the draw's recorded user data carries: the same bytes the shader reads as Push. Pure; the test
// round-trips a plan through it, so a layout change cannot silently move a rectangle.
inline void write_push(const Push& push, std::uint32_t* user_data) {
    std::memcpy(user_data, &push, sizeof(push));
}

// The stand-in switch: PALMETTO_UPSCALE_STANDIN, unset/empty/0/n = off (the default keeps the measured path byte for
// byte). On any other value the layer plans a stand-in draw for the measured upscale pass.
inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_UPSCALE_STANDIN");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The mapping control: PALMETTO_UPSCALE_STANDIN_CONTROL, a diagnostic that adds the project's coordinate ramp draw
// after the shipping stand-in draw so a boot proves the rectangle arithmetic while the colour image is black.
inline bool mapping_control_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_UPSCALE_STANDIN_CONTROL");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

}  // namespace upscale_standin
}  // namespace palmetto
