// BufferVaIndex (port/src/buffer_va_index.h): the VA -> buffer lookup behind VulkanContext::buffer_at. CPU only.
//
// Checks fixed cases (edges, aliasing placed buffers, re-registration), then random tables against a linear scan
// with the same rule, and prints how long a lookup takes against the linear scan over every resource it replaces.
// `--bench` times larger tables.

#include "../src/buffer_va_index.h"
#include "../src/console_va_map.h"
#include "../src/title_va_alias.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <unordered_map>
#include <vector>

using palmetto::BufferVaIndex;
using Index = BufferVaIndex<std::uintptr_t>;

// Single-threaded test: arm only around insert(), after constructing the index and its existing ranges.
static int allocations_before_failure = -1;
void* operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc();
    }
    if (allocations_before_failure > 0) --allocations_before_failure;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

// The id that holds `va` (preferring one that holds `bytes` from it), 0 for none.
std::uintptr_t at(const Index& index, std::uint64_t va, std::uint64_t bytes = 1) {
    Index::Hit hit{};
    return index.find(va, hit, bytes) ? hit.id : 0;
}

// ConsoleVaMap (port/src/console_va_map.h): the title-VA -> arena-VA translation and the V# base rewrite behind
// design B (docs/page_address_space.md). CPU only.
void console_va_map_cases() {
    palmetto::ConsoleVaMap map;
    constexpr std::uint64_t title = 0x25780b00000ull;  // a placed buffer's two-terabyte title VA
    constexpr std::uint64_t arena = 0x400000ull;       // the arena range that stands in for it
    constexpr std::uint64_t bytes = 0x100000ull;
    check(map.add(title, bytes, arena, palmetto::ConsoleVaKind::buffer), "a title range is added");
    check(!map.add(title + 0x80000, 0x80000, arena, palmetto::ConsoleVaKind::buffer),
          "an overlapping title range is refused");
    std::uint64_t rebased = 0;
    check(map.rebase(title + 0x3df10, rebased, palmetto::ConsoleVaKind::buffer) && rebased == arena + 0x3df10,
          "a VA inside the range rebases to the arena range");
    check(!map.rebase(title - 1, rebased, palmetto::ConsoleVaKind::buffer) && !map.rebase(title + bytes, rebased, palmetto::ConsoleVaKind::buffer),
          "a VA outside the range does not rebase");
    check(!map.rebase(0, rebased, palmetto::ConsoleVaKind::buffer), "VA 0 never rebases");
    // A V# the title wrote into its own heap: dword 0 = base bits 31:0, dword 1 = bits 47:32 below its format and
    // stride bits. Only the base changes; a descriptor that names an image key is untouched.
    std::uint32_t slots[8] = {};
    const std::uint64_t named = title + 0x1000;
    slots[0] = std::uint32_t(named);
    slots[1] = std::uint32_t(named >> 32) | 0xabcd0000u;
    slots[4] = 0x00012345u;
    slots[5] = 0x00000001u;
    check(map.patch_descriptor_bases(reinterpret_cast<std::uint8_t*>(slots), sizeof(slots)) == 1,
          "one title-written V# base is rewritten");
    const std::uint64_t expected = arena + 0x1000;
    check(slots[0] == std::uint32_t(expected) && (slots[1] & 0xffffu) == std::uint32_t(expected >> 32) &&
              (slots[1] & 0xffff0000u) == 0xabcd0000u,
          "the V# base is the arena VA and its stride/format bits are kept");
    check(slots[4] == 0x00012345u && slots[5] == 0x00000001u, "an image descriptor is not rewritten");
    // A descriptor-heap range rebases table VAs through the same lookup.
    constexpr std::uint64_t heap_title = 0x25660000000ull;
    constexpr std::uint64_t heap_arena = 0x800000ull;
    check(map.add(heap_title, 0x4000, heap_arena, palmetto::ConsoleVaKind::descriptors),
          "a descriptor-heap range is added");
    check(map.rebase(heap_title + 0x20, rebased, palmetto::ConsoleVaKind::descriptors) && rebased == heap_arena + 0x20,
          "a table VA in a descriptor-heap range rebases");
    check(map.size() == 2, "both ranges are registered");
    map.remove(title);
    check(!map.rebase(named, rebased, palmetto::ConsoleVaKind::buffer) && map.size() == 1,
          "a removed range no longer rebases");
    // Two non-overlapping title VAs may intentionally rebase to one arena range. The pure VA map can express the GPU
    // address equation for an alias once xmem supplies physical-page identity; it does not discover that identity itself.
    constexpr std::uint64_t alias_title_a = 0x31000000000ull;
    constexpr std::uint64_t alias_title_b = 0x32000000000ull;
    constexpr std::uint64_t alias_arena = 0xa00000ull;
    check(map.add(alias_title_a, 0x10000, alias_arena, palmetto::ConsoleVaKind::buffer),
          "the first alias view is rebased into its page's arena range");
    check(map.add(alias_title_b, 0x10000, alias_arena, palmetto::ConsoleVaKind::buffer),
          "a second title VA may target the same arena bytes");
    std::uint64_t alias_a = 0, alias_b = 0;
    check(map.rebase(alias_title_a + 0x3000, alias_a, palmetto::ConsoleVaKind::buffer) &&
              map.rebase(alias_title_b + 0x3000, alias_b, palmetto::ConsoleVaKind::buffer) &&
              alias_a == alias_b && alias_a == alias_arena + 0x3000,
          "distinct title VAs can resolve to one arena address when the caller supplies identity");

    // Measured (docs/page_address_space.md): a console descriptor-heap range and a placed buffer's range may overlap in
    // the title's address space, so the site decides which one a VA means.
    constexpr std::uint64_t buffer2 = 0x30000000000ull;
    constexpr std::uint64_t buffer2_arena = 0x900000ull;
    constexpr std::uint64_t heap2 = buffer2 - 0x100000ull;
    constexpr std::uint64_t heap2_arena = 0x980000ull;
    check(map.add(buffer2, 0x100000, buffer2_arena, palmetto::ConsoleVaKind::buffer),
          "a second title buffer range is added");
    check(map.add(heap2, 0x200000, heap2_arena, palmetto::ConsoleVaKind::descriptors),
          "a descriptor-heap range may overlap a buffer range");
    const std::uint64_t both = buffer2 + 0x400;
    check(map.rebase(both, rebased, palmetto::ConsoleVaKind::buffer) && rebased == buffer2_arena + 0x400,
          "a site that means a buffer rebases to the buffer range");
    check(map.rebase(both, rebased, palmetto::ConsoleVaKind::descriptors) &&
              rebased == heap2_arena + (both - heap2),
          "a site that means a table rebases to the descriptor-heap range");
    // Incomplete means "a title VA the title can name has no stand-in": the replay then refuses every draw.
    check(!map.incomplete(), "the map starts complete");
    map.mark_incomplete();
    check(map.incomplete(), "mark_incomplete is reported");
}

