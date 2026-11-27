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
#include <vector>

#include "shims.h"
#include "xg_xs.h"

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

// The movie-plane linear upload rule (docs/peer_port_movie.md section 12). The measured movie planes are three
// single-level, single-slice R8 textures in the linear layout: one 3840x2160 luma plane and two 1920x1080 chroma
// planes. The rule keeps the layout record as it is (the 256-byte aligned linear row pitch) and places a linear
// source's rows at that pitch, so a 1920-byte source row lands on a 2048-byte layout row. The checks below pin:
// the record for both shapes, the row-aware copy and its edges, the measured shapes' byte-identical result on both
// paths, the switch parsing, and the named refusals that write nothing.
bool checks_movie_plane_upload() {
    using rt::xg_xs::parse_movie_plane_upload;
    using rt::xg_xs::set_movie_plane_upload_for_test;
    check(!parse_movie_plane_upload(nullptr) && !parse_movie_plane_upload("") && !parse_movie_plane_upload("0") &&
              !parse_movie_plane_upload("off") && !parse_movie_plane_upload("no") &&
              !parse_movie_plane_upload("maybe"),
          "the movie-plane switch is off for unset, empty, 0, off, no and an unknown word");
    check(parse_movie_plane_upload("on") && parse_movie_plane_upload("1") && parse_movie_plane_upload("ON") &&
              parse_movie_plane_upload("yes") && parse_movie_plane_upload("true"),
          "the movie-plane switch is on for on, 1, yes and true (any case)");

    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto slot2_of = [](void* computer) {
        return reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
    };
    auto slot8_of = [](void* computer) {
        return reinterpret_cast<Slot8Fn>((*static_cast<void* const* const*>(computer))[8]);
    };

    // The record for the two measured movie-plane shapes. 3840x2160 R8 has a 3840 B row (already 256-byte
    // aligned); 1920x1080 R8 has a 1920 B packed row, which the linear layout pads to 2048 B.
    Desc luma = Desc(3, 3840, 2160, 1, 1, 61, 1, kLinearMode, 0x00300000);
    Desc chroma = Desc(3, 1920, 1080, 1, 1, 61, 1, kLinearMode, 0x00300000);
    void* luma_computer = nullptr;
    void* chroma_computer = nullptr;
    check(create(luma.bytes, &luma_computer) == S_OK, "the 3840x2160 R8 luma description is laid out");
    check(create(chroma.bytes, &chroma_computer) == S_OK, "the 1920x1080 R8 chroma description is laid out");
    if (!luma_computer || !chroma_computer) return false;
    uint8_t record[kLayoutBytes];
    std::memset(record, 0xcd, sizeof(record));
    check(slot2_of(luma_computer)(luma_computer, record) == S_OK, "the luma shape gets a layout record");
    check(u64(record, 0x00) == 3840ull * 2160 && u32(record, kMip0 + 0x1C) == 3840,
          "the luma record is 8,294,400 B at a 3840 B row pitch");
    check(u32(record, kMip0 + 0x18) == 3840 && u32(record, kMip0 + 0x24) == 3840,
          "the luma record's pitch and padded width are the 3840 aligned pixels");
    std::memset(record, 0xcd, sizeof(record));
    check(slot2_of(chroma_computer)(chroma_computer, record) == S_OK, "the chroma shape gets a layout record");
    check(u64(record, 0x00) == 2048ull * 1080 && u32(record, kMip0 + 0x1C) == 2048,
          "the chroma record is 2,211,840 B at the linear layout's 2048 B padded row pitch");
    check(u32(record, kMip0 + 0x18) == 2048 && u32(record, kMip0 + 0x24) == 2048,
          "the chroma record's pitch and padded width are the 2048 padded elements");
    check(u32(record, kMip0 + 0x30) == 1920 && u32(record, kMip0 + 0x3C) == 1080,
          "the chroma record keeps the real 1920x1080 element extent");

    // The row-aware copy of the chroma shape: 1920-byte source rows at the layout's 2048-byte rows.
    set_movie_plane_upload_for_test(true);
    const uint32_t src_pitch = 1920;
    const uint32_t dst_pitch = 2048;
    const uint32_t rows = 1080;
    std::vector<uint8_t> chroma_src(static_cast<size_t>(src_pitch) * rows);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < src_pitch; ++x)
            chroma_src[static_cast<size_t>(y) * src_pitch + x] = static_cast<uint8_t>((y * 7 + x * 13) & 0xff);
    std::vector<uint8_t> chroma_dst(static_cast<size_t>(dst_pitch) * rows + 32, 0xcd);
    check(slot8_of(chroma_computer)(chroma_computer, chroma_dst.data(), 0, 0, chroma_src.data(), src_pitch,
                                    src_pitch * rows) == S_OK,
          "the switch on places the chroma shape's rows");
    bool rows_match = true, padding_untouched = true;
    for (uint32_t y = 0; y < rows; ++y) {
        if (std::memcmp(chroma_dst.data() + static_cast<size_t>(y) * dst_pitch,
                        chroma_src.data() + static_cast<size_t>(y) * src_pitch, src_pitch) != 0)
            rows_match = false;
        if (chroma_dst[static_cast<size_t>(y) * dst_pitch + src_pitch] != 0xcd) padding_untouched = false;
    }
    check(rows_match, "every chroma source row lands at the layout's 2048 B row pitch");
    check(padding_untouched, "the 128 B the layout pads each row with are not written");
    check(all_bytes(chroma_dst.data() + static_cast<size_t>(dst_pitch) * rows, 32, 0xcd),
          "the copy writes exactly the layout's 2,211,840 B");

    // The aligned luma shape is byte-identical on both paths.
    std::vector<uint8_t> luma_src(static_cast<size_t>(3840) * 2160);
    for (size_t i = 0; i < luma_src.size(); ++i) luma_src[i] = static_cast<uint8_t>((i * 11) & 0xff);
    std::vector<uint8_t> luma_on(luma_src.size(), 0xcd);
    void* luma_on_computer = nullptr;
    check(create(luma.bytes, &luma_on_computer) == S_OK, "a second luma computer is created");
    check(slot8_of(luma_on_computer)(luma_on_computer, luma_on.data(), 0, 0, luma_src.data(), 3840, 3840 * 2160) ==
              S_OK,
          "the switch on copies the luma shape");
    check(std::memcmp(luma_on.data(), luma_src.data(), luma_src.size()) == 0,
          "the luma shape is the source bytes, row pitch 3840 = layout pitch 3840");
    set_movie_plane_upload_for_test(false);
    std::vector<uint8_t> luma_off(luma_src.size(), 0xcd);
    void* luma_off_computer = nullptr;
    check(create(luma.bytes, &luma_off_computer) == S_OK, "a third luma computer is created");
    check(slot8_of(luma_off_computer)(luma_off_computer, luma_off.data(), 0, 0, luma_src.data(), 3840, 3840 * 2160) ==
              S_OK,
          "the switch off copies the luma shape on the tree's path");
    check(luma_on == luma_off, "the aligned luma shape is byte-identical with the switch on and off");

    // The measured 256x128 and 1x1 shapes are byte-identical on both paths (the switch only changes shapes whose
    // packed row pitch differs from the layout's).
    Desc atlas = atlas_desc();
    std::vector<uint8_t> atlas_src(0x8000);
    for (size_t i = 0; i < atlas_src.size(); ++i) atlas_src[i] = static_cast<uint8_t>((i * 5) & 0xff);
    std::vector<uint8_t> atlas_on(0x8000 + 16, 0xcd), atlas_off(0x8000 + 16, 0xcd);
    void* atlas_on_computer = nullptr;
    void* atlas_off_computer = nullptr;
    check(create(atlas.bytes, &atlas_on_computer) == S_OK && create(atlas.bytes, &atlas_off_computer) == S_OK,
          "two 256x128 computers are created");
    set_movie_plane_upload_for_test(true);
    check(slot8_of(atlas_on_computer)(atlas_on_computer, atlas_on.data(), 0, 0, atlas_src.data(), 0x100, 0x8000) == S_OK,
          "the switch on copies the measured 256x128 shape");
    set_movie_plane_upload_for_test(false);
    check(slot8_of(atlas_off_computer)(atlas_off_computer, atlas_off.data(), 0, 0, atlas_src.data(), 0x100, 0x8000) ==
              S_OK,
          "the switch off copies the measured 256x128 shape");
    check(atlas_on == atlas_off, "the measured 256x128 shape is byte-identical with the switch on and off");
    check(all_bytes(atlas_on.data() + 0x8000, 16, 0xcd), "the 256x128 copy writes only the measured bytes");

    // The named refusals: a plane, a subresource, a second source, a row pitch under one row and a byte count under
    // the description's rows are refused by name and write nothing.
    const Slot8Fn slot8 = slot8_of(chroma_computer);
    auto fresh_chroma = [&]() {
        void* c = nullptr;
        create(chroma.bytes, &c);
        return c;
    };
    set_movie_plane_upload_for_test(true);
    {
        std::vector<uint8_t> dst(static_cast<size_t>(dst_pitch) * rows + 32, 0xcd);
        void* c = fresh_chroma();
        check(slot8(c, dst.data(), 1, 0, chroma_src.data(), src_pitch, src_pitch * rows) == kNotImpl &&
                  all_bytes(dst.data(), dst.size(), 0xcd),
              "a plane other than 0 is refused by name and writes nothing");
        check(slot8(fresh_chroma(), dst.data(), 0, 1, chroma_src.data(), src_pitch, src_pitch * rows) == kNotImpl,
              "a subresource other than 0 is refused by name");
        check(slot8(fresh_chroma(), dst.data(), 0, 0, chroma_src.data(), 1024, src_pitch * rows) == kNotImpl,
              "a source row pitch smaller than the description's row is refused by name");
        check(slot8(fresh_chroma(), dst.data(), 0, 0, chroma_src.data(), src_pitch, 1920u * 100u) == kNotImpl,
              "a source byte count that does not hold the description's rows is refused by name");
    }
    {
        std::vector<uint8_t> dst(static_cast<size_t>(dst_pitch) * rows + 32, 0xcd);
        void* c = fresh_chroma();
        check(slot8(c, dst.data(), 0, 0, chroma_src.data(), src_pitch, src_pitch * rows) == S_OK,
              "the first source is placed");
        check(slot8(c, dst.data(), 0, 0, chroma_src.data() + 4, src_pitch, src_pitch * rows) == kNotImpl,
              "a second source for a one-level description is refused by name");
    }

    // A description the linear model does not lay out keeps the tree's flat path with the switch on: the switch does
    // not change a shape it has no layout for (its census line names it instead).
    Desc multi_mip = Desc(3, 256, 128, 1, 2, 61, 1, kLinearMode, 0x00300000);
    void* multi_computer = nullptr;
    check(create(multi_mip.bytes, &multi_computer) == kNotImpl, "a two-level description is still refused at the create");
    std::vector<uint8_t> multi_dst(0x8000 + 16, 0xcd);
    check(slot8(multi_computer, multi_dst.data(), 0, 0, atlas_src.data(), 0x100, 0x8000) == S_OK,
          "the switch on keeps the tree's flat copy for a description it does not lay out");
    check(std::memcmp(multi_dst.data(), atlas_src.data(), 0x8000) == 0, "that fallback is the tree's byte range");
    set_movie_plane_upload_for_test(false);
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
        check(checks_movie_plane_upload(), "the movie-plane upload rule cases ran");
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
