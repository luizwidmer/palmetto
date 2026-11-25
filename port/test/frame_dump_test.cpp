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
    expect(!blit_transform_degenerate(blit_transform(scalars)), "a non-zero w scalar is not degenerate");
    expect(blit_transform(scalars).w == 1.0f, "s28 is the w scale");
    scalars[8] = 0x40000000;  // x = 2
    expect(blit_transform(scalars).x == 2.0f, "s8 is the x scale");
    scalars[9] = 0xbf800000;  // z = -1
    scalars[10] = 0x40400000;  // y = 3
    const BlitTransform transform = blit_transform(scalars);
    expect(transform.y == 3.0f && transform.z == -1.0f, "s10 is y and s9 is z");

    if (failures) return 1;
    std::printf("frame_dump_test: command kinds, ordered writers and table windows exact\n");
    return 0;
}
