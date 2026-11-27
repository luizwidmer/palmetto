#pragma once

// Readiness plan for the title's Series-S (kind-7) FSR output consumer. The title's branch and resource shape are
// measured, but no arm has reached an FFX dispatch or shown the output being consumed by a compose draw. This header
// therefore describes a fail-closed candidate, not an active runtime route. A future caller must supply the measured
// resource provenance, compose link, filter and numeric interpretation; shape/format alone never tags an image as FSR.
//
// The current stand-in copies sampled values with point texelFetch into the measured compose target. It does not
// emulate FSR or infer the transfer function. Keep this path separate from upscale_standin::plan: that plan is for the
// default six-sample graphics draw and its first resolved colour image, not the Series-S kind-7 output.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace palmetto {
namespace upscale_kind7 {

inline constexpr char switch_name[] = "PALMETTO_UPSCALE_KIND7_CONSUMER";
// The candidate source is the kind-7 branch's created 1920x1080 DXGI-67 output shape; shape alone does not prove an
// FFX dispatch wrote it. The measured 1280x720 colour/depth textures are dispatch inputs, not this consumer source.
inline constexpr std::uint32_t source_width = 1920;
inline constexpr std::uint32_t source_height = 1080;
// The 4K target is the measured default kind-6 compose target, used only as candidate geometry. No kind-7-to-compose
// link or target has been observed, so `consumer_observed` must be supplied independently before a plan can draw.
inline constexpr std::uint32_t target_width = 3840;
inline constexpr std::uint32_t target_height = 2160;

// Formats are symbolic here so the decision stays GPU-free. The Vulkan adapter in vulkan_present_compose.h maps only
// the measured source and target VkFormats to these values; an arbitrary format is not a provenance signal.
enum class Format : std::uint32_t {
    unknown = 0,
    r9g9b9e5_ufloat,        // DXGI format 67 -> VK_FORMAT_E5B9G9R9_UFLOAT_PACK32
    r16g16b16a16_float,     // DXGI format 37 -> VK_FORMAT_R16G16B16A16_SFLOAT
    other,
};

enum class Provenance : std::uint32_t {
    unknown = 0,
    series_s_kind7_output,
};

enum class Filter : std::uint32_t {
    unmeasured = 0,
    point,
    linear,
};

enum class NumericMode : std::uint32_t {
    unmeasured = 0,
    passthrough,  // the stand-in's current texelFetch -> float-target behavior
    transformed,  // no measured kind-7 transfer/colour transform is implemented here
};

enum class SourceLayout : std::uint32_t {
    unmeasured = 0,
    shader_read_only,
    general,
    other,
};

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct Inputs {
    bool switch_on = false;
    Provenance provenance = Provenance::unknown;  // caller must prove the Series-S kind-7 producer, not infer from format
    bool source_resolved = false;
    Format source_format = Format::unknown;
    bool source_sampled = false;  // the actual Vulkan view format advertises sampled-image support
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    SourceLayout source_layout = SourceLayout::unmeasured;
    Format target_format = Format::unknown;
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    bool has_source_rect = false;  // must be explicitly measured before an observed consumer can draw
    Rect source_rect{};
    bool has_target_rect = false;  // absence is unmeasured, not an implicit whole-target rectangle
    Rect target_rect{};
    bool consumer_observed = false;  // false until an FFX-produced image is seen entering this compose draw
    Filter filter = Filter::unmeasured;
    NumericMode numeric_mode = NumericMode::unmeasured;
};

enum class Refusal : std::uint32_t {
    none = 0,
    switch_off,
    no_kind7_provenance,
    source_unresolved,
    source_format_unmeasured,
    source_not_sampled,
    source_extent_degenerate,
    source_extent_unmeasured,
    target_format_unmeasured,
    target_extent_degenerate,
    target_extent_unmeasured,
    source_layout_unmeasured,
    source_layout_unsupported,
    rect_unmeasured,
    consumer_not_observed,
    filter_unmeasured,
    filter_not_supported,
    numeric_mode_unmeasured,
    numeric_transform_unimplemented,
};

inline const char* refusal_name(Refusal refusal) {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::switch_off: return "PALMETTO_UPSCALE_KIND7_CONSUMER is not measured";
    case Refusal::no_kind7_provenance: return "source has no measured Series-S kind-7 provenance";
    case Refusal::source_unresolved: return "kind-7 source image is unresolved";
    case Refusal::source_format_unmeasured: return "source is not the measured R9G9B9E5 format";
    case Refusal::source_not_sampled: return "the kind-7 source view lacks sampled-image support";
    case Refusal::source_extent_degenerate: return "source extent is empty";
    case Refusal::source_extent_unmeasured: return "source extent is not the measured 1920x1080 shape";
    case Refusal::target_format_unmeasured: return "target is not the measured RGBA16F format";
    case Refusal::target_extent_degenerate: return "target extent is empty";
    case Refusal::target_extent_unmeasured: return "target extent is not the measured 3840x2160 shape";
    case Refusal::source_layout_unmeasured: return "the kind-7 source image layout is unmeasured";
    case Refusal::source_layout_unsupported: return "the compose source layout must be SHADER_READ_ONLY_OPTIMAL or GENERAL";
    case Refusal::rect_unmeasured: return "only explicitly measured whole source and target rectangles are admitted";
    case Refusal::consumer_not_observed: return "no measured link from the kind-7 output to this compose draw";
    case Refusal::filter_unmeasured: return "kind-7 consumer filter is unmeasured";
    case Refusal::filter_not_supported: return "the stand-in only implements point texelFetch";
    case Refusal::numeric_mode_unmeasured: return "kind-7 output numeric interpretation is unmeasured";
    case Refusal::numeric_transform_unimplemented: return "a kind-7 colour/numeric transform is not implemented";
    }
    return "unknown";
}

