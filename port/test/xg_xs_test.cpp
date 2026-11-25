// Checks the registered xg_xs.dll shim against the call shapes measured in the game executable (port/shims/xg_xs.cpp).
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs.
//
// Default (PALMETTO_XG_LAYOUT unset or "linear") is the linear answer for a row-major backend: both exports return
// S_OK, XGSuggestSwizzleMode writes 0x100, and slot 2 fills the whole 0x1B40-byte XG_RESOURCE_LAYOUT from the
// description the computer was created with. The record offsets checked below are the ones the executable itself
// reads (docs/texture_layout.md): header size @0, alignment @8, mip count @0x10, plane count @0x14, plane 0 from
// 0x18 (0x6C8 bytes), mip level 0 at 0x50 with its size @0, offset @8, slice size @0x10, pitch pixels @0x18, pitch
// bytes @0x1C and padded extent @0x24. The measured shapes are a 256x128 8-bit image (row pitch 0x100, 0x8000 B) and
// a 1x1 4-byte image (row pitch 4 in the source, 0x100 padded in the linear layout). Descriptions the runtime cannot
// lay out honestly are refused from XGCreateTextureComputer with the field named, and slot 2 then writes nothing.
//
// PALMETTO_XG_LAYOUT=refuse is the previous shim, bit for bit: both exports E_NOTIMPL, the mode field untouched, the
// computer still handed out (two measured callers use it without checking), every slot writes nothing.
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
constexpr uint32_t kLinearMode = 0x100;  // the explicit linear layout the title's own description helper writes

// The slot-2 record: header 0x18, four planes of 0x6C8 (0x38-byte plane header + 15 mip levels of 0x70).
constexpr size_t kLayoutBytes = 0x1B40;
constexpr size_t kPlane0 = 0x18;
constexpr size_t kMip0 = kPlane0 + 0x38;
constexpr size_t kMipStride = 0x70;

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
    return !(n > 0 && _stricmp(value, "refuse") == 0);
}

uint32_t u32(const uint8_t* p, size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, p + at, sizeof(value));
    return value;
}

uint64_t u64(const uint8_t* p, size_t at) {
    uint64_t value = 0;
    std::memcpy(&value, p + at, sizeof(value));
    return value;
}

// The title's own 0x38-byte description (D3D12_RESOURCE_DESC): dimension @0, width @0x10, height @0x18,
// depth-or-array @0x1C, mip levels @0x1E, format @0x20, sample count @0x24, layout @0x2C, flags @0x30.
struct Desc {
    uint8_t bytes[0x38] = {};

    Desc(uint32_t dimension, uint64_t width, uint32_t height, uint16_t array_size, uint16_t mip_levels,
         uint32_t format, uint32_t samples, uint32_t layout, uint32_t flags) {
        std::memcpy(bytes, &dimension, sizeof(dimension));
        std::memcpy(bytes + 0x10, &width, sizeof(width));
        std::memcpy(bytes + 0x18, &height, sizeof(height));
        std::memcpy(bytes + 0x1C, &array_size, sizeof(array_size));
        std::memcpy(bytes + 0x1E, &mip_levels, sizeof(mip_levels));
        std::memcpy(bytes + 0x20, &format, sizeof(format));
        std::memcpy(bytes + 0x24, &samples, sizeof(samples));
        std::memcpy(bytes + 0x2C, &layout, sizeof(layout));
        std::memcpy(bytes + 0x30, &flags, sizeof(flags));
    }
};

// The two shapes measured in the boot (docs/texture_layout.md section 1): a 256x128 R8_UNORM image (data format 61,
// row pitch 256 B, 32,768 B) and a 1x1 R8G8B8A8_UNORM default texture (format 28, 4 B).
Desc atlas_desc() { return Desc(3, 256, 128, 1, 1, 61, 1, kLinearMode, 0x00300000); }
Desc white_desc() { return Desc(3, 1, 1, 1, 1, 28, 1, kLinearMode, 0x00300000); }

