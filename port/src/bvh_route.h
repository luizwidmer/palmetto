// Opt-in adapter from the measured Xbox build description to the layer's command-list IR.
// The planner is GPU-free; d3d12_com.cpp consumes this result only when PALMETTO_BVH_ROUTE=own.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "command_list.h"
#include "xdxr_route.h"

// Resolves an opt-in XboxBvhBuilder export bridge. The D3D12 COM layer supplies the route/refusal thunks; the PE loader
// passes the original export pointer so the disabled and unrelated paths keep pointer identity.
extern "C" void* palmetto_bvh_helper_export(const char* module, const char* symbol, void* package_export);

namespace palmetto::xdxr::command_route {

// VkBuildAccelerationStructureFlagsKHR values used by the IR's Vulkan replay path. Keep these local so the planner
// and its tests remain independent of Vulkan headers; each bit is mapped explicitly from the public D3D12 flag.
inline constexpr std::uint32_t kVkAllowUpdate = 0x00000001;
inline constexpr std::uint32_t kVkAllowCompaction = 0x00000002;
inline constexpr std::uint32_t kVkPreferFastTrace = 0x00000004;
inline constexpr std::uint32_t kVkPreferFastBuild = 0x00000008;
inline constexpr std::uint32_t kVkLowMemory = 0x00000010;

// VkFormat values for the two vertex forms the measured XDXR GPU builder reads.
inline constexpr std::uint32_t kVkR32G32Sfloat = 103;
inline constexpr std::uint32_t kVkR32G32B32Sfloat = 106;

struct BuildCommandPlan {
    route::Refusal refusal = route::Refusal::none;
    BuildAccelerationStructure build{};
    std::vector<AccelerationStructurePostbuild> postbuilds;
    std::uint64_t ignored_postbuilds = 0;

    bool ok() const { return refusal == route::Refusal::none; }
};

inline bool vertex_format(std::uint32_t dxgi_format, std::uint32_t& vk_format) {
    switch (dxgi_format) {
    case route::kFormatR32G32Float: vk_format = kVkR32G32Sfloat; return true;
    case route::kFormatR32G32B32Float: vk_format = kVkR32G32B32Sfloat; return true;
    default: return false;
    }
}

// Build a command record from the same plan used by the builder-shaped route. This is intentionally a translation
// layer, not a second builder: Vulkan replay still writes the project's own XDXR format into the supplied arena VAs.
inline BuildCommandPlan plan_build(const route::BuildDesc& desc, std::uint32_t postbuild_count,
                                   const PostbuildInfoDesc* postbuilds,
                                   bool armed = route::route_armed()) {
    BuildCommandPlan out;
    const route::BuildPlan plan = route::plan_single(desc, postbuild_count, postbuilds, armed);
    out.refusal = plan.refusal;
    if (!plan.ok()) return out;
    if (plan.records.size() != 1) {
        out.refusal = route::Refusal::counts;
        return out;
    }

    const route::BuildRecord& source = plan.records.front();
    out.build.is_tlas = source.top_level;
    out.build.update = source.update;
    out.build.dst_va = source.dest_va;
    out.build.src_va = source.source_va;
    out.build.scratch_va = source.scratch_va;
    out.build.instances_va = source.instances_va;
    out.build.instance_count = source.instance_count;
    out.build.instance_pointers = source.pointer_layout;

    if (source.flags & route::kBuildAllowUpdate) out.build.build_flags |= kVkAllowUpdate;
    if (source.flags & route::kBuildAllowCompaction) out.build.build_flags |= kVkAllowCompaction;
    if (source.flags & route::kBuildPreferFastTrace) out.build.build_flags |= kVkPreferFastTrace;
    if (source.flags & route::kBuildPreferFastBuild) out.build.build_flags |= kVkPreferFastBuild;
    if (source.flags & route::kBuildMinimizeMemory) out.build.build_flags |= kVkLowMemory;

    out.build.triangles.reserve(source.triangles.size());
    for (const route::TriangleGeometry& geometry : source.triangles) {
        RtTriangleGeometry triangle{};
        triangle.vertex_va = geometry.vertex_va;
        triangle.vertex_count = geometry.vertex_count;
        triangle.vertex_stride = geometry.vertex_stride;
        if (!vertex_format(geometry.vertex_format, triangle.vertex_format)) {
            out.refusal = route::Refusal::vertex_format;
            out.build = {};
            return out;
        }
        triangle.index_va = geometry.index_va;
        triangle.index_count = geometry.index_count;
        switch (geometry.index_format) {
        case route::kFormatUnknown: triangle.index_type = 0; break;
        case route::kFormatR16Uint: triangle.index_type = 1; break;
        case route::kFormatR32Uint: triangle.index_type = 2; break;
        default:
            out.refusal = route::Refusal::index_format;
            out.build = {};
            return out;
        }
        triangle.transform_va = geometry.transform_va;
        triangle.opaque = (geometry.flags & route::kGeometryFlagOpaque) != 0;
        triangle.flags = geometry.flags;
        out.build.triangles.push_back(triangle);
    }

    out.postbuilds.reserve(source.postbuilds.size());
    for (const PostbuildInfoDesc& postbuild : source.postbuilds) {
        AccelerationStructurePostbuild item{};
        item.dest_va = postbuild.dest_buffer;
        item.type = postbuild.info_type;
        item.sources.push_back(source.dest_va);
        out.postbuilds.push_back(std::move(item));
    }
    out.ignored_postbuilds = plan.ignored_postbuilds;
    return out;
}

}  // namespace palmetto::xdxr::command_route
