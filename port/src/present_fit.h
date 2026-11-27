#pragma once

// Where the presented picture lands in the window (docs/peer_port_first_ui.md section 15).
//
// WHAT THE TREE DID. The plain present path (`VulkanPresenter::present`, reached by every measured PresentX of this
// title) blits the plane's whole source, or the source rectangle the title names, to the swapchain image, and the
// swapchain is the adopted window's client area. So the measured picture is the source stretched over the whole client
// area: at the measured window (2564x1421 client) and the measured plane (3840x2160) that is a horizontal scale of
// ~0.668 and a vertical one of ~0.658, i.e. about 1.5% of horizontal stretch and no bars (`rule = fill` below).
//
// WHAT THE PEER/CONSOLE MODEL DOES. A console presents its output plane to a fixed-aspect display, so the plane's own
// aspect ratio is the picture's; the peers that run on PC windows keep that property by fitting the plane into the
// largest rectangle of its aspect ratio and leaving the rest of the window black (five-star's PresentScaler: "over black
// (PresentScaler, black bars)"; the other peers present the console plane whole). A PC window's client area is not the
// console's output aspect, so the faithful host rule is `fit` below: the source rectangle's aspect preserved, the
// picture centred, integer pixels, the remainder of the swapchain image black.
//
// The rule is pure integer arithmetic so every case is testable without a GPU (port/test/vk_presenter_capture_colour_test.cpp);
// the presenter only decides which rule to pass. It is behind PALMETTO_PRESENT_FIT (off = `fill`, the tree's measured
// path byte for byte), and it changes nothing about which texture is presented, its colour space or its filtering.

#include <cstdint>

namespace palmetto::present_fit {

enum class Rule : std::uint8_t { fill = 0, fit = 1 };

struct Rect {  // output pixels; left/top inclusive, right/bottom exclusive, as VkImageBlit's offsets
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

inline const char* rule_name(Rule rule) { return rule == Rule::fit ? "fit" : "fill"; }

// The destination rectangle in an out_w x out_h output for a source region of src_w x src_h texels.
//   fill: the whole output (the tree's measured behaviour).
//   fit:  the largest rectangle of the source's aspect ratio that fits the output, centred; rounding is to the nearest
//         pixel, so the bar on the two sides differs by at most one pixel.
// A non-positive source or output keeps the tree's whole-output rectangle: a caller with no measured sizes must not get
// a guessed geometry.
inline Rect scale_rect(std::int32_t src_w, std::int32_t src_h, std::int32_t out_w, std::int32_t out_h, Rule rule) {
    if (rule != Rule::fit || src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) return {0, 0, out_w, out_h};
    // Compare src_w/src_h with out_w/out_h without floating point: src_w * out_h against out_w * src_h.
    const std::int64_t source = std::int64_t(src_w) * out_h;
    const std::int64_t output = std::int64_t(out_w) * src_h;
    const auto round_div = [](std::int64_t numerator, std::int64_t denominator) {
        return std::int32_t((numerator + denominator / 2) / denominator);  // both positive here
    };
    std::int32_t width = out_w, height = out_h;
    if (source > output) {
        // The source is relatively wider: the width fills the output and the height follows the aspect ratio.
        height = round_div(std::int64_t(out_w) * src_h, src_w);
        if (height < 1) height = 1;
        if (height > out_h) height = out_h;
    } else if (source < output) {
        // The source is relatively taller (or equal, which stays whole): the height fills, the width follows.
        width = round_div(std::int64_t(out_h) * src_w, src_h);
        if (width < 1) width = 1;
        if (width > out_w) width = out_w;
    }
    const std::int32_t left = (out_w - width) / 2;
    const std::int32_t top = (out_h - height) / 2;
    return {left, top, left + width, top + height};
}

// True when `rect` leaves any of the output untouched: those pixels are black in a fitted present.
inline bool has_bars(const Rect& rect, std::int32_t out_w, std::int32_t out_h) {
    return rect.left > 0 || rect.top > 0 || rect.right < out_w || rect.bottom < out_h;
}

// PALMETTO_PRESENT_FIT: unset/empty/off/false/no/0/fill/stretch = fill (the tree's measured path);
// on/true/yes/1/fit/aspect/letterbox = fit; anything else is refused by name (`refusal` points at the reason), so a
// typo cannot silently change the geometry. Case and surrounding blanks are ignored.
inline bool parse_rule(const char* value, Rule& out, const char** refusal) {
    const auto equals = [](const char* text, const char* word) {
        while (*text == ' ' || *text == '\t') ++text;
        for (; *word; ++text, ++word) {
            char a = *text;
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (a != *word) return false;
        }
        while (*text == ' ' || *text == '\t') ++text;
        return *text == '\0';
    };
    if (!value || !*value) {
        out = Rule::fill;
        return true;
    }
    for (const char* word : {"off", "false", "no", "0", "fill", "stretch"})
        if (equals(value, word)) {
            out = Rule::fill;
            return true;
        }
    for (const char* word : {"on", "true", "yes", "1", "fit", "aspect", "letterbox"})
        if (equals(value, word)) {
            out = Rule::fit;
            return true;
        }
    if (refusal) *refusal = "PALMETTO_PRESENT_FIT is not fill/off or fit/on: refusing to guess the present geometry";
    return false;
}

}  // namespace palmetto::present_fit
