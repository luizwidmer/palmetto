// The builder-shaped XDXR route: the prebuild and build answers of the package's BVH builder, expressed as the layer's
// own XDXR build records. The package's XboxBvhBuilder.dll exports GetRaytracingAccelerationStructurePrebuildInfo,
// BuildRaytracingAccelerationStructure and its Batched2X form (docs/xdxr.md section 1, port/src/xdxr_descs.h); a
// runtime that replaces that helper answers the same shapes from its own measured builder (docs/bvh_route.md).
//
// The numbers are the layer's own: XdxrLayout (port/src/xdxr_layout.h), the layout BuildRaytracingAccelerationStructure
// writes from port/src/xdxr_build.h and GetRaytracingAccelerationStructurePrebuildInfo already answers (docs/bvh.md
// "Building runtime BVHs"). This header is pure arithmetic: no Vulkan, no D3D12, no GPU, so the shapes are testable
// offline (port/test/xdxr_descs_test.cpp).
//
// The switch. The route is off by default: PALMETTO_BVH_ROUTE=own arms it, and while it is off every answer is the
// named refusal `disabled` (a caller keeps the package helper's answer). It is not wired into a caller yet; see
// docs/bvh_route.md "What is not wired, and why".
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "xdxr_descs.h"
#include "xdxr_layout.h"

