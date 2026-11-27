// src/xdxr_descs.h: the XDXR build descriptions measured in XboxBvhBuilder.dll (docs/xdxr.md). The layouts are
// static_asserts in the header; this checks the stream lookup and the flag defaults against the DLL's measured
// behaviour, and, where the Windows SDK is available, that the public part matches d3d12.h. No GPU, no game files.
// src/xdxr_route.h: the builder-shaped prebuild and build answers from the layer's own XDXR layout
// (docs/bvh_route.md). Synthetic descriptions only; no GPU and no game data.
#include "../src/xdxr_descs.h"
#include "../src/xdxr_layout.h"
#include "../src/xdxr_route.h"
#include "../src/bvh_route.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32) && __has_include(<d3d12.h>)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
// Older d3d12.h headers (MinGW's, for one) have no DXR types.
#ifdef D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT
#define XDXR_HAVE_D3D12 1
#endif
#endif

#define CHECK(condition) do {                                    \
    if (!(condition)) {                                          \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                \
    }                                                            \
} while (0)

using namespace palmetto::xdxr;

namespace {

StreamDesc stream_of(const StreamRecord* records, size_t count) {
    return StreamDesc{count * sizeof(StreamRecord), records};
}

int stream_lookup() {
    StreamRecord records[4] = {};

    // No stream, a zero size or a null pointer: every setting keeps its default.
    CHECK(find_stream_record(StreamDesc{}, 0) == nullptr);
    CHECK(find_stream_record(StreamDesc{0, records}, 0) == nullptr);
    CHECK(find_stream_record(StreamDesc{sizeof(records), nullptr}, 0) == nullptr);

    // Types 0, 1 and 2 are stepped over until the wanted one.
    records[0].type = 2;
    records[1].type = 0;
    records[2].type = 1;
    const StreamDesc three = stream_of(records, 3);
    CHECK(find_stream_record(three, 0) == &records[1]);
    CHECK(find_stream_record(three, 1) == &records[2]);
    CHECK(find_stream_record(three, 2) == &records[0]);

    // The first record of a type wins.
    records[3].type = 0;
    CHECK(find_stream_record(stream_of(records, 4), 0) == &records[1]);

    // Any other type ends the walk, so records after it are not seen.
    records[0].type = 3;
    CHECK(find_stream_record(three, 0) == nullptr);
    CHECK(find_stream_record(three, 2) == nullptr);
    // ...but a match before it is still found.
    records[0].type = 0;
    records[1].type = 7;
    CHECK(find_stream_record(three, 0) == &records[0]);
    CHECK(find_stream_record(three, 1) == nullptr);

    // Nothing past SizeInBytes is read.
    records[0].type = 1;
    records[1].type = 0;
    CHECK(find_stream_record(stream_of(records, 1), 0) == nullptr);
    CHECK(find_stream_record(stream_of(records, 2), 0) == &records[1]);
    return 0;
}

int flag_defaults() {
    CHECK(default_record0_value_08(0) == 1.005f);
    CHECK(default_record0_value_08(kBuildFlagPreferFastTrace) == 0.0f);
    CHECK(default_record0_value_08(kBuildFlagMinimizeMemory) == 1.01f);
    CHECK(default_record0_value_08(kBuildFlagPreferFastTrace | kBuildFlagMinimizeMemory) == 1.005f);
    // Other bits (ALLOW_UPDATE, ALLOW_COMPACTION, PREFER_FAST_BUILD, PERFORM_UPDATE) don't change it.
    CHECK(default_record0_value_08(0x01 | 0x02 | 0x08 | 0x20) == 1.005f);
    return 0;
}

int record_views() {
    // The typed views alias the generic record: the type word is shared and the payload starts at 4.
    StreamRecord record = {};
    record.type = 1;
    record.payload[0x0d - 4] = 9;
    record.payload[0x0e - 4] = 1;
    StreamRecord1 one;
    std::memcpy(&one, &record, sizeof(one));
    CHECK(one.type == 1 && one.level_0d == 9 && one.flag_0e == 1);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The builder-shaped route (port/src/xdxr_route.h). Synthetic descriptions only: the checks are arithmetic over the
// layer's XdxrLayout and the plan's fields, no GPU and no game data.
// ---------------------------------------------------------------------------------------------------------------

namespace {

route::GeometryDesc triangle_geometry(std::uint32_t vertex_count, bool indexed = false,
                                      std::uint32_t index_count = 0) {
    route::GeometryDesc geometry;
    geometry.type = route::kGeometryTriangles;
    geometry.flags = route::kGeometryFlagOpaque;
    geometry.vertex_va = 0x10000;
    geometry.vertex_stride = 12;
    geometry.vertex_count = vertex_count;
    geometry.vertex_format = route::kFormatR32G32B32Float;
    geometry.index_va = indexed ? 0x20000 : 0;
    geometry.index_count = index_count;
    geometry.index_format = indexed ? route::kFormatR32Uint : route::kFormatUnknown;
    return geometry;
}

BuildInputs bottom_level(const route::GeometryDesc* descs, std::uint32_t count,
                                std::uint32_t flags = 0) {
    BuildInputs inputs;
    inputs.type = route::kBottomLevel;
    inputs.flags = flags;
    inputs.num_descs = count;
    inputs.descs_layout = route::kLayoutArray;
    inputs.descs = reinterpret_cast<std::uint64_t>(descs);
    return inputs;
}

int route_prebuild() {
    route::GeometryDesc geoms[2] = {triangle_geometry(36), triangle_geometry(0, true, 18)};
    const BuildInputs blas = bottom_level(geoms, 2);

    // The switch is off by default: a caller keeps the package helper's answer.
    CHECK(!route::route_armed());
    CHECK(route::prebuild(blas, nullptr, false).refusal == route::Refusal::disabled);

    // The sizes are the layer's own layout for the measured element count (36/3 + 18/3 = 18 triangles).
    const route::PrebuildAnswer answer = route::prebuild(blas, nullptr, true);
    palmetto::XdxrLayout expected;
    expected.compute(18, false, false);
    CHECK(answer.ok());
    CHECK(answer.result_size == expected.size);
    CHECK(answer.scratch_size == expected.scratch && answer.update_scratch_size == expected.scratch);

    // MINIMIZE_MEMORY gives the BOX16 layout, and the result is smaller (docs/bvh.md "BOX16 nodes").
    palmetto::XdxrLayout box16;
    box16.compute(18, false, true);
    const route::PrebuildAnswer minimized =
        route::prebuild(bottom_level(geoms, 2, route::kBuildMinimizeMemory), nullptr, true);
    CHECK(minimized.ok() && minimized.result_size == box16.size && box16.size < expected.size);

    // A top-level build lays out instances, and keeps BOX32 whatever the flags.
    BuildInputs tlas;
    tlas.type = route::kTopLevel;
    tlas.num_descs = 7;
    palmetto::XdxrLayout tlas_layout;
    tlas_layout.compute(7, true, false);
    const route::PrebuildAnswer top = route::prebuild(tlas, nullptr, true);
    CHECK(top.ok() && top.result_size == tlas_layout.size);

    // The measured refusals: the type and layout values, a flag outside the public bits, a bad index format, a
    // count that is not whole triangles, and a count the layout cannot hold.
    BuildInputs bad = blas;
    bad.type = 2;
    CHECK(route::prebuild(bad, nullptr, true).refusal == route::Refusal::input_type);
    bad = blas;
    bad.descs_layout = 2;
    CHECK(route::prebuild(bad, nullptr, true).refusal == route::Refusal::descs_layout);
    bad = bottom_level(geoms, 2, 0x40);
    CHECK(route::prebuild(bad, nullptr, true).refusal == route::Refusal::unknown_flag);
    route::GeometryDesc bad_index = triangle_geometry(3);
    bad_index.index_format = 99;
    bad_index.index_count = 3;
    CHECK(route::prebuild(bottom_level(&bad_index, 1), nullptr, true).refusal == route::Refusal::index_format);
    route::GeometryDesc not_triangles = triangle_geometry(10);
    CHECK(route::prebuild(bottom_level(&not_triangles, 1), nullptr, true).refusal ==
          route::Refusal::triangle_count);
    route::GeometryDesc huge = triangle_geometry(0xfffffff0u);
    route::GeometryDesc huge2 = triangle_geometry(0xfffffff0u);
    route::GeometryDesc two[2] = {huge, huge2};
    CHECK(route::prebuild(bottom_level(two, 2), nullptr, true).refusal == route::Refusal::counts);
    return 0;
}

int route_plan() {
    route::GeometryDesc geometry = triangle_geometry(12);  // 4 triangles
    BuildInputs inputs = bottom_level(&geometry, 1);
    PostbuildInfoDesc postbuilds[3] = {};
    postbuilds[0].info_type = 0;  // COMPACTED_SIZE
    postbuilds[1].info_type = 3;  // CURRENT_SIZE
    postbuilds[2].info_type = 1;  // TOOLS_VISUALIZATION: the shipped builder ignores it
    BatchedDesc2 batched[2] = {};
    batched[0].dest_acceleration_structure_data = 0x100000;  // 256-byte aligned and below 2^40
    batched[0].inputs = inputs;
    batched[0].scratch_acceleration_structure_data = 0x200000;
    batched[0].num_postbuild_info_descs = 3;
    batched[0].postbuild_info_descs = postbuilds;
    // A top-level element with an ARRAY_OF_POINTERS instance list.
    const std::uint64_t instance_pointers[2] = {0x300000, 0x300100};
    batched[1].dest_acceleration_structure_data = 0x400000;
    batched[1].inputs.type = route::kTopLevel;
    batched[1].inputs.num_descs = 2;
    batched[1].inputs.descs_layout = route::kLayoutArrayOfPointers;
    batched[1].inputs.descs = reinterpret_cast<std::uint64_t>(instance_pointers);
    batched[1].scratch_acceleration_structure_data = 0x500000;

    route::BuildPlan plan = route::plan_batched(batched, 2, true);
    CHECK(plan.ok());
    CHECK(plan.records.size() == 2);
    const route::BuildRecord& blas = plan.records[0];
    CHECK(!blas.top_level && !blas.update && !blas.pointer_layout);
    CHECK(blas.dest_va == 0x100000 && blas.scratch_va == 0x200000 && blas.source_va == 0);
    CHECK(blas.triangles.size() == 1);
    CHECK(blas.triangles[0].vertex_va == 0x10000 && blas.triangles[0].vertex_count == 12);
    CHECK(blas.triangles[0].vertex_stride == 12);
    CHECK(blas.postbuilds.size() == 2 && blas.postbuilds[0].info_type == 0 &&
          blas.postbuilds[1].info_type == 3);
    CHECK(plan.ignored_postbuilds == 1);
    const route::BuildRecord& tlas = plan.records[1];
    CHECK(tlas.top_level && tlas.pointer_layout && tlas.instance_count == 2);
    CHECK(tlas.instances_va == reinterpret_cast<std::uint64_t>(instance_pointers));

    // Updates: PERFORM_UPDATE needs ALLOW_UPDATE, and an in-place update (no source) is the destination itself.
    BatchedDesc2 update = batched[0];
    update.inputs.flags = route::kBuildPerformUpdate;
    CHECK(route::plan_batched(&update, 1, true).refusal == route::Refusal::update_flags);
    update.inputs.flags = route::kBuildPerformUpdate | route::kBuildAllowUpdate;
    update.source_acceleration_structure_data = 0x100000;
    const route::BuildPlan updated = route::plan_batched(&update, 1, true);
    CHECK(updated.ok() && updated.records[0].update && updated.records[0].source_va == 0x100000);

    // The destination rules the shipped builder enforces, plus the layout's own address rule.
    BatchedDesc2 bad = batched[0];
    bad.dest_acceleration_structure_data = 0x100001;  // not 256-byte aligned
    CHECK(route::plan_batched(&bad, 1, true).refusal == route::Refusal::alignment);
    bad.dest_acceleration_structure_data = std::uint64_t(1) << 40;
    CHECK(route::plan_batched(&bad, 1, true).refusal == route::Refusal::address);
    bad = batched[0];
    bad.scratch_acceleration_structure_data = 0;
    CHECK(route::plan_batched(&bad, 1, true).refusal == route::Refusal::address);

    // The postbuild types: the shipped builder throws for offline statistics, and the layer refuses serialization.
    PostbuildInfoDesc offline = {};
    offline.info_type = kPostbuildInfoOfflineStatistics;  // 0x10
    BatchedDesc2 with_postbuild = batched[0];
    with_postbuild.num_postbuild_info_descs = 1;
    with_postbuild.postbuild_info_descs = &offline;
    CHECK(route::plan_batched(&with_postbuild, 1, true).refusal == route::Refusal::offline_statistics);
    offline.info_type = 2;  // SERIALIZATION
    CHECK(route::plan_batched(&with_postbuild, 1, true).refusal == route::Refusal::serialization);

    // A geometry pointer list: the route reads the list on the CPU and refuses a null entry by name.
    const route::GeometryDesc* pointers[2] = {&geometry, &geometry};
    BuildInputs pointer_inputs = inputs;
    pointer_inputs.num_descs = 2;
    pointer_inputs.descs_layout = route::kLayoutArrayOfPointers;
    pointer_inputs.descs = reinterpret_cast<std::uint64_t>(pointers);
    BatchedDesc2 pointer_build = batched[0];
    pointer_build.inputs = pointer_inputs;
    const route::BuildPlan pointer_plan = route::plan_batched(&pointer_build, 1, true);
    CHECK(pointer_plan.ok() && pointer_plan.records[0].triangles.size() == 2);
    pointers[1] = nullptr;
    CHECK(route::plan_batched(&pointer_build, 1, true).refusal == route::Refusal::pointers_unreadable);

    // A vertex format the GPU builder cannot read is refused at plan time.
    route::GeometryDesc xy = triangle_geometry(3);
    xy.vertex_format = route::kFormatR16Uint;
    BatchedDesc2 xy_build = batched[0];
    xy_build.inputs = bottom_level(&xy, 1);
    CHECK(route::plan_batched(&xy_build, 1, true).refusal == route::Refusal::vertex_format);

    // The non-batched export is the same route: the shipped one wraps the public desc into a BATCHED_DESC2.
    route::BuildDesc single = {};
    single.dest_acceleration_structure_data = 0x100000;
    single.inputs = inputs;
    single.scratch_acceleration_structure_data = 0x200000;
    PostbuildInfoDesc compacted = {};
    compacted.info_type = 0;
    const route::BuildPlan single_plan = route::plan_single(single, 1, &compacted, true);
    CHECK(single_plan.ok() && single_plan.records.size() == 1 && single_plan.records[0].postbuilds.size() == 1);

    // The stream description changes no size here; its measured records are reported, and the alternate
    // bottom-level path the shipped builder takes is noted (docs/bvh_route.md "limits").
    StreamRecord0 fast = {};
    fast.type = 0;
    fast.value_08 = 11.0f;
    StreamRecord1 level = {};
    level.type = 1;
    level.level_0d = 4;
    StreamRecord records[3] = {};
    std::memcpy(&records[0], &fast, sizeof fast);
    std::memcpy(&records[1], &level, sizeof level);
    records[2].type = 0x7;  // an unknown type stops the walk
    StreamDesc stream{sizeof records, records};
    BatchedDesc2 streamed = batched[0];
    streamed.inputs.flags = route::kBuildMinimizeMemory;
    streamed.stream = stream;
    const route::BuildPlan streamed_plan = route::plan_batched(&streamed, 1, true);
    CHECK(streamed_plan.ok());
    CHECK(streamed_plan.streams[0].record0 && streamed_plan.streams[0].record1);
    CHECK(streamed_plan.streams[0].level == 4);
    CHECK(streamed_plan.streams[0].alternate_bottom_level_path);
    route::BuildPlan off = route::plan_batched(batched, 2, false);
    CHECK(off.refusal == route::Refusal::disabled && off.records.empty());
    return 0;
}

int route_command_bridge() {
    using palmetto::xdxr::command_route::BuildCommandPlan;
    using palmetto::xdxr::command_route::plan_build;

    route::GeometryDesc geometry = triangle_geometry(6, true, 6);
    geometry.transform_va = 0x30000;
    route::BuildDesc desc{};
    desc.dest_acceleration_structure_data = 0x100000;
    desc.inputs = bottom_level(&geometry, 1, route::kBuildAllowUpdate | route::kBuildAllowCompaction |
                                                   route::kBuildPreferFastTrace | route::kBuildPreferFastBuild |
                                                   route::kBuildMinimizeMemory | route::kBuildPerformUpdate);
    desc.source_acceleration_structure_data = 0x200000;
    desc.scratch_acceleration_structure_data = 0x400000;
    PostbuildInfoDesc postbuilds[3]{};
    postbuilds[0].dest_buffer = 0x600000;
    postbuilds[0].info_type = 0;  // COMPACTED_SIZE
    postbuilds[1].dest_buffer = 0x600008;
    postbuilds[1].info_type = 3;  // CURRENT_SIZE
    postbuilds[2].dest_buffer = 0x600010;
    postbuilds[2].info_type = 1;  // tools visualization is ignored by the measured batched builder

    const BuildCommandPlan disabled = plan_build(desc, 3, postbuilds, false);
    CHECK(disabled.refusal == route::Refusal::disabled && disabled.build.triangles.empty() &&
          disabled.postbuilds.empty());

    // The adapter produces the same D3D12 -> command-list fields as the legacy COM mapping, but only after the
    // measured planner accepts the description. It does not add another builder or change the XDXR layout.
    const BuildCommandPlan plan = plan_build(desc, 3, postbuilds, true);
    CHECK(plan.ok() && plan.ignored_postbuilds == 1 && plan.postbuilds.size() == 2);
    CHECK(plan.build.dst_va == 0x100000 && plan.build.src_va == 0x200000 && plan.build.scratch_va == 0x400000);
    CHECK(!plan.build.is_tlas && plan.build.update && plan.build.build_flags == 0x1f);
    CHECK(plan.build.triangles.size() == 1);
    const palmetto::RtTriangleGeometry& triangle = plan.build.triangles[0];
    CHECK(triangle.vertex_va == 0x10000 && triangle.vertex_count == 6 && triangle.vertex_stride == 12);
    CHECK(triangle.vertex_format == palmetto::xdxr::command_route::kVkR32G32B32Sfloat);
    CHECK(triangle.index_va == 0x20000 && triangle.index_count == 6 && triangle.index_type == 2);
    CHECK(triangle.transform_va == 0x30000 && triangle.opaque && triangle.flags == route::kGeometryFlagOpaque);
    CHECK(plan.postbuilds[0].dest_va == 0x600000 && plan.postbuilds[0].type == 0);
    CHECK(plan.postbuilds[0].sources.size() == 1 && plan.postbuilds[0].sources[0] == 0x100000);
    CHECK(plan.postbuilds[1].dest_va == 0x600008 && plan.postbuilds[1].type == 3);

    const std::uint64_t instances[2] = {0x900000, 0x900100};
    route::BuildDesc tlas{};
    tlas.dest_acceleration_structure_data = 0xa00000;
    tlas.inputs.type = route::kTopLevel;
    tlas.inputs.num_descs = 2;
    tlas.inputs.descs_layout = route::kLayoutArrayOfPointers;
    tlas.inputs.descs = reinterpret_cast<std::uint64_t>(instances);
    tlas.scratch_acceleration_structure_data = 0xb00000;
    const BuildCommandPlan tlas_plan = plan_build(tlas, 0, nullptr, true);
    CHECK(tlas_plan.ok() && tlas_plan.build.is_tlas && tlas_plan.build.instances_va == tlas.inputs.descs);
    CHECK(tlas_plan.build.instance_count == 2 && tlas_plan.build.instance_pointers);

    // The other RT COM slots have narrowly named answers under the same switch; off remains a named refusal.
    using route::Refusal;
    CHECK(route::command_list_refusal(false, false) == Refusal::disabled);
    CHECK(route::command_list_refusal(true, true) == Refusal::command_list_type);
    CHECK(route::command_list_refusal(false, true) == Refusal::none);
    CHECK(route::backend_refusal(false, false) == Refusal::disabled);
    CHECK(route::backend_refusal(false, true) == Refusal::backend);
    CHECK(route::backend_refusal(true, true) == Refusal::none);
    CHECK(route::copy_refusal(0, 0x100000, 0x200000, false) == Refusal::disabled);
    CHECK(route::copy_refusal(0, 0x100000, 0x200000, true) == Refusal::none);
    CHECK(route::copy_refusal(1, 0x100000, 0x200000, true) == Refusal::none);
    CHECK(route::copy_refusal(2, 0x100000, 0x200000, true) == Refusal::copy_mode);
    CHECK(route::copy_refusal(0, 0, 0x200000, true) == Refusal::zero_address);
    CHECK(route::copy_refusal(0, 0x100001, 0x200000, true) == Refusal::alignment);
    CHECK(route::copy_refusal(0, std::uint64_t(1) << 40, 0x200000, true) == Refusal::address);
    CHECK(route::emit_postbuild_refusal(0, false) == Refusal::disabled);
    CHECK(route::emit_postbuild_refusal(0, true) == Refusal::none);
    CHECK(route::emit_postbuild_refusal(3, true) == Refusal::none);
    CHECK(route::emit_postbuild_refusal(1, true) == Refusal::postbuild_type);
    CHECK(route::emit_postbuild_refusal(2, true) == Refusal::serialization);
    CHECK(route::emit_postbuild_refusal(kPostbuildInfoOfflineStatistics, true) == Refusal::offline_statistics);
    CHECK(route::emit_postbuild_count_refusal(1, false) == Refusal::disabled);
    CHECK(route::emit_postbuild_count_refusal(0, true) == Refusal::null_desc);
    CHECK(route::emit_postbuild_count_refusal(1, true) == Refusal::none);
    CHECK(route::emit_postbuild_count_refusal(0x10001, true) == Refusal::too_many_elements);

    route::BuildDesc too_many_geometries = desc;
    too_many_geometries.inputs.num_descs = 0x10001;
    CHECK(plan_build(too_many_geometries, 0, nullptr, true).refusal == Refusal::too_many_elements);
    CHECK(plan_build(desc, 0x10001, postbuilds, true).refusal == Refusal::too_many_elements);
    route::GeometryDesc too_wide_stride = geometry;
    too_wide_stride.vertex_stride = std::uint64_t(1) << 32;
    route::BuildDesc invalid_stride = desc;
    invalid_stride.inputs.descs = reinterpret_cast<std::uint64_t>(&too_wide_stride);
    CHECK(plan_build(invalid_stride, 0, nullptr, true).refusal == Refusal::vertex_stride);
    return 0;
}

int route_switch_environment() {
#if defined(_WIN32)
    _putenv_s("PALMETTO_BVH_ROUTE", "own");
#else
    setenv("PALMETTO_BVH_ROUTE", "own", 1);
#endif
    CHECK(route::route_armed());
    const route::GeometryDesc geometry = triangle_geometry(3);
    CHECK(route::prebuild(bottom_level(&geometry, 1), nullptr, route::route_armed()).ok());
#if defined(_WIN32)
    _putenv_s("PALMETTO_BVH_ROUTE", "");
#else
    unsetenv("PALMETTO_BVH_ROUTE");
#endif
    CHECK(!route::route_armed());
    return 0;
}

}  // namespace

#ifdef XDXR_HAVE_D3D12
// The public part of BATCHED_DESC2 is the public build desc, as the DLL's standard-build export copies it.
static_assert(sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS) == sizeof(BuildInputs));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, Flags) == offsetof(BuildInputs, flags));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, NumDescs) ==
              offsetof(BuildInputs, num_descs));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, DescsLayout) ==
              offsetof(BuildInputs, descs_layout));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, InstanceDescs) ==
              offsetof(BuildInputs, descs));