bool checks_layout_record(void* computer) {
    auto slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
    // One guard byte past the record and a sentinel everywhere: the fill must write exactly the 0x1B40 bytes and
    // zero every field it does not model, never leak stack bytes.
    uint8_t record[kLayoutBytes + 0x40];
    std::memset(record, 0xcd, sizeof(record));
    check(slot2(computer, record) == S_OK, "slot 2 fills the layout record in the linear layout");
    check(u64(record, 0x00) == 0x8000, "record SizeBytes is the 256x128 one-byte image's 0x8000 B");
    check(u64(record, 0x08) == 0x100, "record BaseAlignmentBytes is one 256-byte linear block");
    check(u32(record, 0x10) == 1 && u32(record, 0x14) == 1, "record has one mip level and one plane");
    check(u32(record, 0x1B38) == 3, "record Dimension is the description's TEXTURE2D");
    check(u32(record, kPlane0) == 1, "plane 0 usage is colour");
    check(u64(record, kPlane0 + 0x08) == 0x8000, "plane 0 size is the resource size");
    check(u64(record, kPlane0 + 0x10) == 0x8000, "plane 0 slice size is the resource size (one slice)");
    check(u64(record, kPlane0 + 0x20) == 0x100, "plane 0 base alignment is 0x100");
    check(u32(record, kPlane0 + 0x28) == 1, "plane 0 bytes per element is the 8-bit format's one");
    check(u32(record, kPlane0 + 0x2C) == 1 && u32(record, kPlane0 + 0x30) == 0,
          "plane 0 reports no mip tail");
    check(u64(record, kMip0) == 0x8000, "mip 0 level size is 0x8000 B");
    check(u64(record, kMip0 + 0x08) == 0, "mip 0 sits at offset 0");
    check(u64(record, kMip0 + 0x10) == 0x8000, "mip 0 slice size is 0x8000 B");
    check(u32(record, kMip0 + 0x18) == 256 && u32(record, kMip0 + 0x1C) == 256,
          "mip 0 pitch is 256 pixels / 256 B, the measured row pitch");
    check(u32(record, kMip0 + 0x24) == 256 && u32(record, kMip0 + 0x28) == 128,
          "mip 0 padded extent is 256x128 elements");
    check(u32(record, kMip0 + 0x30) == 256 && u32(record, kMip0 + 0x34) == 128,
          "mip 0 extent is 256x128 elements");
    check(u32(record, kMip0 + 0x38) == 256 && u32(record, kMip0 + 0x3C) == 128,
          "mip 0 extent is 256x128 pixels");
    check(u32(record, kMip0 + 0x40) == 1 && u32(record, kMip0 + 0x44) == 1,
          "mip 0 is one slice, one sample");
    check(u32(record, kMip0 + 0x48) == 0, "mip 0 swizzle mode is linear (0)");
    check(u32(record, kMip0 + 0x60) == 1 && u32(record, kMip0 + 0x64) == 0,
          "mip 0 block depth is 1 and it is not in a mip tail");
    check(all_bytes(record + kMip0 + kMipStride, kPlane0 + 0x6C8 - (kMip0 + kMipStride), 0),
          "the fourteen unused mip levels are zeroed");
    check(all_bytes(record + kPlane0 + 0x6C8, 0x1B38 - (kPlane0 + 0x6C8), 0),
          "the three unused planes are zeroed");
    check(u32(record, 0x1B38) == 3 && all_bytes(record + 0x1B3C, 4, 0),
          "the record ends with the description's dimension and no stray bytes");
    check(all_bytes(record + kLayoutBytes, 0x40, 0xcd), "the fill does not write past the 0x1B40-byte record");

    // The 1x1 white default texture: a 4-byte row, padded to one 256-byte linear block.
    uint8_t small[kLayoutBytes + 0x10];
    std::memset(small, 0xcd, sizeof(small));
    Desc white = white_desc();
    void* white_computer = nullptr;
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    check(create(white.bytes, &white_computer) == S_OK, "the 1x1 description gets a linear layout computer");
    if (white_computer) {
        auto small_slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(white_computer))[2]);
        check(small_slot2(white_computer, small) == S_OK, "slot 2 fills the 1x1 description's record");
        check(u64(small, 0x00) == 0x100 && u64(small, kPlane0 + 0x08) == 0x100,
              "the 1x1 record is one 256-byte linear block");
        check(u32(small, kMip0 + 0x18) == 64 && u32(small, kMip0 + 0x1C) == 0x100,
              "the 1x1 layout pads its 4-byte row to 64 elements / 256 B");
        check(u32(small, kMip0 + 0x30) == 1 && u32(small, kMip0 + 0x34) == 1,
              "the 1x1 layout keeps the real 1x1 element extent");
        check(all_bytes(small + kLayoutBytes, 0x10, 0xcd), "the 1x1 fill does not write past the record");
    }
    return true;
}