inline bool parse_switch(const char* value) {
    if (!value) return false;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
    const char* end = value + std::strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    static constexpr char token[] = "measured";
    if (end - value != static_cast<std::ptrdiff_t>(sizeof(token) - 1)) return false;
    for (std::size_t i = 0; i < sizeof(token) - 1; ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != token[i]) return false;
    }
    return true;
}

inline bool enabled() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv(switch_name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return parse_switch(value);
}

inline bool same_rect(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

struct Plan {
    bool compose = false;
    bool candidate_mapping = false;  // geometry only; it is not permission to draw while refusal != none
    Refusal refusal = Refusal::switch_off;
    Rect source_rect{};
    Rect target_rect{};
};

// The candidate is the whole created kind-7 output shape mapped to the measured default 4K target; the link is not
// measured. The existing stand-in's filter and numeric behavior are admitted only after the producer->consumer link,
// point filter and passthrough semantics are explicit; unknown values refuse by name. No resource is marked as kind 7
// here, and this plan does not invoke PresentComposer's separate SDR/HDR colour-transform pass.
inline Plan plan(const Inputs& in) {
    Plan out;
    if (!in.switch_on) {
        out.refusal = Refusal::switch_off;
        return out;
    }
    if (in.provenance != Provenance::series_s_kind7_output) {
        out.refusal = Refusal::no_kind7_provenance;
        return out;
    }
    if (!in.source_resolved) {
        out.refusal = Refusal::source_unresolved;
        return out;
    }
    if (in.source_format != Format::r9g9b9e5_ufloat) {
        out.refusal = Refusal::source_format_unmeasured;
        return out;
    }
    if (!in.source_sampled) {
        out.refusal = Refusal::source_not_sampled;
        return out;
    }
    if (!in.source_width || !in.source_height) {
        out.refusal = Refusal::source_extent_degenerate;
        return out;
    }
    if (in.source_width != source_width || in.source_height != source_height) {
        out.refusal = Refusal::source_extent_unmeasured;
        return out;
    }
    if (in.target_format != Format::r16g16b16a16_float) {
        out.refusal = Refusal::target_format_unmeasured;
        return out;
    }
    if (!in.target_width || !in.target_height) {
        out.refusal = Refusal::target_extent_degenerate;
        return out;
    }
    if (in.target_width != target_width || in.target_height != target_height) {
        out.refusal = Refusal::target_extent_unmeasured;
        return out;
    }

    const Rect full_source{0.0f, 0.0f, static_cast<float>(source_width), static_cast<float>(source_height)};
    const Rect full_target{0.0f, 0.0f, static_cast<float>(target_width), static_cast<float>(target_height)};
    out.candidate_mapping = true;
    out.source_rect = full_source;
    out.target_rect = full_target;
    if (!in.consumer_observed) {
        out.refusal = Refusal::consumer_not_observed;
        return out;
    }
    if (in.source_layout == SourceLayout::unmeasured) {
        out.refusal = Refusal::source_layout_unmeasured;
        return out;
    }
    if (in.source_layout != SourceLayout::shader_read_only && in.source_layout != SourceLayout::general) {
        out.refusal = Refusal::source_layout_unsupported;
        return out;
    }
    if (!in.has_source_rect || !in.has_target_rect || !same_rect(in.source_rect, full_source) ||
        !same_rect(in.target_rect, full_target)) {
        out.refusal = Refusal::rect_unmeasured;
        return out;
    }
    if (in.filter == Filter::unmeasured) {
        out.refusal = Refusal::filter_unmeasured;
        return out;
    }
    if (in.filter != Filter::point) {
        out.refusal = Refusal::filter_not_supported;
        return out;
    }
    if (in.numeric_mode == NumericMode::unmeasured) {
        out.refusal = Refusal::numeric_mode_unmeasured;
        return out;
    }
    if (in.numeric_mode != NumericMode::passthrough) {
        out.refusal = Refusal::numeric_transform_unimplemented;
        return out;
    }
    out.compose = true;
    out.refusal = Refusal::none;
    return out;
}

// Return the point-sampled source texel for a destination pixel centre, using the same whole-rectangle mapping as the
// current stand-in shader. This operates on candidate geometry for tests/diagnostics; callers must still require
// Plan::compose before recording a draw.
inline bool map_point_texel(const Plan& plan, std::uint32_t x, std::uint32_t y, std::int32_t& source_x,
                            std::int32_t& source_y) {
    if (!plan.candidate_mapping || x >= target_width || y >= target_height) return false;
    const float u = (static_cast<float>(x) + 0.5f - plan.target_rect.left) /
                    (plan.target_rect.right - plan.target_rect.left);
    const float v = (static_cast<float>(y) + 0.5f - plan.target_rect.top) /
                    (plan.target_rect.bottom - plan.target_rect.top);
    const float sx = plan.source_rect.left + u * (plan.source_rect.right - plan.source_rect.left);
    const float sy = plan.source_rect.top + v * (plan.source_rect.bottom - plan.source_rect.top);
    source_x = static_cast<std::int32_t>(sx);
    source_y = static_cast<std::int32_t>(sy);
    if (source_x >= static_cast<std::int32_t>(source_width)) source_x = static_cast<std::int32_t>(source_width - 1);
    if (source_y >= static_cast<std::int32_t>(source_height)) source_y = static_cast<std::int32_t>(source_height - 1);
    return true;
}

}  // namespace upscale_kind7
}  // namespace palmetto