static_assert(sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC) == offsetof(BatchedDesc2, num_postbuild_info_descs));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC, Inputs) == offsetof(BatchedDesc2, inputs));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC, SourceAccelerationStructureData) ==
              offsetof(BatchedDesc2, source_acceleration_structure_data));
static_assert(offsetof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC, ScratchAccelerationStructureData) ==
              offsetof(BatchedDesc2, scratch_acceleration_structure_data));
static_assert(sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC) == sizeof(PostbuildInfoDesc));
static_assert(offsetof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC, InfoType) ==
              offsetof(PostbuildInfoDesc, info_type));
// The alignment the DLL enforces on DestAccelerationStructureData.
static_assert(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT == 256);
static_assert(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE == kBuildFlagPreferFastTrace);
static_assert(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY == kBuildFlagMinimizeMemory);

int public_desc_copy() {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.DestAccelerationStructureData = 0x100000;
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    desc.Inputs.NumDescs = 3;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    desc.Inputs.InstanceDescs = 0x2000;
    desc.SourceAccelerationStructureData = 0x300000;
    desc.ScratchAccelerationStructureData = 0x400000;
    BatchedDesc2 batched = {};
    std::memcpy(&batched, &desc, sizeof(desc));
    CHECK(batched.dest_acceleration_structure_data == 0x100000);
    CHECK(batched.inputs.type == 1 && batched.inputs.flags == kBuildFlagMinimizeMemory);
    CHECK(batched.inputs.num_descs == 3 && batched.inputs.descs_layout == 1 && batched.inputs.descs == 0x2000);
    CHECK(batched.source_acceleration_structure_data == 0x300000);
    CHECK(batched.scratch_acceleration_structure_data == 0x400000);
    CHECK(batched.stream.size_in_bytes == 0 && batched.stream.stream == nullptr);
    return 0;
}
#endif

}  // namespace

int main() {
    if (stream_lookup() || flag_defaults() || record_views() || route_prebuild() || route_plan() ||
        route_command_bridge() || route_switch_environment())
        return 1;
#ifdef XDXR_HAVE_D3D12
    if (public_desc_copy())
        return 1;
    std::printf("xdxr_descs: ok (checked against d3d12.h)\n");
#else
    std::printf("xdxr_descs: ok (no DXR d3d12.h: public layout not compared)\n");
#endif
    return 0;
}
