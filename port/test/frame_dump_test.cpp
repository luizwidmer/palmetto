// The frame dump's pure helpers (src/vulkan_replay.h, docs/frame_capture.md): the command kind and the ordered
// writers of a resource in one command list. The diagnostic itself prints one frame's commands so a black frame can
// be explained by what writes its target after the draws; these cases pin the classification and the order it relies
// on, including a resolve that writes the target AFTER its last draw. No Vulkan loader, GPU or game data.
#include "../src/vulkan_replay.h"
#include "../src/vulkan_backend.h"  // docs/vertex_buffer_va.md section 12: the pacing knobs' pure mapping
#include "../src/vulkan_present_compose.h"  // kind-7 compose-format adapter; no device is created here
#include "../src/upscale_kind7.h"

#include <cmath>
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

    // The console graphics user-data layout (docs/vertex_buffer_va.md section 18): a descriptor table is ONE console
    // dword (its 32-bit heap-relative VA, not the PC packing's 64-bit title-VA pair), a root descriptor two, a constant
    // parameter its value count. The block puts each parameter at its console slot and zeroes every other dword, so the
    // PC-packed dwords are discarded and s0/s1 are free for the console driver block.
    expect(frame_dump::console_parameter_dwords(2, 0) == 1, "a descriptor table takes one console dword");
    expect(frame_dump::console_parameter_dwords(1, 0) == 2, "a root descriptor takes two console dwords");
    expect(frame_dump::console_parameter_dwords(0, 4) == 4, "a constant parameter takes its value count");
    expect(frame_dump::console_parameter_dwords(7, 0) == 0, "an unknown parameter type takes none");
    {
        // The measured content draws' table-only signature: the PC pair at s0/s1 becomes the console table dword at s8.
        frame_dump::ConsoleBlockParameter parameter{};
        parameter.console_offset = 8;
        parameter.dwords = 1;
        parameter.value[0] = 0x0C16EF00u;
        std::array<std::uint32_t, 32> block{};
        expect(frame_dump::graphics_console_block(&parameter, 1, block) == frame_dump::ConsoleBlockResult::ok,
               "a table-only signature places its console block");
        expect(block[8] == 0x0C16EF00u, "the table's arena-relative VA is the console table dword at s8");
        expect(block[0] == 0 && block[1] == 0, "s0/s1 are zero: the console driver block owns them");
        expect(block[9] == 0 && block[31] == 0, "the dwords the placement does not name stay zero");
    }
    {
        // The measured content pass's signature: a descriptor table at s8, then two 32-bit constants at s9/s10.
        frame_dump::ConsoleBlockParameter parameters[2]{};
        parameters[0].console_offset = 8;
        parameters[0].dwords = 1;
        parameters[0].value[0] = 0x0C2CE1A0u;
        parameters[1].console_offset = 9;
        parameters[1].dwords = 2;
        parameters[1].value[0] = 0x11111111u;
        parameters[1].value[1] = 0x22222222u;
        std::array<std::uint32_t, 32> block{};
        expect(frame_dump::graphics_console_block(parameters, 2, block) == frame_dump::ConsoleBlockResult::ok,
               "a table plus two constants places");
        expect(block[8] == 0x0C2CE1A0u && block[9] == 0x11111111u && block[10] == 0x22222222u,
               "the constants follow the table at their console slots");
    }
    {
        // Refusals: no parameter, two parameters on one dword, a placement past s31, an unrepresentable count.
        std::array<std::uint32_t, 32> block{};
        expect(frame_dump::graphics_console_block(nullptr, 0, block) == frame_dump::ConsoleBlockResult::no_parameters,
               "an empty console placement feeds nothing");
        frame_dump::ConsoleBlockParameter overlap[2]{};
        overlap[0].console_offset = 8;
        overlap[0].dwords = 2;
        overlap[1].console_offset = 9;
        overlap[1].dwords = 1;
        expect(frame_dump::graphics_console_block(overlap, 2, block) == frame_dump::ConsoleBlockResult::overlap,
               "two parameters on one console dword are refused, not guessed");
        frame_dump::ConsoleBlockParameter spill{};
        spill.console_offset = 31;
        spill.dwords = 2;
        expect(frame_dump::graphics_console_block(&spill, 1, block) == frame_dump::ConsoleBlockResult::spill,
               "a placement past s31 is refused");
        frame_dump::ConsoleBlockParameter wide{};
        wide.console_offset = 8;
        wide.dwords = 5;
        expect(frame_dump::graphics_console_block(&wide, 1, block) == frame_dump::ConsoleBlockResult::out_of_range,
               "a dword count past a parameter's value array is refused");
        expect(std::strlen(frame_dump::console_block_result_name(frame_dump::ConsoleBlockResult::overlap)) != 0,
               "a refused console placement has a name");
    }

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

    // The upscale stand-in (docs/upscale_standin.md): the measured predicate, every named refusal, the measured
    // full-rect geometry (4K colour image -> 4K presented target), the non-1:1 stretch mapping the shader's formula
    // gives, the project mapping control's coordinate ramp, and the push dwords the draw's user data carries.
    namespace su = palmetto::upscale_standin;
    expect(su::is_compose_pass(true, su::compose_image_samples), "the six-sample pixel stage is the measured upscale pass");
    expect(!su::is_compose_pass(true, 0), "the constant/background pass (0 image samples) is not the upscale pass");
    expect(!su::is_compose_pass(false, su::compose_image_samples), "a depth-only pipeline is not the upscale pass");
    expect(!su::is_compose_pass(true, su::compose_image_samples - 1), "one image short is not the measured shape");
    const auto inputs = [](bool on, bool ps, std::uint32_t samples, bool colour, std::uint32_t cw, std::uint32_t ch,
                           std::uint32_t tw, std::uint32_t th) {
        su::Inputs in;
        in.switch_on = on;
        in.has_pixel_stage = ps;
        in.pixel_image_samples = samples;
        in.colour_resolved = colour;
        in.colour_width = cw;
        in.colour_height = ch;
        in.target_width = tw;
        in.target_height = th;
        return in;
    };
    expect(su::plan(inputs(false, true, 6, true, 3840, 2160, 3840, 2160)).refusal == su::Refusal::switch_off,
           "the stand-in is off until its switch is set");
    expect(su::plan(inputs(true, true, 6, true, 3840, 2160, 3840, 2160)).draw,
           "the measured upscale draw with a resolved colour image takes the stand-in");
    expect(su::plan(inputs(true, true, 0, true, 3840, 2160, 3840, 2160)).refusal == su::Refusal::not_compose_pass,
           "the constant pass is refused, not stretched");
    expect(su::plan(inputs(true, true, 6, false, 0, 0, 3840, 2160)).refusal == su::Refusal::no_colour_image,
           "no resolved colour image is a named refusal");
    expect(su::plan(inputs(true, true, 6, true, 0, 2160, 3840, 2160)).refusal == su::Refusal::colour_extent_degenerate,
           "a zero-width colour image is a named refusal");
    expect(su::plan(inputs(true, true, 6, true, 3840, 0, 3840, 2160)).refusal == su::Refusal::colour_extent_degenerate,
           "a zero-height colour image is a named refusal");
    expect(su::plan(inputs(true, true, 6, true, 3840, 2160, 0, 2160)).refusal == su::Refusal::target_extent_degenerate,
           "a zero-width presented target is a named refusal");
    expect(std::strlen(su::refusal_name(su::Refusal::not_compose_pass)) != 0, "a refusal names its reason");
    expect(std::strcmp(su::refusal_name(su::Refusal::none), "none") == 0, "the taken plan's reason is none");
    const su::Plan measured = su::plan(inputs(true, true, 6, true, 3840, 2160, 3840, 2160));
    expect(measured.draw && measured.refusal == su::Refusal::none, "the measured plan records a draw");
    expect(measured.push.src[0] == 0.0f && measured.push.src[1] == 0.0f && measured.push.src[2] == 3840.0f &&
               measured.push.src[3] == 2160.0f,
           "the source rectangle is the whole colour image");
    expect(measured.push.dst[0] == 0.0f && measured.push.dst[1] == 0.0f && measured.push.dst[2] == 3840.0f &&
               measured.push.dst[3] == 2160.0f,
           "the destination rectangle is the whole presented target");
    expect(measured.push.mode == su::mode_sample_image && su::samples_image(measured),
           "the shipping stand-in samples the colour image (it never invents colour)");
    // The measured 4K->4K case is a 1:1 stretch: a destination pixel's centre maps to its own texel.
    std::int32_t tx = -1, ty = -1;
    su::sample_texel(measured.push, 0, 0, tx, ty);
    expect(tx == 0 && ty == 0, "the first destination pixel samples the colour image's first texel");
    su::sample_texel(measured.push, 3839, 2159, tx, ty);
    expect(tx == 3839 && ty == 2159, "the last destination pixel samples the colour image's last texel");
    su::sample_texel(measured.push, 1919, 1079, tx, ty);
    expect(tx == 1919 && ty == 1079, "a mid destination pixel samples the matching texel (no offset, no crop)");
    // A lower-resolution colour image is stretched: two destination pixels per source texel, nearest filtered.
    const su::Plan half = su::plan(inputs(true, true, 6, true, 1920, 1080, 3840, 2160));
    expect(half.draw && half.push.src[2] == 1920.0f && half.push.dst[2] == 3840.0f,
           "a 1080p colour image over a 4K target keeps both rectangles");
    su::sample_texel(half.push, 0, 0, tx, ty);
    expect(tx == 0 && ty == 0, "the stretch's first destination pixel samples source texel 0");
    su::sample_texel(half.push, 1, 0, tx, ty);
    expect(tx == 0, "the second destination pixel still samples source texel 0");
    su::sample_texel(half.push, 2, 0, tx, ty);
    expect(tx == 1, "the third destination pixel samples source texel 1");
    su::sample_texel(half.push, 3839, 2159, tx, ty);
    expect(tx == 1919 && ty == 1079, "the stretch's last destination pixel stays inside the source image");
    // The project's mapping control is a diagnostic arm, not the shipping plan: same geometry, a coordinate instead of
    // a colour sample, so a boot whose colour image is black can still measure the rectangles.
    const su::Plan ramp = su::mapping_control(measured);
    expect(!su::samples_image(ramp) && ramp.push.mode == su::mode_mapping_control,
           "the mapping control is not the shipping (sampling) plan");
    expect(ramp.push.src[2] == measured.push.src[2] && ramp.push.dst[3] == measured.push.dst[3],
           "the mapping control keeps the measured geometry");
    float colour_px[4] = {};
    su::mapping_control_colour(ramp.push, 0, 0, colour_px);
    const float first_u = colour_px[0];
    expect(first_u > 0.0f && first_u < 0.001f && colour_px[1] > 0.0f && colour_px[1] < 0.001f,
           "the control's first pixel writes the source coordinate near 0");
    float last = 0.0f;
    su::mapping_control_colour(ramp.push, 3839, 2159, colour_px);
    last = colour_px[0];
    expect(last > 0.999f && colour_px[1] > 0.999f, "the control's last pixel writes the source coordinate near 1");
    expect(std::fabs(first_u + last - 1.0f) < 0.001f,
           "the control's ramp spans the destination rectangle (mean about 0.5 across the row)");
    // The push block is the draw's user data: a plan round-trips through it exactly.
    std::uint32_t dwords[16] = {};
    su::write_push(measured.push, dwords);
    su::Push restored{};
    std::memcpy(&restored, dwords, sizeof(restored));
    expect(std::memcmp(&restored, &measured.push, sizeof(restored)) == 0, "the push dwords round-trip the rectangles");
    expect(su::pipeline_id != palmetto::frame_dump::control_pipeline_id &&
               su::pipeline_id != palmetto::frame_dump::ps_control_pipeline_id,
           "the stand-in's pipeline id is its own (not one of the frame dump's controls)");

    // Series-S kind-7 consumer readiness (docs/fsr_route.md): the measured output/target formats and extents define
    // only a candidate whole-rect mapping. The title has not yet shown a kind-7 output entering a compose draw, and the
    // filter/numeric interpretation remain unknown, so the real readiness input must refuse to draw by name.
    namespace k7 = palmetto::upscale_kind7;
    expect(!k7::parse_switch(nullptr) && !k7::parse_switch("") && !k7::parse_switch("0") &&
               !k7::parse_switch("on") && !k7::parse_switch("measuredly") && k7::parse_switch("measured") &&
               k7::parse_switch(" MeAsUrEd \t"),
           "the kind-7 consumer requires the exact opt-in token `measured`");
    const auto kind7_inputs = [] {
        k7::Inputs in;
        in.switch_on = true;
        in.provenance = k7::Provenance::series_s_kind7_output;
        in.source_resolved = true;
        in.source_format = k7::Format::r9g9b9e5_ufloat;
        in.source_sampled = true;
        in.source_width = k7::source_width;
        in.source_height = k7::source_height;
        in.target_format = k7::Format::r16g16b16a16_float;
        in.target_width = k7::target_width;
        in.target_height = k7::target_height;
        return in;
    };
    const auto observed_kind7_inputs = [&] {
        auto in = kind7_inputs();
        in.consumer_observed = true;
        in.source_layout = k7::SourceLayout::shader_read_only;
        in.has_source_rect = true;
        in.source_rect = {0.0f, 0.0f, float(k7::source_width), float(k7::source_height)};
        in.has_target_rect = true;
        in.target_rect = {0.0f, 0.0f, float(k7::target_width), float(k7::target_height)};
        return in;
    };
    auto kind7_off = kind7_inputs();
    kind7_off.switch_on = false;
    const k7::Plan kind7_off_plan = k7::plan(kind7_off);
    expect(kind7_off_plan.refusal == k7::Refusal::switch_off && !kind7_off_plan.candidate_mapping &&
               !kind7_off_plan.compose,
           "switch-off refuses before producing candidate geometry or a draw");
    auto no_provenance = kind7_inputs();
    no_provenance.provenance = k7::Provenance::unknown;
    expect(k7::plan(no_provenance).refusal == k7::Refusal::no_kind7_provenance,
           "format and extent alone never establish kind-7 provenance");
    const k7::Plan compose_adapter_candidate = k7::plan_present_candidate(
        kind7_inputs(), VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R16G16B16A16_SFLOAT);
    expect(compose_adapter_candidate.refusal == k7::Refusal::consumer_not_observed &&
               compose_adapter_candidate.candidate_mapping && !compose_adapter_candidate.compose,
           "the Vulkan compose adapter validates formats but still refuses the unobserved consumer link");
    auto adapter_no_provenance = kind7_inputs();
    adapter_no_provenance.provenance = k7::Provenance::unknown;
    expect(k7::plan_present_candidate(adapter_no_provenance, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_FORMAT_R16G16B16A16_SFLOAT).refusal ==
               k7::Refusal::no_kind7_provenance,
           "the compose format adapter does not manufacture kind-7 provenance");
    expect(k7::plan_present_candidate(kind7_inputs(), VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_FORMAT_R16G16B16A16_SFLOAT).refusal ==
               k7::Refusal::source_format_unmeasured,
           "a different Vulkan source view format has a named refusal");
    expect(k7::plan_present_candidate(kind7_inputs(), VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R16G16B16A16_SFLOAT).refusal == k7::Refusal::source_not_sampled,
           "the measured E5 source is refused if the Vulkan view lacks sampled-image support");
    expect(k7::plan_present_candidate(kind7_inputs(), VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_FORMAT_R8G8B8A8_UNORM).refusal ==
               k7::Refusal::target_format_unmeasured,
           "the compose adapter refuses a target format outside the measured RGBA16F target");
    auto adapter_bad_layout = kind7_inputs();
    adapter_bad_layout.consumer_observed = true;
    expect(k7::plan_present_candidate(adapter_bad_layout, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_FORMAT_R16G16B16A16_SFLOAT).refusal == k7::Refusal::source_layout_unsupported,
           "the compose adapter names a source layout that the compositor cannot sample");
    auto unresolved = kind7_inputs();
    unresolved.source_resolved = false;
    expect(k7::plan(unresolved).refusal == k7::Refusal::source_unresolved,
           "an unresolved kind-7 source is refused");
    auto wrong_source_format = kind7_inputs();
    wrong_source_format.source_format = k7::Format::other;
    expect(k7::plan(wrong_source_format).refusal == k7::Refusal::source_format_unmeasured,
           "a non-RGB9E5 source format is refused");
    auto empty_source = kind7_inputs();
    empty_source.source_width = 0;
    expect(k7::plan(empty_source).refusal == k7::Refusal::source_extent_degenerate,
           "a zero-width source is a named refusal");
    auto unmeasured_source = kind7_inputs();
    unmeasured_source.source_width = 1280;
    expect(k7::plan(unmeasured_source).refusal == k7::Refusal::source_extent_unmeasured,
           "a non-1920x1080 source shape is refused");
    auto wrong_target_format = kind7_inputs();
    wrong_target_format.target_format = k7::Format::other;
    expect(k7::plan(wrong_target_format).refusal == k7::Refusal::target_format_unmeasured,
           "a non-RGBA16F target format is refused");
    auto empty_target = kind7_inputs();
    empty_target.target_height = 0;
    expect(k7::plan(empty_target).refusal == k7::Refusal::target_extent_degenerate,
           "a zero-height target is a named refusal");
    auto unmeasured_target = kind7_inputs();
    unmeasured_target.target_width = 1920;
    expect(k7::plan(unmeasured_target).refusal == k7::Refusal::target_extent_unmeasured,
           "a non-3840x2160 target shape is refused");
    auto cropped_source = observed_kind7_inputs();
    cropped_source.source_rect = {1.0f, 0.0f, float(k7::source_width), float(k7::source_height)};
    expect(k7::plan(cropped_source).refusal == k7::Refusal::rect_unmeasured,
           "an unmeasured source crop is refused");
    auto placed_target = observed_kind7_inputs();
    placed_target.target_rect = {0.0f, 0.0f, float(k7::target_width - 1), float(k7::target_height)};
    expect(k7::plan(placed_target).refusal == k7::Refusal::rect_unmeasured,
           "an unmeasured target rectangle is refused");
    auto missing_rects = kind7_inputs();
    missing_rects.consumer_observed = true;
    missing_rects.source_layout = k7::SourceLayout::shader_read_only;
    missing_rects.filter = k7::Filter::point;
    missing_rects.numeric_mode = k7::NumericMode::passthrough;
    expect(k7::plan(missing_rects).refusal == k7::Refusal::rect_unmeasured,
           "an observed consumer with unmeasured rectangles cannot reuse the candidate mapping");
    auto unknown_layout = observed_kind7_inputs();
    unknown_layout.source_layout = k7::SourceLayout::unmeasured;
    expect(k7::plan(unknown_layout).refusal == k7::Refusal::source_layout_unmeasured,
           "an observed consumer with no sampled-image layout evidence is refused");
    auto bad_layout = observed_kind7_inputs();
    bad_layout.source_layout = k7::SourceLayout::other;
    expect(k7::plan(bad_layout).refusal == k7::Refusal::source_layout_unsupported,
           "the generic compose contract rejects a layout other than SHADER_READ_ONLY_OPTIMAL or GENERAL");
    const k7::Plan candidate = k7::plan(kind7_inputs());
    expect(candidate.candidate_mapping && !candidate.compose &&
               candidate.refusal == k7::Refusal::consumer_not_observed,
           "measured shapes expose candidate geometry but no unobserved consumer is drawn");
    expect(candidate.source_rect.left == 0.0f && candidate.source_rect.top == 0.0f &&
               candidate.source_rect.right == 1920.0f && candidate.source_rect.bottom == 1080.0f &&
               candidate.target_rect.left == 0.0f && candidate.target_rect.top == 0.0f &&
               candidate.target_rect.right == 3840.0f && candidate.target_rect.bottom == 2160.0f,
           "candidate mapping uses the full 1080p source and full measured 4K target");
    auto unknown_filter = observed_kind7_inputs();
    expect(k7::plan(unknown_filter).refusal == k7::Refusal::filter_unmeasured,
           "the kind-7 consumer's unobserved filter is refused");
    auto linear_filter = observed_kind7_inputs();
    linear_filter.filter = k7::Filter::linear;
    expect(k7::plan(linear_filter).refusal == k7::Refusal::filter_not_supported,
           "the stand-in refuses a filter it does not implement");
    auto unknown_numeric_mode = observed_kind7_inputs();
    unknown_numeric_mode.filter = k7::Filter::point;
    expect(k7::plan(unknown_numeric_mode).refusal == k7::Refusal::numeric_mode_unmeasured,
           "the output numeric interpretation must be measured");
    auto transformed_numeric_mode = observed_kind7_inputs();
    transformed_numeric_mode.filter = k7::Filter::point;
    transformed_numeric_mode.numeric_mode = k7::NumericMode::transformed;
    expect(k7::plan(transformed_numeric_mode).refusal == k7::Refusal::numeric_transform_unimplemented,
           "a requested colour transform is refused rather than guessed");
    auto synthetic_ready_input = observed_kind7_inputs();
    synthetic_ready_input.source_layout = k7::SourceLayout::general;
    synthetic_ready_input.filter = k7::Filter::point;
    synthetic_ready_input.numeric_mode = k7::NumericMode::passthrough;
    const k7::Plan synthetic_ready = k7::plan_present_candidate(
        synthetic_ready_input, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R16G16B16A16_SFLOAT);
    expect(synthetic_ready.compose && synthetic_ready.refusal == k7::Refusal::none,
           "an explicitly complete synthetic input can exercise the opt-in plan through the Vulkan adapter");
    expect(k7::plan_present_candidate(synthetic_ready_input, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                      VK_FORMAT_R16G16B16A16_SFLOAT).compose,
           "the Vulkan adapter accepts the compositor's explicitly supported GENERAL layout");
    std::int32_t k7_x = -1, k7_y = -1;
    expect(k7::map_point_texel(synthetic_ready, 0, 0, k7_x, k7_y) && k7_x == 0 && k7_y == 0,
           "the first 4K pixel maps to source texel 0,0");
    expect(k7::map_point_texel(synthetic_ready, 1, 0, k7_x, k7_y) && k7_x == 0,
           "the second 4K pixel still maps to source texel 0");
    expect(k7::map_point_texel(synthetic_ready, 2, 0, k7_x, k7_y) && k7_x == 1,
           "the third 4K pixel advances to source texel 1");
    expect(k7::map_point_texel(synthetic_ready, 3839, 2159, k7_x, k7_y) && k7_x == 1919 && k7_y == 1079,
           "the last 4K pixel maps inside the last 1080p source texel");
    expect(!k7::map_point_texel(synthetic_ready, 3840, 0, k7_x, k7_y),
           "a destination coordinate outside the target is refused");
    expect(std::strcmp(k7::refusal_name(k7::Refusal::consumer_not_observed),
                       "no measured link from the kind-7 output to this compose draw") == 0,
           "the readiness refusal names the missing consumer observation");

    // ---- Title pages a command names directly (docs/vertex_buffer_va.md) --------------------------------------
    // A draw's vertex views and index view and a WriteBufferImmediate address are the command arguments the replay
    // resolves through buffer_at. The submission-time scan offers exactly those spans to the device's import, and the
    // bounded rule refuses a span inside the 32-bit arena window, an oversized span and a spent budget while it accepts
    // the measured stage-3 shape.
    {
        CommandList title_va_list;
        Draw indexed_draw;
        indexed_draw.pipeline = 0x2000;
        indexed_draw.count = 30;
        indexed_draw.instances = 1;
        indexed_draw.indexed = true;
        indexed_draw.index.va = 0x18d58d20380ull;
        indexed_draw.index.size = 0x60000;
        indexed_draw.index.format = 57;
        auto views = std::make_shared<DrawViews>();
        views->vertex_buffers[0].va = 0x18d58c00300ull;
        views->vertex_buffers[0].size = 0x120000;
        views->vertex_buffers[0].stride = 16;
        views->vertex_buffers[3].va = 0x18d58c00380ull;
        views->vertex_buffers[3].size = 0x11ff80;
        views->vertex_buffers[3].stride = 16;
        indexed_draw.views = views;
        expect(title_va_list.draw(indexed_draw), "record the measured stage-3 indexed draw");
        expect(title_va_list.write_immediate(WriteImmediate{0x18d58c00300ull, 0x5A5A5A5A, 0}), "record its write");
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected = {
            {0x18d58c00300ull, 0x120000ull},
            {0x18d58c00380ull, 0x11ff80ull},
            {0x18d58d20380ull, 0x60000ull},
            {0x18d58c00300ull, 4ull}};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> requests;
        title_buffers::collect_requests(title_va_list, requests);
        expect(requests == expected, "the scan collects the vertex, index and write-immediate VAs in command order");
        const CommandList empty_list;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> none;
        title_buffers::collect_requests(empty_list, none);
        expect(none.empty(), "a list that names no buffer VA asks for no import");
        expect(title_buffers::refusal(0x18d58c00300ull, 0x120000ull, 0, 0) == nullptr,
               "the measured stage-3 span may be imported at first use");
        expect(title_buffers::refusal(0x10000ull, 0x1000ull, 0, 0) != nullptr,
               "a VA inside the 32-bit arena window stays refused");
        expect(title_buffers::refusal(0x18d58c00300ull, title_buffers::kMaxRangeBytes + 1, 0, 0) != nullptr,
               "a span over the per-range bound stays refused");
        expect(title_buffers::refusal(0x18d58c00300ull, 0x1000ull, title_buffers::kMaxRanges, 0) != nullptr,
               "a spent range budget keeps the span refused");
        expect(title_buffers::refusal(0x18d58c00300ull, 0x1000ull, 0, title_buffers::kMaxTotalBytes) != nullptr,
               "a spent byte budget keeps the span refused");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> bounded;
        for (unsigned i = 0; i < 3 * title_buffers::kMaxRanges; ++i) bounded.emplace_back(0x18d00000000ull + i * 0x1000, 4);
        for (const auto& [va, bytes] : bounded) title_buffers::collect_requests(title_va_list, bounded);
        expect(bounded.size() == 3 * title_buffers::kMaxRanges,
               "the scan appends nothing past its own collection (the budget is the caller's)");
    }

    {
        // docs/vertex_buffer_va.md section 10: the content-frame readback arming rule. The draw PALMETTO_READBACK_AT_DRAW
        // lists arms; so does every draw of a submission whose lists imported title pages at first use (the stage-3
        // content submits, which no present follows); neither stays off.
        expect(frame_dump::draw_readback_arms(true, false), "a listed draw arms its target readback");
        expect(frame_dump::draw_readback_arms(false, true), "a title-import submission arms every draw it holds");
        expect(frame_dump::draw_readback_arms(true, true), "both triggers together arm the readback");
        expect(!frame_dump::draw_readback_arms(false, false), "an unlisted draw of an ordinary submission stays unread");
    }

    {
        // docs/vertex_buffer_va.md section 17: the content-probe arming rule and its bound. Off by default, only a draw
        // of a title-import submission (the stage-3 content window) arms the per-draw diagnostics, and the ordinal bound
        // stops the arming; the default bound is nine window draws (the measured three 3-draw content submissions) and
        // the hard bound is twelve.
        expect(!frame_dump::content_probe_enabled_from(nullptr), "an unset content probe stays off");
        expect(!frame_dump::content_probe_enabled_from(""), "an empty content probe value stays off");
        expect(!frame_dump::content_probe_enabled_from("0"), "a zero content probe value stays off");
        expect(frame_dump::content_probe_enabled_from("1"), "a set content probe arms");
        expect(!frame_dump::content_probe_arms(false, true, 0, 9), "the probe off arms no content draw");
        expect(!frame_dump::content_probe_arms(true, false, 0, 9), "a draw outside the content window stays unarmed");
        expect(frame_dump::content_probe_arms(true, true, 0, 9), "the first window draw arms");
        expect(frame_dump::content_probe_arms(true, true, 8, 9), "the last draw inside the bound arms");
        expect(!frame_dump::content_probe_arms(true, true, 9, 9), "the draw at the bound stays unarmed");
        expect(!frame_dump::content_probe_arms(true, true, 0, 0), "a zero bound arms nothing");
        expect(frame_dump::content_probe_draw_limit_from(nullptr) == 9, "the content probe bound defaults to 9");
        expect(frame_dump::content_probe_draw_limit_from("") == 9, "an empty bound keeps the default");
        expect(frame_dump::content_probe_draw_limit_from("4") == 4, "the bound is settable");
        expect(frame_dump::content_probe_draw_limit_from("999") == 12, "the bound is capped at 12");
    }

    {
        // docs/vertex_buffer_va.md section 20: the fixed control-arm ordinal. The measured two-submission window never
        // reaches the compose draw under the section-17 cycling rule (submission 1 arms draw 0, submission 2 draw 1), so
        // the ordinal is settable; a fixed ordinal arms only the first window submission - the later ones stay
        // uncontrolled, so their readbacks are the frame's own content. Unset keeps the cycling rule.
        expect(frame_dump::content_probe_arm_from(nullptr) == -1, "an unset arm ordinal keeps the cycling control");
        expect(frame_dump::content_probe_arm_from("") == -1, "an empty arm ordinal keeps the cycling control");
        expect(frame_dump::content_probe_arm_from("0") == 0, "the constant draw's ordinal is settable");
        expect(frame_dump::content_probe_arm_from("2") == 2, "the compose draw's ordinal is settable");
        expect(frame_dump::content_probe_arm_from("11") == 11, "the last promised ordinal is settable");
        expect(frame_dump::content_probe_arm_from("12") == -1, "an ordinal past the window bound is refused");
        expect(frame_dump::content_probe_arm_from("-1") == -1, "a negative ordinal is refused");
        expect(frame_dump::content_probe_arm_from("x") == -1, "a non-numeric ordinal is refused");
        expect(frame_dump::content_probe_arm_ordinal(0, 2, 3) == 2, "the fixed ordinal names the third draw");
        expect(frame_dump::content_probe_arm_ordinal(9, 2, 3) == 2, "the fixed ordinal ignores the window counter");
        expect(frame_dump::content_probe_arm_ordinal(0, 7, 3) == 2, "a fixed ordinal past the draws clamps to the last");
        expect(frame_dump::content_probe_arm_ordinal(2, -1, 3) == 2, "the cycling rule takes the window counter");
        expect(frame_dump::content_probe_arm_ordinal(9, -1, 3) == 2, "the cycling ordinal clamps to the last draw");
        expect(frame_dump::content_probe_arm_ordinal(0, 2, 0) == 0, "a submission with no draw has no ordinal");
        expect(frame_dump::content_probe_window_arm(true, true, 0, 2), "the fixed ordinal arms the first submission");
        expect(!frame_dump::content_probe_window_arm(true, true, 1, 2),
               "the fixed ordinal leaves later window submissions uncontrolled");
        expect(frame_dump::content_probe_window_arm(true, true, 2, -1), "the cycling rule arms the third submission");
        expect(!frame_dump::content_probe_window_arm(true, true, 3, -1), "the cycling rule stops after three");
        expect(!frame_dump::content_probe_window_arm(false, true, 0, 2), "an off probe arms nothing");
        expect(!frame_dump::content_probe_window_arm(true, false, 0, 2), "a draw outside the window stays unarmed");
    }

    {
        // docs/vertex_buffer_va.md section 22: the per-draw cull-only variant for content-window draws. The switch is
        // off by default and only a draw of the title-import content window is selected; mesh draws, draws that already
        // take the unbound-attachment variant, and pipelines with no cull-only variant offered are refused by name. The
        // pure selection is what prepare_draw feeds from the window gate, the pipeline and the shim's offer.
        expect(!frame_dump::content_cull_variant_enabled_from(nullptr), "an unset cull variant switch stays off");
        expect(!frame_dump::content_cull_variant_enabled_from(""), "an empty cull variant value stays off");
        expect(!frame_dump::content_cull_variant_enabled_from("0"), "a zero cull variant value stays off");
        expect(!frame_dump::content_cull_variant_enabled_from("n"), "a no cull variant value stays off");
        expect(frame_dump::content_cull_variant_enabled_from("1"), "a set cull variant switch arms");
        expect(frame_dump::content_cull_variant_enabled_from("true"), "any other non-zero value arms");
        // The measured pair (section 21): cull none, clockwise front; the key of that pair is 0.
        expect(frame_dump::content_cull_variant_cull_mode == 0u, "the measured cull mode is NONE");
        expect(!frame_dump::content_cull_variant_front_ccw, "the measured front face is clockwise");
        expect(frame_dump::cull_variant_key(frame_dump::content_cull_variant_cull_mode,
                                            frame_dump::content_cull_variant_front_ccw) == 0u,
               "the measured pair is the cache's zero key");
        // Variant identity: the key carries BOTH axes the variant changes (mode and front face), and cull modes are
        // masked to the two enum bits. The measured pair's key 0 and (front, clockwise)'s key 1 are exactly the
        // with_ps cache's two keys, which is why the shim keeps the cull-only variants in a separate map.
        expect(frame_dump::cull_variant_key(0, false) == 0u, "NONE/clockwise keys 0");
        expect(frame_dump::cull_variant_key(0, true) == 0x10u, "NONE/counter-clockwise keys 0x10");
        expect(frame_dump::cull_variant_key(1, false) == 1u, "front/clockwise keys 1, the PS cache's cull_none key");
        expect(frame_dump::cull_variant_key(2, false) == 2u, "back/clockwise keys 2");
        expect(frame_dump::cull_variant_key(3, true) == 0x13u, "front-and-back/counter-clockwise keys 0x13");
        expect(frame_dump::cull_variant_key(0xFFFFFFFCu, false) == 0u, "the cull mode is masked to its enum bits");
        expect(frame_dump::cull_variant_key(1, false) != frame_dump::cull_variant_key(1, true),
               "the front face is part of the variant's identity");
        expect(frame_dump::cull_variant_key(1, false) != frame_dump::cull_variant_key(2, false),
               "the cull mode is part of the variant's identity");
        {
            // Six pairs the cache can hold at once, all distinct.
            const std::uint32_t keys[] = {
                frame_dump::cull_variant_key(0, false), frame_dump::cull_variant_key(0, true),
                frame_dump::cull_variant_key(1, false), frame_dump::cull_variant_key(1, true),
                frame_dump::cull_variant_key(2, false), frame_dump::cull_variant_key(2, true)};
            bool distinct = true;
            for (std::size_t i = 0; i < std::size(keys); ++i)
                for (std::size_t j = i + 1; j < std::size(keys); ++j) distinct = distinct && keys[i] != keys[j];
            expect(distinct, "the (cull mode, front face) cache keys are distinct");
        }
        // The window selection. The switch off is the fallback that keeps every draw on the documented stand-in.
        expect(frame_dump::content_cull_variant_selection(false, true, false, false, true) ==
                   frame_dump::ContentCullVariant::off,
               "the switch off selects nothing even inside the window");
        expect(frame_dump::content_cull_variant_selection(false, false, false, false, false) ==
                   frame_dump::ContentCullVariant::off,
               "the switch off is the fallback whatever the draw is");
        expect(frame_dump::content_cull_variant_selection(true, false, false, false, true) ==
                   frame_dump::ContentCullVariant::outside_window,
               "a draw outside the content window keeps its pipeline");
        expect(frame_dump::content_cull_variant_selection(true, true, true, false, true) ==
                   frame_dump::ContentCullVariant::mesh,
               "a mesh draw is refused: no mesh draw was measured");
        expect(frame_dump::content_cull_variant_selection(true, true, false, true, true) ==
                   frame_dump::ContentCullVariant::unbound,
               "a draw that takes the unbound-attachment variant is refused by name");
        expect(frame_dump::content_cull_variant_selection(true, true, false, false, false) ==
                   frame_dump::ContentCullVariant::unsupported,
               "a pipeline with no cull-only variant offered is refused by name");
        expect(frame_dump::content_cull_variant_selection(true, true, false, false, true) ==
                   frame_dump::ContentCullVariant::apply,
               "a window draw with the switch set and an offer applies the measured pair");
        expect(frame_dump::content_cull_variant_selection(true, true, true, true, true) ==
                   frame_dump::ContentCullVariant::mesh,
               "a mesh draw is refused before the unbound question");
        expect(frame_dump::content_cull_variant_selection(true, true, false, true, false) ==
                   frame_dump::ContentCullVariant::unbound,
               "the unbound refusal comes before the offer question");
        // The rule is total: every combination returns a decision, and `apply` is exactly the arm above.
        unsigned applies = 0;
        for (unsigned bits = 0; bits < 16; ++bits) {
            const bool enabled = (bits & 1u) != 0, window = (bits & 2u) != 0;
            const bool mesh = (bits & 4u) != 0, unbound = (bits & 8u) != 0;
            for (const bool offered : {false, true}) {
                const frame_dump::ContentCullVariant selection =
                    frame_dump::content_cull_variant_selection(enabled, window, mesh, unbound, offered);
                if (selection == frame_dump::ContentCullVariant::apply) ++applies;
                if (selection == frame_dump::ContentCullVariant::apply)
                    expect(enabled && window && !mesh && !unbound && offered,
                           "only the armed, in-window, plain offer applies");
            }
        }
        expect(applies == 1, "exactly one of the reviewable combinations applies");
        // Each decision has a distinct name, so a boot's refusal line is not ambiguous.
        const frame_dump::ContentCullVariant decisions[] = {
            frame_dump::ContentCullVariant::off, frame_dump::ContentCullVariant::outside_window,
            frame_dump::ContentCullVariant::mesh, frame_dump::ContentCullVariant::unbound,
            frame_dump::ContentCullVariant::unsupported, frame_dump::ContentCullVariant::apply};
        bool named = true;
        for (std::size_t i = 0; i < std::size(decisions); ++i) {
            const char* name = frame_dump::content_cull_variant_name(decisions[i]);
            named = named && name && *name;
            for (std::size_t j = i + 1; j < std::size(decisions); ++j)
                named = named && std::strcmp(name, frame_dump::content_cull_variant_name(decisions[j])) != 0;
        }
        expect(named, "every selection has its own non-empty name");
    }

    {
        // docs/vertex_buffer_va.md section 12: the two opt-in submission pacing knobs' truth table. Both are off by
        // default, "0" is off, and the delay is decimal microseconds; the queue reads them once at startup.
        const palmetto::SubmissionPace none = palmetto::submission_pace_from(nullptr, nullptr);
        expect(!none.idle_after_execute && !none.idle_at_content_boundary && none.delay_us == 0,
               "no value leaves all pacing knobs off");
        const palmetto::SubmissionPace zero = palmetto::submission_pace_from("0", "0", "0");
        expect(!zero.idle_after_execute && !zero.idle_at_content_boundary && zero.delay_us == 0,
               "\"0\" leaves all pacing knobs off");
        const palmetto::SubmissionPace idle = palmetto::submission_pace_from("1", nullptr);
        expect(idle.idle_after_execute && !idle.idle_at_content_boundary && idle.delay_us == 0,
               "only the idle knob is set");
        const palmetto::SubmissionPace delayed = palmetto::submission_pace_from(nullptr, "1000");
        expect(!delayed.idle_after_execute && !delayed.idle_at_content_boundary && delayed.delay_us == 1000,
               "the delay is decimal microseconds");
        const palmetto::SubmissionPace both = palmetto::submission_pace_from("1", "25");
        expect(both.idle_after_execute && both.delay_us == 25 && !both.idle_at_content_boundary,
               "both knobs together keep both values");
        // docs/vertex_buffer_va.md section 13: the content-boundary drain is its own knob, so the narrow arm is chosen
        // without also draining every submission.
        const palmetto::SubmissionPace content = palmetto::submission_pace_from(nullptr, nullptr, "1");
        expect(content.idle_at_content_boundary && !content.idle_after_execute && content.delay_us == 0,
               "the content-boundary drain arms only itself");
    }

    {
        // docs/vertex_buffer_va.md section 13: the content-frame readback window. The default is the measured three
        // submissions; PALMETTO_READBACK_AFTER widens it for later stage-3 frames and is bounded; an unusable value
        // falls back to the default, so an unset or malformed environment keeps the measured arm.
        expect(palmetto::title_import_capture_window_from(nullptr) == 3, "the content readback window defaults to 3");
        expect(palmetto::title_import_capture_window_from("") == 3, "an empty window value keeps the default");
        expect(palmetto::title_import_capture_window_from("0") == 3, "a zero window value keeps the default");
        expect(palmetto::title_import_capture_window_from("12") == 12, "PALMETTO_READBACK_AFTER sets the window");
        expect(palmetto::title_import_capture_window_from("9999") == 512, "the window is bounded");
    }

    {
        // docs/peer_port_streaming.md section 15: the widened flight window is explicitly opt-in, keeps 64 when off,
        // defaults to 96 when on, separates the existing COPY lane, and never crosses its [64,128] hard range.
        using palmetto::SubmissionFlightSettings;
        using palmetto::SubmissionLane;
        const auto off = SubmissionFlightSettings::parse(nullptr, "128", "72");
        expect(!off.enabled && off.bound(SubmissionLane::direct) == 64 && off.bound(SubmissionLane::compute) == 64 &&
                   off.bound(SubmissionLane::copy) == 64,
               "unset overlap switch ignores per-lane overrides and keeps the legacy 64 bound");
        const auto zero = SubmissionFlightSettings::parse("0", "128", "80");
        expect(!zero.enabled && zero.bound(SubmissionLane::direct) == 64 && zero.bound(SubmissionLane::copy) == 64,
               "zero overlap switch leaves every lane at the legacy bound");
        const auto defaults = SubmissionFlightSettings::parse("1", nullptr, nullptr);
        expect(defaults.enabled && defaults.bound(SubmissionLane::direct) == 96 &&
                   defaults.bound(SubmissionLane::compute) == 96 && defaults.bound(SubmissionLane::copy) == 96,
               "opt-in defaults each lane to 96 command buffers");
        const auto separate = SubmissionFlightSettings::parse("1", "80", "72");
        expect(separate.bound(SubmissionLane::direct) == 80 && separate.bound(SubmissionLane::compute) == 80 &&
                   separate.bound(SubmissionLane::copy) == 72,
               "COPY has an independent bound while direct and compute share theirs");
        const auto clamped = SubmissionFlightSettings::parse("1", "0", "9999");
        expect(clamped.bound(SubmissionLane::direct) == 64 && clamped.bound(SubmissionLane::copy) == 128,
               "parsed bounds clamp to the legacy minimum and the hard maximum");
        const auto malformed = SubmissionFlightSettings::parse("1", "not-a-number", "96tail");
        expect(malformed.bound(SubmissionLane::direct) == 96 && malformed.bound(SubmissionLane::copy) == 96,
               "malformed bound values fall back to the enabled default");
        SubmissionFlightSettings manual{};
        manual.enabled = true;
        manual.direct_bound = 999;
        manual.copy_bound = 1;
        expect(manual.bound(SubmissionLane::direct) == 128 && manual.bound(SubmissionLane::copy) == 64,
               "explicit settings cannot bypass the hard bound");
    }

    if (failures) return 1;
    std::printf("frame_dump_test: command kinds, ordered writers, kind-7 consumer readiness, table windows and "
                "submission-flight policy exact\n");
    return 0;
}
