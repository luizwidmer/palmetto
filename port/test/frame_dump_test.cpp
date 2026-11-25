// The frame dump's pure helpers (src/vulkan_replay.h, docs/frame_capture.md): the command kind and the ordered
// writers of a resource in one command list. The diagnostic itself prints one frame's commands so a black frame can
// be explained by what writes its target after the draws; these cases pin the classification and the order it relies
// on, including a resolve that writes the target AFTER its last draw. No Vulkan loader, GPU or game data.
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <memory>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool kind_is(const palmetto::Command& command, const char* want) {
    return std::strcmp(palmetto::frame_dump::kind_name(command), want) == 0;
}

palmetto::TargetView target(palmetto::ResourceId id) {
    palmetto::TargetView view{};
    view.resource = id;
    view.mip = 0;
    view.first_layer = 0;
    view.layer_count = 1;
    return view;
}

palmetto::Draw draw(std::uintptr_t pipeline, palmetto::ResourceId color, palmetto::ResourceId depth = 0) {
    palmetto::Draw d;
    d.pipeline = pipeline;
    d.count = 3;
    d.instances = 1;
    if (color) {
        auto views = std::make_shared<palmetto::DrawViews>();
        views->render_target_count = 1;
        views->render_targets[0] = target(color);
        d.views = std::move(views);
    }
    d.depth = target(depth);
    return d;
}

}  // namespace

