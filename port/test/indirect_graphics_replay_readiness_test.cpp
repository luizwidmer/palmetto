// Strict D3D12X multi-count indirect graphics is refused in replay until the IR carries its full call-site state.
// Synthetic CPU-only Vulkan callbacks prove the refusal precedes resource resolution/emission and the switch-off path
// retains the existing fixed/count-buffer indirect calls. No Vulkan loader, GPU, shader, or game data is used.
#include "../src/vulkan_replay.h"
#include "../src/indirect_graphics_replay_readiness.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {
int failures = 0;
struct Calls {
    unsigned draw = 0;
    unsigned draw_count = 0;
    unsigned draw_indexed = 0;
    unsigned draw_indexed_count = 0;
    unsigned dispatch = 0;
    VkBuffer argument = VK_NULL_HANDLE;
    VkBuffer count_buffer = VK_NULL_HANDLE;
    VkDeviceSize argument_offset = 0;
    VkDeviceSize count_offset = 0;
    std::uint32_t max_count = 0;
    std::uint32_t stride = 0;
} calls;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template<class Handle> Handle fake_handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
    else return static_cast<Handle>(value);
}

VKAPI_ATTR void VKAPI_CALL draw_indirect(VkCommandBuffer, VkBuffer args, VkDeviceSize offset,
                                          std::uint32_t count, std::uint32_t stride) {
    ++calls.draw; calls.argument = args; calls.argument_offset = offset; calls.max_count = count; calls.stride = stride;
}
VKAPI_ATTR void VKAPI_CALL draw_indirect_count(VkCommandBuffer, VkBuffer args, VkDeviceSize offset, VkBuffer count_buffer,
                                                VkDeviceSize count_offset, std::uint32_t count, std::uint32_t stride) {
    ++calls.draw_count; calls.argument = args; calls.argument_offset = offset; calls.count_buffer = count_buffer;
    calls.count_offset = count_offset; calls.max_count = count; calls.stride = stride;
}
VKAPI_ATTR void VKAPI_CALL draw_indexed_indirect(VkCommandBuffer, VkBuffer args, VkDeviceSize offset,
                                                  std::uint32_t count, std::uint32_t stride) {
    ++calls.draw_indexed; calls.argument = args; calls.argument_offset = offset; calls.max_count = count; calls.stride = stride;
}
VKAPI_ATTR void VKAPI_CALL draw_indexed_indirect_count(VkCommandBuffer, VkBuffer args, VkDeviceSize offset, VkBuffer count_buffer,
                                                        VkDeviceSize count_offset, std::uint32_t count, std::uint32_t stride) {
    ++calls.draw_indexed_count; calls.argument = args; calls.argument_offset = offset; calls.count_buffer = count_buffer;
    calls.count_offset = count_offset; calls.max_count = count; calls.stride = stride;
}
VKAPI_ATTR void VKAPI_CALL dispatch_indirect(VkCommandBuffer, VkBuffer, VkDeviceSize) { ++calls.dispatch; }

bool strict_switch(const char* value) {
#ifdef _WIN32
    return _putenv_s(palmetto::viewport_replay_readiness::kStrictSwitch, value ? value : "") == 0;
#else
    return value ? setenv(palmetto::viewport_replay_readiness::kStrictSwitch, value, 1) == 0
                 : unsetenv(palmetto::viewport_replay_readiness::kStrictSwitch) == 0;
#endif
}

palmetto::ExecuteIndirect make_indirect(palmetto::IndirectKind kind, std::uint32_t max_count, bool counted) {
    palmetto::ExecuteIndirect command{};
    command.kind = kind;
    command.stride = kind == palmetto::IndirectKind::draw ? 16 :
                     kind == palmetto::IndirectKind::draw_indexed ? 20 : 12;
    command.max_count = max_count;
    command.argument_buffer = 1;
    command.argument_offset = 8;
    command.count_buffer = counted ? 2 : 0;
    command.count_offset = counted ? 4 : 0;
    return command;
}

palmetto::CommandList make_list(const palmetto::ExecuteIndirect& command) {
    palmetto::CommandList list;
    if (!list.execute_indirect(command) || !list.close()) ++failures;
    return list;
}

palmetto::ReplayFunctions functions() {
    palmetto::ReplayFunctions vk{};
    vk.draw_indirect = draw_indirect;
    vk.draw_indirect_count = draw_indirect_count;
    vk.draw_indexed_indirect = draw_indexed_indirect;
    vk.draw_indexed_indirect_count = draw_indexed_indirect_count;
    vk.dispatch_indirect = dispatch_indirect;
    return vk;
}

