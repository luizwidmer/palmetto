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

Desc array_desc(uint16_t array_size, uint64_t width = 1, uint32_t height = 1, uint16_t mip_levels = 1,
                uint32_t format = 28, uint32_t samples = 1, uint32_t layout = kLinearMode,
                uint32_t flags = 0x00300000, uint32_t dimension = 3) {
    return Desc(dimension, width, height, array_size, mip_levels, format, samples, layout, flags);
}

bool checks_census_shape_helpers() {
    using rt::xg_xs::CensusClass;
    using rt::xg_xs::CensusShape;
    using rt::xg_xs::census_class;
    using rt::xg_xs::parse_census_shape_line;
    using rt::xg_xs::parse_linear_array_coverage;

    check(!parse_linear_array_coverage(nullptr) && !parse_linear_array_coverage("") &&
              !parse_linear_array_coverage("0") && !parse_linear_array_coverage("off") &&
              !parse_linear_array_coverage("NO") && !parse_linear_array_coverage("unknown"),
          "the linear-array switch parser defaults off for unset, empty, 0, off, no and unknown values");
    check(parse_linear_array_coverage("on") && parse_linear_array_coverage("1") &&
              parse_linear_array_coverage("YES") && parse_linear_array_coverage("TrUe"),
          "the linear-array switch parser accepts on, 1, yes and true case-insensitively");

    const char* line = "[rt] xg_xs census shape #7: dim 3 1x1 array 2 mips 1 format 28 layout 0x100 "
                       "flags 0x300000 samples 1 -> linear array 2 x 1x1";
    CensusShape shape{};
    check(parse_census_shape_line(line, &shape), "the census parser accepts a bounded synthetic array shape line");
    check(shape.index == 7 && shape.dimension == 3 && shape.width == 1 && shape.height == 1 &&
              shape.array_size == 2 && shape.mip_levels == 1 && shape.format == 28 && shape.layout == 0x100 &&
              shape.flags == 0x300000 && shape.samples == 1,
          "the census parser retains all description words before an interleaved suffix");
    check(census_class(shape) == CensusClass::kArray, "the census classifier identifies an array as the first gate");
    CensusShape single = shape;
    single.array_size = 1;
    check(census_class(single) == CensusClass::kLinearCandidate,
          "the census classifier keeps an explicit-linear one-slice shape as a candidate");
    single.mip_levels = 2;
    check(census_class(single) == CensusClass::kMipChain, "the census classifier names a multi-mip shape");
    single.mip_levels = 1;
    single.layout = 0x10a;
    check(census_class(single) == CensusClass::kNonLinear, "the census classifier names a non-linear layout");
    single.dimension = 4;
    check(census_class(single) == CensusClass::kThreeDimensional, "the census classifier names a 3D shape first");
    single.dimension = 1;
    check(census_class(single) == CensusClass::kOther, "the census classifier leaves other dimensions unclassified");
    check(!parse_census_shape_line(nullptr, &shape) && !parse_census_shape_line(line, nullptr) &&
              !parse_census_shape_line("[rt] xg_xs census shape #7: malformed", &shape),
          "the census parser refuses null pointers and incomplete synthetic lines");
    return true;
}

bool checks_linear_array_environment_read_once() {
    using rt::xg_xs::linear_array_coverage;
    using rt::xg_xs::set_linear_array_coverage_for_test;
    check(SetEnvironmentVariableA("PALMETTO_XG_LINEAR_ARRAYS", "on") != 0,
          "the array switch test sets a synthetic process environment value");
    check(linear_array_coverage(), "the array switch reads its environment value on first use");
    check(SetEnvironmentVariableA("PALMETTO_XG_LINEAR_ARRAYS", "off") != 0,
          "the array switch test changes the synthetic process environment value");
    check(linear_array_coverage(), "the array switch remains fixed after its first environment read");
    SetEnvironmentVariableA("PALMETTO_XG_LINEAR_ARRAYS", nullptr);
    set_linear_array_coverage_for_test(false);
    return true;
}

