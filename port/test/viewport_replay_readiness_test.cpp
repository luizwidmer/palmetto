#include "../src/viewport_replay_readiness.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char* current_case = "startup";

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", current_case, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

bool set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value ? value : "") == 0;
#else
    return value ? ::setenv(name, value, 1) == 0 : ::unsetenv(name) == 0;
#endif
}

struct RestoreEnvironment {
    explicit RestoreEnvironment(const char* name) : name(name) {
        if (const char* previous = std::getenv(name)) {
            existed = true;
            value = previous;
        }
    }
    ~RestoreEnvironment() { set_environment(name.c_str(), existed ? value.c_str() : nullptr); }

    std::string name;
    std::string value;
    bool existed = false;
};

int exact_switch_case() {
    using namespace palmetto::viewport_replay_readiness;
    CHECK(!switch_value_enabled(nullptr));
    CHECK(!switch_value_enabled(""));
    CHECK(!switch_value_enabled("0"));
    CHECK(!switch_value_enabled("true"));
    CHECK(!switch_value_enabled("01"));
    CHECK(!switch_value_enabled(" 1"));
    CHECK(!switch_value_enabled("1 "));
    CHECK(switch_value_enabled("1"));

    CHECK(set_environment(kStrictSwitch, "1"));
    CHECK(strict_enabled());
    CHECK(set_environment(kStrictSwitch, "true"));
    CHECK(!strict_enabled());
    return 0;
}

int switch_off_preserves_legacy_case() {
    using namespace palmetto::viewport_replay_readiness;
    CHECK(set_environment(kStrictSwitch, nullptr));
    CHECK(!strict_enabled());
    const Assessment off_multi = assess(2, 2, strict_enabled());
    CHECK(off_multi.disposition == Disposition::legacy_unchanged);
    CHECK(off_multi.refusal == Refusal::none);
    CHECK(std::string(refusal_name(off_multi.refusal)) == "none");
    CHECK(assess(17, 17, strict_enabled()).disposition == Disposition::legacy_unchanged);

    CHECK(set_environment(kStrictSwitch, "0"));
    CHECK(!strict_enabled());
    CHECK(assess(2, 1, strict_enabled()).disposition == Disposition::legacy_unchanged);
    CHECK(set_environment(kStrictSwitch, ""));
    CHECK(!strict_enabled());
    CHECK(set_environment(kStrictSwitch, "true"));
    CHECK(!strict_enabled());
    return 0;
}

int zero_and_single_element_counts_case() {
    using namespace palmetto::viewport_replay_readiness;
    CHECK(set_environment(kStrictSwitch, "1"));
    CHECK(strict_enabled());

    const Assessment no_arrays = assess(0, 0, strict_enabled());
    CHECK(no_arrays.disposition == Disposition::ready);
    CHECK(no_arrays.refusal == Refusal::none);
    const Assessment no_viewport = assess(0, 1, strict_enabled());
    CHECK(no_viewport.disposition == Disposition::ready);
    CHECK(no_viewport.refusal == Refusal::none);
    const Assessment no_scissor = assess(1, 0, strict_enabled());
    CHECK(no_scissor.disposition == Disposition::ready);
    CHECK(no_scissor.refusal == Refusal::none);
    const Assessment one_each = assess(1, 1, strict_enabled());
    CHECK(one_each.disposition == Disposition::ready);
    CHECK(one_each.refusal == Refusal::none);
    return 0;
}

int multiple_viewports_case() {
    using namespace palmetto::viewport_replay_readiness;
    CHECK(assess(2, 1, true).disposition == Disposition::refuse);
    CHECK(assess(2, 1, true).refusal == Refusal::multiple_viewports_not_supported);
    CHECK(std::string(refusal_name(assess(2, 1, true).refusal)) ==
          "multiple_viewports_require_dynamic_viewport_count");
    CHECK(assess(16, 1, true).refusal == Refusal::multiple_viewports_not_supported);
    // Viewport refusals have deterministic priority when both arrays exceed one.
    CHECK(assess(2, 2, true).refusal == Refusal::multiple_viewports_not_supported);
    return 0;
}