namespace palmetto::xdxr::route {

// Public D3D12 enum values the console descriptions carry (measured in docs/xdxr.md sections 1 and 3).
inline constexpr std::uint32_t kTopLevel = 0;
inline constexpr std::uint32_t kBottomLevel = 1;
inline constexpr std::uint32_t kLayoutArray = 0;
inline constexpr std::uint32_t kLayoutArrayOfPointers = 1;
inline constexpr std::uint64_t kMaxGuardedAddress = std::uint64_t(1) << 40;  // the builder's measured 40-bit rule
inline constexpr std::uint32_t kDestinationAlignment = 256;  // D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT

// D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS: the public bits; the DLL reads 0 (allow update), 2 (prefer fast
// trace), 4 (minimize memory) and 5 (perform update) of them (docs/xdxr.md section 1).
inline constexpr std::uint32_t kBuildAllowUpdate = 0x01;
inline constexpr std::uint32_t kBuildAllowCompaction = 0x02;
inline constexpr std::uint32_t kBuildPreferFastTrace = 0x04;
inline constexpr std::uint32_t kBuildPreferFastBuild = 0x08;
inline constexpr std::uint32_t kBuildMinimizeMemory = 0x10;
inline constexpr std::uint32_t kBuildPerformUpdate = 0x20;
inline constexpr std::uint32_t kBuildFlagsKnown = 0x3f;

// Public D3D12_RAYTRACING_GEOMETRY_TYPE and D3D12_RAYTRACING_GEOMETRY_FLAGS values.
inline constexpr std::uint32_t kGeometryTriangles = 0;
inline constexpr std::uint32_t kGeometryFlagOpaque = 0x01;

// DXGI_FORMAT values the route names; the GPU builder reads f32 positions only (port/src/vulkan_backend.h
// xdxr_inputs refuses any other vertex format, and the traversal's vertex reads are f32).
inline constexpr std::uint32_t kFormatUnknown = 0;
inline constexpr std::uint32_t kFormatR32G32B32Float = 6;
inline constexpr std::uint32_t kFormatR32G32Float = 16;
inline constexpr std::uint32_t kFormatR16Uint = 57;
inline constexpr std::uint32_t kFormatR32Uint = 42;

// The CPU-side mirror of D3D12_RAYTRACING_GEOMETRY_DESC (56 bytes), the array Inputs.pGeometryDescs points at. The
// console build description's Inputs is the public struct (docs/xdxr.md section 1), so its geometry descs are too.
struct GeometryDesc {
    std::uint32_t type = kGeometryTriangles;
    std::uint32_t flags = 0;
    std::uint64_t vertex_va = 0;      // D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE.StartAddress
    std::uint64_t vertex_stride = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t vertex_format = kFormatUnknown;
    std::uint64_t index_va = 0;
    std::uint32_t index_count = 0;
    std::uint32_t index_format = kFormatUnknown;
    std::uint64_t transform_va = 0;
};
static_assert(sizeof(GeometryDesc) == 56, "the public geometry description is 56 bytes");

// D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC's first 0x30 bytes: the non-batched export's shape (the shipped
// export copies these into a BATCHED_DESC2 on the stack, docs/xdxr.md section 1).
struct BuildDesc {
    std::uint64_t dest_acceleration_structure_data = 0;
    BuildInputs inputs;
    std::uint64_t source_acceleration_structure_data = 0;
    std::uint64_t scratch_acceleration_structure_data = 0;
};
static_assert(sizeof(BuildDesc) == 0x30, "the public build description prefix is 48 bytes");

// What the build route hands a builder: the fields the layer's BuildAccelerationStructure record takes, with the
// console VAs the title named (the layer adds its arena base when it records).
struct TriangleGeometry {
    std::uint64_t vertex_va = 0;
    std::uint64_t index_va = 0;
    std::uint64_t transform_va = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_format = kFormatUnknown;
    std::uint32_t index_count = 0;
    std::uint32_t index_format = kFormatUnknown;
    std::uint32_t flags = 0;
};

struct BuildRecord {
    std::uint64_t dest_va = 0;
    std::uint64_t scratch_va = 0;
    std::uint64_t source_va = 0;
    bool top_level = false;
    bool update = false;          // PERFORM_UPDATE: refit the source
    bool pointer_layout = false;  // a top-level build's instance list is ARRAY_OF_POINTERS
    std::uint32_t flags = 0;      // the console build flags, unchanged
    std::vector<TriangleGeometry> triangles;  // bottom level
    std::uint64_t instances_va = 0;           // top level
    std::uint32_t instance_count = 0;
    std::vector<PostbuildInfoDesc> postbuilds;  // the types the layer writes (0 compacted, 3 current)
};

// A build refusal names itself; the text is the one the caller logs.
enum class Refusal {
    none,
    disabled,               // the switch is off
    null_desc,
    input_type,             // Inputs.Type is neither 0 nor 1
    descs_layout,           // Inputs.DescsLayout is neither 0 nor 1
    unknown_flag,           // a build flag bit outside the public 0x3f
    geometry_type,          // a geometry description that is not D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES
    vertex_format,          // a vertex format the GPU builder cannot read
    index_format,           // an index format the GPU builder cannot read
    triangle_count,         // a count that is not a whole number of triangles
    geometry_pointer,       // a null geometry description
    counts,                 // an element count the layout cannot hold
    alignment,              // a destination that is not 256-byte aligned
    address,                // an address at or above 2^40 (the builder's measured rule)
    update_flags,           // PERFORM_UPDATE without ALLOW_UPDATE
    offline_statistics,     // postbuild type 0x10: the shipped builder throws E_INVALIDARG for it
    serialization,          // postbuild type 2: the layer refuses serialization at replay
    pointers_unreadable,    // a null entry of an ARRAY_OF_POINTERS geometry list
    too_many_elements,      // the batch is larger than the bounded plan
};

inline const char* refusal_name(Refusal refusal) {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::disabled: return "route disabled (PALMETTO_BVH_ROUTE is not own)";
    case Refusal::null_desc: return "null description";
    case Refusal::input_type: return "Inputs.Type is neither top level nor bottom level";
    case Refusal::descs_layout: return "Inputs.DescsLayout is neither ARRAY nor ARRAY_OF_POINTERS";
    case Refusal::unknown_flag: return "a build flag outside the public bits";
    case Refusal::geometry_type: return "a geometry that is not triangles";
    case Refusal::vertex_format: return "a vertex format the GPU builder cannot read";
    case Refusal::index_format: return "an index format the GPU builder cannot read";
    case Refusal::triangle_count: return "a count that is not a whole number of triangles";
    case Refusal::geometry_pointer: return "a null geometry description";
    case Refusal::counts: return "an element count the layout cannot hold";
    case Refusal::alignment: return "a destination that is not 256-byte aligned";
    case Refusal::address: return "an address at or above 2^40";
    case Refusal::update_flags: return "PERFORM_UPDATE without ALLOW_UPDATE";
    case Refusal::offline_statistics: return "postbuild type 0x10 (offline statistics) is refused";
    case Refusal::serialization: return "postbuild type 2 (serialization) is refused";
    case Refusal::pointers_unreadable: return "a null entry in the geometry pointer list";
    case Refusal::too_many_elements: return "the batch is larger than the bounded plan";
    }
    return "unknown refusal";
}

