#include "../src/query_heap_readiness.h"

#include <cstdio>
#include <string>

namespace {
using namespace palmetto::query_heap_readiness;
const char* current_case = "startup";
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", current_case, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

bool set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    return ::_putenv_s(name, value ? value : "") == 0;
#else
    return value ? ::setenv(name, value, 1) == 0 : ::unsetenv(name) == 0;
#endif
}

struct RestoreEnvironment {
    explicit RestoreEnvironment(const char* key) : name(key) {
#if defined(_WIN32)
        char* previous = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&previous, &length, name.c_str()) == 0 && previous) {
            existed = true;
            value = previous;
        }
        std::free(previous);
#else
        if (const char* previous = std::getenv(name.c_str())) {
            existed = true;
            value = previous;
        }
#endif
    }
    ~RestoreEnvironment() { set_environment(name.c_str(), existed ? value.c_str() : nullptr); }
    std::string name;
    std::string value;
    bool existed = false;
};

int environment_switch_case() {
    current_case = "environment switch";
    RestoreEnvironment restore(kSwitch);
    CHECK(set_environment(kSwitch, nullptr));
    CHECK(!enabled());
    CHECK(set_environment(kSwitch, "1"));
    CHECK(enabled());
    CHECK(set_environment(kSwitch, "true"));
    CHECK(!enabled());
    return 0;
}

int exact_switch_case() {
    current_case = "exact switch";
    CHECK(!switch_value_enabled(nullptr));
    CHECK(!switch_value_enabled(""));
    CHECK(!switch_value_enabled("0"));
    CHECK(!switch_value_enabled("true"));
    CHECK(!switch_value_enabled("01"));
    CHECK(!switch_value_enabled(" 1"));
    CHECK(!switch_value_enabled("1 "));
    CHECK(switch_value_enabled("1"));
    return 0;
}

int default_path_is_unchanged_case() {
    current_case = "default path unchanged";
    const Descriptor unknown_invalid{99, 0, 9};
    const auto result = assess(unknown_invalid, false, {});
    CHECK(result.disposition == Disposition::legacy_unchanged);
    CHECK(result.reason == Reason::none);
    CHECK(result.type == Type::outside_host_range);
    return 0;
}

int descriptor_validation_case() {
    current_case = "descriptor validation";
    const auto zero_count = assess({0, 0, 0}, true, {});
    CHECK(zero_count.disposition == Disposition::invalid_argument);
    CHECK(zero_count.reason == Reason::zero_query_count);
    CHECK(std::string(reason_name(zero_count.reason)) == "query_heap_count_zero");
    const auto bad_node = assess({0, 1, 2}, true, {});
    CHECK(bad_node.disposition == Disposition::invalid_argument);
    CHECK(bad_node.reason == Reason::node_mask_out_of_range);
    const auto bad_type = assess({8, 1, 0}, true, {});
    CHECK(bad_type.disposition == Disposition::invalid_argument);
    CHECK(bad_type.reason == Reason::type_outside_host_range);
    CHECK(std::string(type_name(classify(8))) == "outside_host_range");
    return 0;
}

int supported_types_case() {
    current_case = "supported types";
    const Capabilities full{true, true, true, false, true};
    const auto occlusion = assess({0, 1, 0}, true, full);
    CHECK(occlusion.disposition == Disposition::ready);
    const auto timestamp = assess({1, 8, 0}, true, full);
    CHECK(timestamp.disposition == Disposition::ready);
    const auto copy_timestamp = assess({5, 1, 1}, true, full);
    CHECK(copy_timestamp.disposition == Disposition::ready);
    const auto statistics = assess({2, 2, 0}, true, full);
    CHECK(statistics.disposition == Disposition::ready);
    CHECK(std::string(type_name(statistics.type)) == "pipeline_statistics");
    const auto so_zero = assess({3, 1, 0}, true, full);
    CHECK(so_zero.disposition == Disposition::ready_zero);
    CHECK(so_zero.reason == Reason::none);
    return 0;
}

int capability_refusals_case() {
    current_case = "capability refusals";
    const auto missing_precise_occlusion = assess({0, 1, 0}, true, {true, true, true, false, false});
    CHECK(missing_precise_occlusion.disposition == Disposition::refuse);
    CHECK(missing_precise_occlusion.reason == Reason::occlusion_query_precise_unavailable);
    CHECK(std::string(reason_name(missing_precise_occlusion.reason)) == "occlusion_query_precise_not_enabled");
    const auto missing_period = assess({1, 1, 0}, true, {false, true, false, false, false});
    CHECK(missing_period.disposition == Disposition::refuse);
    CHECK(missing_period.reason == Reason::timestamp_capability_unavailable);
    const auto missing_bits = assess({5, 1, 0}, true, {true, false, false, false, false});
    CHECK(missing_bits.reason == Reason::timestamp_capability_unavailable);
    const auto missing_statistics = assess({2, 1, 0}, true, {true, true, false, false, false});
    CHECK(missing_statistics.reason == Reason::pipeline_statistics_feature_unavailable);
    CHECK(std::string(reason_name(missing_statistics.reason)) == "pipeline_statistics_query_not_enabled");
    const auto real_so_without_pool = assess({3, 1, 0}, true, {true, true, true, true, false});
    CHECK(real_so_without_pool.disposition == Disposition::refuse);
    CHECK(real_so_without_pool.reason == Reason::stream_output_pool_not_implemented);
    return 0;
}

int known_unsupported_and_unidentified_case() {
    current_case = "known unsupported and unidentified types";
    const Capabilities full{true, true, true, false, true};
    const auto video = assess({4, 1, 0}, true, full);
    CHECK(video.disposition == Disposition::refuse);
    CHECK(video.reason == Reason::video_decode_statistics_not_implemented);
    const auto unnamed = assess({6, 1, 0}, true, full);
    CHECK(unnamed.disposition == Disposition::refuse);
    CHECK(unnamed.reason == Reason::unnamed_heap_type_6);
    CHECK(std::string(type_name(unnamed.type)) == "unnamed_6");
    CHECK(std::string(reason_name(unnamed.reason)) == "query_heap_type_6_unidentified");
    const auto stats1 = assess({7, 1, 0}, true, full);
    CHECK(stats1.disposition == Disposition::refuse);
    CHECK(stats1.reason == Reason::pipeline_statistics1_not_implemented);
    return 0;
}

} // namespace

int main() {
    if (environment_switch_case()) return 1;
    if (exact_switch_case()) return 1;
    if (default_path_is_unchanged_case()) return 1;
    if (descriptor_validation_case()) return 1;
    if (supported_types_case()) return 1;
    if (capability_refusals_case()) return 1;
    if (known_unsupported_and_unidentified_case()) return 1;
    std::puts("query_heap_readiness_test: OK (7 synthetic cases)");
    return 0;
}