int multiple_scissors_case() {
    using namespace palmetto::viewport_replay_readiness;
    CHECK(assess(1, 2, true).disposition == Disposition::refuse);
    CHECK(assess(1, 2, true).refusal == Refusal::multiple_scissors_not_supported);
    CHECK(std::string(refusal_name(assess(1, 2, true).refusal)) ==
          "multiple_scissors_require_dynamic_scissor_count");
    CHECK(assess(1, 16, true).refusal == Refusal::multiple_scissors_not_supported);
    return 0;
}

int multi_count_capability_case() {
    using namespace palmetto::viewport_replay_readiness;
    const ReplayCapability ready{true, true, 16};
    CHECK(assess(2, 2, true, ready).disposition == Disposition::ready);
    CHECK(assess(16, 16, true, ready).disposition == Disposition::ready);
    CHECK(assess(1, 1, true, {}).disposition == Disposition::ready);  // strict single-count path needs no new feature

    const ReplayCapability no_multi_viewport{false, true, 16};
    const Assessment missing_feature = assess(2, 2, true, no_multi_viewport);
    CHECK(missing_feature.refusal == Refusal::multiple_viewports_not_supported);
    CHECK(std::string(refusal_name(missing_feature.refusal)) == "multiple_viewports_require_dynamic_viewport_count");
    const ReplayCapability no_dynamic_count{true, false, 16};
    CHECK(assess(2, 1, true, no_dynamic_count).refusal == Refusal::multiple_viewports_not_supported);
    CHECK(assess(1, 2, true, no_dynamic_count).refusal == Refusal::multiple_scissors_not_supported);

    const ReplayCapability limited{true, true, 2};
    const Assessment viewport_too_many = assess(3, 1, true, limited);
    CHECK(viewport_too_many.refusal == Refusal::viewport_count_exceeds_vulkan_limit);
    CHECK(std::string(refusal_name(viewport_too_many.refusal)) ==
          "viewport_count_exceeds_vulkan_max_viewport_count");
    const Assessment scissor_too_many = assess(1, 3, true, limited);
    CHECK(scissor_too_many.refusal == Refusal::scissor_count_exceeds_vulkan_limit);
    CHECK(std::string(refusal_name(scissor_too_many.refusal)) ==
          "scissor_count_exceeds_vulkan_max_viewport_count");
    return 0;
}

int viewport_count_limit_case() {
    using namespace palmetto::viewport_replay_readiness;
    const Assessment too_many = assess(17, 1, true);
    CHECK(too_many.disposition == Disposition::refuse);
    CHECK(too_many.refusal == Refusal::viewport_count_exceeds_limit);
    CHECK(std::string(refusal_name(too_many.refusal)) == "viewport_count_exceeds_d3d12_limit");
    CHECK(assess(17, 17, true).refusal == Refusal::viewport_count_exceeds_limit);
    return 0;
}

int scissor_count_limit_case() {
    using namespace palmetto::viewport_replay_readiness;
    const Assessment too_many = assess(1, 17, true);
    CHECK(too_many.disposition == Disposition::refuse);
    CHECK(too_many.refusal == Refusal::scissor_count_exceeds_limit);
    CHECK(std::string(refusal_name(too_many.refusal)) == "scissor_count_exceeds_d3d12_limit");
    CHECK(assess(2, 17, true).refusal == Refusal::scissor_count_exceeds_limit);
    return 0;
}

