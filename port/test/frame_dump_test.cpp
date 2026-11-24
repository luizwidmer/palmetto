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

    if (failures) return 1;
    std::printf("frame_dump_test: command kinds and ordered writers exact\n");
    return 0;
}