// The switch, read once per call from the environment: "own" arms the route, anything else (or unset) leaves it off.
// A test sets the variable itself.
inline bool route_armed() {
    const char* value = std::getenv("PALMETTO_BVH_ROUTE");
    return value && std::string(value) == "own";
}

struct PrebuildAnswer {
    Refusal refusal = Refusal::none;
    std::uint64_t result_size = 0;        // the layer's XdxrLayout size
    std::uint64_t scratch_size = 0;       // the layer's XdxrLayout scratch
    std::uint64_t update_scratch_size = 0;  // same scratch: a refit needs no sort
    bool ok() const { return refusal == Refusal::none; }
};

// What the stream description carried, for the route's summary: which of the three measured records were found and
// whether type 0's 0x08 value selects the shipped builder's alternate bottom-level path, which this layout does not
// reproduce. The records change no size here (the layout depends on count and BOX16 only).
struct StreamNotes {
    bool record0 = false;
    bool record1 = false;
    bool record2 = false;
    bool alternate_bottom_level_path = false;  // MINIMIZE_MEMORY with type 0's +0x08 above 10.0
    std::uint8_t level = 0;                    // type 1's +0x0d (bottom level)
};

inline StreamNotes read_stream(const StreamDesc* stream, std::uint32_t flags) {
    StreamNotes notes;
    if (!stream || !stream->stream || !stream->size_in_bytes) return notes;
    if (const StreamRecord0* r = reinterpret_cast<const StreamRecord0*>(find_stream_record(*stream, 0))) {
        notes.record0 = true;
        const float value = r->value_08 < 0 ? default_record0_value_08(flags) : r->value_08;
        notes.alternate_bottom_level_path = (flags & kBuildMinimizeMemory) != 0 && value > 10.0f;
    }
    if (const StreamRecord1* r = reinterpret_cast<const StreamRecord1*>(find_stream_record(*stream, 1))) {
        notes.record1 = true;
        notes.level = r->level_0d;
    }
    notes.record2 = find_stream_record(*stream, 2) != nullptr;
    return notes;
}

// The element count XdxrLayout is computed from: triangles (BLAS: index count or vertex count, divided by three) or
// instances (TLAS). False with the named refusal when the descriptions cannot be read.
inline bool count_elements(const BuildInputs& inputs, std::uint32_t& elements, Refusal& refusal) {
    if (inputs.type != kTopLevel && inputs.type != kBottomLevel) {
        refusal = Refusal::input_type;
        return false;
    }
    if (inputs.descs_layout != kLayoutArray && inputs.descs_layout != kLayoutArrayOfPointers) {
        refusal = Refusal::descs_layout;
        return false;
    }
    if (inputs.flags & ~kBuildFlagsKnown) {
        refusal = Refusal::unknown_flag;
        return false;
    }
    if (inputs.type == kTopLevel) {
        elements = inputs.num_descs;
        return true;
    }
    std::uint64_t count = 0;
    for (std::uint32_t i = 0; i < inputs.num_descs; ++i) {
        const GeometryDesc* geometry =
            inputs.descs_layout == kLayoutArrayOfPointers
                ? (inputs.descs ? reinterpret_cast<const GeometryDesc* const*>(inputs.descs)[i] : nullptr)
                : (inputs.descs ? &reinterpret_cast<const GeometryDesc*>(inputs.descs)[i] : nullptr);
        if (!geometry) {
            refusal = inputs.descs_layout == kLayoutArrayOfPointers ? Refusal::pointers_unreadable
                                                                   : Refusal::geometry_pointer;
            return false;
        }
        if (geometry->type != kGeometryTriangles) {
            refusal = Refusal::geometry_type;
            return false;
        }
        std::uint32_t per_triangle = 0;
        if (geometry->index_format == kFormatUnknown) {
            per_triangle = geometry->vertex_count;
        } else if (geometry->index_format == kFormatR32Uint || geometry->index_format == kFormatR16Uint) {
            per_triangle = geometry->index_count;
        } else {
            refusal = Refusal::index_format;
            return false;
        }
        if (per_triangle % 3) {
            refusal = Refusal::triangle_count;
            return false;
        }
        count += per_triangle / 3;
    }
    if (count > 0xffffffffull) {
        refusal = Refusal::counts;
        return false;
    }
    elements = static_cast<std::uint32_t>(count);
    return true;
}