bool check_array_record(void* computer, uint32_t array_size) {
    auto slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
    uint8_t record[kLayoutBytes + 0x20];
    std::memset(record, 0xcd, sizeof(record));
    check(slot2(computer, record) == S_OK, "slot 2 fills the measured array's layout record");
    check(u64(record, 0x00) == static_cast<uint64_t>(array_size) * 0x100 && u64(record, 0x08) == 0x100,
          "array record size is N 256-byte slices with 256-byte base alignment");
    check(u32(record, 0x10) == 1 && u32(record, 0x14) == 1 && u32(record, 0x1B38) == 3,
          "array record has one mip, one colour plane and TEXTURE2D dimension");
    check(u64(record, kPlane0 + 0x08) == static_cast<uint64_t>(array_size) * 0x100 &&
              u64(record, kPlane0 + 0x10) == 0x100 && u64(record, kPlane0 + 0x20) == 0x100 &&
              u32(record, kPlane0 + 0x28) == 4,
          "array plane size is N slices, with one 256-byte slice and a four-byte element");
    check(u64(record, kMip0) == 0x100 && u64(record, kMip0 + 0x08) == 0 && u64(record, kMip0 + 0x10) == 0x100 &&
              u32(record, kMip0 + 0x18) == 64 && u32(record, kMip0 + 0x1C) == 0x100,
          "array mip 0 is one 1x1 RGBA8 slice at a 64-element / 256-byte pitch");
    check(u32(record, kMip0 + 0x24) == 64 && u32(record, kMip0 + 0x28) == 1 &&
              u32(record, kMip0 + 0x2C) == array_size && u32(record, kMip0 + 0x30) == 1 &&
              u32(record, kMip0 + 0x34) == 1 && u32(record, kMip0 + 0x40) == array_size &&
              u32(record, kMip0 + 0x44) == 1 && u32(record, kMip0 + 0x48) == 0,
          "array mip 0 keeps the 1x1 texel extent, N slices, one sample and linear mode");
    check(all_bytes(record + kMip0 + kMipStride, kPlane0 + 0x6C8 - (kMip0 + kMipStride), 0) &&
              all_bytes(record + kPlane0 + 0x6C8, 0x1B38 - (kPlane0 + 0x6C8), 0),
          "array record zeroes unused mips and planes");
    check(all_bytes(record + kLayoutBytes, 0x20, 0xcd), "array record does not write past 0x1B40 bytes");
    return true;
}

