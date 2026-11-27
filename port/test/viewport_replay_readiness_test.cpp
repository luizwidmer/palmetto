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
        {"viewport count over the API limit is refused", viewport_count_limit_case},
        {"scissor count over the API limit is refused", scissor_count_limit_case},
    };
    for (const NamedCase& test : cases) {
        current_case = test.name;
        if (test.run() != 0) return 1;
        std::printf("PASS: %s\n", test.name);
    }
    std::printf("viewport replay readiness: %zu named cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