int capture_all_array_elements_case() {
    using namespace palmetto::viewport_replay_readiness;
    Arrays arrays;
    palmetto::Viewport viewports[] = {
        {1.0f, 2.0f, 10.0f, 20.0f, 0.0f, 1.0f},
        {30.0f, 40.0f, 50.0f, 60.0f, 0.25f, 0.75f},
        {70.0f, 80.0f, 90.0f, 100.0f, 0.1f, 0.9f},
    };
    palmetto::Rect scissors[] = {{1, 2, 11, 22}, {31, 42, 51, 62}};
    CHECK(arrays.capture_viewports(3, viewports));
    CHECK(arrays.viewport_count == 3 && arrays.scissor_count == 0);
    CHECK(arrays.viewports[0].x == 1.0f && arrays.viewports[1].x == 30.0f && arrays.viewports[2].x == 70.0f);
    viewports[1].x = -1.0f;
    CHECK(arrays.viewports[1].x == 30.0f);  // the caller may change its input after RSSetViewports returns
    viewports[1].x = 30.0f;
    CHECK(arrays.viewports[2].min_depth == 0.1f && arrays.viewports[2].max_depth == 0.9f);
    CHECK(arrays.capture_scissors(2, scissors));
    CHECK(arrays.viewport_count == 3 && arrays.scissor_count == 2);
    CHECK(arrays.scissors[0].left == 1 && arrays.scissors[1].right == 51);
    scissors[1].left = -1;
    CHECK(arrays.scissors[1].left == 31);  // the caller may change its input after RSSetScissorRects returns
    scissors[1].left = 31;
    CHECK(arrays.capture_viewports(1, viewports + 1));
    CHECK(arrays.viewport_count == 1 && arrays.viewports[0].x == 30.0f);
    CHECK(arrays.viewports[1].x == 0.0f);  // elements past the new count cannot become stale replay state
    CHECK(arrays.scissor_count == 2 && arrays.scissors[1].top == 42);

    std::array<palmetto::Viewport, kD3D12ViewportScissorLimit> max_viewports{};
    std::array<palmetto::Rect, kD3D12ViewportScissorLimit> max_scissors{};
    for (std::uint32_t i = 0; i < kD3D12ViewportScissorLimit; ++i) {
        max_viewports[i] = {float(i), 2.0f, 3.0f, 4.0f, 0.0f, 1.0f};
        max_scissors[i] = {int(i), 2, int(i + 3), 4};
    }
    CHECK(arrays.capture_viewports(kD3D12ViewportScissorLimit, max_viewports.data()));
    CHECK(arrays.viewport_count == kD3D12ViewportScissorLimit && arrays.viewports[15].x == 15.0f);
    CHECK(arrays.capture_scissors(kD3D12ViewportScissorLimit, max_scissors.data()));
    CHECK(arrays.scissor_count == kD3D12ViewportScissorLimit && arrays.scissors[15].right == 18);

    CHECK(arrays.capture_scissors(0, nullptr));
    CHECK(arrays.scissor_count == 0 && arrays.scissors[0].left == 0);
    arrays.reset();
    CHECK(arrays.viewport_count == 0 && arrays.scissor_count == 0);
    return 0;
}

int capture_rejects_unreadable_or_over_limit_case() {
    using namespace palmetto::viewport_replay_readiness;
    Arrays arrays;
    const palmetto::Viewport one[] = {{1, 2, 3, 4, 0, 1}};
    const palmetto::Rect one_scissor[] = {{1, 2, 3, 4}};
    CHECK(arrays.capture_viewports(1, one));
    CHECK(arrays.capture_scissors(1, one_scissor));
    CHECK(!arrays.capture_viewports(17, nullptr));
    CHECK(!arrays.capture_scissors(17, nullptr));
    CHECK(!arrays.capture_viewports(1, nullptr));
    CHECK(!arrays.capture_scissors(1, nullptr));
    CHECK(arrays.viewport_count == 1 && arrays.viewports[0].x == 1.0f);
    CHECK(arrays.scissor_count == 1 && arrays.scissors[0].left == 1);
    return 0;
}

struct NamedCase {
    const char* name;
    int (*run)();
};

} // namespace

int main() {
    using namespace palmetto::viewport_replay_readiness;
    RestoreEnvironment restore(kStrictSwitch);
    const NamedCase cases[] = {
        {"strict switch requires exact one", exact_switch_case},
        {"switch-off preserves legacy behavior", switch_off_preserves_legacy_case},
        {"zero and single-element counts are ready", zero_and_single_element_counts_case},
        {"multiple viewports return a named refusal", multiple_viewports_case},
        {"multiple scissors return a named refusal", multiple_scissors_case},
        {"multi-count capability and maxViewports are enforced", multi_count_capability_case},
        {"viewport count over the API limit is refused", viewport_count_limit_case},
        {"scissor count over the API limit is refused", scissor_count_limit_case},
        {"all viewport and scissor array elements are copied", capture_all_array_elements_case},
        {"array capture rejects null and over-limit input atomically", capture_rejects_unreadable_or_over_limit_case},
    };
    for (const NamedCase& test : cases) {
        current_case = test.name;
        if (test.run() != 0) return 1;
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("viewport replay readiness: %zu named cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
