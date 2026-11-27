// The presenter capture colour helpers, and the pure presenter geometry/colour-family rules they are checked against:
// the fit/scale rectangle rule (src/present_fit.h, docs/peer_port_first_ui.md section 15) decides where the presented
// picture lands in the window, so its cases are pinned here without a GPU; the GPU test presents real frames.
#include "vk_presenter_capture_colour.h"
#include "../src/present_fit.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

int main() {
    constexpr std::uint32_t width = 5, height = 3;
    for (const bool bgra : {false, true}) {
        const std::array<std::uint8_t, 4> clear = bgra ? std::array<std::uint8_t, 4>{230, 153, 51, 255}
                                                      : std::array<std::uint8_t, 4>{51, 153, 230, 255};
        std::vector<std::uint8_t> good;
        for (std::uint32_t i = 0; i < width * height; ++i)
            good.insert(good.end(), clear.begin(), clear.end());
        EXPECT(presenter_capture_matches_clear(good, width, height, bgra));

        auto stale_edge = good;
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto offset = (y * width + width - 1) * 4;
            stale_edge[offset] = stale_edge[offset + 1] = stale_edge[offset + 2] = 0;
        }
        EXPECT(!presenter_capture_matches_clear(stale_edge, width, height, bgra));

        auto drift = good;
        drift[1] += 2;
        EXPECT(presenter_capture_matches_clear(drift, width, height, bgra));
        ++drift[1];
        EXPECT(!presenter_capture_matches_clear(drift, width, height, bgra));

        EXPECT(!presenter_capture_matches_clear({}, width, height, bgra));
        EXPECT(!presenter_capture_matches_clear({}, 0, 0, bgra));
        good.pop_back();
        EXPECT(!presenter_capture_matches_clear(good, width, height, bgra));
        good.resize(good.size() - 3);
        EXPECT(!presenter_capture_matches_clear(good, width, height, bgra));

        const std::array<std::uint8_t, 4> green = bgra ? std::array<std::uint8_t, 4>{102, 204, 51, 255}
                                                      : std::array<std::uint8_t, 4>{51, 204, 102, 255};
        std::vector<std::uint8_t> composed;
        for (std::uint32_t i = 0; i < width * height; ++i)
            composed.insert(composed.end(), green.begin(), green.end());
        constexpr std::array<int, 3> expected_green{51, 204, 102};
        const auto edge_green = composed.size() - 3;
        composed[edge_green] -= 12;
        EXPECT(presenter_capture_matches_rgb(composed, width, height, bgra, expected_green, 12));
        composed[edge_green] = clear[1];
        EXPECT(!presenter_capture_matches_rgb(composed, width, height, bgra, expected_green, 12));
        composed.resize(composed.size() - 4);
        EXPECT(!presenter_capture_matches_rgb(composed, width, height, bgra, expected_green, 12));
    }

    // ---- The present fit/scale rule (docs/peer_port_first_ui.md section 15) ---------------------------------------
    using palmetto::present_fit::Rule;
    using palmetto::present_fit::Rect;
    EXPECT((Rect{0, 0, 1280, 720}.right == 1280));  // the aggregate stays a plain four-int rectangle

    // fill is the tree's measured behaviour: the source always covers the whole output, whatever its aspect ratio.
    for (const auto& source : {std::array<int, 2>{3840, 2160}, std::array<int, 2>{32, 18}, std::array<int, 2>{8, 8}}) {
        const Rect whole = palmetto::present_fit::scale_rect(source[0], source[1], 2564, 1421, Rule::fill);
        EXPECT(whole.left == 0 && whole.top == 0 && whole.right == 2564 && whole.bottom == 1421);
        EXPECT(!palmetto::present_fit::has_bars(whole, 2564, 1421));
    }

    // fit keeps the source's aspect ratio: the measured plane (3840x2160, 16:9) in the measured window (2564x1421)
    // is height-limited, so the picture is 2526 wide centred in 2564 with 19 pixels of black bar on each side.
    {
        const Rect picture = palmetto::present_fit::scale_rect(3840, 2160, 2564, 1421, Rule::fit);
        EXPECT(picture.left == 19 && picture.top == 0);
        EXPECT(picture.right == 2545 && picture.bottom == 1421);
        EXPECT(palmetto::present_fit::has_bars(picture, 2564, 1421));
        // Aspect preserved to the nearest pixel: |2526/1421 - 3840/2160| * 2160 <= 0.5 texels of source.
        EXPECT(std::abs((picture.right - picture.left) * 2160 - 3840 * (picture.bottom - picture.top)) <= 2160);
    }
    // An exact-aspect window has no bars and keeps the whole output.
    {
        const Rect picture = palmetto::present_fit::scale_rect(640, 360, 1920, 1080, Rule::fit);
        EXPECT(picture.left == 0 && picture.top == 0 && picture.right == 1920 && picture.bottom == 1080);
        EXPECT(!palmetto::present_fit::has_bars(picture, 1920, 1080));
    }
    // A 16:9 source in a narrower window is width-limited: bars above and below (the GPU test's 128x96 case).
    {
        const Rect picture = palmetto::present_fit::scale_rect(32, 18, 128, 96, Rule::fit);
        EXPECT(picture.left == 0 && picture.right == 128);
        EXPECT(picture.top == 12 && picture.bottom == 84);
        EXPECT(palmetto::present_fit::has_bars(picture, 128, 96));
    }
    // A square source in a wide output is pillarboxed: height fills, the horizontal bars are equal and within one pixel.
    {
        const Rect picture = palmetto::present_fit::scale_rect(8, 8, 128, 96, Rule::fit);
        EXPECT(picture.top == 0 && picture.bottom == 96);
        EXPECT(picture.right - picture.left == 96);
        EXPECT(picture.left == 16 && picture.right == 112);
    }
    // A 4:3 source in the measured window is height-limited too; the rounding is to the nearest pixel.
    {
        const Rect picture = palmetto::present_fit::scale_rect(640, 480, 2564, 1421, Rule::fit);
        EXPECT(picture.top == 0 && picture.bottom == 1421);
        EXPECT(picture.right - picture.left == 1895);  // round(1421 * 640 / 480) = 1895
        EXPECT(picture.left == 334 && picture.right == 2229);
    }
    // The output rectangle only ever stays inside the output and the two bars of an axis differ by at most one pixel.
    for (int w = 1; w <= 40; ++w)
        for (int h = 1; h <= 40; ++h) {
            const Rect picture = palmetto::present_fit::scale_rect(16, 9, w, h, Rule::fit);
            EXPECT(picture.left >= 0 && picture.top >= 0 && picture.right <= w && picture.bottom <= h);
            EXPECT(picture.left < picture.right && picture.top < picture.bottom);
            EXPECT(std::abs(picture.left - (w - picture.right)) <= 1);
            EXPECT(std::abs(picture.top - (h - picture.bottom)) <= 1);
        }
    // A caller with no measured sizes gets the tree's whole-output rectangle, never a guessed geometry.
    for (const auto& source : {std::array<int, 2>{0, 100}, std::array<int, 2>{100, 0}, std::array<int, 2>{-4, 9}}) {
        const Rect picture = palmetto::present_fit::scale_rect(source[0], source[1], 128, 96, Rule::fit);
        EXPECT(picture.left == 0 && picture.top == 0 && picture.right == 128 && picture.bottom == 96);
    }
    EXPECT(palmetto::present_fit::has_bars(palmetto::present_fit::scale_rect(16, 9, 0, 0, Rule::fit), 0, 0) == false);

    // PALMETTO_PRESENT_FIT: off/fill spellings keep the tree's path, on/fit spellings fit, anything else is refused.
    {
        Rule rule = Rule::fit;
        const char* refusal = nullptr;
        for (const char* off : {"", "0", "off", "OFF", "false", "no", "fill", "stretch", " Stretch "}) {
            EXPECT(palmetto::present_fit::parse_rule(off, rule, &refusal));
            EXPECT(rule == Rule::fill);
        }
        EXPECT(palmetto::present_fit::parse_rule(nullptr, rule, &refusal));
        EXPECT(rule == Rule::fill);
        for (const char* on : {"1", "on", "TRUE", "yes", "fit", "Fit", "aspect", "letterbox", "\tfit  "}) {
            EXPECT(palmetto::present_fit::parse_rule(on, rule, &refusal));
            EXPECT(rule == Rule::fit);
        }
        for (const char* bad : {"2", "banana", "fitt", "offf", "fit fill"}) {
            rule = Rule::fit;
            refusal = nullptr;
            EXPECT(!palmetto::present_fit::parse_rule(bad, rule, &refusal));
            EXPECT(rule == Rule::fit);  // a refused value leaves the caller's value alone (the caller's default is fill)
            EXPECT(refusal && std::string(refusal).find("PALMETTO_PRESENT_FIT") != std::string::npos);
        }
        EXPECT(std::string(palmetto::present_fit::rule_name(Rule::fill)) == "fill");
        EXPECT(std::string(palmetto::present_fit::rule_name(Rule::fit)) == "fit");
    }
    std::printf("present fit/scale rule: fill, fit and the switch spellings ok\n");
    return 0;
}