// TitleVaAliasTable (port/src/title_va_alias.h): bounded raw title VA aliases to BDA windows outside the arena.
// Synthetic addresses only; this checks address arithmetic and policy, not a Vulkan allocation or a title boot.
void title_va_alias_cases() {
    using palmetto::TitleVaAliasRefusal;
    using palmetto::TitleVaAliasResolution;
    using palmetto::TitleVaAliasTable;
    using palmetto::TitleVaWindowsSwitch;

    check(palmetto::title_va_windows_switch_from(nullptr) == TitleVaWindowsSwitch::off,
          "unset title-window switch is off");
    check(palmetto::title_va_windows_switch_from("") == TitleVaWindowsSwitch::off,
          "empty title-window switch is off");
    check(palmetto::title_va_windows_switch_from("0") == TitleVaWindowsSwitch::off &&
              palmetto::title_va_windows_switch_from("off") == TitleVaWindowsSwitch::off,
          "false title-window switch values stay off");
    check(palmetto::title_va_windows_switch_from("1") == TitleVaWindowsSwitch::on &&
              palmetto::title_va_windows_switch_from("yes") == TitleVaWindowsSwitch::on,
          "only explicit true title-window values opt in");
    check(palmetto::title_va_windows_switch_from("maybe") == TitleVaWindowsSwitch::invalid &&
              palmetto::title_va_windows_switch_from(" 1") == TitleVaWindowsSwitch::invalid,
          "malformed title-window switch values are named invalid");

    constexpr std::uint64_t arena = 0x1000000000ull;
    constexpr std::uint64_t arena_bytes = 0x2000000ull;
    constexpr std::uint64_t arena_end = arena + arena_bytes;
    constexpr std::uint64_t external = arena_end + 0x100000ull;
    constexpr std::uint64_t external_bytes = 0x200000ull;
    constexpr std::uint64_t title_a = 0x18000000000ull;
    constexpr std::uint64_t title_b = 0x19000000000ull;

    TitleVaAliasTable off;
    const auto off_window = off.add_window(1, external, 0x1000);
    TitleVaAliasResolution untouched{0x11, 0x22, 0x33, 0x44};
    const auto before = untouched;
    const auto off_resolve = off.resolve(title_a, 0x100, untouched);
    check(off_window == TitleVaAliasRefusal::switch_off && off_resolve == TitleVaAliasRefusal::switch_off &&
              std::memcmp(&untouched, &before, sizeof(before)) == 0 && off.window_count() == 0 &&
              off.alias_count() == 0,
          "switch-off refusal leaves output and registry state unchanged");
    TitleVaAliasTable invalid(TitleVaWindowsSwitch::invalid, arena, arena_bytes);
    check(invalid.add_window(1, external, 0x1000) == TitleVaAliasRefusal::invalid_switch,
          "invalid switch refuses window registration by name");

    TitleVaAliasTable table(TitleVaWindowsSwitch::on, arena, arena_bytes);
    check(table.add_window(7, external, external_bytes) == TitleVaAliasRefusal::none &&
              table.window_count() == 1 && table.window_bytes() == external_bytes,
          "one external BDA window is accepted within the per-window and total bounds");
    check(table.add_alias(101, title_a, 0x100000, 7, 0x10000) == TitleVaAliasRefusal::none &&
              table.add_alias(102, title_b, 0x100000, 7, 0x10000) == TitleVaAliasRefusal::none,
          "two raw title VAs may alias the same outside-arena window bytes");

    TitleVaAliasResolution a{};
    TitleVaAliasResolution b{};
    constexpr std::uint64_t inside = 0x1234;
    check(table.resolve(title_a + inside, 0x200, a) == TitleVaAliasRefusal::none &&
              table.resolve(title_b + inside, 0x200, b) == TitleVaAliasRefusal::none &&
              a.window_id == 7 && a.console_va == external - arena + 0x10000 + inside &&
              a.device_address == external + 0x10000 + inside && a.console_va + arena == a.device_address &&
              a.console_va == b.console_va && a.device_address == b.device_address &&
              a.available_bytes == 0x100000 - inside,
          "both aliases resolve to the same representable GPU address with remaining bounds");

    // Replay integration contract: raw title VA -> ConsoleVaMap -> console-relative BDA window -> buffer_at index.
    // The same mapping also patches the V# base while preserving its non-base dword bits.
    palmetto::ConsoleVaMap replay_map;
    const std::uint64_t window_console_base = external - arena + 0x10000;
    Index window_buffers;
    window_buffers.insert(window_console_base, 0x100000, 707);
    std::uint64_t replay_va = 0;
    Index::Hit window_hit{};
    check(replay_map.add(title_a, 0x100000, window_console_base, palmetto::ConsoleVaKind::buffer) &&
              replay_map.rebase(title_a + inside, replay_va, palmetto::ConsoleVaKind::buffer) &&
              replay_va == a.console_va && window_buffers.find(replay_va, window_hit, 0x200) &&
              window_hit.id == 707 && replay_va - window_hit.start == inside && arena + replay_va == a.device_address,
          "raw replay VA rebases and resolves through buffer_at to the window's exact BDA");
    std::uint32_t window_descriptor[8]{};
    window_descriptor[0] = std::uint32_t(title_a + inside);
    window_descriptor[1] = std::uint32_t((title_a + inside) >> 32) | 0xabcd0000u;
    check(replay_map.patch_descriptor_bases(reinterpret_cast<std::uint8_t*>(window_descriptor),
                                            sizeof(window_descriptor)) == 1 &&
              (std::uint64_t(window_descriptor[0]) | (std::uint64_t(window_descriptor[1] & 0xffffu) << 32)) ==
                  a.console_va && arena + a.console_va == a.device_address &&
              (window_descriptor[1] & 0xffff0000u) == 0xabcd0000u,
          "shader-visible V# base rewrites to the external BDA delta without changing format/stride bits");
    check(table.resolve(title_a - 0x1000, 0x20, a) == TitleVaAliasRefusal::alias_unmapped,
          "unregistered raw title VA is refused by name");
    check(table.resolve(title_a + 0xfffff, 2, a) == TitleVaAliasRefusal::alias_view_out_of_bounds,
          "a view crossing its alias end is refused");
    check(table.resolve(title_a, 0, a) == TitleVaAliasRefusal::zero_bytes &&
              table.resolve(UINT64_MAX - 1, 4, a) == TitleVaAliasRefusal::range_overflow,
          "empty and overflowing view spans are named refusals");

    check(table.add_alias(103, title_a + 0x80000, 0x1000, 7) == TitleVaAliasRefusal::alias_overlap,
          "overlapping title-VA aliases are refused as ambiguous");
    check(table.add_alias(104, title_a + 0x200000, 0x1000, 99) == TitleVaAliasRefusal::alias_window_missing,
          "alias without a registered window is refused");
    check(table.add_alias(105, title_a + 0x200000, external_bytes + 1, 7) ==
              TitleVaAliasRefusal::alias_outside_window,
          "alias larger than its backing window is refused");
    check(table.add_alias(106, 0x10000000ull, 0x1000, 7) ==
              TitleVaAliasRefusal::title_va_inside_arena_window,
          "a raw VA inside the 32-bit arena window is not aliased");
    check(table.add_alias(107, UINT64_MAX - 0x10, 0x20, 7) == TitleVaAliasRefusal::range_overflow,
          "a raw title range that wraps is refused");
    check(table.add_alias(0, title_a + 0x300000, 0x1000, 7) == TitleVaAliasRefusal::zero_alias_id &&
              table.add_alias(108, 0, 0x1000, 7) == TitleVaAliasRefusal::zero_address &&
              table.add_alias(109, title_a + 0x300000, 0, 7) == TitleVaAliasRefusal::zero_bytes &&
              table.add_alias(101, title_a + 0x300000, 0x1000, 7) == TitleVaAliasRefusal::duplicate_alias_id,
          "zero and duplicate alias inputs are refused by name");

    check(table.add_window(7, external + 0x400000, 0x1000) == TitleVaAliasRefusal::duplicate_window_id &&
              table.add_window(8, external + 0x1000, 0x1000) == TitleVaAliasRefusal::window_overlap,
          "duplicate window IDs and overlapping BDA windows are refused");
    check(table.add_window(9, arena - 0x1000, 0x1000) == TitleVaAliasRefusal::window_below_heap_base &&
              table.add_window(10, arena + 0x1000, 0x1000) == TitleVaAliasRefusal::window_overlaps_arena,
          "negative-bias and arena-overlapping windows are refused");

    // Mirror the production allocator's BDA ordering refusal with a synthetic heap_base and arena extent. A candidate
    // ending exactly at heap_base still has a negative bias; equality is not a valid external window because the arena
    // itself begins there; the first byte at arena_end is representable with a nonnegative console VA.
    TitleVaAliasTable ordering(TitleVaWindowsSwitch::on, arena, arena_bytes);
    const auto below_base = ordering.add_window(1, arena - 0x1000, 0x1000);
    const auto at_base = ordering.add_window(2, arena, 0x1000);
    const auto overlaps_end = ordering.add_window(3, arena_end - 1, 1);
    const auto at_arena_end = ordering.add_window(4, arena_end, 0x1000);
    check(below_base == TitleVaAliasRefusal::window_below_heap_base &&
              at_base == TitleVaAliasRefusal::window_overlaps_arena &&
              overlaps_end == TitleVaAliasRefusal::window_overlaps_arena &&
              at_arena_end == TitleVaAliasRefusal::none && ordering.window_count() == 1 &&
              ordering.window_bytes() == 0x1000,
          "BDA ordering refuses below-base or overlapping candidates and accepts the exact arena-end boundary");
    TitleVaAliasResolution boundary{};
    check(ordering.add_alias(401, title_a + 0x400000, 0x1000, 4) == TitleVaAliasRefusal::none &&
              ordering.resolve(title_a + 0x400000, 0x1000, boundary) == TitleVaAliasRefusal::none &&
              boundary.console_va == arena_bytes && boundary.device_address == arena_end &&
              arena + boundary.console_va == boundary.device_address,
          "the first accepted BDA above the arena has no underflow and preserves heap_base + console_va");
    check(table.add_window(11, arena + TitleVaAliasTable::kConsoleVaSpace, 0x1000) ==
              TitleVaAliasRefusal::window_v48_unrepresentable,
          "a window outside the 48-bit V# address extent is refused");
    check(table.add_window(12, external + 0x400000, TitleVaAliasTable::kMaxWindowBytes + 1) ==
              TitleVaAliasRefusal::window_too_large &&
              table.add_window(13, UINT64_MAX - 0x10, 0x20) == TitleVaAliasRefusal::range_overflow &&
              table.add_window(0, external + 0x400000, 0x1000) == TitleVaAliasRefusal::zero_window_id &&
              table.add_window(14, 0, 0x1000) == TitleVaAliasRefusal::zero_address &&
              table.add_window(15, external + 0x400000, 0) == TitleVaAliasRefusal::zero_bytes,
          "oversized, wrapping, and empty window requests are named refusals");
    TitleVaAliasTable no_geometry(TitleVaWindowsSwitch::on);
    check(no_geometry.add_window(1, external, 0x1000) == TitleVaAliasRefusal::no_arena_geometry,
          "a window without the one heap_base and arena extent is refused");
    check(std::strcmp(palmetto::title_va_alias_refusal_name(TitleVaAliasRefusal::window_v48_unrepresentable),
                      "window_v48_unrepresentable") == 0 &&
              std::strcmp(palmetto::title_va_alias_refusal_name(TitleVaAliasRefusal::window_below_heap_base),
                          "window_below_heap_base") == 0 &&
              std::strcmp(palmetto::title_va_alias_refusal_name(TitleVaAliasRefusal::window_overlaps_arena),
                          "window_overlaps_arena") == 0,
          "BDA ordering refusal names are stable for startup-trace reporting");

    TitleVaAliasTable budget(TitleVaWindowsSwitch::on, arena, arena_bytes);
    constexpr std::uint64_t budget_base = arena_end + 0x10000000ull;
    bool budget_windows_added = true;
    for (std::uint64_t i = 0; i < 8; ++i)
        budget_windows_added = budget_windows_added &&
                               budget.add_window(100 + i, budget_base + i * TitleVaAliasTable::kMaxWindowBytes,
                                                 TitleVaAliasTable::kMaxWindowBytes) == TitleVaAliasRefusal::none;
    check(budget_windows_added && budget.window_bytes() == TitleVaAliasTable::kMaxTotalWindowBytes &&
              budget.add_window(200, budget_base + 8 * TitleVaAliasTable::kMaxWindowBytes, 1) ==
                  TitleVaAliasRefusal::window_budget_spent,
          "the 32 MiB aggregate window budget is enforced before the fixed table is full");

    TitleVaAliasTable window_capacity(TitleVaWindowsSwitch::on, arena, arena_bytes);
    constexpr std::uint64_t cap_base = arena_end + 0x20000000ull;
    bool all_windows_added = true;
    for (std::uint64_t i = 0; i < TitleVaAliasTable::kMaxWindows; ++i)
        all_windows_added = all_windows_added &&
                            window_capacity.add_window(300 + i, cap_base + i * 0x1000, 1) ==
                                TitleVaAliasRefusal::none;
    check(all_windows_added && window_capacity.window_count() == TitleVaAliasTable::kMaxWindows &&
              window_capacity.add_window(400, cap_base + TitleVaAliasTable::kMaxWindows * 0x1000, 1) ==
                  TitleVaAliasRefusal::window_table_full,
          "the fixed outside-window table refuses its 33rd entry");

    TitleVaAliasTable alias_capacity(TitleVaWindowsSwitch::on, arena, arena_bytes);
    constexpr std::uint64_t alias_window_base = arena_end + 0x30000000ull;
    check(alias_capacity.add_window(1, alias_window_base, TitleVaAliasTable::kMaxWindowBytes) ==
              TitleVaAliasRefusal::none,
          "alias-cap test has one bounded backing window");
    bool all_aliases_added = true;
    for (std::uint64_t i = 0; i < TitleVaAliasTable::kMaxAliases; ++i)
        all_aliases_added = all_aliases_added &&
                            alias_capacity.add_alias(500 + i, title_a + i * 0x1000, 0x1000, 1) ==
                                TitleVaAliasRefusal::none;
    check(all_aliases_added && alias_capacity.alias_count() == TitleVaAliasTable::kMaxAliases &&
              alias_capacity.add_alias(600, title_a + TitleVaAliasTable::kMaxAliases * 0x1000, 0x1000, 1) ==
                  TitleVaAliasRefusal::alias_table_full,
          "the fixed title-VA alias table refuses its 33rd entry");

    check(table.remove_window(7) == TitleVaAliasRefusal::alias_has_live_views &&
              table.remove_alias(101) == TitleVaAliasRefusal::none &&
              table.remove_alias(102) == TitleVaAliasRefusal::none &&
              table.remove_window(7) == TitleVaAliasRefusal::none && table.window_count() == 0 &&
              table.alias_count() == 0 && table.window_bytes() == 0 &&
              table.resolve(title_a, 0x100, a) == TitleVaAliasRefusal::alias_unmapped,
          "window lifetime cannot end before its aliases and releases all bounded bytes");
}