palmetto::ReplayResult replay(const palmetto::CommandList& list, palmetto::ReplayFunctions& vk, unsigned& resolves) {
    palmetto::ResourceBinding args{};
    args.buffer = fake_handle<VkBuffer>(0x101);
    args.size = 256;
    palmetto::ResourceBinding count{};
    count.buffer = fake_handle<VkBuffer>(0x202);
    count.size = 16;
    return palmetto::replay(list, VK_NULL_HANDLE, [&](palmetto::ResourceId id) -> const palmetto::ResourceBinding* {
        ++resolves;
        if (id == 1) return &args;
        if (id == 2) return &count;
        return nullptr;
    }, vk);
}

void readiness_matrix() {
    namespace r = palmetto::indirect_graphics_replay_readiness;
    using palmetto::IndirectKind;
    using r::Disposition;
    const auto off = r::assess(false, IndirectKind::draw_indexed, 7);
    expect(off.disposition == Disposition::legacy_unchanged && off.refusal == r::Refusal::none,
           "switch-off indirect graphics assessment preserves the legacy path");
    const auto draw = r::assess(true, IndirectKind::draw, 2);
    const auto indexed = r::assess(true, IndirectKind::draw_indexed, 9);
    expect(draw.disposition == Disposition::refuse && indexed.disposition == Disposition::refuse,
           "strict fixed-count and count-buffer graphics may both execute multiple records");
    expect(std::strcmp(r::refusal_name(draw.refusal), "indirect_multi_count_graphics_state_not_captured") == 0,
           "strict multi-count refusal has a stable exact name");
    expect(r::assess(true, IndirectKind::draw, 1).disposition == Disposition::ready &&
               r::assess(true, IndirectKind::draw_indexed, 0).disposition == Disposition::ready,
           "single-command and zero-command bounds do not hit the multi-count refusal");
    expect(r::assess(true, IndirectKind::dispatch, 32).disposition == Disposition::ready,
           "compute ExecuteIndirect is outside this graphics-state contract");
}

void replay_gate() {
    using palmetto::IndirectKind;
    using palmetto::ReplayResult;
    auto vk = functions();
    unsigned resolves = 0;

    expect(strict_switch("0"), "set exact strict-off environment");
    calls = {};
    auto fixed = make_list(make_indirect(IndirectKind::draw, 3, false));
    const auto* saved = std::get_if<palmetto::ExecuteIndirect>(&fixed.commands().front());
    expect(saved && saved->argument_buffer == 1 && saved->argument_offset == 8 && saved->max_count == 3 &&
               saved->stride == 16 && saved->count_buffer == 0,
           "off-path IR retains the original fixed-count argument tuple");
    resolves = 0;
    expect(replay(fixed, vk, resolves) == ReplayResult::success && calls.draw == 1 && calls.max_count == 3 &&
               calls.stride == 16 && calls.argument_offset == 8,
           "switch-off fixed-count indirect draw reaches the unchanged Vulkan command");

    calls = {};
    auto counted_indexed = make_list(make_indirect(IndirectKind::draw_indexed, 4, true));
    resolves = 0;
    expect(replay(counted_indexed, vk, resolves) == ReplayResult::success && calls.draw_indexed_count == 1 &&
               calls.max_count == 4 && calls.stride == 20 && calls.argument_offset == 8 && calls.count_offset == 4,
           "switch-off count-buffer indexed indirect preserves its arguments and bound");

    expect(strict_switch("1"), "enable exact strict opt-in");
    calls = {}; resolves = 0;
    expect(replay(fixed, vk, resolves) == ReplayResult::unsupported && calls.draw == 0 && resolves == 0,
           "strict multi-count fixed indirect is refused before resolution or emission");
    calls = {}; resolves = 0;
    expect(replay(counted_indexed, vk, resolves) == ReplayResult::unsupported && calls.draw_indexed_count == 0 &&
               resolves == 0,
           "strict multi-count GPU-count-buffer indirect is refused before resolution or emission");

    calls = {}; resolves = 0;
    auto one = make_list(make_indirect(IndirectKind::draw, 1, false));
    expect(replay(one, vk, resolves) == ReplayResult::success && calls.draw == 1 && calls.max_count == 1,
           "strict single-count indirect keeps the existing path");
    calls = {}; resolves = 0;
    auto dispatch = make_list(make_indirect(IndirectKind::dispatch, 3, false));
    expect(replay(dispatch, vk, resolves) == ReplayResult::success && calls.dispatch == 3,
           "strict mode does not refuse multi-count indirect dispatch");

    expect(strict_switch("true"), "set a non-exact switch spelling");
    calls = {}; resolves = 0;
    expect(replay(fixed, vk, resolves) == ReplayResult::success && calls.draw == 1,
           "non-exact switch value leaves the legacy multi-count path unchanged");
    strict_switch(nullptr);
}
} // namespace

int main() {
    readiness_matrix();
    replay_gate();
    if (failures) return 1;
    std::puts("indirect_graphics_replay_readiness_test: 16 checks passed");
    return 0;
}
