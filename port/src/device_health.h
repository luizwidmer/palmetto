#pragma once

// Optional, bounded device-loss attribution. This header has no Vulkan/Windows dependency so the record and parser can
// be tested without a device. It does not query driver state; the caller supplies cached counters only.
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <system_error>

namespace palmetto {

inline constexpr std::uint64_t kDeviceHealthUnknown = UINT64_MAX;
inline constexpr std::size_t kDeviceHealthLineCapacity = 640;

enum class DeviceHealthWorkload : std::uint8_t {
    unknown,
    empty,
    draw,
    dispatch,
    indirect,
    ray_build,
    copy,
    mixed,
    other,
};

constexpr std::string_view device_health_workload_name(DeviceHealthWorkload workload) noexcept {
    switch (workload) {
    case DeviceHealthWorkload::unknown: return "unknown";
    case DeviceHealthWorkload::empty: return "empty";
    case DeviceHealthWorkload::draw: return "draw";
    case DeviceHealthWorkload::dispatch: return "dispatch";
    case DeviceHealthWorkload::indirect: return "indirect";
    case DeviceHealthWorkload::ray_build: return "ray-build";
    case DeviceHealthWorkload::copy: return "copy";
    case DeviceHealthWorkload::mixed: return "mixed";
    case DeviceHealthWorkload::other: return "other";
    }
    return "unknown";
}

inline bool device_health_workload_from_name(std::string_view name, DeviceHealthWorkload& workload) noexcept {
    for (const auto candidate : {DeviceHealthWorkload::unknown, DeviceHealthWorkload::empty,
                                 DeviceHealthWorkload::draw, DeviceHealthWorkload::dispatch,
                                 DeviceHealthWorkload::indirect, DeviceHealthWorkload::ray_build,
                                 DeviceHealthWorkload::copy, DeviceHealthWorkload::mixed,
                                 DeviceHealthWorkload::other}) {
        if (device_health_workload_name(candidate) == name) {
            workload = candidate;
            return true;
        }
    }
    return false;
}

// Classification inputs are coarse command categories, not a record of individual command details.
struct DeviceHealthWorkloadFlags {
    bool has_command = false;
    bool graphics = false;
    bool compute = false;
    bool indirect = false;
    bool ray_build = false;
    bool transfer = false;
};

constexpr DeviceHealthWorkload device_health_workload_from(const DeviceHealthWorkloadFlags& flags) noexcept {
    const unsigned categories = unsigned(flags.graphics) + unsigned(flags.compute) + unsigned(flags.indirect) +
                                unsigned(flags.ray_build) + unsigned(flags.transfer);
    if (categories > 1) return DeviceHealthWorkload::mixed;
    if (flags.graphics) return DeviceHealthWorkload::draw;
    if (flags.compute) return DeviceHealthWorkload::dispatch;
    if (flags.indirect) return DeviceHealthWorkload::indirect;
    if (flags.ray_build) return DeviceHealthWorkload::ray_build;
    if (flags.transfer) return DeviceHealthWorkload::copy;
    return flags.has_command ? DeviceHealthWorkload::other : DeviceHealthWorkload::empty;
}

// Opt-in is intentionally exact: no whitespace, aliases, or truthy spellings silently enable diagnostics.
constexpr bool device_health_enabled_from(const char* value) noexcept {
    return value && value[0] == '1' && value[1] == '\0';
}

struct DeviceHealthBudgetSnapshot {
    std::uint64_t arena_live_bytes = kDeviceHealthUnknown;
    std::uint64_t arena_peak_bytes = kDeviceHealthUnknown;
    std::uint64_t arena_capacity_bytes = kDeviceHealthUnknown;
    std::uint64_t arena_allocations = kDeviceHealthUnknown;
    std::uint64_t image_count = kDeviceHealthUnknown;
    std::uint64_t image_bytes = kDeviceHealthUnknown;
    std::uint64_t graphics_pipelines_live = kDeviceHealthUnknown;
    std::uint64_t graphics_pipelines_created = kDeviceHealthUnknown;
    std::uint64_t graphics_pipeline_cap = kDeviceHealthUnknown;
    std::uint64_t deferred_pipelines_built = kDeviceHealthUnknown;
};

struct DeviceHealthRecord {
    std::uint64_t lost_after_ms = 0;
    std::uint64_t last_submit_ms = kDeviceHealthUnknown;
    std::uint64_t submit_age_ms = kDeviceHealthUnknown;
    DeviceHealthWorkload last_submitted = DeviceHealthWorkload::unknown;
    // The batch associated with a failed submit when loss was observed there. It is an attempt, not proof that the
    // driver accepted or executed it.
    DeviceHealthWorkload attempted = DeviceHealthWorkload::unknown;
    DeviceHealthBudgetSnapshot budget{};
};

constexpr DeviceHealthRecord make_device_health_record(std::uint64_t lost_after_ms,
                                                        std::uint64_t last_submit_ms,
                                                        DeviceHealthWorkload last_submitted,
                                                        DeviceHealthWorkload attempted,
                                                        const DeviceHealthBudgetSnapshot& budget) noexcept {
    DeviceHealthRecord record;
    record.lost_after_ms = lost_after_ms;
    record.last_submit_ms = last_submit_ms;
    record.submit_age_ms = last_submit_ms == kDeviceHealthUnknown
                               ? kDeviceHealthUnknown
                               : (lost_after_ms >= last_submit_ms ? lost_after_ms - last_submit_ms : 0);
    record.last_submitted = last_submitted;
    record.attempted = attempted;
    record.budget = budget;
    return record;
}

namespace device_health_detail {

inline const char* number_text(std::uint64_t value, char* buffer, std::size_t capacity) noexcept {
    if (value == kDeviceHealthUnknown) return "n/a";
    const int written = std::snprintf(buffer, capacity, "%llu", static_cast<unsigned long long>(value));
    return written > 0 && static_cast<std::size_t>(written) < capacity ? buffer : "n/a";
}

inline bool take_token(std::string_view& input, std::string_view key, std::string_view& token,
                       bool final) noexcept {
    if (!input.starts_with(key)) return false;
    input.remove_prefix(key.size());
    const std::size_t separator = input.find(' ');
    if (final) {
        if (separator != std::string_view::npos) return false;
        token = input;
        input = {};
        return !token.empty();
    }
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= input.size()) return false;
    token = input.substr(0, separator);
    input.remove_prefix(separator + 1);
    return !token.empty();
}

inline bool parse_optional_number(std::string_view token, std::uint64_t& value) noexcept {
    if (token == "n/a") {
        value = kDeviceHealthUnknown;
        return true;
    }
    if (token.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

inline bool take_number(std::string_view& input, std::string_view key, std::uint64_t& value,
                        bool final = false) noexcept {
    std::string_view token;
    return take_token(input, key, token, final) && parse_optional_number(token, value);
}

inline bool take_workload(std::string_view& input, std::string_view key, DeviceHealthWorkload& value,
                          bool final = false) noexcept {
    std::string_view token;
    return take_token(input, key, token, final) && device_health_workload_from_name(token, value);
}

}  // namespace device_health_detail

// The line is capped and consists only of monotonic times, workload labels and coarse counters. The caller appends the
// newline. Returns 0 on invalid output storage or truncation.
inline std::size_t format_device_health_record(const DeviceHealthRecord& record, char* output,
                                               std::size_t capacity) noexcept {
    if (!output || !capacity) return 0;
    char number_buffers[13][24]{};
    const auto number = [&number_buffers](std::size_t index, std::uint64_t value) {
        return device_health_detail::number_text(value, number_buffers[index], sizeof(number_buffers[index]));
    };
    const auto last = device_health_workload_name(record.last_submitted);
    const auto attempted = device_health_workload_name(record.attempted);
    const auto& b = record.budget;
    const int written = std::snprintf(
        output, capacity,
        "[rt] d3d12: device-health v1 lost_after_ms=%s last_submit_ms=%s submit_age_ms=%s last_workload=%.*s "
        "attempted=%.*s arena_live_bytes=%s arena_peak_bytes=%s arena_capacity_bytes=%s arena_allocations=%s "
        "image_count=%s image_bytes=%s graphics_pipelines_live=%s graphics_pipelines_created=%s "
        "graphics_pipeline_cap=%s deferred_built=%s",
        number(0, record.lost_after_ms), number(1, record.last_submit_ms), number(2, record.submit_age_ms),
        static_cast<int>(last.size()), last.data(), static_cast<int>(attempted.size()), attempted.data(),
        number(3, b.arena_live_bytes), number(4, b.arena_peak_bytes), number(5, b.arena_capacity_bytes),
        number(6, b.arena_allocations), number(7, b.image_count), number(8, b.image_bytes),
        number(9, b.graphics_pipelines_live), number(10, b.graphics_pipelines_created),
        number(11, b.graphics_pipeline_cap),
        number(12, b.deferred_pipelines_built));
    return written > 0 && static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written) : 0;
}

// Pure parser for the canonical line above. Rejects unknown fields, extra tokens, invalid numbers and unknown workload
// labels so a saved log line can be consumed offline without interpreting driver or game data.
inline bool parse_device_health_record(std::string_view line, DeviceHealthRecord& output) noexcept {
    constexpr std::string_view prefix = "[rt] d3d12: device-health v1 ";
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (!line.starts_with(prefix)) return false;
    line.remove_prefix(prefix.size());
    DeviceHealthRecord record{};
    using namespace device_health_detail;
    if (!take_number(line, "lost_after_ms=", record.lost_after_ms) ||
        !take_number(line, "last_submit_ms=", record.last_submit_ms) ||
        !take_number(line, "submit_age_ms=", record.submit_age_ms) ||
        !take_workload(line, "last_workload=", record.last_submitted) ||
        !take_workload(line, "attempted=", record.attempted) ||
        !take_number(line, "arena_live_bytes=", record.budget.arena_live_bytes) ||
        !take_number(line, "arena_peak_bytes=", record.budget.arena_peak_bytes) ||
        !take_number(line, "arena_capacity_bytes=", record.budget.arena_capacity_bytes) ||
        !take_number(line, "arena_allocations=", record.budget.arena_allocations) ||
        !take_number(line, "image_count=", record.budget.image_count) ||
        !take_number(line, "image_bytes=", record.budget.image_bytes) ||
        !take_number(line, "graphics_pipelines_live=", record.budget.graphics_pipelines_live) ||
        !take_number(line, "graphics_pipelines_created=", record.budget.graphics_pipelines_created) ||
        !take_number(line, "graphics_pipeline_cap=", record.budget.graphics_pipeline_cap) ||
        !take_number(line, "deferred_built=", record.budget.deferred_pipelines_built, true) || !line.empty())
        return false;
    if (record.lost_after_ms == kDeviceHealthUnknown) return false;
    if (record.last_submit_ms == kDeviceHealthUnknown) {
        if (record.submit_age_ms != kDeviceHealthUnknown || record.last_submitted != DeviceHealthWorkload::unknown)
            return false;
    } else {
        if (record.submit_age_ms == kDeviceHealthUnknown || record.last_submitted == DeviceHealthWorkload::unknown)
            return false;
        const std::uint64_t expected_age = record.lost_after_ms >= record.last_submit_ms
                                               ? record.lost_after_ms - record.last_submit_ms
                                               : 0;
        if (record.submit_age_ms != expected_age) return false;
    }
    output = record;
    return true;
}

// One device, one record. Enabled only by the exact opt-in switch; successful submissions update a cheap cached label and
// monotonic time, while the loss path formats one bounded line. No Vulkan entry points, waits, driver queries or heap
// walks are called from this helper.
class DeviceHealth {
public:
    using Clock = std::chrono::steady_clock;