void fixed_cases() {
    Index index;    check(at(index, 0x1000) == 0, "an empty index holds nothing");

    index.insert(0x1000, 0x100, 1);  // [0x1000, 0x1100)
    index.insert(0x2000, 0x10, 2);   // [0x2000, 0x2010)
    check(at(index, 0x1000) == 1, "the first byte");
    check(at(index, 0x10ff) == 1, "the last byte");
    check(at(index, 0x1100) == 0, "one past the end");
    check(at(index, 0x0fff) == 0, "one before the start");
    check(at(index, 0x1800) == 0, "a gap between two buffers");
    check(at(index, 0x200f) == 2, "the second buffer's last byte");
    check(at(index, ~std::uint64_t(0)) == 0, "the highest VA");

    Index::Hit hit{};
    check(index.find(0x1080, hit) && hit.start == 0x1000 && hit.size == 0x100, "a hit names its range");

    index.insert(0, 0x100, 3);
    index.insert(0x3000, 0, 4);
    check(index.size() == 2 && at(index, 0) == 0 && at(index, 0x3000) == 0, "a null VA or an empty range is not indexed");

    // Aliasing placed buffers: a long one with two short ones inside it. The nearest start that holds the VA wins.
    Index alias;
    alias.insert(0x10000, 0x10000, 10);  // [0x10000, 0x20000)
    alias.insert(0x11000, 0x100, 11);    // inside it
    alias.insert(0x12000, 0x100, 12);    // inside it
    check(at(alias, 0x11080) == 11, "a short buffer inside a long one");
    check(at(alias, 0x11100) == 10, "past the short buffer, the long one still holds the VA");
    check(at(alias, 0x1f000) == 10, "past every short buffer");
    check(at(alias, 0x20000) == 0, "past the long buffer");

    // A view that starts in a short buffer and runs past its end: the long buffer around it holds all of it.
    check(at(alias, 0x11080, 0x80) == 11, "a view that fits in the short buffer stays in it");
    check(at(alias, 0x11080, 0x81) == 10, "a view past the short buffer's end goes to the long one around it");
    check(at(alias, 0x1ff00, 0x200) == 10, "a view no range holds: the closest that holds its start");
    check(at(alias, 0x11080, 0x10000) == 11, "a view past the long buffer too: the closest start, as without a size");
    check(at(alias, 0x11080, 0) == 11, "a size of 0 counts as one byte");

    // Two resources placed at the same offset: the larger ID, either way they were added.
    alias.insert(0x30000, 0x100, 21);
    alias.insert(0x30000, 0x200, 20);
    check(at(alias, 0x30080) == 21, "the same start: the larger ID");
    check(at(alias, 0x30180) == 20, "the same start: past the shorter one");

    // Erasing and re-registering.
    alias.erase(0x11000, 11);
    check(at(alias, 0x11080) == 10, "an erased buffer leaves the one around it");
    alias.erase(0x11000, 11);  // twice: nothing
    alias.erase(0x10000, 99);  // a wrong ID: nothing
    check(at(alias, 0x10000) == 10, "an erase with the wrong ID keeps the range");
    alias.erase(0x10000, 10);
    alias.erase(0x12000, 12);
    alias.erase(0x30000, 20);
    alias.erase(0x30000, 21);
    check(alias.size() == 0 && at(alias, 0x10000) == 0, "everything erased");
    alias.insert(0x40000, 0x10, 30);  // the bound restarts from an empty index
    check(at(alias, 0x40008) == 30 && at(alias, 0x40010) == 0, "an index refilled after it emptied");

    // The bound follows the live ranges: freeing the longest one brings it down to the next longest.
    Index bound;
    bound.insert(0x10000, 0x100, 1);
    bound.insert(0x20000, 0x100, 2);  // the same size twice: dropping one keeps the other's
    bound.insert(0x30000, 0x40000000, 3);
    check(bound.longest() == 0x40000000, "the bound is the longest range");
    bound.erase(0x30000, 3);
    check(bound.longest() == 0x100, "freeing the longest range lowers the bound");
    bound.erase(0x10000, 1);
    check(bound.longest() == 0x100 && at(bound, 0x20080) == 2, "a range of the same size keeps the bound");
    bound.insert(0x20000, 0x40, 2);  // re-registered with a smaller size: the old size goes
    check(bound.longest() == 0x40 && at(bound, 0x20040) == 0, "re-registering replaces the size");
    bound.erase(0x20000, 2);
    check(bound.longest() == 0 && bound.size() == 0, "an empty index has no bound");
}

