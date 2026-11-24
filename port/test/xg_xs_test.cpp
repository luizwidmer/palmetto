// Checks the registered xg_xs.dll shim against the call shapes measured in the game executable (port/shims/xg_xs.cpp).
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs. The default checks: both
// return E_NOTIMPL, XGSuggestSwizzleMode leaves the caller's field alone, XGCreateTextureComputer still hands out a
// computer (two measured callers use it without checking the HRESULT), and the computer's slots called at the measured
// sites (1, 2, 8, 10) return without writing what their arguments point to.
//
// With PALMETTO_XG_LAYOUT=linear (the honest answer for a row-major backend) the same test expects the linear contract
// instead: S_OK from both exports, 0x100 in the mode field, and slot 8 copying the linear subresource to the offset the
// row-major source names, while slots 2 and 10 (the layout and tile structures) still refuse by name and write nothing.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using Slot2Fn = HRESULT(__stdcall*)(void*, void*);
using Slot8Fn = HRESULT(__stdcall*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
using Slot10Fn = HRESULT(__stdcall*)(void*, uint32_t*, void*, void*, uint32_t*, uint32_t, void*);
constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr uint32_t kLinearMode = 0x100;  // what the title's own description helper writes for a known mode byte 0

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool all_bytes(const uint8_t* p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) return false;
    return true;
}

bool linear_layout() {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_XG_LAYOUT", value, sizeof(value));
    return n > 0 && _stricmp(value, "linear") == 0;
}

}  // namespace

int main() {
    const bool linear = linear_layout();
    std::printf("layout: %s\n", linear ? "linear (PALMETTO_XG_LAYOUT=linear)" : "refuse (default)");
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    uint8_t desc[0x38] = {};
    uint32_t mode = 0x1234;
    if (linear) {
        check(suggest(desc, 0, &mode) == S_OK, "XGSuggestSwizzleMode returns S_OK in the linear layout");
        check(mode == kLinearMode, "XGSuggestSwizzleMode suggests 0x100 in the linear layout");
        check(suggest(desc, 0, nullptr) == E_POINTER, "XGSuggestSwizzleMode(desc, 0, nullptr) is E_POINTER");
    } else {
        check(suggest(desc, 0, &mode) == kNotImpl, "XGSuggestSwizzleMode returns E_NOTIMPL");
        check(mode == 0x1234, "XGSuggestSwizzleMode leaves the mode field as the caller set it");
    }

    check(create(desc, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");
    void* computer = nullptr;
    check(create(desc, &computer) == (linear ? S_OK : kNotImpl),
          linear ? "XGCreateTextureComputer returns S_OK in the linear layout"
                 : "XGCreateTextureComputer returns E_NOTIMPL");
    check(computer != nullptr, "XGCreateTextureComputer still stores a computer for the callers that do not check");
    if (!computer) return 1;

    void* const* vtable = *static_cast<void* const* const*>(computer);
    uint8_t layout[0x800];
    std::memset(layout, 0xcd, sizeof(layout));
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, layout) == kNotImpl,
          "slot 2 refuses by name (the layout structure is not modelled)");
    check(all_bytes(layout, sizeof(layout), 0xcd), "slot 2 writes nothing to its output");

    // Slot 8 is CopyIntoSubresource. In the refuse layout it must not write; in the linear layout it copies the linear
    // subresource to the offset the row-major source names. A description with width 4, height 4, one mip and one slice
    // bounds the biggest subresource at 4 * 4 * 16 = 256 bytes, so the copies below are inside it.
    const uint64_t width = 4;
    std::memcpy(desc + 0x10, &width, sizeof(width));
    const uint32_t height = 4;
    std::memcpy(desc + 0x18, &height, sizeof(height));
    const uint16_t depth = 1, mips = 1;
    std::memcpy(desc + 0x1c, &depth, sizeof(depth));
    std::memcpy(desc + 0x1e, &mips, sizeof(mips));
    void* linear_computer = nullptr;
    check(create(desc, &linear_computer) == (linear ? S_OK : kNotImpl), "a described computer is handed out");
    if (!linear_computer) return 1;
    void* const* linear_vtable = *static_cast<void* const* const*>(linear_computer);

    uint8_t dst[96];
    std::memset(dst, 0xcd, sizeof(dst));
    uint8_t src[96];
    std::memset(src, 0x5a, sizeof(src));
    const HRESULT copied = reinterpret_cast<Slot8Fn>(linear_vtable[8])(linear_computer, dst, 0, 0, src, 16, 32);
    if (linear) {
        check(copied == S_OK, "slot 8 copies in the linear layout");
        check(dst[0] == 0x5a && dst[31] == 0x5a, "slot 8 writes the linear subresource at the destination base");
        check(dst[32] == 0xcd, "slot 8 copies only the measured byte count");
        check(reinterpret_cast<Slot8Fn>(linear_vtable[8])(linear_computer, dst, 0, 1, src + 16, 16, 32) == S_OK,
              "the next subresource copies at its own source offset");
        check(dst[16] == 0x5a && dst[47] == 0x5a,
              "slot 8 places a subresource at the row-major source offset (destination base + 16)");
    } else {
        check(copied == kNotImpl, "slot 8 returns E_NOTIMPL");
        check(all_bytes(dst, sizeof(dst), 0xcd), "slot 8 writes nothing to its destination");
    }

    uint32_t count = 0, inout = 0x10;
    uint8_t a[12] = {}, b[12] = {}, list[0xc0] = {};
    check(reinterpret_cast<Slot10Fn>(vtable[10])(computer, &count, a, b, &inout, 0, list) == kNotImpl,
          "slot 10 refuses by name (the tile structure is not modelled)");
    check(count == 0 && inout == 0x10 && all_bytes(a, 12, 0) && all_bytes(b, 12, 0) &&
              all_bytes(list, sizeof(list), 0),
          "slot 10 leaves the caller's zero-filled outputs as they were");
    check(reinterpret_cast<Slot2Fn>(vtable[31])(computer, layout) == kNotImpl, "the last slot (31) is traced too");

    auto release = reinterpret_cast<ReleaseFn>(vtable[1]);
    release(computer);
    release(computer);
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, layout) == kNotImpl,
          "the computer stays usable after more Releases than it was handed out");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