// GetRaytracingAccelerationStructurePrebuildInfo's answer, from the layer's layout. Box16 is the layer's rule for
// MINIMIZE_MEMORY bottom-level builds (docs/bvh.md "BOX16 nodes"); a top-level build keeps BOX32. The stream
// description is read for the route's notes only, as recorded in docs/bvh_route.md.
inline PrebuildAnswer prebuild(const BuildInputs& inputs, const StreamDesc* stream, bool armed = route_armed()) {
    PrebuildAnswer answer;
    if (!armed) {
        answer.refusal = Refusal::disabled;
        return answer;
    }
    std::uint32_t elements = 0;
    if (!count_elements(inputs, elements, answer.refusal)) return answer;
    XdxrLayout layout;
    const bool top_level = inputs.type == kTopLevel;
    const bool box16 = !top_level && (inputs.flags & kBuildMinimizeMemory) != 0;
    if (!layout.compute(elements, top_level, box16)) {
        answer.refusal = Refusal::counts;
        return answer;
    }
    answer.result_size = layout.size;
    answer.scratch_size = layout.scratch;
    answer.update_scratch_size = layout.scratch;
    return answer;
}

struct BuildPlan {
    Refusal refusal = Refusal::none;
    std::string reason;  // the first refusal's page reference or detail, when a field is the cause
    std::vector<BuildRecord> records;
    std::vector<StreamNotes> streams;
    std::uint64_t ignored_postbuilds = 0;  // types 1 and others: the shipped builder ignores them without an error
    bool ok() const { return refusal == Refusal::none; }
};

// One element of the batched description: the public build desc plus the Xbox stream desc and postbuild array
// (docs/xdxr.md section 1). A non-batched build is the same shape with count 0.
inline bool plan_one(const BatchedDesc2& element, BuildRecord& record, StreamNotes& notes,
                     std::uint64_t& ignored_postbuilds, Refusal& refusal, std::string& reason) {
    const std::uint64_t dest = element.dest_acceleration_structure_data;
    if (!dest) {
        refusal = Refusal::null_desc;
        return false;
    }
    if (dest % kDestinationAlignment) {
        refusal = Refusal::alignment;
        return false;
    }
    if (dest >= kMaxGuardedAddress) {
        // The shipped builder refuses a destination at or above 2^40 (docs/d3d12_vulkan.md, measured), and this
        // layout keeps every structure in the arena below it.
        refusal = Refusal::address;
        return false;
    }
    if (element.scratch_acceleration_structure_data >= kMaxGuardedAddress || !element.scratch_acceleration_structure_data) {
        refusal = Refusal::address;
        return false;
    }
    if (element.inputs.flags & kBuildPerformUpdate) {
        if (!(element.inputs.flags & kBuildAllowUpdate) || element.source_acceleration_structure_data >= kMaxGuardedAddress) {
            refusal = Refusal::update_flags;
            return false;
        }
    }
    std::uint32_t elements = 0;
    if (!count_elements(element.inputs, elements, refusal)) return false;
    XdxrLayout layout;
    const bool top_level = element.inputs.type == kTopLevel;
    if (!layout.compute(elements, top_level, !top_level && (element.inputs.flags & kBuildMinimizeMemory) != 0)) {
        refusal = Refusal::counts;
        return false;
    }
    record = BuildRecord{};
    record.dest_va = dest;
    record.scratch_va = element.scratch_acceleration_structure_data;
    record.source_va = element.source_acceleration_structure_data;
    record.top_level = top_level;
    record.update = (element.inputs.flags & kBuildPerformUpdate) != 0;
    record.flags = element.inputs.flags;
    if (top_level) {
        record.instances_va = element.inputs.descs;
        record.instance_count = element.inputs.num_descs;
        record.pointer_layout = element.inputs.descs_layout == kLayoutArrayOfPointers;
    } else {
        for (std::uint32_t i = 0; i < element.inputs.num_descs; ++i) {
            const GeometryDesc* geometry =
                element.inputs.descs_layout == kLayoutArrayOfPointers
                    ? (element.inputs.descs ? reinterpret_cast<const GeometryDesc* const*>(element.inputs.descs)[i] : nullptr)
                    : (element.inputs.descs ? &reinterpret_cast<const GeometryDesc*>(element.inputs.descs)[i] : nullptr);
            if (!geometry) {
                refusal = element.inputs.descs_layout == kLayoutArrayOfPointers ? Refusal::pointers_unreadable
                                                                                : Refusal::geometry_pointer;
                return false;
            }
            if (geometry->type != kGeometryTriangles) {
                refusal = Refusal::geometry_type;
                return false;
            }
            if (geometry->vertex_format != kFormatR32G32B32Float && geometry->vertex_format != kFormatR32G32Float) {
                refusal = Refusal::vertex_format;
                return false;
            }
            if (geometry->index_format != kFormatUnknown && geometry->index_format != kFormatR16Uint &&
                geometry->index_format != kFormatR32Uint) {
                refusal = Refusal::index_format;
                return false;
            }
            const std::uint32_t per_triangle = geometry->index_format == kFormatUnknown ? geometry->vertex_count
                                                                                       : geometry->index_count;
            if (per_triangle % 3) {
                refusal = Refusal::triangle_count;
                return false;
            }
            TriangleGeometry t;
            t.vertex_va = geometry->vertex_va;
            t.index_va = geometry->index_va;
            t.transform_va = geometry->transform_va;
            t.vertex_count = geometry->vertex_count;
            t.vertex_stride = static_cast<std::uint32_t>(geometry->vertex_stride);
            t.vertex_format = geometry->vertex_format;
            t.index_count = geometry->index_count;
            t.index_format = geometry->index_format;
            t.flags = geometry->flags;
            record.triangles.push_back(t);
        }
    }
    // The postbuild descriptions, in order: the layer writes COMPACTED_SIZE and CURRENT_SIZE, refuses serialization,
    // and the shipped builder throws for the offline-statistics type (docs/xdxr.md section 4).
    for (std::uint32_t i = 0; i < element.num_postbuild_info_descs; ++i) {
        const PostbuildInfoDesc* info =
            element.postbuild_info_descs ? &element.postbuild_info_descs[i] : nullptr;
        if (!info) {
            refusal = Refusal::null_desc;
            return false;
        }
        if (info->info_type == kPostbuildInfoOfflineStatistics) {
            refusal = Refusal::offline_statistics;
            return false;
        }
        if (info->info_type == 2) {
            refusal = Refusal::serialization;
            return false;
        }
        if (info->info_type == 0 || info->info_type == 3)
            record.postbuilds.push_back(*info);
        else
            ++ignored_postbuilds;
    }
    notes = read_stream(&element.stream, element.inputs.flags);
    (void)reason;
    return true;
}