void allocation_failures() {
    for (bool replace : {false, true}) {
        for (std::uint64_t size : {0x40u, 0x100u, 0x400u}) {
            // New entries allocate a range and a size node; replacement only needs a size node.
            for (int fail_after = 0; fail_after < (replace ? 1 : 2); ++fail_after) {
                Index index;
                index.insert(0x1000, 0x100, 1);
                index.insert(0x2000, 0x100, 2);  // equal sizes must retain separate multiset entries
                const std::uint64_t va = replace ? 0x1000 : 0x3000;
                const std::uintptr_t id = replace ? 1 : 3;
                bool threw = false;
                allocations_before_failure = fail_after;
                try {
                    index.insert(va, size, id);
                } catch (const std::bad_alloc&) {
                    threw = true;
                }
                allocations_before_failure = -1;
                check(threw, "injected insert allocation failure reached");
                Index::Hit first{}, second{};
                const bool unchanged = index.size() == 2 && index.longest() == 0x100 &&
                    index.find(0x1000, first) && first.id == 1 && first.size == 0x100 &&
                    index.find(0x2000, second) && second.id == 2 && second.size == 0x100 &&
                    at(index, 0x10ff) == 1 && at(index, 0x1100) == 0 && at(index, 0x3000) == 0;
                check(unchanged, "failed insert preserves ranges and lookup bound");
                if (!unchanged) continue;  // do not erase an inconsistent index in the negative control

                if (replace && size == 0x100) {
                    // A failed same-size replacement can look unchanged while silently dropping a size node.
                    index.erase(0x2000, 2);
                    const bool counted = index.size() == 1 && index.longest() == 0x100;
                    check(counted, "failed same-size replacement retains both size entries");
                    if (!counted) continue;
                    index.insert(0x2000, 0x100, 2);
                }

                index.insert(va, size, id);
                Index::Hit retried{};
                check(index.size() == (replace ? 2u : 3u) && index.find(va, retried) &&
                      retried.id == id && retried.size == size, "insert succeeds after allocation failure");
                check(index.longest() == (size > 0x100 ? size : 0x100), "retry updates the lookup bound");
                index.erase(va, id);
                check(index.longest() == 0x100 && at(index, 0x20ff) == 2,
                      "erasing retried range keeps the other size entry");
                index.erase(0x1000, 1);
                index.erase(0x2000, 2);
                check(index.size() == 0 && index.longest() == 0, "all size entries disappear after erase");
            }
        }
    }
}

