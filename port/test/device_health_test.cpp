// Pure, GPU-free tests for the bounded device-health record/parse seam. Synthetic counters only; no game data or Vulkan.
#include "../src/device_health.h"

#include <cstdio>
#include <string>
#include <string_view>

#define CHECK(condition) do {                                                               \
    if (!(condition)) {                                                                     \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                      \
        return 1;                                                                            \
    }                                                                                        \
    ++checks;                                                                                \
} while (0)

static int checks = 0;

int main() {
    using namespace palmetto;

    CHECK(!device_health_enabled_from(nullptr));
    CHECK(!device_health_enabled_from(""));
    CHECK(!device_health_enabled_from("0"));
    CHECK(!device_health_enabled_from("true"));
    CHECK(!device_health_enabled_from("01"));
    CHECK(!device_health_enabled_from(" 1"));
    CHECK(device_health_enabled_from("1"));

    DeviceHealthWorkloadFlags flags{};
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::empty);
    flags.has_command = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::other);
    flags.graphics = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::draw);
    flags.compute = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::mixed);
    flags = {};
    flags.indirect = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::indirect);
    flags = {};
    flags.ray_build = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::ray_build);
    flags = {};
    flags.transfer = true;
    CHECK(device_health_workload_from(flags) == DeviceHealthWorkload::copy);

    DeviceHealthBudgetSnapshot budget{};
    budget.arena_live_bytes = 8192;
    budget.arena_peak_bytes = 16384;
    budget.arena_capacity_bytes = 65536;
    budget.arena_allocations = 11;
    budget.image_count = 7;
    budget.image_bytes = 32768;
    budget.graphics_pipelines_live = 21;
    budget.graphics_pipelines_created = 35;
    budget.graphics_pipeline_cap = 128;
    budget.deferred_pipelines_built = 9;
    const auto record = make_device_health_record(1250, 1200, DeviceHealthWorkload::draw,
                                                   DeviceHealthWorkload::dispatch, budget);
    CHECK(record.lost_after_ms == 1250);
    CHECK(record.last_submit_ms == 1200);
    CHECK(record.submit_age_ms == 50);
    CHECK(record.last_submitted == DeviceHealthWorkload::draw);
    CHECK(record.attempted == DeviceHealthWorkload::dispatch);

    char line[kDeviceHealthLineCapacity]{};
    const std::size_t length = format_device_health_record(record, line, sizeof(line));
    CHECK(length > 0);
    CHECK(length < sizeof(line));
    DeviceHealthRecord parsed{};
    CHECK(parse_device_health_record(std::string_view(line, length), parsed));
    CHECK(parsed.lost_after_ms == record.lost_after_ms);
    CHECK(parsed.last_submit_ms == record.last_submit_ms);
    CHECK(parsed.submit_age_ms == record.submit_age_ms);
    CHECK(parsed.last_submitted == record.last_submitted);
    CHECK(parsed.attempted == record.attempted);
    CHECK(parsed.budget.arena_live_bytes == budget.arena_live_bytes);
    CHECK(parsed.budget.arena_peak_bytes == budget.arena_peak_bytes);
    CHECK(parsed.budget.arena_capacity_bytes == budget.arena_capacity_bytes);
    CHECK(parsed.budget.arena_allocations == budget.arena_allocations);
    CHECK(parsed.budget.image_count == budget.image_count);
    CHECK(parsed.budget.image_bytes == budget.image_bytes);
    CHECK(parsed.budget.graphics_pipelines_live == budget.graphics_pipelines_live);
    CHECK(parsed.budget.graphics_pipelines_created == budget.graphics_pipelines_created);
    CHECK(parsed.budget.graphics_pipeline_cap == budget.graphics_pipeline_cap);
    CHECK(parsed.budget.deferred_pipelines_built == budget.deferred_pipelines_built);

    DeviceHealthBudgetSnapshot partial{};
    partial.arena_live_bytes = 0;
    const auto no_submit = make_device_health_record(7, kDeviceHealthUnknown,
                                                      DeviceHealthWorkload::unknown,
                                                      DeviceHealthWorkload::unknown, partial);
    CHECK(no_submit.submit_age_ms == kDeviceHealthUnknown);
    CHECK(format_device_health_record(no_submit, line, sizeof(line)) > 0);
    CHECK(std::string_view(line).find("last_submit_ms=n/a") != std::string_view::npos);
    CHECK(std::string_view(line).find("image_count=n/a") != std::string_view::npos);
    CHECK(parse_device_health_record(std::string_view(line), parsed));
    CHECK(parsed.last_submit_ms == kDeviceHealthUnknown);
    CHECK(parsed.submit_age_ms == kDeviceHealthUnknown);
    CHECK(parsed.budget.arena_live_bytes == 0);
    CHECK(parsed.budget.image_count == kDeviceHealthUnknown);
    const std::string with_newline = std::string(line) + "\r\n";
    CHECK(parse_device_health_record(with_newline, parsed));

    const std::string canonical(line);
    auto invalid = canonical;
    invalid.replace(invalid.find("last_workload=unknown"), std::string("last_workload=unknown").size(),
                    "last_workload=bogus");
    CHECK(!parse_device_health_record(invalid, parsed));
    invalid = canonical + " extra=1";
    CHECK(!parse_device_health_record(invalid, parsed));
    invalid = canonical.substr(0, canonical.size() - 4);
    CHECK(!parse_device_health_record(invalid, parsed));
    invalid = canonical;
    invalid.replace(invalid.find("lost_after_ms=7"), std::string("lost_after_ms=7").size(),
                    "lost_after_ms=18446744073709551616");
    CHECK(!parse_device_health_record(invalid, parsed));
    invalid = canonical;
    invalid.replace(invalid.find("lost_after_ms=7"), std::string("lost_after_ms=7").size(), "lost_after_ms=n/a");
    CHECK(!parse_device_health_record(invalid, parsed));
    invalid = canonical;
    invalid.replace(invalid.find("submit_age_ms=n/a"), std::string("submit_age_ms=n/a").size(), "submit_age_ms=0");
    CHECK(!parse_device_health_record(invalid, parsed));
    CHECK(format_device_health_record(record, line, 1) == 0);

    std::printf("device_health_test: %d checks passed\n", checks);
    return 0;
}
