// The presenter capture colour helpers, and the pure presenter geometry/colour-family rules they are checked against:
// the fit/scale rectangle rule (src/present_fit.h, docs/peer_port_first_ui.md section 15) decides where the presented
// picture lands in the window, so its cases are pinned here without a GPU; the GPU test presents real frames.
#include "vk_presenter_capture_colour.h"
#include "../src/present_fit.h"
#include "../src/present_colour.h"

#include <array>
#include <cmath>
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
    // ---- Measured SDR/HDR transfer, gamut and Vulkan surface-family rules --------------------------------------
    using namespace palmetto::present_colour;
    const auto near = [](float actual, float expected, float epsilon = 0.0001f) {
        return std::abs(actual - expected) <= epsilon;
    };
    for (const float encoded : {0.0f, 0.0031308f, 0.04045f, 0.5f, 1.0f})
        EXPECT(near(srgb_encode(srgb_decode(encoded)), encoded, 0.00002f));
    for (const float encoded : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f})
        EXPECT(near(gamma22_encode(gamma22_decode(encoded)), encoded, 0.00002f));
    EXPECT(near(srgb_decode(0.04045f), 0.0031308f, 0.000002f));
    EXPECT(near(gamma22_decode(0.5f), 0.21763764f, 0.000002f));
    EXPECT(near(nits_to_pq(100.0f), 0.5080784f, 0.0002f));
    EXPECT(near(nits_to_pq(1000.0f), 0.7518271f, 0.0002f));
    EXPECT(near(nits_to_pq(10000.0f), 1.0f, 0.000002f));
    EXPECT(near(pq_to_nits(nits_to_pq(1000.0f)), 1000.0f, 0.1f));

    const Rgb red2020 = bt709_to_bt2020({1.0f, 0.0f, 0.0f});
    EXPECT(near(red2020.r, 0.627404f, 0.000002f));
    EXPECT(near(red2020.g, 0.069097f, 0.000002f));
    EXPECT(near(red2020.b, 0.016391f, 0.000002f));
    const Rgb source_linear{0.12f, 0.43f, 0.78f};
    const Rgb recovered = bt2020_to_bt709(bt709_to_bt2020(source_linear));
    EXPECT(near(recovered.r, source_linear.r, 0.00002f));
    EXPECT(near(recovered.g, source_linear.g, 0.00002f));
    EXPECT(near(recovered.b, source_linear.b, 0.00002f));
    const Rgb sdr_half = sdr_decode({0.5f, 0.5f, 0.5f}, SdrEotf::gamma22);
    const Rgb hdr_working = to_working(sdr_half, 1.0f, false, true, 203.0f, 1000.0f);
    EXPECT(near(hdr_working.r, 0.21763764f * 203.0f, 0.001f));
    EXPECT(near(hdr_working.g, hdr_working.r, 0.0001f));
    EXPECT(near(hdr_working.b, hdr_working.r, 0.0001f));
    const Rgb hdr_recovered = pq_to_nits(nits_to_pq(hdr_working));
    EXPECT(near(hdr_recovered.r, hdr_working.r, 0.1f));
    const Rgb below_knee{0.4f, 0.5f, 0.6f};
    const Rgb unchanged = tone_map_hdr_to_sdr(below_knee, 203.0f, 1000.0f);
    EXPECT(near(unchanged.r, below_knee.r) && near(unchanged.g, below_knee.g) && near(unchanged.b, below_knee.b));
    const float hdr_peak_sdr = 1000.0f / 203.0f;
    const Rgb peak_mapped = tone_map_hdr_to_sdr({hdr_peak_sdr, hdr_peak_sdr, hdr_peak_sdr}, 203.0f, 1000.0f);
    EXPECT(near(peak_mapped.r, 1.0f, 0.0001f));

    // This is the measured plain-present target: UNORM B8G8R8A8 with sRGB-nonlinear colour space.
    constexpr SurfaceFormat measured_surface[] = {
        {SurfacePixelFormat::b8g8r8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
    };
    const SurfaceChoice measured_choice = choose_surface_format(measured_surface, 1, false, false);
    EXPECT(measured_choice.index == 0 && measured_choice.matched && !measured_choice.hdr_active);
    // The preference order, not the surface enumeration order, is B8 then R8 for each colour family.
    constexpr SurfaceFormat srgb_family[] = {
        {SurfacePixelFormat::r8g8b8a8_srgb, SurfaceColourSpace::srgb_nonlinear},
        {SurfacePixelFormat::b8g8r8a8_srgb, SurfaceColourSpace::srgb_nonlinear},
        {SurfacePixelFormat::b8g8r8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
    };
    const SurfaceChoice srgb_choice = choose_surface_format(srgb_family, 3, true, false);
    EXPECT(srgb_choice.index == 1 && srgb_choice.matched && !srgb_choice.srgb_missing);
    constexpr SurfaceFormat unorm_fallback[] = {
        {SurfacePixelFormat::r8g8b8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
        {SurfacePixelFormat::b8g8r8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
    };
    const SurfaceChoice fallback_choice = choose_surface_format(unorm_fallback, 2, true, false);
    EXPECT(fallback_choice.index == 1 && fallback_choice.matched && fallback_choice.srgb_missing);
    constexpr SurfaceFormat hdr_families[] = {
        {SurfacePixelFormat::a2r10g10b10_unorm, SurfaceColourSpace::hdr10_st2084},
        {SurfacePixelFormat::a2b10g10r10_unorm, SurfaceColourSpace::hdr10_st2084},
        {SurfacePixelFormat::b8g8r8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
    };
    const SurfaceChoice hdr_choice = choose_surface_format(hdr_families, 3, false, true);
    EXPECT(hdr_choice.index == 1 && hdr_choice.matched && hdr_choice.hdr_active && !hdr_choice.hdr_missing);
    const SurfaceChoice hdr_fallback_choice = choose_surface_format(measured_surface, 1, false, true);
    EXPECT(hdr_fallback_choice.index == 0 && hdr_fallback_choice.matched && !hdr_fallback_choice.hdr_active &&
           hdr_fallback_choice.hdr_missing);
    constexpr SurfaceFormat wrong_hdr_colour_space[] = {
        {SurfacePixelFormat::a2b10g10r10_unorm, SurfaceColourSpace::srgb_nonlinear},
        {SurfacePixelFormat::b8g8r8a8_unorm, SurfaceColourSpace::srgb_nonlinear},
    };
    const SurfaceChoice wrong_hdr_choice = choose_surface_format(wrong_hdr_colour_space, 2, false, true);
    EXPECT(wrong_hdr_choice.index == 1 && wrong_hdr_choice.matched && !wrong_hdr_choice.hdr_active &&
           wrong_hdr_choice.hdr_missing);
    const SurfaceChoice unmatched = choose_surface_format(nullptr, 0, false, false);
    EXPECT(unmatched.index == 0 && !unmatched.matched);

    Hdr10OutputMode hdr_mode = Hdr10OutputMode::diagnostic;
    const char* refusal = nullptr;
    EXPECT(parse_hdr10_output("", hdr_mode, &refusal));
    EXPECT(hdr_mode == Hdr10OutputMode::off && refusal == nullptr);
    EXPECT(parse_hdr10_output("OuTpUt", hdr_mode, &refusal));
    EXPECT(hdr_mode == Hdr10OutputMode::diagnostic && refusal == nullptr);
    EXPECT(parse_hdr10_output("0", hdr_mode, &refusal));
    EXPECT(hdr_mode == Hdr10OutputMode::off && refusal == nullptr);
    hdr_mode = Hdr10OutputMode::diagnostic;
    EXPECT(!parse_hdr10_output("2", hdr_mode, &refusal));
    EXPECT(hdr_mode == Hdr10OutputMode::diagnostic);
    EXPECT(refusal && std::string(refusal).find("PALMETTO_PRESENT_HDR10") != std::string::npos);

    std::printf("present fit/scale, SDR/HDR conversion math and surface-family policy ok\n");
    return 0;
}