bool checks_linear_array_coverage() {
    using rt::xg_xs::set_linear_array_coverage_for_test;
    using rt::xg_xs::set_movie_plane_upload_for_test;
    set_movie_plane_upload_for_test(false);
    set_linear_array_coverage_for_test(true);

    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto slot8_of = [](void* computer) {
        return reinterpret_cast<Slot8Fn>((*static_cast<void* const* const*>(computer))[8]);
    };
    const uint16_t measured_sizes[] = {2, 6, 12};
    for (uint16_t array_size : measured_sizes) {
        Desc desc = array_desc(array_size);
        void* computer = nullptr;
        check(create(desc.bytes, &computer) == S_OK, "the measured format-28 1x1 array shape is accepted opt-in");
        check(computer != nullptr, "the measured array shape gets a texture computer");
        if (!computer) continue;
        check_array_record(computer, array_size);

        std::vector<uint8_t> source(static_cast<size_t>(array_size) * 4);
        for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>((i * 37 + array_size * 11) & 0xff);
        std::vector<uint8_t> destination(static_cast<size_t>(array_size) * 0x100 + 16, 0xcd);
        for (uint32_t subresource = 0; subresource < array_size; ++subresource) {
            check(slot8_of(computer)(computer, destination.data(), 0, subresource,
                                     source.data() + static_cast<size_t>(subresource) * 4, 4, 4) == S_OK,
                  "slot 8 copies one measured RGBA8 array subresource");
        }
        bool contents_match = true, slice_padding_untouched = true;
        for (uint32_t subresource = 0; subresource < array_size; ++subresource) {
            const size_t dst_at = static_cast<size_t>(subresource) * 0x100;
            const size_t src_at = static_cast<size_t>(subresource) * 4;
            if (std::memcmp(destination.data() + dst_at, source.data() + src_at, 4) != 0) contents_match = false;
            if (!all_bytes(destination.data() + dst_at + 4, 0xfc, 0xcd)) slice_padding_untouched = false;
        }
        check(contents_match, "each array subresource lands at its 0x100-byte slice offset");
        check(slice_padding_untouched, "array copies write only the four texel bytes of each slice");
        check(all_bytes(destination.data() + static_cast<size_t>(array_size) * 0x100, 16, 0xcd),
              "array copies leave the resource guard bytes untouched");
    }

    // The single-slice path remains the existing general linear model while the array opt-in is on.
    Desc single = white_desc();
    void* single_computer = nullptr;
    check(create(single.bytes, &single_computer) == S_OK,
          "enabling measured-array coverage does not reject the existing one-slice 1x1 shape");
    if (single_computer) {
        uint8_t record[kLayoutBytes];
        std::memset(record, 0xcd, sizeof(record));
        auto slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(single_computer))[2]);
        check(slot2(single_computer, record) == S_OK && u64(record, 0x00) == 0x100 &&
                  u64(record, kPlane0 + 0x10) == 0x100,
              "a single 1x1 format-28 texture still gets the ordinary linear record");
    }

    // Only the exact measured descriptor class is accepted; each nearby shape remains refused by name and slot 2
    // leaves a sentinel record untouched.
    struct RefusalCase {
        Desc desc;
        const char* name;
    };
    const RefusalCase refusals[] = {
        {array_desc(3), "an unmeasured three-slice count"},
        {array_desc(2, 2), "a non-1-pixel width"},
        {array_desc(2, 1, 2), "a non-1-pixel height"},
        {array_desc(2, 1, 1, 2), "a multi-mip array"},
        {array_desc(2, 1, 1, 1, 29), "the distinct RGBA8 sRGB format 29"},
        {array_desc(2, 1, 1, 1, 61), "a different supported format"},
        {array_desc(2, 1, 1, 1, 28, 4), "a multisampled array"},
        {array_desc(2, 1, 1, 1, 28, 1, 0), "a layout selected by the library"},
        {array_desc(2, 1, 1, 1, 28, 1, 0x10a), "an explicit swizzled layout"},
        {array_desc(2, 1, 1, 1, 28, 1, kLinearMode, 0x00300001), "a render-target array"},
        {array_desc(2, 1, 1, 1, 28, 1, kLinearMode, 0x00300002), "a depth-stencil array"},
        {array_desc(2, 1, 1, 1, 28, 1, kLinearMode, 0x00300000, 2), "a 1D array description"},
        {array_desc(2, 1, 1, 1, 28, 1, kLinearMode, 0x00300000, 4), "a 3D description"},
        {array_desc(2, 0), "a zero-width array"},
        {array_desc(0), "a zero-array-count description"},
    };
    for (const RefusalCase& refusal : refusals) {
        void* computer = nullptr;
        check(create(refusal.desc.bytes, &computer) == kNotImpl, refusal.name);
        if (!computer) {
            check(false, "a refused array description still returns the measured non-null computer contract");
            continue;
        }
        uint8_t record[kLayoutBytes];
        std::memset(record, 0x5a, sizeof(record));
        auto slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
        check(slot2(computer, record) == kNotImpl, "slot 2 refuses the same unmeasured array description");
        check(all_bytes(record, sizeof(record), 0x5a), "a refused array record writes nothing");
    }

    // Slot-8 argument edges on a supported computer: plane, range, pitch and source-size refusals are no-write.
    Desc edge_desc = array_desc(2);
    void* edge_computer = nullptr;
    check(create(edge_desc.bytes, &edge_computer) == S_OK && edge_computer != nullptr,
          "a fresh measured two-slice computer is available for edge refusals");
    if (edge_computer) {
        auto slot8 = slot8_of(edge_computer);
        uint8_t source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<uint8_t> destination(2 * 0x100 + 8, 0xcd);
        check(slot8(edge_computer, destination.data(), 1, 0, source, 4, 4) == kNotImpl,
              "a nonzero plane is refused by the measured array upload");
        check(slot8(edge_computer, destination.data(), 0, 2, source, 4, 4) == kNotImpl,
              "a subresource at the array-size boundary is refused");
        check(slot8(edge_computer, destination.data(), 0, 0, source, 3, 4) == kNotImpl,
              "a row pitch shorter than the four-byte texel is refused");
        check(slot8(edge_computer, destination.data(), 0, 0, source, 4, 3) == kNotImpl,
              "a byte count shorter than the four-byte texel is refused");
        check(slot8(edge_computer, destination.data(), 0, 0, source, 0, 0) == kNotImpl,
              "an empty source row is refused");
        check(slot8(edge_computer, nullptr, 0, 0, source, 4, 4) == kNotImpl &&
                  slot8(edge_computer, destination.data(), 0, 0, nullptr, 4, 4) == kNotImpl,
              "null source and destination pointers are refused");
        check(all_bytes(destination.data(), destination.size(), 0xcd),
              "every rejected array-copy edge leaves the destination untouched");

        // A padded source row is safe for this one-texel subresource; only the texel is read and written.
        check(slot8(edge_computer, destination.data(), 0, 0, source, 8, 8) == S_OK,
              "a source row with safe trailing padding still copies the one measured texel");
        check(std::memcmp(destination.data(), source, 4) == 0 && all_bytes(destination.data() + 4, 0xfc, 0xcd),
              "a padded source row still writes exactly four destination bytes");
        const std::vector<uint8_t> after_first = destination;
        check(slot8(edge_computer, destination.data(), 0, 0, source, 4, 4) == kNotImpl,
              "a duplicate subresource is refused for the same computer");
        check(destination == after_first, "a refused duplicate does not rewrite any array bytes");
    }

    // A destination subresource that crosses into a guard page is refused before the copy.
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const size_t page_size = system_info.dwPageSize;
    void* guarded = VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(guarded != nullptr, "the range-edge test reserves synthetic committed pages");
    if (guarded) {
        auto* bytes = static_cast<uint8_t*>(guarded);
        std::memset(bytes, 0xcd, page_size);
        DWORD old_protect = 0;
        const bool guard_set = VirtualProtect(bytes + page_size, page_size, PAGE_NOACCESS, &old_protect) != 0;
        check(guard_set, "the range-edge test protects the following synthetic page");
        if (guard_set && edge_computer) {
            auto slot8 = slot8_of(edge_computer);
            const void* source = "RGBA";
            check(slot8(edge_computer, bytes + page_size - 128, 0, 1, source, 4, 4) == kNotImpl,
                  "a destination slice landing in a no-access page is refused");
            check(all_bytes(bytes, page_size, 0xcd), "a range refusal does not modify the committed page");
        }
        VirtualFree(guarded, 0, MEM_RELEASE);
    }

    set_linear_array_coverage_for_test(false);
    return true;
}

