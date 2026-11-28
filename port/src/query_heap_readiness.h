#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// CPU-only contract for the query-heap types and capability gates measured in
// docs/d3d12_vulkan.md section 4.6. This is intentionally not wired to the COM
// shim: a future diagnostic/call-site may opt into the preflight without changing
// the default query-heap behavior in this slice.
namespace palmetto::query_heap_readiness {

inline constexpr char kSwitch[] = "PALMETTO_D3D12X_QUERY_HEAP_READINESS";

// Numeric values mirror D3D12_QUERY_HEAP_TYPE as used in command_list.h. Value 6
// has no SDK name in this tree but the host accepts it within the 0..7 envelope.
enum class Type : std::uint8_t {
    occlusion = 0,
    timestamp = 1,
    pipeline_statistics = 2,
    stream_output_statistics = 3,
    video_decode_statistics = 4,
    copy_queue_timestamp = 5,
    unnamed_6 = 6,
    pipeline_statistics1 = 7,
    outside_host_range = 0xff,
};

enum class Disposition : std::uint8_t {
    legacy_unchanged,  // exact switch is off; do not gate or reinterpret the call
    ready,             // known heap type and its required feature is available
    ready_zero,        // SO query resolves zero only while SO remains unavailable
    invalid_argument,  // matches the host's Count / NodeMask / enum-range validation
    refuse,            // recognized/unnamed type has no implemented result path
};

enum class Reason : std::uint8_t {
    none,
    zero_query_count,
    node_mask_out_of_range,
    type_outside_host_range,
    timestamp_capability_unavailable,
    occlusion_query_precise_unavailable,
    pipeline_statistics_feature_unavailable,
    stream_output_pool_not_implemented,
    video_decode_statistics_not_implemented,
    unnamed_heap_type_6,
    pipeline_statistics1_not_implemented,
};

struct Descriptor {
    std::uint32_t type = 0;  // raw D3D12_QUERY_HEAP_TYPE numeric value
    std::uint32_t count = 0;
    std::uint32_t node_mask = 0;
};

// The timestamp bit is the queue family's timestampValidBits; period is the
// physical device's timestampPeriod. Pipeline statistics use the enabled logical
// device feature. stream_output_available must remain false until real SO output
// and its Vulkan transform-feedback query pool are implemented.
struct Capabilities {
    bool timestamp_period = false;
    bool timestamp_valid_bits = false;
    bool pipeline_statistics_query = false;
    bool stream_output_available = false;
    bool occlusion_query_precise = false;
};

struct Assessment {
    Disposition disposition = Disposition::legacy_unchanged;
    Reason reason = Reason::none;
    Type type = Type::outside_host_range;
};

constexpr bool switch_value_enabled(const char* value) noexcept {
    return value && value[0] == '1' && value[1] == '\0';
}

inline bool enabled() noexcept {
#if defined(_WIN32)
    char value[2]{};  // exact opt-in "1" plus its NUL, with no truncation accepted
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), kSwitch) != 0 || length <= 1) return false;
    return switch_value_enabled(value);
#else
    return switch_value_enabled(std::getenv(kSwitch));
#endif
}

constexpr Type classify(std::uint32_t value) noexcept {
    return value <= 7 ? static_cast<Type>(value) : Type::outside_host_range;
}

// This assessment deliberately runs only when the caller passes the exact opt-in.
// It is a bounded readiness contract, not a substitute for a runtime query test:
// it does not allocate a Vulkan pool, issue a command, or inspect result bytes.
constexpr Assessment assess(Descriptor desc, bool opt_in, Capabilities caps) noexcept {
    if (!opt_in) return {Disposition::legacy_unchanged, Reason::none, classify(desc.type)};
    if (!desc.count) return {Disposition::invalid_argument, Reason::zero_query_count, classify(desc.type)};
    if (desc.node_mask > 1)
        return {Disposition::invalid_argument, Reason::node_mask_out_of_range, classify(desc.type)};
    if (desc.type > 7)
        return {Disposition::invalid_argument, Reason::type_outside_host_range, Type::outside_host_range};

    const Type type = classify(desc.type);
    switch (type) {
    case Type::occlusion:
        if (!caps.occlusion_query_precise)
            return {Disposition::refuse, Reason::occlusion_query_precise_unavailable, type};
        return {Disposition::ready, Reason::none, type};
    case Type::timestamp:
    case Type::copy_queue_timestamp:
        if (!caps.timestamp_period || !caps.timestamp_valid_bits)
            return {Disposition::refuse, Reason::timestamp_capability_unavailable, type};
        return {Disposition::ready, Reason::none, type};
    case Type::pipeline_statistics:
        if (!caps.pipeline_statistics_query)
            return {Disposition::refuse, Reason::pipeline_statistics_feature_unavailable, type};
        return {Disposition::ready, Reason::none, type};
    case Type::stream_output_statistics:
        if (caps.stream_output_available)
            return {Disposition::refuse, Reason::stream_output_pool_not_implemented, type};
        return {Disposition::ready_zero, Reason::none, type};
    case Type::video_decode_statistics:
        return {Disposition::refuse, Reason::video_decode_statistics_not_implemented, type};
    case Type::unnamed_6:
        return {Disposition::refuse, Reason::unnamed_heap_type_6, type};
    case Type::pipeline_statistics1:
        return {Disposition::refuse, Reason::pipeline_statistics1_not_implemented, type};
    case Type::outside_host_range:
        return {Disposition::invalid_argument, Reason::type_outside_host_range, type};
    }
    return {Disposition::refuse, Reason::type_outside_host_range, Type::outside_host_range};
}

constexpr const char* type_name(Type type) noexcept {
    switch (type) {
    case Type::occlusion: return "occlusion";
    case Type::timestamp: return "timestamp";
    case Type::pipeline_statistics: return "pipeline_statistics";
    case Type::stream_output_statistics: return "stream_output_statistics";
    case Type::video_decode_statistics: return "video_decode_statistics";
    case Type::copy_queue_timestamp: return "copy_queue_timestamp";
    case Type::unnamed_6: return "unnamed_6";
    case Type::pipeline_statistics1: return "pipeline_statistics1";
    case Type::outside_host_range: return "outside_host_range";
    }
    return "unknown_query_heap_type";
}

constexpr const char* reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::none: return "none";
    case Reason::zero_query_count: return "query_heap_count_zero";
    case Reason::node_mask_out_of_range: return "query_heap_node_mask_out_of_range";
    case Reason::type_outside_host_range: return "query_heap_type_outside_host_range";
    case Reason::timestamp_capability_unavailable: return "query_heap_timestamp_capability_unavailable";
    case Reason::occlusion_query_precise_unavailable: return "occlusion_query_precise_not_enabled";
    case Reason::pipeline_statistics_feature_unavailable: return "pipeline_statistics_query_not_enabled";
    case Reason::stream_output_pool_not_implemented: return "stream_output_query_pool_not_implemented";
    case Reason::video_decode_statistics_not_implemented: return "video_decode_statistics_not_implemented";
    case Reason::unnamed_heap_type_6: return "query_heap_type_6_unidentified";
    case Reason::pipeline_statistics1_not_implemented: return "pipeline_statistics1_not_implemented";
    }
    return "unknown_query_heap_readiness_reason";
}

} // namespace palmetto::query_heap_readiness