    explicit DeviceHealth(bool enabled = false) noexcept
        : enabled_(enabled), started_(enabled ? Clock::now() : Clock::time_point{}) {}
    DeviceHealth(const DeviceHealth&) = delete;
    DeviceHealth& operator=(const DeviceHealth&) = delete;

    bool enabled() const noexcept { return enabled_; }

    void note_submitted(DeviceHealthWorkload workload) {
        if (!enabled_ || workload == DeviceHealthWorkload::unknown) return;
        const std::uint64_t now_ms = elapsed_ms();
        std::lock_guard lock(mutex_);
        if (last_submit_ms_ == kDeviceHealthUnknown || now_ms >= last_submit_ms_) {
            last_submit_ms_ = now_ms;
            last_submitted_ = workload;
        }
    }

    bool report_device_lost(const DeviceHealthBudgetSnapshot& budget,
                            DeviceHealthWorkload attempted = DeviceHealthWorkload::unknown) {
        if (!enabled_ || reported_.test_and_set(std::memory_order_acq_rel)) return false;
        const std::uint64_t lost_ms = elapsed_ms();
        std::uint64_t last_submit_ms = kDeviceHealthUnknown;
        DeviceHealthWorkload last_submitted = DeviceHealthWorkload::unknown;
        {
            std::lock_guard lock(mutex_);
            last_submit_ms = last_submit_ms_;
            last_submitted = last_submitted_;
        }
        const DeviceHealthRecord record = make_device_health_record(
            lost_ms, last_submit_ms, last_submitted, attempted, budget);
        std::array<char, kDeviceHealthLineCapacity> line{};
        const std::size_t length = format_device_health_record(record, line.data(), line.size());
        if (!length) return false;
        std::fprintf(stderr, "%s\n", line.data());
        return true;
    }

private:
    std::uint64_t elapsed_ms() const noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count();
        return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    }

    const bool enabled_;
    const Clock::time_point started_;
    std::mutex mutex_;
    std::uint64_t last_submit_ms_ = kDeviceHealthUnknown;
    DeviceHealthWorkload last_submitted_ = DeviceHealthWorkload::unknown;
    std::atomic_flag reported_ = ATOMIC_FLAG_INIT;
};

}  // namespace palmetto