// The rule BufferVaIndex follows, by a scan: among the ranges that hold all of [va, va + bytes), the greatest
// (start, id); when none does, the greatest among those that hold va.
struct Range {
    std::uint64_t start, size;
    std::uintptr_t id;
};

std::uintptr_t scan(const std::vector<Range>& ranges, std::uint64_t va, std::uint64_t bytes = 1) {
    const Range* best = nullptr;
    const Range* whole = nullptr;
    const auto greater = [](const Range& a, const Range* b) {
        return !b || a.start > b->start || (a.start == b->start && a.id > b->id);
    };
    for (const Range& r : ranges) {
        if (va < r.start || va - r.start >= r.size) continue;
        if (greater(r, best)) best = &r;
        if (bytes <= r.size - (va - r.start) && greater(r, whole)) whole = &r;
    }
    return whole ? whole->id : best ? best->id : 0;
}

void random_tables() {
    std::mt19937_64 rng(0x1e0a1da);
    for (int round = 0; round < 200; ++round) {
        Index index;
        std::vector<Range> ranges;
        const bool aliasing = round % 2;  // half the tables pack buffers apart, half overlap them
        std::uint64_t next = 0x10000;
        const int count = 1 + int(rng() % 64);
        for (int i = 0; i < count; ++i) {
            const std::uint64_t size = 1 + rng() % 0x4000;
            const std::uint64_t start = aliasing ? 0x10000 + rng() % 0x20000 : next;
            next += size + rng() % 0x1000;
            const Range r{start, size, std::uintptr_t(i + 1)};
            ranges.push_back(r);
            index.insert(r.start, r.size, r.id);
        }
        // Drop a few, as remove_resource does.
        for (int i = 0; i < count / 4; ++i) {
            const std::size_t k = rng() % ranges.size();
            index.erase(ranges[k].start, ranges[k].id);
            ranges.erase(ranges.begin() + std::ptrdiff_t(k));
            if (ranges.empty()) break;
        }
        std::uint64_t longest = 0;
        for (const Range& r : ranges) longest = r.size > longest ? r.size : longest;
        if (index.longest() != longest) {
            std::printf("FAIL: round %d: bound %llu, longest live range %llu\n", round,
                        (unsigned long long)index.longest(), (unsigned long long)longest);
            ++failures;
            return;
        }
        for (int q = 0; q < 2000; ++q) {
            const std::uint64_t va = 0x8000 + rng() % (next + 0x20000);
            // A quarter of the lookups name one byte, the rest a view of up to twice the largest buffer.
            const std::uint64_t bytes = q % 4 ? 1 + rng() % 0x8000 : 1;
            if (at(index, va, bytes) != scan(ranges, va, bytes)) {
                std::printf("FAIL: round %d va 0x%llx bytes 0x%llx: index %llu, scan %llu\n", round,
                            (unsigned long long)va, (unsigned long long)bytes,
                            (unsigned long long)at(index, va, bytes), (unsigned long long)scan(ranges, va, bytes));
                ++failures;
                return;
            }
        }
    }
}