bool checks_refusals() {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto slot2_of = [](void* computer) {
        return reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
    };
    Desc multi_mip = Desc(3, 256, 128, 1, 2, 61, 1, kLinearMode, 0x00300000);
    Desc array = Desc(3, 256, 128, 2, 1, 61, 1, kLinearMode, 0x00300000);
    Desc chosen_mode = Desc(3, 256, 128, 1, 1, 61, 1, 0, 0x00300000);
    Desc swizzled = Desc(3, 256, 128, 1, 1, 61, 1, 0x105, 0x00300000);
    Desc volume = Desc(4, 64, 64, 1, 1, 61, 1, kLinearMode, 0x00300000);
    Desc msaa = Desc(3, 256, 128, 1, 1, 61, 4, kLinearMode, 0x00300000);
    Desc render_target = Desc(3, 256, 128, 1, 1, 61, 1, kLinearMode, 0x00000001);
    Desc unknown_format = Desc(3, 256, 128, 1, 1, 0xFFFF, 1, kLinearMode, 0x00300000);
    Desc planar = Desc(3, 256, 128, 1, 1, 20, 1, kLinearMode, 0x00300000);
    struct Case {
        const Desc* desc;
        const char* what;
    } cases[] = {
        {&multi_mip, "a two-level description is refused (only one level is laid out)"},
        {&array, "a two-slice description is refused (only one slice is laid out)"},
        {&chosen_mode, "layout 0 (the library's choice) is refused: only the explicit linear 0x100 is served"},
        {&swizzled, "an explicit swizzled layout is refused: the runtime serves linear"},
        {&volume, "a 3D description is refused"},
        {&msaa, "an MSAA description is refused"},
        {&render_target, "a render-target description is refused (its CMASK plane is not modelled)"},
        {&unknown_format, "an unknown format is refused"},
        {&planar, "a multi-plane depth format is refused"},
    };
    uint8_t record[kLayoutBytes];
    for (const Case& c : cases) {
        void* computer = nullptr;
        check(create(c.desc->bytes, &computer) == kNotImpl, c.what);
        check(computer != nullptr, "a refused description still hands out a computer to the callers that do not check");
        if (!computer) continue;
        std::memset(record, 0x5a, sizeof(record));
        check(slot2_of(computer)(computer, record) == kNotImpl, "slot 2 refuses the same description");
        check(all_bytes(record, sizeof(record), 0x5a), "slot 2 writes nothing for a refused description");
    }
    return true;
}

}  // namespace