bool checks_array_switch_off_is_unchanged() {
    using rt::xg_xs::set_linear_array_coverage_for_test;
    set_linear_array_coverage_for_test(false);
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    const uint16_t measured_sizes[] = {2, 6, 12};
    for (uint16_t array_size : measured_sizes) {
        Desc desc = array_desc(array_size);
        void* computer = nullptr;
        check(create(desc.bytes, &computer) == kNotImpl,
              "the opt-out keeps the previous one-slice create refusal for arrays");
        check(computer != nullptr, "the opt-out retains the prior non-null computer contract");
        if (!computer) continue;
        auto slot2 = reinterpret_cast<Slot2Fn>((*static_cast<void* const* const*>(computer))[2]);
        uint8_t record[kLayoutBytes];
        std::memset(record, 0x5a, sizeof(record));
        check(slot2(computer, record) == kNotImpl && all_bytes(record, sizeof(record), 0x5a),
              "the opt-out keeps the previous slot-2 refusal and does not write the record");

        auto slot8 = reinterpret_cast<Slot8Fn>((*static_cast<void* const* const*>(computer))[8]);
        std::vector<uint8_t> source(static_cast<size_t>(array_size) * 4);
        for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>((i * 19 + array_size) & 0xff);
        std::vector<uint8_t> destination(static_cast<size_t>(array_size) * 0x100 + 16, 0xcd);
        for (uint32_t subresource = 0; subresource < array_size; ++subresource) {
            check(slot8(computer, destination.data(), 0, subresource,
                        source.data() + static_cast<size_t>(subresource) * 4, 4, 4) == S_OK,
                  "the opt-out retains the previous flat slot-8 copy for an array");
        }
        std::vector<uint8_t> expected(destination.size(), 0xcd);
        std::memcpy(expected.data(), source.data(), source.size());
        check(destination == expected,
              "the opt-out array bytes are identical to the previous packed flat-copy result");
    }
    return true;
}

bool checks_linear_array_computer_reuse() {
    using rt::xg_xs::set_linear_array_coverage_for_test;
    set_linear_array_coverage_for_test(true);
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    Desc desc = array_desc(2);
    void* first = nullptr;
    check(create(desc.bytes, &first) == S_OK && first != nullptr,
          "a measured array computer is prepared for the static-pool reuse test");
    if (!first) return true;
    auto slot8 = reinterpret_cast<Slot8Fn>((*static_cast<void* const* const*>(first))[8]);
    uint8_t source[4] = {0x91, 0x82, 0x73, 0x64};
    uint8_t destination[2 * 0x100];
    std::memset(destination, 0xcd, sizeof(destination));
    check(slot8(first, destination, 0, 0, source, 4, 4) == S_OK,
          "the first array subresource copy is recorded before pool reuse");

    void* last = nullptr;
    for (size_t i = 0; i < 64; ++i) {
        last = nullptr;
        check(create(desc.bytes, &last) == S_OK && last != nullptr,
              "a synthetic creation cycles the fixed computer pool safely");
    }
    check(last == first, "64 creations wrap to the same fixed computer slot");
    check(slot8(first, destination, 0, 0, source, 4, 4) == S_OK,
          "reusing a computer clears its per-array duplicate mask");
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
    check(checks_census_shape_helpers(), "the census and array-switch parser cases ran");
    check(checks_linear_array_environment_read_once(), "the linear-array environment gate cases ran");

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
        check(checks_linear_array_coverage(), "the measured linear-array coverage cases ran");
        check(checks_array_switch_off_is_unchanged(), "the array switch-off byte-parity cases ran");
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
    if (linear) check(checks_linear_array_computer_reuse(), "the linear-array computer-pool reuse cases ran");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