// What buffer_at did before: a walk over every registered resource, images included, in an unordered_map.
struct Binding {
    bool image;
    std::uint64_t va, size;
};

void time_lookups(std::size_t resources) {
    std::unordered_map<std::uintptr_t, Binding> all;
    Index index;
    std::vector<std::uint64_t> vas;
    std::mt19937_64 rng(resources);
    std::uint64_t next = 0x10000;
    for (std::size_t i = 1; i <= resources; ++i) {
        if (i % 2) {  // half images, as in a frame's working set
            all[i] = {true, 0, 0};
            continue;
        }
        const std::uint64_t size = 256 + rng() % (1 << 20);
        all[i] = {false, next, size};
        index.insert(next, size, i);
        vas.push_back(next + rng() % size);
        next += (size + 255) / 256 * 256;
    }
    constexpr int lookups = 20000;
    using clock = std::chrono::steady_clock;
    std::uint64_t sink = 0;

    auto t0 = clock::now();
    for (int i = 0; i < lookups; ++i) {
        const std::uint64_t va = vas[std::size_t(i) % vas.size()];
        for (const auto& [id, b] : all) {
            if (b.image || va < b.va || va - b.va >= b.size) continue;
            sink += id;
            break;
        }
    }
    const double linear = std::chrono::duration<double, std::nano>(clock::now() - t0).count() / lookups;

    t0 = clock::now();
    for (int i = 0; i < lookups; ++i) sink += at(index, vas[std::size_t(i) % vas.size()]);
    const double indexed = std::chrono::duration<double, std::nano>(clock::now() - t0).count() / lookups;

    std::printf("%7zu resources: linear scan %10.1f ns/lookup, index %6.1f ns/lookup (x%.0f)%s\n", resources, linear,
                indexed, linear / (indexed > 0 ? indexed : 1), sink ? "" : " ");
}