int main() {
    const bool linear = linear_layout();
    std::printf("layout: %s\n", linear ? "linear (default; PALMETTO_XG_LAYOUT=linear or unset)"
                                       : "refuse (PALMETTO_XG_LAYOUT=refuse)");
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    Desc atlas = atlas_desc();
    uint32_t mode = 0x1234;
    if (linear) {
        check(suggest(atlas.bytes, 0, &mode) == S_OK, "XGSuggestSwizzleMode returns S_OK in the linear layout");
        check(mode == kLinearMode, "XGSuggestSwizzleMode suggests the explicit linear layout 0x100");
        check(suggest(atlas.bytes, 0, nullptr) == E_POINTER, "XGSuggestSwizzleMode(desc, 0, nullptr) is E_POINTER");
    } else {
        check(suggest(atlas.bytes, 0, &mode) == kNotImpl, "XGSuggestSwizzleMode returns E_NOTIMPL in refuse mode");
        check(mode == 0x1234, "XGSuggestSwizzleMode leaves the mode field as the caller set it");
    }

    check(create(atlas.bytes, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");
    void* computer = nullptr;
    check(create(atlas.bytes, &computer) == (linear ? S_OK : kNotImpl),
          linear ? "XGCreateTextureComputer returns S_OK for the measured 256x128 description"
                 : "XGCreateTextureComputer returns E_NOTIMPL");
    check(computer != nullptr, "XGCreateTextureComputer still stores a computer for the callers that do not check");
    if (!computer) return 1;
    void* const* vtable = *static_cast<void* const* const*>(computer);

    uint8_t record[kLayoutBytes + 0x40];
    std::memset(record, 0xcd, sizeof(record));
    if (linear) {
        checks_layout_record(computer);
        check(checks_refusals(), "the refusal cases ran");
    } else {
        check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, record) == kNotImpl,
              "slot 2 refuses by name in refuse mode");
        check(all_bytes(record, sizeof(record), 0xcd), "slot 2 writes nothing in refuse mode");
    }

    // Slot 8 is CopyIntoSubresource. In refuse mode it must not write; in the linear layout it copies the linear
    // subresource to the offset the row-major source names. A description with width 4, height 4, one mip and one
    // slice bounds the biggest subresource at 4 * 4 * 16 = 256 bytes, so the copies below are inside it. The layout
    // record above was filled for the 256x128 description; the copy only uses the recorded description's extent.
    uint8_t dst[96];
    std::memset(dst, 0xcd, sizeof(dst));
    uint8_t src[96];
    std::memset(src, 0x5a, sizeof(src));
    const HRESULT copied = reinterpret_cast<Slot8Fn>(vtable[8])(computer, dst, 0, 0, src, 16, 32);
    if (linear) {
        check(copied == S_OK, "slot 8 copies in the linear layout");
        check(dst[0] == 0x5a && dst[31] == 0x5a, "slot 8 writes the linear subresource at the destination base");
        check(dst[32] == 0xcd, "slot 8 copies only the measured byte count");
        check(reinterpret_cast<Slot8Fn>(vtable[8])(computer, dst, 0, 1, src + 16, 16, 32) == S_OK,
              "the next subresource copies at its own source offset");
        check(dst[16] == 0x5a && dst[47] == 0x5a,
              "slot 8 places a subresource at the row-major source offset (destination base + 16)");
    } else {
        check(copied == kNotImpl, "slot 8 returns E_NOTIMPL");
        check(all_bytes(dst, sizeof(dst), 0xcd), "slot 8 writes nothing to its destination");
    }

    // Slot 10 is GetResourceTiling, at the measured seven-argument call: (total tiles u32*, packed-mip-desc out,
    // tile-shape out, in/out subresource-tiling count = 16, first subresource = 0, 12-byte tiling list).
    uint32_t tiles = 0, count = 0x10;
    uint8_t packed[0x10] = {}, shape[0x10] = {}, list[0x10 * 12] = {};
    check(reinterpret_cast<Slot10Fn>(vtable[10])(computer, &tiles, packed, shape, &count, 0, list) == kNotImpl,
          "slot 10 refuses by name (no 64 KiB tile layout on the PC)");
    check(tiles == 0 && count == 0x10 && all_bytes(packed, sizeof(packed), 0) && all_bytes(shape, sizeof(shape), 0) &&
              all_bytes(list, sizeof(list), 0),
          "slot 10 leaves the caller's zero-filled outputs as they were");
    check(reinterpret_cast<Slot2Fn>(vtable[31])(computer, record) == kNotImpl, "the last slot (31) is traced too");

    auto release = reinterpret_cast<ReleaseFn>(vtable[1]);
    release(computer);
    release(computer);
    check(reinterpret_cast<Slot2Fn>(vtable[2])(computer, record) == (linear ? S_OK : kNotImpl),
          "the computer stays usable after more Releases than it was handed out");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