// One batched build as the game calls it: `count` elements at the measured 0x50-byte stride (docs/xdxr.md section 1).
inline BuildPlan plan_batched(const BatchedDesc2* descs, std::uint32_t count, bool armed = route_armed()) {
    BuildPlan plan;
    if (!armed) {
        plan.refusal = Refusal::disabled;
        return plan;
    }
    if (!descs || count == 0) {
        plan.refusal = Refusal::null_desc;
        return plan;
    }
    if (count > 0x10000) {  // a bounded plan: the runtime never sees a larger batch
        plan.refusal = Refusal::too_many_elements;
        return plan;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        BuildRecord record;
        StreamNotes notes;
        Refusal refusal = Refusal::none;
        std::string reason;
        if (!plan_one(descs[i], record, notes, plan.ignored_postbuilds, refusal, reason)) {
            plan.refusal = refusal;
            plan.reason = reason;
            plan.records.clear();
            return plan;
        }
        plan.records.push_back(std::move(record));
        plan.streams.push_back(notes);
    }
    return plan;
}

// The non-batched export: one standard build description wrapped into a BATCHED_DESC2 on the stack, which is exactly
// what the shipped export does to it (docs/xdxr.md section 1), so both export forms take one route.
inline BuildPlan plan_single(const BuildDesc& desc, std::uint32_t postbuild_count,
                             const PostbuildInfoDesc* postbuilds, bool armed = route_armed()) {
    BatchedDesc2 batched{};
    batched.dest_acceleration_structure_data = desc.dest_acceleration_structure_data;
    batched.inputs = desc.inputs;
    batched.source_acceleration_structure_data = desc.source_acceleration_structure_data;
    batched.scratch_acceleration_structure_data = desc.scratch_acceleration_structure_data;
    batched.num_postbuild_info_descs = postbuild_count;
    batched.postbuild_info_descs = postbuilds;
    return plan_batched(&batched, 1, armed);
}

}  // namespace palmetto::xdxr::route