// A miss past the last of many small buffers, once a large buffer has come and gone (found in review of #2823): the
// index that held it must be as fast as one that never did.
void time_miss_after_free() {
    constexpr std::uint64_t count = 20000, stride = 256;
    Index fresh, churned;
    for (std::uint64_t i = 0; i < count; ++i) {
        fresh.insert(0x10000 + i * stride, stride, i + 1);
        churned.insert(0x10000 + i * stride, stride, i + 1);
    }
    churned.insert(0x10000, std::uint64_t(1) << 30, count + 1);  // 1 GiB over all of them
    churned.erase(0x10000, count + 1);
    check(churned.longest() == stride, "the bound comes back down after a large buffer is freed");

    const std::uint64_t miss = 0x10000 + count * stride + stride;
    constexpr int lookups = 20000;
    using clock = std::chrono::steady_clock;
    std::uint64_t sink = 0;
    double ns[2];
    const Index* indexes[2] = {&fresh, &churned};
    for (int k = 0; k < 2; ++k) {
        const auto t0 = clock::now();
        for (int i = 0; i < lookups; ++i) sink += at(*indexes[k], miss + std::uint64_t(i % 64));
        ns[k] = std::chrono::duration<double, std::nano>(clock::now() - t0).count() / lookups;
    }
    std::printf("miss past %llu buffers: fresh index %.1f ns, after a freed 1 GiB buffer %.1f ns%s\n",
                (unsigned long long)count, ns[0], ns[1], sink ? " (hit?)" : "");
}

} // namespace

int main(int argc, char** argv) {
    console_va_map_cases();
    title_va_alias_cases();
    fixed_cases();
    allocation_failures();
    random_tables();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    time_miss_after_free();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::puts("BufferVaIndex: fixed cases, title-VA windows, and 200 random tables pass");
    const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    for (const std::size_t n : {std::size_t(1000), std::size_t(10000)}) time_lookups(n);
    if (bench)
        for (const std::size_t n : {std::size_t(50000), std::size_t(100000)}) time_lookups(n);
    return 0;
}