int main() {
    using namespace palmetto;

    CommandList list;
    expect(list.transition(TransitionBarrier{7, 0, 4, 2}), "record a transition barrier");
    expect(list.clear_render_target(target(7), ClearColor{0.0f, 0.0f, 0.0f, 1.0f}), "record a black clear of 7");
    expect(list.transition(TransitionBarrier{7, 0, 2, 4}), "record the barrier into render target state");
    expect(list.draw(draw(0x1000, 7)), "record the first draw into 7");
    expect(list.transition(TransitionBarrier{7, 0, 2, 4}), "record a barrier before the second draw");
    expect(list.draw(draw(0x1008, 7, 12)), "record the second draw into 7, with depth 12");
    expect(list.resolve_subresource(ResolveSubresource{7, 0, 11, 0}), "record a resolve writing 7 again");
    expect(list.clear_render_target(target(8), ClearColor{1.0f, 0.0f, 0.0f, 1.0f}), "record a clear of 8");
    expect(list.copy_buffer(CopyBuffer{8, 0, 9, 0, 4}), "record a copy writing 8");
    expect(list.draw(draw(0x1010, 0)), "record a draw with no render target");
    expect(list.close(), "close the list");

    const auto commands = list.commands();
    expect(commands.size() == std::size_t{10}, "ten commands recorded");

    // Kind names, in the order the dump prints them.
    const char* const kinds[] = {"barrier", "clear", "barrier", "draw", "barrier",
                                 "draw",    "resolve", "clear", "copy", "draw"};
    for (std::size_t i = 0; i < commands.size() && i < std::size(kinds); ++i)
        expect(kind_is(commands[i], kinds[i]), "command kind matches its place in the sequence");

    // Which command writes a resource: barriers never do, a draw does through its colour or depth attachment, and a
    // draw with no attachment writes nothing.
    expect(!frame_dump::command_writes(commands[0], 7), "a transition barrier writes nothing");
    expect(frame_dump::command_writes(commands[1], 7), "the clear writes 7");
    expect(frame_dump::command_writes(commands[3], 7), "the first draw writes 7");
    expect(frame_dump::command_writes(commands[5], 12), "the second draw writes its depth 12");
    expect(!frame_dump::command_writes(commands[5], 8), "the second draw does not write 8");
    expect(frame_dump::command_writes(commands[6], 7), "the resolve writes 7");
    expect(frame_dump::command_writes(commands[8], 8), "the copy writes 8");
    expect(!frame_dump::command_writes(commands[9], 8), "a draw with no render target writes nothing");
    expect(!frame_dump::command_writes(commands[1], 0), "resource 0 never matches");

    // The ordered writer list of the target: clear, draw, draw, resolve - in that order.
    const std::vector<std::size_t> writers = frame_dump::writers(commands, 7);
    expect(writers == std::vector<std::size_t>({1, 3, 5, 6}), "writers of 7 are the clear, two draws and the resolve");
    const std::vector<std::size_t> writers8 = frame_dump::writers(commands, 8);
    expect(writers8 == std::vector<std::size_t>({7, 8}), "writers of 8 are its clear and its copy");
    expect(frame_dump::writers(commands, 77).empty(), "a resource nothing writes has no writers");

    // The black-frame question: is the target written again after its last draw? Here the resolve is, and nothing
    // follows the resolve.
    expect(frame_dump::first_writer_after(commands, 7, 2) == 3, "the first draw is the first write after the barrier");
    expect(frame_dump::first_writer_after(commands, 7, 5) == 6, "the resolve writes 7 after the last draw");
    expect(frame_dump::first_writer_after(commands, 7, 6) == commands.size(), "nothing writes 7 after the resolve");
    expect(frame_dump::first_writer_after(commands, 8, 8) == commands.size(), "nothing writes 8 after its copy");

    // The kinds of the other commands the dump must label.
    const Command marker = DebugMarker{DebugMarker::Kind::set, "label"};
    expect(kind_is(marker, "marker"), "a debug marker is a marker");
    const Command dispatch = Dispatch{1, {}, 1, 1, 1};
    expect(kind_is(dispatch, "dispatch"), "a dispatch is a dispatch");
    const Command atomic = AtomicCopyBuffer{7, 0, 9, 0, 4, {}};
    expect(frame_dump::command_writes(atomic, 7), "an atomic copy writes its destination");
    expect(kind_is(atomic, "atomic-copy"), "an atomic copy is an atomic-copy");
    const Command query = BeginQuery{3, 4, 0};
    expect(kind_is(query, "query"), "a query begin is a query");
    const Command discard = DiscardResource{7, 0, 1, false, true};
    expect(frame_dump::command_writes(discard, 7), "a discard is listed with the writers of its resource");
    expect(kind_is(discard, "discard"), "a discard is a discard");

    // The descriptor tables a draw's recorded root arguments name (docs/frame_capture.md, the sampled-texture readback):
    // one 64-bit value per descriptor-table dword the pipeline's root signature marks, in dword order, bounded.
    Draw tabled;
    tabled.user_data[0] = 0x11111111u;
    tabled.user_data[1] = 0x22222222u;  // window 0: a table
    tabled.user_data[4] = 0x33333333u;
    tabled.user_data[5] = 0x44444444u;  // window 4: a table
    tabled.user_data[6] = 0x55555555u;
    tabled.user_data[7] = 0x66666666u;  // window 6: not marked a table
    const std::vector<std::uint64_t> tables =
        frame_dump::descriptor_tables(tabled, (1u << 0) | (1u << 4), 8);
    expect(tables.size() == 2, "two table windows give two tables");
    expect(tables.size() == 2 && tables[0] == 0x2222222211111111ull, "a table's VA is its two dwords, low first");
    expect(tables.size() == 2 && tables[1] == 0x4444444433333333ull, "the second table follows the first");
    expect(frame_dump::descriptor_tables(tabled, 0).empty(), "no table windows give no tables");
    const std::vector<std::uint64_t> one = frame_dump::descriptor_tables(tabled, (1u << 0) | (1u << 4), 1);
    expect(one.size() == 1 && one[0] == 0x2222222211111111ull, "the table count is bounded");

    // The vertex-path helpers (docs/vertex_path.md): attribute byte sizes and input-layout problems.
    expect(vertex_format_bytes(VK_FORMAT_R32G32B32_SFLOAT) == 12, "a 3-float attribute is 12 bytes");
    expect(vertex_format_bytes(VK_FORMAT_R16G16_SFLOAT) == 4, "a 2-half attribute is 4 bytes");
    expect(vertex_format_bytes(VK_FORMAT_R8G8B8A8_UNORM) == 4, "an RGBA8 attribute is 4 bytes");
    expect(vertex_format_bytes(VK_FORMAT_R32G32_SFLOAT) == 8, "a 2-float attribute is 8 bytes");
    expect(vertex_format_bytes(9999) == 0, "an unknown format has no size");
    VertexPathInfo layout;
    layout.binding_count = 1;
    layout.binding[0] = VertexPathInfo::Binding{0, 24, VK_VERTEX_INPUT_RATE_VERTEX, 0};
    layout.attribute_count = 2;
    layout.attribute[0] = VertexPathInfo::Attribute{0, VK_FORMAT_R32G32B32_SFLOAT, 0, 0};
    layout.attribute[1] = VertexPathInfo::Attribute{1, VK_FORMAT_R32G32B32_SFLOAT, 12, 0};
    expect(vertex_layout_problems(layout) == 0, "an attribute ending exactly at the stride is fed");
    layout.attribute[1].offset = 16;  // 16 + 12 = 28 > 24
    expect((vertex_layout_problems(layout) & kVertexOverrunsStride) != 0, "an attribute past the stride is reported");
    layout.attribute[1].offset = 0;
    layout.attribute[1].location = 40;
    expect((vertex_layout_problems(layout) & kVertexLocationOutOfRange) != 0, "a location past 31 is reported");
    layout.attribute[1].location = 1;
    layout.attribute[1].format = 9999;
    expect((vertex_layout_problems(layout) & kVertexFormatUnknown) != 0, "an unknown format is reported");
    layout.attribute[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    layout.binding[0].stride = 0;  // dynamic: set from the draw's view
    layout.attribute[1].offset = 100;
    expect(vertex_layout_problems(layout) == 0, "a dynamic stride is not judged at pipeline creation");

    // The FSPX blit position transform (docs/vertex_path.md): the module's whole transform is four push-constant
    // scalars; all zero is the measured state and collapses every vertex to (0,0,0,0).
    std::uint32_t scalars[32] = {};
    expect(blit_transform_degenerate(blit_transform(scalars)), "all-zero position scalars are degenerate");
    scalars[28] = 0x3f800000;  // w = 1
    expect(blit_transform_degenerate(blit_transform(scalars)),
           "a positive w with x = y = z = 0 still covers nothing, so it is degenerate");
    scalars[8] = 0x3f800000;  // x = 1: the triangle now has extent
    expect(!blit_transform_degenerate(blit_transform(scalars)), "a non-zero x with a positive w is not degenerate");
    expect(blit_transform(scalars).w == 1.0f, "s28 is the w scale");
    scalars[8] = 0x40000000;  // x = 2
    expect(blit_transform(scalars).x == 2.0f, "s8 is the x scale");
    scalars[9] = 0xbf800000;  // z = -1
    scalars[10] = 0x40400000;  // y = 3
    const BlitTransform transform = blit_transform(scalars);
    expect(transform.y == 3.0f && transform.z == -1.0f, "s10 is y and s9 is z");
    // Clipping keeps a vertex only with w > 0, so a denormal x with w = 0 is still degenerate; w already 1 here.
    std::uint32_t denormal[32] = {};
    denormal[8] = 1;  // the smallest subnormal as a float: nonzero bits, ~1.4e-45
    expect(blit_transform_degenerate(blit_transform(denormal)), "a denormal x with w = 0 is degenerate");
    denormal[28] = 0x3f800000;  // w = 1
    expect(!blit_transform_degenerate(blit_transform(denormal)), "a positive w with nonzero x is not degenerate");
    denormal[10] = 0xbf800000;  // y = -1
    expect(!blit_transform_degenerate(blit_transform(denormal)), "negative x/y/z with positive w is not degenerate");
    denormal[28] = 0x80000000;  // w = -0.0: not > 0
    expect(blit_transform_degenerate(blit_transform(denormal)), "a non-positive w is degenerate whatever x says");
    denormal[28] = 0x7fc00000;  // w = NaN
    expect(blit_transform_degenerate(blit_transform(denormal)), "a NaN w is degenerate");

    // Push-constant provenance (docs/vertex_path.md): the root signature parameter table plus the replay's table mask
    // say where each of the 32 dwords comes from.
    VertexPathInfo root;
    root.parameter_count = 2;
    root.parameter[0] = VertexPathInfo::RootParameter{2, 0, 2, 0};  // descriptor table at s0..s1
    root.parameter[1] = VertexPathInfo::RootParameter{0, 8, 4, 4};  // 4 root constants at s8..s11
    std::uint32_t user_data[32] = {};
    user_data[0] = 0x1234;
    user_data[1] = 0x5678;
    user_data[8] = 0x3f800000;
    PushConstantProvenance provenance = push_constant_provenance(user_data, 0x3u, root, 0);
    expect(provenance.kind == kPushConstantTable && provenance.parameter == 0, "s0 is the table parameter");
    expect(push_constant_provenance(user_data, 0x3u, root, 1).kind == kPushConstantTable, "s1 is the table pair");
    provenance = push_constant_provenance(user_data, 0x3u, root, 8);
    expect(provenance.kind == kPushConstantRootArgument && provenance.parameter == 1, "s8 is a root constant");
    expect(push_constant_provenance(user_data, 0x3u, root, 2).kind == kPushConstantUnset, "s2 is unset");
    expect(push_constant_provenance(user_data, 0x3u, root, 5).kind == kPushConstantUnset, "s5 is unset");
    user_data[30] = 1;
    expect(push_constant_provenance(user_data, 0x3u, root, 30).kind == kPushConstantLoose,
           "a nonzero dword no parameter covers is loose");
    expect(std::strcmp(push_constant_provenance_name(kPushConstantTable), "table") == 0, "provenance names are the dump's");
    VertexPathInfo bare;
    expect(push_constant_provenance(user_data, 0x3u, bare, 0).kind == kPushConstantTable,
           "the replay's table mask alone marks a table window");

    // The console driver-block vertex-stream V# the geometry fix writes (docs/vertex_path.md, docs/root_arguments.md
    // section 7): encode/decode round trip, a non-zero format (the module gates its colour conversion on it), and the
    // matrix V# shape the coverage experiment writes at the s8 table's slot 0.
    const gfx10::BufferWords stream = frame_dump::vertex_stream_descriptor(0x86df10, 24, 170);
    gfx10::Buffer decoded_stream{};
    expect(gfx10::decode(stream, decoded_stream), "the synthesised vertex-stream V# is a valid buffer descriptor");
    expect(decoded_stream.address == 0x86df10, "the V# carries the view's arena-relative address");
    expect(decoded_stream.stride == 24 && decoded_stream.num_records == 170,
           "the V# carries the vertex stride and record count");
    expect(decoded_stream.format == 22, "the V# carries a non-zero 32-bit float format");
    expect(decoded_stream.swizzle == gfx10::identity_swizzle, "the V# keeps the identity swizzle");
    expect((stream[1] & 0x80000000u) == 0, "word 1's top bit is clear (the module requires it)");
    const gfx10::BufferWords matrix = frame_dump::matrix_descriptor(0x86dc40, 480);
    expect(matrix[3] == 0x30000FACu, "the matrix V#'s word 3 is 0x30000FAC (bounds raw, identity select)");
    gfx10::Buffer decoded_matrix{};
    expect(gfx10::decode(matrix, decoded_matrix) && decoded_matrix.address == 0x86dc40 &&
               decoded_matrix.stride == 0 && decoded_matrix.num_records == 480,
           "the matrix V# is a raw buffer over the matrix bytes");

    // The measured feed's decision (docs/vertex_constant_feed.md): which draws take the synthesised driver block. A draw
    // whose pipeline has a vertex-input slot 0, a slot-0 view with a stride, and a descriptor table pair at s0/s1 (the
    // dwords the feed replaces) takes it; every refusal keeps the recorded s0/s1 pair and has a named reason.
    const auto feeds = [](bool mesh, std::uint32_t slots, bool view, std::uint32_t stride, std::uint64_t size, bool s0) {
        return frame_dump::driver_block_decision(mesh, slots, view, stride, size, s0).feed;
    };
    expect(feeds(false, 1u, true, 24, 4080, true), "the measured draw takes the driver block");
    expect(!feeds(true, 1u, true, 24, 4080, true), "a mesh pipeline has no input assembly");
    expect(!feeds(false, 0u, true, 24, 4080, true), "a pipeline whose layout uses no slot 0 does not");
    expect(!feeds(false, 1u, false, 24, 4080, true), "a draw with no slot-0 view does not");
    expect(!feeds(false, 1u, true, 0, 4080, true), "a stride-0 slot-0 view does not");
    expect(!feeds(false, 1u, true, 24, 0, true), "a slot-0 view shorter than one record does not");
    expect(!feeds(false, 1u, true, 24, 4080, false),
           "a draw whose root signature puts no table pair at s0/s1 does not (keeps s0/s1)");
    expect(std::strlen(frame_dump::driver_block_decision(false, 0u, true, 24, 4080, true).why) != 0,
           "a refused feed names its reason");
    expect(std::strlen(frame_dump::driver_block_decision(false, 1u, true, 24, 4080, true).why) == 0,
           "a taken feed has no reason text");

    // The colour path (docs/colour_path.md): the constant pass's colour source. The measured table's slot 0 V# names a
    // 96-byte raw block (stride 0, records 96, format 20); a V#'s declared size is the record count when the stride is 0
    // and stride x records otherwise; the table's data slots at +0x40/+0x60 are float data, not descriptors, so neither
    // can be mistaken for the block; and the exact-content writer watch's comparison is byte-exact (a write that keeps
    // the non-zero byte count is still a change).
    const gfx10::BufferWords block_words{0x0116FF20u, 0x00000000u, 0x00000060u, 0x30014FACu};
    gfx10::Buffer block{};
    expect(gfx10::decode(block_words, block), "the measured slot-0 words are a V#");
    expect(block.address == 0x116FF20u, "the V# names the block's arena-relative address");
    expect(block.stride == 0 && block.num_records == 96 && block.format == 20,
           "the measured V# is a 96-record raw buffer with format 20");
    expect(frame_dump::colour_block_bytes(block) == 96, "a stride-0 V# declares its record count in bytes");
    gfx10::Buffer strided{};
    strided.stride = 24;
    strided.num_records = 170;
    expect(frame_dump::colour_block_bytes(strided) == 4080, "a strided V# declares stride x records bytes");
    expect(frame_dump::colour_dword_float(0x3f800000u) == 1.0f, "3f800000 is 1.0f");
    expect(frame_dump::colour_dword_float(0xbf800000u) == -1.0f, "bf800000 is -1.0f");
    expect(frame_dump::colour_dword_float(0x00000000u) == 0.0f, "0 is 0.0f");
    const gfx10::BufferWords data60{0x3f800000u, 0xbf800000u, 0x00000000u, 0xffffffffu};
    gfx10::Buffer data60_buffer{};
    expect(!gfx10::decode(data60, data60_buffer), "the measured +0x60 float-data slot is not a V#");
    const gfx10::ImageWords data60_image{data60[0], data60[1], data60[2], data60[3], 0, 0, 0, 0};
    gfx10::Image data60_decoded{};
    expect(!gfx10::decode(data60_image, data60_decoded), "the measured +0x60 float-data slot does not decode as a T#");
    const gfx10::BufferWords data40{0x00000000u, 0x00000000u, 0xbf800000u, 0xbf800000u};
    gfx10::Buffer data40_buffer{};
    expect(!gfx10::decode(data40, data40_buffer), "the measured +0x40 float-data slot is not a V#");
    const std::uint8_t content_before[8] = {0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    const std::uint8_t content_same[8] = {0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    const std::uint8_t content_changed[8] = {0, 0, 0, 0, 0x00, 0xff, 0xff, 0xff};
    expect(!frame_dump::colour_bytes_differ(content_before, content_same, 8), "equal content is not a change");
    expect(frame_dump::colour_bytes_differ(content_before, content_changed, 8),
           "a write that keeps the non-zero byte count is still a change");
    expect(!frame_dump::colour_bytes_differ(content_before, content_changed, 0), "an empty comparison is not a change");

    if (failures) return 1;
    std::printf("frame_dump_test: command kinds, ordered writers and table windows exact\n");
    return 0;
}
