#pragma once

// GPU-free reference math and surface-family policy for the PresentX compositor. The corresponding shader is
// port/src/present_compose.comp; its values and anchors are exercised by the CPU and presenter-capture tests.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace palmetto::present_colour {

enum class SdrEotf : std::uint32_t { srgb = 0, gamma22 = 1 };

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

inline float srgb_decode(float encoded) {
    const float e = std::max(encoded, 0.0f);
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

inline float srgb_encode(float linear) {
    const float l = std::clamp(linear, 0.0f, 1.0f);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

inline float gamma22_decode(float encoded) { return std::pow(std::max(encoded, 0.0f), 2.2f); }
inline float gamma22_encode(float linear) { return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f); }

inline Rgb sdr_decode(Rgb encoded, SdrEotf eotf) {
    const auto decode = [eotf](float value) {
        return eotf == SdrEotf::gamma22 ? gamma22_decode(value) : srgb_decode(value);
    };
    return {decode(encoded.r), decode(encoded.g), decode(encoded.b)};
}

inline Rgb sdr_encode(Rgb linear, SdrEotf eotf) {
    const auto encode = [eotf](float value) {
        return eotf == SdrEotf::gamma22 ? gamma22_encode(value) : srgb_encode(value);
    };
    return {encode(linear.r), encode(linear.g), encode(linear.b)};
}

inline float pq_to_nits(float encoded) {
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float ep = std::pow(std::clamp(encoded, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(ep - c1, 0.0f) / (c2 - c3 * ep), 1.0f / m1);
}

inline float nits_to_pq(float nits) {
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

inline Rgb pq_to_nits(Rgb encoded) { return {pq_to_nits(encoded.r), pq_to_nits(encoded.g), pq_to_nits(encoded.b)}; }
inline Rgb nits_to_pq(Rgb nits) { return {nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b)}; }

inline Rgb bt709_to_bt2020(Rgb v) {
    return {0.627404f * v.r + 0.329283f * v.g + 0.043313f * v.b,
            0.069097f * v.r + 0.919540f * v.g + 0.011362f * v.b,
            0.016391f * v.r + 0.088013f * v.g + 0.895595f * v.b};
}

inline Rgb bt2020_to_bt709(Rgb v) {
    return {1.660491f * v.r - 0.587641f * v.g - 0.072850f * v.b,
            -0.124550f * v.r + 1.132900f * v.g - 0.008349f * v.b,
            -0.018151f * v.r - 0.100579f * v.g + 1.118730f * v.b};
}

inline float luminance_bt709(Rgb v) { return 0.2126f * v.r + 0.7152f * v.g + 0.0722f * v.b; }

// Matches the compositor's continuous extended-Reinhard shoulder: identity to 0.8 SDR white and content peak to white.
inline Rgb tone_map_hdr_to_sdr(Rgb linear_709, float sdr_white_nits, float hdr_peak_nits) {
    constexpr float knee = 0.8f;
    const float y = luminance_bt709(linear_709);
    if (y <= knee) return linear_709;
    const float t = 1.0f - knee;
    const float w = (std::max(hdr_peak_nits / sdr_white_nits, 1.0f) - knee) / t;
    const float u = (y - knee) / t;
    const float mapped = knee + t * u * (1.0f + u / (w * w)) / (1.0f + u);
    const float scale = mapped / y;
    return {linear_709.r * scale, linear_709.g * scale, linear_709.b * scale};
}

// Convert linear, premultiplied plane colour between the two working spaces used in the shader.
inline Rgb to_working(Rgb linear, float alpha, bool plane_hdr10, bool output_hdr10,
                      float sdr_white_nits, float hdr_peak_nits) {
    if (plane_hdr10 == output_hdr10) return linear;
    if (!plane_hdr10) {
        const Rgb rec2020 = bt709_to_bt2020(linear);
        return {rec2020.r * sdr_white_nits, rec2020.g * sdr_white_nits, rec2020.b * sdr_white_nits};
    }
    if (alpha <= 0.0f) return {};
    const Rgb unassociated_2020{linear.r / alpha / sdr_white_nits,
                                linear.g / alpha / sdr_white_nits,
                                linear.b / alpha / sdr_white_nits};
    const Rgb unassociated_709 = bt2020_to_bt709(unassociated_2020);
    const Rgb nonnegative{std::max(unassociated_709.r, 0.0f), std::max(unassociated_709.g, 0.0f),
                          std::max(unassociated_709.b, 0.0f)};
    const Rgb mapped = tone_map_hdr_to_sdr(nonnegative, sdr_white_nits, hdr_peak_nits);
    return {mapped.r * alpha, mapped.g * alpha, mapped.b * alpha};
}

enum class Hdr10OutputMode : std::uint8_t { off, diagnostic };

inline char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
inline bool ascii_equal(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right && ascii_lower(*left) == ascii_lower(*right)) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

// PALMETTO_PRESENT_HDR10 is an output request for composed planes, not an input transfer-function tag.
// Unset/empty and the explicit off values preserve the existing path; an unknown spelling leaves `out` untouched.
inline bool parse_hdr10_output(const char* value, Hdr10OutputMode& out, const char** refusal = nullptr) {
    if (refusal) *refusal = nullptr;
    if (!value || !*value || ascii_equal(value, "0") || ascii_equal(value, "off") ||
        ascii_equal(value, "false") || ascii_equal(value, "no")) {
        out = Hdr10OutputMode::off;
        return true;
    }
    if (ascii_equal(value, "1") || ascii_equal(value, "on") || ascii_equal(value, "true") ||
        ascii_equal(value, "yes") || ascii_equal(value, "output") || ascii_equal(value, "diagnostic")) {
        out = Hdr10OutputMode::diagnostic;
        return true;
    }
    if (refusal) *refusal = "PALMETTO_PRESENT_HDR10 must be off/0 or output/1";
    return false;
}

enum class SurfacePixelFormat : std::uint8_t {
    b8g8r8a8_srgb,
    r8g8b8a8_srgb,
    b8g8r8a8_unorm,
    r8g8b8a8_unorm,
    a2b10g10r10_unorm,
    a2r10g10b10_unorm,
    other,
};

enum class SurfaceColourSpace : std::uint8_t { srgb_nonlinear, hdr10_st2084, other };

struct SurfaceFormat {
    SurfacePixelFormat format = SurfacePixelFormat::other;
    SurfaceColourSpace colour_space = SurfaceColourSpace::other;
};

struct SurfaceChoice {
    std::size_t index = 0;
    bool matched = false;
    bool hdr_active = false;
    bool hdr_missing = false;
    bool srgb_missing = false;
};

// Keep the presenter's selection order explicit and GPU-free: HDR10 B10G10R10, then R10G10B10A2; otherwise B8 before R8
// in the source's family. An sRGB source falls back to UNORM only when the surface exposes no matching sRGB 8-bit entry.
inline SurfaceChoice choose_surface_format(const SurfaceFormat* formats, std::size_t count,
                                            bool srgb_source, bool hdr_request) {
    SurfaceChoice choice{};
    if (!formats || count == 0) return choice;

    if (hdr_request) {
        constexpr std::array hdr_order{SurfacePixelFormat::a2b10g10r10_unorm,
                                       SurfacePixelFormat::a2r10g10b10_unorm};
        for (SurfacePixelFormat wanted : hdr_order) {
            for (std::size_t i = 0; i < count; ++i) {
                if (formats[i].format == wanted && formats[i].colour_space == SurfaceColourSpace::hdr10_st2084)
                    return {i, true, true, false, false};
            }
        }
        choice.hdr_missing = true;
    }

    const std::array sdr_order = srgb_source
                                     ? std::array{SurfacePixelFormat::b8g8r8a8_srgb,
                                                  SurfacePixelFormat::r8g8b8a8_srgb}
                                     : std::array{SurfacePixelFormat::b8g8r8a8_unorm,
                                                  SurfacePixelFormat::r8g8b8a8_unorm};
    for (SurfacePixelFormat wanted : sdr_order) {
        for (std::size_t i = 0; i < count; ++i) {
            if (formats[i].format == wanted && formats[i].colour_space == SurfaceColourSpace::srgb_nonlinear) {
                choice.index = i;
                choice.matched = true;
                return choice;
            }
        }
    }

    if (srgb_source) {
        choice.srgb_missing = true;
        constexpr std::array unorm_order{SurfacePixelFormat::b8g8r8a8_unorm,
                                         SurfacePixelFormat::r8g8b8a8_unorm};
        for (SurfacePixelFormat wanted : unorm_order) {
            for (std::size_t i = 0; i < count; ++i) {
                if (formats[i].format == wanted && formats[i].colour_space == SurfaceColourSpace::srgb_nonlinear) {
                    choice.index = i;
                    choice.matched = true;
                    return choice;
                }
            }
        }
    }
    return choice;  // no policy match: retain surface format 0, as the prior presenter did
}

}  // namespace palmetto::present_colour
