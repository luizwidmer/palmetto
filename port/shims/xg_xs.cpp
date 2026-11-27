// xg_xs.dll: the console's texture layout library. The game executable (1.0.2.1) imports two functions from it.
//
// Measured from the call sites in the executable, read statically (shapes and checks only):
//   HRESULT XGSuggestSwizzleMode(const desc*, 0, uint32_t* mode)
//     2 call sites, both in the game's helper that fills a texture description before XGCreateTextureComputer. The
//     out pointer is a 32-bit field of that description (offset 0x2C), which the helper first sets to 0 and, when the
//     mode is known, writes itself as 0x100 | mode. The HRESULT is only passed back up; no caller branches on it.
//   HRESULT XGCreateTextureComputer(const desc*, computer** out)
//     4 call sites. Two test the HRESULT and take their failure path when it is negative; two ignore it and call
//     through *out at once. On the computer the callers use vtable slot 1 after they are done (3 sites, read as
//     Release; no site calls slot 0, read as AddRef, nor QueryInterface), slot 2 with one out pointer to a large
//     structure (2 sites), slot 8 with six arguments in a loop over subresources (1 site) and slot 10 with seven
//     arguments (1 site). No site reads the return value of slots 2, 8 or 10.
//
// Measured at run time (docs/texture_layout.md; PALMETTO_XG_TRACE=1 counts every call, the E_NOTIMPL lines are
// warn_once lines and are not a count): a 150 s boot reaches XGSuggestSwizzleMode 15 times, XGCreateTextureComputer 8
// times and slot 8 8 times (slot 2 and slot 10 not at all), all from the title's placed-resource fill path
// (game+0x193DE10 -> game+0x16F7100). Slot 8's measured arguments are (destination resource address, 0, subresource,
// linear source for that subresource, bytes per row, subresource bytes); it is a CPU copy into the title's own pages,
// not a D3D12 copy (the measured window has no copy commands).  With the family answering (PALMETTO_XG_LAYOUT=linear)
// the same window reaches 44 suggest calls, 24 computer creations, 2 slot-2 calls and 65 slot-8 copies writing
// 34,044 B - so the refusal does gate this title-side upload, and the frame still does not change because the
// runtime's placed-texture stand-in is an ordinary committed image that does not read the title's pages.
//
// The two remaining calls, decoded from the executable's own code and from a boot (bounded PALMETTO_XG_DUMP=1):
// - Slot 2 is GetResourceLayout: one out pointer to the whole XG_RESOURCE_LAYOUT record. The record is 0x1B40 bytes,
//   not the 0x6C8 the previous page said: the caller at game+0x979C40 allocates 0x1B98 bytes and passes rsp+0x58, and
//   the one at game+0x5121980 allocates 0x1BA0 and passes rsp+0x60, so exactly 0x1B40 bytes follow the pointer. What
//   0x6C8 is, is one plane of that record (0x38-byte plane header + 15 mip levels of 0x70): game+0x5121980 copies
//   plane[arg] out of the record with memcpy(rsp+0x60+0x18+arg*0x6C8, 0x6C8). Header: size u64 @0, alignment u64 @8,
//   mip levels u32 @0x10, plane count u32 @0x14, planes from @0x18 (0x6C8 each), dimension u32 @0x1B38. Plane:
//   usage u32 @0, size u64 @8, slice size u64 @0x10, alignment u64 @0x20, bytes per element u32 @0x28, first mip in
//   the tail u32 @0x2C, tail mips u32 @0x30, mip levels from @0x38. Mip level: size u64 @0, offset u64 @8, slice
//   size u64 @0x10, pitch in pixels u32 @0x18, pitch in bytes u32 @0x1C, alignment u32 @0x20, padded width / height /
//   slices u32 @0x24 / 0x28 / 0x2C, width / height in elements u32 @0x30 / 0x34, width / height in pixels u32 @0x38 /
//   0x3C, depth-or-slices u32 @0x40, samples u32 @0x44, swizzle mode u32 @0x48, 0x60 = 1, 0x64 = in-tail flag. Those
//   field positions are the ones the executable itself reads: game+0x979C40 reads record+0x50/0x60/0x6C for mip level
//   i (mip i starts at 0x50+i*0x70, i.e. plane 0's level i), and the layout-cache callers (game+0x5121690 and
//   game+0x555E9E0) read plane slice size @0x10 and, per level, size @+0, offset @+8, slice size @+0x10 and pitch
//   bytes @+0x1C. The record the title reads is therefore modelled from its own reads, not guessed.
// - Slot 10 is GetResourceTiling, D3D12's seven-argument shape: the caller at game+0x3441F0 passes (u32* total tiles,
//   packed-mip-desc out, tile-shape out, in/out subresource-tiling count = 16, first subresource = 0, tiling list) and
//   its 12-byte entries are read back as WidthInTiles u32 @0, HeightInTiles u16 @4, DepthInTiles u16 @6 and
//   StartTileIndexInOverallResource u32 @8 (the caller builds its own packed width/height/tile-index word from those
//   three). A tile list only exists for a 64 KiB-tiled resource; the runtime creates none, so answering it would mean
//   fabricating tile indices, and it stays refused by name.
//
// The runtime has no model of the console's tiled resource layout (port/src/gfx10_tiling.h covers two swizzle modes of
// one 2D level, without mips or the layout record), so nothing here computes a console tiled layout:
// - Default (PALMETTO_XG_LAYOUT unset or "linear"): the honest linear answer for a backend whose textures are
//   ordinary row-major committed images. XGSuggestSwizzleMode returns S_OK and writes 0x100 - the explicit linear
//   layout the title's own description helper writes for a known texture; XGCreateTextureComputer returns S_OK for a
//   description whose layout can be answered (a one-level, one-slice 1D/2D texture that names layout 0x100, no
//   render-target/depth-stencil flags, a format with an element the linear rule covers) and stores its description;
//   slot 2 fills the whole 0x1B40 record with that description's linear layout (rows padded to 256 bytes, one plane,
//   record bytes that are not a field zeroed - no stack bytes survive); slot 8 copies a linear subresource at the
//   offset its row-major source names. Anything the runtime cannot represent is refused with the field named: a
//   buffer / 3D / multi-level / arrayed description, MSAA, a render-target or depth-stencil resource (its CMASK /
//   HTILE metadata plane), an unsupported or planar format, or a layout field other than the explicit linear 0x100.
//   Refusing at the create is what keeps the layout cache from consuming the record at all: its callers check that
//   HRESULT, and slot 2 refuses too should one reach it anyway.
// - PALMETTO_XG_LAYOUT=refuse: bit-identical to the previous shim. Both exports refuse with E_NOTIMPL,
//   XGSuggestSwizzleMode writes nothing, XGCreateTextureComputer still stores a computer (the two callers that do not
//   check would otherwise call through a null pointer, so this deliberately breaks COM's rule that *out is null on
//   failure); every computer slot prints once and returns E_NOTIMPL without writing its arguments.
// - PALMETTO_XG_TRACE=1 logs the first calls with their desc words and arguments, and a count line every 1024 calls.
//   PALMETTO_XG_DUMP=1 additionally dumps the first 8 descriptions and the first 8 filled layout records (bounded), so
//   one boot shows the shapes the decode above was taken from. The counters are also printed on the first 8 calls of
//   each entry, so a boot killed at its timeout still shows how often the title reached the API.
//
// Hypothesis, not measured: slots 0 and 1 being AddRef and Release; that callers reach no slot above kSlots - 1 (the
// computer is also stored in an object at one site and may be called later); the slot-2 record fields this file does
// not fill (they follow the linear layout rule, and no measured reader reads them, so they are zeroed rather than
// guessed).
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "shims.h"
#include "xg_xs.h"

// The movie-plane upload rule's switch (docs/peer_port_movie.md section 12). It is a separate switch from the layout
// one: the layout answer the tree serves is measured and unchanged, and only the placement of a linear subresource
// whose row pitch differs from the layout's padded row pitch is behind it. Unset/empty/0/off/no and any unknown word
// are off; on/1/yes/true (any case) are on. Read once, on the first copy, so the switch is fixed for the process.
namespace rt::xg_xs {

namespace {
std::atomic<int> g_movie_plane_upload{-1};  // -1: not read yet; 0 off; 1 on
}

bool parse_movie_plane_upload(const char* text) {
    if (!text || !text[0]) return false;
    return _stricmp(text, "on") == 0 || _stricmp(text, "1") == 0 || _stricmp(text, "yes") == 0 ||
           _stricmp(text, "true") == 0;
}

bool movie_plane_upload() {
    int value = g_movie_plane_upload.load(std::memory_order_relaxed);
    if (value < 0) {
        char text[8] = {};
        const DWORD n = GetEnvironmentVariableA("PALMETTO_XG_MOVIE_PLANES", text, sizeof(text));
        value = (n > 0 && n < sizeof(text) && parse_movie_plane_upload(text)) ? 1 : 0;
        g_movie_plane_upload.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

void set_movie_plane_upload_for_test(bool enabled) {
    g_movie_plane_upload.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

}  // namespace rt::xg_xs

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;
constexpr size_t kDescBytes = 0x38;     // XG resource description the measured call sites build
constexpr size_t kMaxComputers = 64;    // static pool; the measured boot creates one per placed texture

// XG_RESOURCE_LAYOUT, the slot-2 record: header + four planes; a plane is a 0x38-byte plane header and 15 mip
// levels of 0x70. The fitted layout of docs/texture_layout.md: size, alignment, mip count and plane count in the
// header, one plane per surface of the resource.
constexpr size_t kLayoutBytes = 0x1B40;
constexpr size_t kLayoutHeaderBytes = 0x18;
constexpr size_t kLayoutPlaneBytes = 0x6C8;      // 0x38 + 15 * 0x70
constexpr size_t kLayoutMipBytes = 0x70;
constexpr size_t kLayoutMipCount = 15;

// The description's layout field is the explicit swizzle mode: 0x100 | mode, and mode 0 is the linear layout. It is
// the value the title's own description helper writes for a known texture, so it is the one linear answer that is not
// this file's own choice.
constexpr uint32_t kLinearLayoutField = 0x100;

// Behaviour: "linear" (default; the honest linear layout a row-major backend can serve) or "refuse" (the measured
// default of the previous shim: every entry and slot returns E_NOTIMPL and writes nothing).
enum class Layout { kRefuse, kLinear };

Layout read_layout() {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_XG_LAYOUT", value, sizeof(value));
    if (n > 0 && _stricmp(value, "refuse") == 0) return Layout::kRefuse;
    return Layout::kLinear;
}

bool read_flag(const char* name) {
    char value[8] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n > 0 && value[0] != '0';
}

// Read once, on the first call, so a test (or a debugger) can set the switch in the importing process before it
// reaches the shim.  The measured boot reaches the shim about a second after startup, so the switch is in place.
Layout layout() {
    static const Layout cached = read_layout();
    return cached;
}

const bool kTrace = read_flag("PALMETTO_XG_TRACE");
const bool kDump = read_flag("PALMETTO_XG_DUMP");

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_suggest{0};
std::atomic<uint64_t> g_create{0};
std::atomic<uint64_t> g_slots[kSlots] = {};
std::atomic<uint64_t> g_copied_bytes{0};
std::atomic<uint64_t> g_copies{0};

// Measurement census (docs/peer_port_movie.md section 12). The family's measured call sites hand the shim a
// 0x38-byte D3D12_RESOURCE_DESC; the census keys the calls on the description words, so one boot reports which
// resource shapes the title actually asks for, how many creations, records and subresource copies each shape gets,
// the largest plane / subresource a copy names and the row pitch and byte count the copy passes. It is trace-only:
// the counters are updated on every call, but nothing is printed unless PALMETTO_XG_TRACE=1, so the default path and
// its log are the tree's, byte for byte. The shape table is bounded (32 shapes) and append-only; a shape past the
// bound is counted, not stored.
constexpr size_t kCensusShapes = 128;

struct DescShape {
    uint32_t dimension;
    uint64_t width;
    uint32_t height;
    uint16_t array;
    uint16_t mips;
    uint32_t format;
    uint32_t samples;
    uint32_t layout;
    uint32_t flags;
};

DescShape shape_of(const uint8_t* desc) {
    DescShape shape{};
    if (!desc) return shape;
    auto u16 = [desc](size_t at) {
        uint16_t v = 0;
        std::memcpy(&v, desc + at, sizeof(v));
        return v;
    };
    auto u32 = [desc](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, desc + at, sizeof(v));
        return v;
    };
    auto u64 = [desc](size_t at) {
        uint64_t v = 0;
        std::memcpy(&v, desc + at, sizeof(v));
        return v;
    };
    shape.dimension = u32(0x00);
    shape.width = u64(0x10);
    shape.height = u32(0x18);
    shape.array = u16(0x1c);
    shape.mips = u16(0x1e);
    shape.format = u32(0x20);
    shape.samples = u32(0x24);
    shape.layout = u32(0x2c);
    shape.flags = u32(0x30);
    return shape;
}

bool same_shape(const DescShape& a, const DescShape& b) {
    return a.dimension == b.dimension && a.width == b.width && a.height == b.height && a.array == b.array &&
           a.mips == b.mips && a.format == b.format && a.samples == b.samples && a.layout == b.layout &&
           a.flags == b.flags;
}

struct ShapeCounts {
    DescShape shape;
    bool used;
    std::atomic<uint32_t> creates;
    std::atomic<uint32_t> records;
    std::atomic<uint32_t> copies;
    std::atomic<uint32_t> copies_after_first_source;
    std::atomic<uint32_t> negative_source_offsets;
    std::atomic<uint32_t> max_plane;
    std::atomic<uint32_t> max_subresource;
    std::atomic<uint32_t> min_row_pitch;
    std::atomic<uint32_t> max_row_pitch;
    std::atomic<uint32_t> min_bytes;
    std::atomic<uint32_t> max_bytes;
    std::atomic<uint64_t> copied_bytes;
};

ShapeCounts g_shape_counts[kCensusShapes] = {};
std::atomic<uint32_t> g_shape_count{0};      // shapes stored
std::atomic<uint32_t> g_shape_overflow{0};   // calls whose shape could not be stored
std::atomic_flag g_shape_lock = ATOMIC_FLAG_INIT;

struct ShapeSpinLock {
    explicit ShapeSpinLock(std::atomic_flag& flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~ShapeSpinLock() { flag_.clear(std::memory_order_release); }
    std::atomic_flag& flag_;
};

ShapeCounts* census_find_or_add(const uint8_t* desc, bool create, bool* inserted = nullptr) {
    if (inserted) *inserted = false;
    const DescShape shape = shape_of(desc);
    {
        ShapeSpinLock lock(g_shape_lock);
        for (uint32_t i = 0; i < g_shape_count.load(std::memory_order_relaxed); ++i) {
            ShapeCounts& entry = g_shape_counts[i];
            if (entry.used && same_shape(entry.shape, shape)) {
                if (create) entry.creates.fetch_add(1, std::memory_order_relaxed);
                return &entry;
            }
        }
        const uint32_t index = g_shape_count.load(std::memory_order_relaxed);
        if (index >= kCensusShapes) {
            g_shape_overflow.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        ShapeCounts& entry = g_shape_counts[index];
        entry.shape = shape;
        entry.used = true;
        entry.creates.store(create ? 1 : 0, std::memory_order_relaxed);
        entry.records.store(0, std::memory_order_relaxed);
        entry.copies.store(0, std::memory_order_relaxed);
        entry.copies_after_first_source.store(0, std::memory_order_relaxed);
        entry.negative_source_offsets.store(0, std::memory_order_relaxed);
        entry.max_plane.store(0, std::memory_order_relaxed);
        entry.max_subresource.store(0, std::memory_order_relaxed);
        entry.min_row_pitch.store(0xffffffffu, std::memory_order_relaxed);
        entry.max_row_pitch.store(0, std::memory_order_relaxed);
        entry.min_bytes.store(0xffffffffu, std::memory_order_relaxed);
        entry.max_bytes.store(0, std::memory_order_relaxed);
        entry.copied_bytes.store(0, std::memory_order_relaxed);
        g_shape_count.store(index + 1, std::memory_order_relaxed);
        if (inserted) *inserted = true;
        return &entry;
    }
}

void census_note_min(std::atomic<uint32_t>& slot, uint32_t value) {
    uint32_t current = slot.load(std::memory_order_relaxed);
    while (value < current && !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void census_note_max(std::atomic<uint32_t>& slot, uint32_t value) {
    uint32_t current = slot.load(std::memory_order_relaxed);
    while (value > current && !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

uint32_t census_index_of(const ShapeCounts* entry) {
    if (!entry) return 0;
    for (uint32_t i = 0; i < g_shape_count.load(std::memory_order_relaxed); ++i)
        if (&g_shape_counts[i] == entry) return i;
    return 0;
}

// The census line for a shape being stored, with the linear model's verdict (the model is defined below the
// census, so this print is defined after it too).
void census_print_shape(uint32_t index, const uint8_t* desc);

// Prints once per entry point; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const void* caller) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: no console texture layout on the PC; E_NOTIMPL (caller %s, logged once)\n",
                 name, rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

const char* caller_text(const void* caller) {
    static thread_local char text[96];
    std::snprintf(text, sizeof(text), "%s",
                  rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
    return text;
}

void print_counters(const char* why) {
    std::fprintf(stderr,
                 "[rt] xg_xs counters (%s): suggest %llu, create %llu, slot2 %llu, slot8 %llu, slot10 %llu, "
                 "slots1 %llu, copied %llu bytes in %llu call(s), shapes %u (census: %u over the bound)\n",
                 why, static_cast<unsigned long long>(g_suggest.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_create.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[8].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[10].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_copied_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_copies.load(std::memory_order_relaxed)),
                 g_shape_count.load(std::memory_order_relaxed),
                 g_shape_overflow.load(std::memory_order_relaxed));
}

void note_call(std::atomic<uint64_t>& entry, const char* name) {
    const uint64_t n = entry.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t all = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (kTrace) {
        if (n <= 8 || (n % 1024) == 0) {
            std::fprintf(stderr, "[rt] xg_xs trace: %s call #%llu (all %llu)\n", name,
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(all));
            if ((n & (n - 1)) == 0 || (n % 1024) == 0) print_counters(name);
        }
    } else if (n == 1) {
        std::fprintf(stderr, "[rt] xg_xs trace: %s first call (set PALMETTO_XG_TRACE=1 for details)\n", name);
    }
}

void dump_words(const char* what, const void* data, size_t bytes) {
    if (!data || bytes == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::fprintf(stderr, "[rt] xg_xs   %s (%zu B):", what, bytes);
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t word = 0;
        std::memcpy(&word, p + i, 4);
        std::fprintf(stderr, " %08x", word);
    }
    std::fprintf(stderr, "\n");
}

// True when [address, address+bytes) is all committed, readable, and writable when `need_write` says so.
bool range_ok(const void* address, size_t bytes, bool need_write) {
    if (!address || bytes == 0) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    if (bytes > static_cast<size_t>(UINTPTR_MAX - begin)) return false;
    const uintptr_t end = begin + bytes;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION info{};
    for (uintptr_t at = begin; at < end;) {
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0) return false;
        if (info.State != MEM_COMMIT) return false;
        if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        if (need_write && (info.Protect & kWritable) == 0) return false;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= at) return false;
        at = region_end;
    }
    return true;
}

uint16_t read16(const uint8_t* p, size_t at) {
    uint16_t value = 0;
    std::memcpy(&value, p + at, sizeof(value));
    return value;
}

uint32_t read32(const uint8_t* p, size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, p + at, sizeof(value));
    return value;
}

uint64_t read64(const uint8_t* p, size_t at) {
    uint64_t value = 0;
    std::memcpy(&value, p + at, sizeof(value));
    return value;
}

void write32(uint8_t* p, size_t at, uint32_t value) { std::memcpy(p + at, &value, sizeof(value)); }
void write64(uint8_t* p, size_t at, uint64_t value) { std::memcpy(p + at, &value, sizeof(value)); }

// Bytes per element and pixels per element side (4 for a BCn block) of a DXGI format the linear rule covers.
// 96-bit, packed 4:2:2 and the multi-plane depth/YUV formats are refused (their element is not a whole number of
// bytes, or a resource of them has more than one plane).
bool format_element(uint32_t format, uint32_t& element_bytes, uint32_t& side) {
    element_bytes = 0;
    side = 1;
    if ((format >= 1 && format <= 4) || (format >= 9 && format <= 14)) {  // 128-bit RGBA32 / RGBA16
        element_bytes = 16;
    } else if ((format >= 15 && format <= 18) || (format >= 33 && format <= 38)) {  // 64-bit RG32 / RG16
        element_bytes = 8;
    } else if ((format >= 23 && format <= 32) || (format >= 39 && format <= 43) || format == 67 ||
               (format >= 87 && format <= 93)) {  // 32-bit RGB10A2, RGBA8, R32, R9G9B9E5, BGRA8
        element_bytes = 4;
    } else if ((format >= 48 && format <= 59) || format == 85 || format == 86) {  // 16-bit RG8 / R16, B5G6R5
        element_bytes = 2;
    } else if (format >= 60 && format <= 65) {  // 8-bit R8 / A8
        element_bytes = 1;
    } else if (format >= 70 && format <= 72) {  // BC1
        element_bytes = 8;
        side = 4;
    } else if ((format >= 73 && format <= 78) || (format >= 82 && format <= 84) ||
               (format >= 94 && format <= 99)) {  // BC2, BC3, BC5, BC6H, BC7
        element_bytes = 16;
        side = 4;
    } else if (format >= 79 && format <= 81) {  // BC4
        element_bytes = 8;
        side = 4;
    } else {
        return false;
    }
    return true;
}

// What the whole slot-2 record is filled from: a description whose layout the runtime can answer honestly. Only the
// one-level, one-slice 1D/2D texture is answered, because that is the case the measured window pins (every measured
// description is one level and one slice, and every measured copy is subresource 0); every other description is
// refused with the field named rather than laid out by a rule the window has not measured.
struct LinearLayout {
    uint32_t element_bytes;
    uint32_t side;
    uint32_t dimension;
    uint32_t width;       // pixels, <= 16384
    uint32_t height;      // pixels (1 for a 1D texture)
    uint32_t pitch_pixels;  // mip 0's padded width, pixels
    uint32_t pitch_bytes;   // mip 0's row pitch, bytes (a multiple of 256)
    uint64_t size_bytes;    // the resource: one level, one slice
};

// Fills `out` from the description when the whole record can be written honestly; otherwise leaves it alone and names
// the field that stopped it. `why` points at a static string.
bool linear_layout(const uint8_t* desc, LinearLayout& out, const char** why) {
    if (!desc) {
        *why = "description (null)";
        return false;
    }
    const uint32_t dimension = read32(desc, 0x00);
    const uint64_t width64 = read64(desc, 0x10);
    const uint32_t height = read32(desc, 0x18);
    const uint32_t array_size = read16(desc, 0x1C);
    const uint32_t mip_levels = read16(desc, 0x1E);
    const uint32_t format = read32(desc, 0x20);
    const uint32_t samples = read32(desc, 0x24);
    const uint32_t layout_field = read32(desc, 0x2C);
    const uint32_t flags = read32(desc, 0x30);
    if (dimension != 2 && dimension != 3) {  // D3D12_RESOURCE_DIMENSION_TEXTURE1D / TEXTURE2D
        *why = "dimension (only 1D and 2D textures)";
        return false;
    }
    if (dimension == 2 && height != 1) {
        *why = "height of a 1D texture";
        return false;
    }
    if (width64 == 0 || width64 > 16384 || height == 0 || height > 16384) {
        *why = "extent";
        return false;
    }
    if (array_size != 1) {
        *why = "array size (only one slice is laid out)";
        return false;
    }
    if (mip_levels != 1) {
        *why = "mip count (only one level is laid out)";
        return false;
    }
    if (samples != 1) {
        *why = "sample count";
        return false;
    }
    if (layout_field != kLinearLayoutField) {
        *why = "layout field (only the explicit linear 0x100 is served)";
        return false;
    }
    if ((flags & 0x3) != 0) {  // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | ALLOW_DEPTH_STENCIL
        *why = "resource flags (the CMASK / HTILE metadata plane is not modelled)";
        return false;
    }
    uint32_t element_bytes = 0;
    uint32_t side = 1;
    if (!format_element(format, element_bytes, side)) {
        *why = "format (no whole-byte, single-plane element)";
        return false;
    }
    const uint64_t elements_w = (width64 + side - 1) / side;
    const uint64_t elements_h = (height + static_cast<uint64_t>(side) - 1) / side;
    const uint64_t block = 256 / element_bytes;  // elements in a 256-byte linear row block
    const uint64_t padded_w = ((elements_w + block - 1) / block) * block;
    const uint64_t pitch_bytes = padded_w * element_bytes;
    const uint64_t size_bytes = pitch_bytes * elements_h;
    out.element_bytes = element_bytes;
    out.side = side;
    out.dimension = dimension;
    out.width = static_cast<uint32_t>(width64);
    out.height = height;
    out.pitch_pixels = static_cast<uint32_t>(padded_w * side);
    out.pitch_bytes = static_cast<uint32_t>(pitch_bytes);
    out.size_bytes = size_bytes;
    return true;
}

// The census's shape line, with the linear model's verdict on the description (the same model that decides the
// create and the slot-2 record): one boot then names every shape the title's upload path asks the family about.
void census_print_shape(uint32_t index, const uint8_t* desc) {
    const DescShape shape = shape_of(desc);
    std::fprintf(stderr,
                 "[rt] xg_xs census shape #%u: dim %u %llux%u array %u mips %u format %u layout 0x%x flags 0x%x "
                 "samples %u",
                 index, shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.array,
                 shape.mips, shape.format, shape.layout, shape.flags, shape.samples);
    if (layout() != Layout::kLinear) {
        std::fprintf(stderr, " -> refuse mode (PALMETTO_XG_LAYOUT=refuse)\n");
        return;
    }
    LinearLayout linear{};
    const char* why = "description (null)";
    if (linear_layout(desc, linear, &why)) {
        std::fprintf(stderr,
                     " -> linear %ux%u, one level: row pitch %u B, level %llu B\n", linear.width, linear.height,
                     linear.pitch_bytes, static_cast<unsigned long long>(linear.size_bytes));
    } else {
        std::fprintf(stderr, " -> refused: %s\n", why);
    }
}

// Counts a creation (or a record/copy that finds the shape first) and prints the shape once, on the call that first
// stores it. Trace-only: with PALMETTO_XG_TRACE unset nothing is counted and nothing is printed, so the default path
// does no new work and its log is the tree's, byte for byte.
void census_register(const uint8_t* desc, bool create) {
    if (!kTrace) return;
    bool inserted = false;
    ShapeCounts* entry = census_find_or_add(desc, create, &inserted);
    if (entry && inserted) census_print_shape(census_index_of(entry), desc);
}

void census_note_record(const uint8_t* desc) {
    if (!kTrace) return;
    ShapeCounts* entry = census_find_or_add(desc, false);
    if (!entry) return;
    const uint32_t n = entry->records.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0) return;
    std::fprintf(stderr, "[rt] xg_xs census shape #%u record #%u\n", census_index_of(entry), n);
}

// One copy call: plane, subresource, the caller's row pitch and byte count, and whether the source is not the first
// source the computer saw (the flat path derives a destination offset from that difference). Printed at each
// power-of-two copy count of the shape, bounded.
void census_note_copy(const uint8_t* desc, uint32_t plane, uint32_t subresource, uint32_t row_pitch, uint32_t bytes,
                      bool after_first_source, bool negative_source) {
    if (!kTrace) return;
    ShapeCounts* entry = census_find_or_add(desc, false);
    if (!entry) return;
    const uint32_t n = entry->copies.fetch_add(1, std::memory_order_relaxed) + 1;
    entry->copied_bytes.fetch_add(bytes, std::memory_order_relaxed);
    census_note_max(entry->max_plane, plane);
    census_note_max(entry->max_subresource, subresource);
    census_note_min(entry->min_row_pitch, row_pitch);
    census_note_max(entry->max_row_pitch, row_pitch);
    census_note_min(entry->min_bytes, bytes);
    census_note_max(entry->max_bytes, bytes);
    if (after_first_source) entry->copies_after_first_source.fetch_add(1, std::memory_order_relaxed);
    if (negative_source) entry->negative_source_offsets.fetch_add(1, std::memory_order_relaxed);
    if ((n & (n - 1)) != 0) return;
    std::fprintf(stderr,
                 "[rt] xg_xs census shape #%u copy #%u: plane %u subresource %u row pitch %u B subresource bytes %u B, "
                 "sources after the first %u, negative sources %u\n",
                 census_index_of(entry), n, plane, subresource, row_pitch, bytes,
                 entry->copies_after_first_source.load(std::memory_order_relaxed),
                 entry->negative_source_offsets.load(std::memory_order_relaxed));
}

// Writes the whole XG_RESOURCE_LAYOUT from a linear description: one plane, one mip level, rows padded to 256 bytes,
// mip 0 at offset 0, every byte the structure has that is not one of these fields zeroed (no stack bytes survive).
// Field offsets are the ones the executable reads (see the top of the file and docs/texture_layout.md).
void fill_linear_layout(const LinearLayout& linear, void* out) {
    uint8_t* const record = static_cast<uint8_t*>(out);
    std::memset(record, 0, kLayoutBytes);
    write64(record, 0x00, linear.size_bytes);            // SizeBytes
    write64(record, 0x08, 0x100);                        // BaseAlignmentBytes: one 256-byte linear block
    write32(record, 0x10, 1);                            // MipLevels
    write32(record, 0x14, 1);                            // PlaneCount
    write32(record, 0x1B38, linear.dimension);           // Dimension
    const size_t plane = kLayoutHeaderBytes;             // 0x18
    write32(record, plane + 0x00, 1);                    // usage: colour
    write64(record, plane + 0x08, linear.size_bytes);    // plane size
    write64(record, plane + 0x10, linear.size_bytes);    // slice size (one array slice)
    write64(record, plane + 0x20, 0x100);                // plane base alignment
    write32(record, plane + 0x28, linear.element_bytes); // bytes per element
    write32(record, plane + 0x2C, 1);                    // first mip in the tail: none (1 = one level, no tail)
    write32(record, plane + 0x30, 0);                    // mips in the tail
    const size_t mip = plane + 0x38;                     // mip level 0
    write64(record, mip + 0x00, linear.size_bytes);      // level size (one slice)
    write64(record, mip + 0x08, 0);                      // offset from the plane base: mip 0 sits first
    write64(record, mip + 0x10, linear.size_bytes);      // level slice size
    write32(record, mip + 0x18, linear.pitch_pixels);    // pitch in pixels
    write32(record, mip + 0x1C, linear.pitch_bytes);     // pitch in bytes
    write32(record, mip + 0x20, 0x100);                  // level alignment
    write32(record, mip + 0x24, linear.pitch_pixels / linear.side);  // padded width in elements
    write32(record, mip + 0x28, (linear.height + linear.side - 1) / linear.side);  // padded height in elements
    write32(record, mip + 0x2C, 1);                      // padded slices
    write32(record, mip + 0x30, (linear.width + linear.side - 1) / linear.side);   // width in elements
    write32(record, mip + 0x34, (linear.height + linear.side - 1) / linear.side);  // height in elements
    write32(record, mip + 0x38, linear.width);           // width in pixels
    write32(record, mip + 0x3C, linear.height);          // height in pixels
    write32(record, mip + 0x40, 1);                      // depth-or-slices
    write32(record, mip + 0x44, 1);                      // samples
    write32(record, mip + 0x48, 0);                      // swizzle mode: linear
    write32(record, mip + 0x60, 1);                      // block depth
}

// One bounded dump of a filled record: header, plane and the levels, so a boot shows the same offsets the tests and
// docs pin. Only the first 8 slot-2 calls dump.
void dump_layout_record(const uint8_t* record, const char* caller) {
    std::fprintf(stderr,
                 "[rt] xg_xs   layout record: size 0x%llx align 0x%llx mips %u planes %u dimension %u (caller %s)\n",
                 static_cast<unsigned long long>(read64(record, 0x00)),
                 static_cast<unsigned long long>(read64(record, 0x08)), read32(record, 0x10),
                 read32(record, 0x14), read32(record, 0x1B38), caller);
    for (size_t plane = 0; plane < static_cast<size_t>(read32(record, 0x14)); ++plane) {
        const size_t at = kLayoutHeaderBytes + plane * kLayoutPlaneBytes;
        std::fprintf(stderr,
                     "[rt] xg_xs   plane %zu: usage %u size 0x%llx slice 0x%llx align 0x%llx bpe %u first_tail %u "
                     "tail_mips %u\n",
                     plane, read32(record, at), static_cast<unsigned long long>(read64(record, at + 0x08)),
                     static_cast<unsigned long long>(read64(record, at + 0x10)),
                     static_cast<unsigned long long>(read64(record, at + 0x20)), read32(record, at + 0x28),
                     read32(record, at + 0x2C), read32(record, at + 0x30));
        const size_t mips = read32(record, 0x10);
        for (size_t mip = 0; mip < mips && mip < kLayoutMipCount; ++mip) {
            const size_t level = at + 0x38 + mip * kLayoutMipBytes;
            std::fprintf(stderr,
                         "[rt] xg_xs   plane %zu mip %zu: size 0x%llx offset 0x%llx slice 0x%llx pitch %u px / %u B "
                         "padded %ux%ux%u elements %ux%u pixels %ux%u slices %u samples %u mode %u\n",
                         plane, mip, static_cast<unsigned long long>(read64(record, level + 0x00)),
                         static_cast<unsigned long long>(read64(record, level + 0x08)),
                         static_cast<unsigned long long>(read64(record, level + 0x10)),
                         read32(record, level + 0x18), read32(record, level + 0x1C), read32(record, level + 0x24),
                         read32(record, level + 0x28), read32(record, level + 0x2C), read32(record, level + 0x30),
                         read32(record, level + 0x34), read32(record, level + 0x38), read32(record, level + 0x3C),
                         read32(record, level + 0x40), read32(record, level + 0x44), read32(record, level + 0x48));
        }
    }
}

// Prints a description that cannot be laid out, bounded, naming the field that stopped it. `seen` is the number of
// refusals so far, so the first 8 print.
void warn_refusal(uint32_t seen, const char* entry, const char* why, const void* caller) {
    if (seen > 8) return;
    std::fprintf(stderr, "[rt] xg_xs: %s: no linear layout: %s (E_NOTIMPL; caller %s)\n", entry, why,
                 caller_text(caller));
}

ULONG __stdcall computer_add_ref(void*) { return 1; }
ULONG __stdcall computer_release(void*) { return 1; }

// Per-computer state: the description the title created it with, and the linear source base the first
// CopyIntoSubresource call names (its source offset 0). A linear destination subresource sits at the same offset as
// the row-major source the title passes, so one recorded base is enough to place every subresource.
struct Computer {
    const void* const* vtable;
    const void* linear_source_base;
    std::atomic<bool> have_source;
    std::atomic<bool> warned;
    uint8_t desc[kDescBytes];
};

// What the movie-plane upload rule decided for a slot-8 call.
enum class UploadAnswer { kNotServed, kDone, kRefused };

// Named refusals of the movie-plane rule, once per kind (bounded: the first 8 of each). An unmeasured shape is named
// rather than placed, so a later boot can read which shape stopped the upload instead of finding skewed pages.
void warn_movie_refusal(std::atomic<uint32_t>& seen, const char* what, uint64_t serial, const void* caller) {
    if (seen.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    std::fprintf(stderr, "[rt] xg_xs: slot 8 movie-plane rule refused: %s (call #%llu, caller %s)\n", what,
                 static_cast<unsigned long long>(serial), caller_text(caller));
}

// The movie-plane upload rule (opt-in, PALMETTO_XG_MOVIE_PLANES; docs/peer_port_movie.md section 12). The linear
// layout's rows are 256-byte aligned, so a description whose packed row width is not 256-byte aligned gets a row
// pitch different from its source's; the tree's flat copy places the source's rows at the source's own offsets and
// would skew them. This places the served description's one level row by row: the caller's row pitch is the source
// stride, the layout's pitch is the destination stride, and only `row_bytes` of each row is written (the padding the
// layout adds is left as it was). The measured shapes are unchanged byte for byte because their row pitch already
// equals the layout's (256x128 R8: 256 B rows; 1x1 RGBA8: one 4 B row).
//
// Returns kNotServed when the description is not one the linear model lays out (the caller keeps the tree's flat
// path, byte for byte), kDone when the copy ran, kRefused when the description is served but the call names
// something the one-plane, one-level model has no place for (plane, subresource, a second source) or the source /
// destination cannot hold the rows; a refusal writes nothing.
UploadAnswer upload_linear_rows(Computer* computer, const LinearLayout& linear, void* destination, uint32_t plane,
                                uint32_t subresource, const void* source, uint32_t row_pitch, uint32_t bytes,
                                uint64_t serial, const void* caller) {
    static std::atomic<uint32_t> refused_plane{0}, refused_source{0}, refused_pitch{0}, refused_range{0};
    const DescShape shape = shape_of(computer ? computer->desc : nullptr);
    if (plane != 0 || subresource != 0) {
        char what[224];
        std::snprintf(what, sizeof(what),
                      "plane %u subresource %u of a one-plane, one-level description (dim %u %llux%u format %u array %u "
                      "mips %u); the rule places subresource 0 of plane 0 only",
                      plane, subresource, shape.dimension, static_cast<unsigned long long>(shape.width), shape.height,
                      shape.format, shape.array, shape.mips);
        warn_movie_refusal(refused_plane, what, serial, caller);
        return UploadAnswer::kRefused;
    }
    const bool had_source = computer->have_source.load(std::memory_order_relaxed);
    if (had_source && computer->linear_source_base != source) {
        char what[224];
        std::snprintf(what, sizeof(what),
                      "a second linear source for a one-level description (dim %u %llux%u format %u array %u mips %u); "
                      "the rule places subresource 0 once",
                      shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.format,
                      shape.array, shape.mips);
        warn_movie_refusal(refused_source, what, serial, caller);
        return UploadAnswer::kRefused;
    }
    const uint32_t row_bytes = ((linear.width + linear.side - 1) / linear.side) * linear.element_bytes;
    const uint32_t rows = (linear.height + linear.side - 1) / linear.side;
    if (row_pitch == 0 || row_pitch < row_bytes) {
        char what[224];
        std::snprintf(what, sizeof(what),
                      "source row pitch %u B is smaller than the description's %u B row (dim %u %llux%u format %u "
                      "array %u mips %u, %u B subresource)",
                      row_pitch, row_bytes, shape.dimension, static_cast<unsigned long long>(shape.width), shape.height,
                      shape.format, shape.array, shape.mips, bytes);
        warn_movie_refusal(refused_pitch, what, serial, caller);
        return UploadAnswer::kRefused;
    }
    const uint64_t source_bytes = static_cast<uint64_t>(row_pitch) * (rows - 1) + row_bytes;
    if (source_bytes > bytes) {
        char what[224];
        std::snprintf(what, sizeof(what),
                      "source byte count %u B does not hold the description's %u rows of %u B (%llu B; dim %u %llux%u "
                      "format %u array %u mips %u)",
                      bytes, rows, row_bytes, static_cast<unsigned long long>(source_bytes), shape.dimension,
                      static_cast<unsigned long long>(shape.width), shape.height, shape.format, shape.array,
                      shape.mips);
        warn_movie_refusal(refused_pitch, what, serial, caller);
        return UploadAnswer::kRefused;
    }
    if (!range_ok(destination, linear.size_bytes, true) || !range_ok(source, source_bytes, false)) {
        char what[224];
        std::snprintf(what, sizeof(what),
                      "the destination's layout span or the source's rows are not committed/accessible (dim %u %llux%u "
                      "format %u, %llu B layout)",
                      shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.format,
                      static_cast<unsigned long long>(linear.size_bytes));
        warn_movie_refusal(refused_range, what, serial, caller);
        return UploadAnswer::kRefused;
    }
    if (!computer->have_source.exchange(true, std::memory_order_relaxed)) computer->linear_source_base = source;
    uint8_t* const to = static_cast<uint8_t*>(destination);
    const uint8_t* const from = static_cast<const uint8_t*>(source);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(to + static_cast<uint64_t>(y) * linear.pitch_bytes, from + static_cast<uint64_t>(y) * row_pitch,
                    row_bytes);
    g_copied_bytes.fetch_add(static_cast<uint64_t>(row_bytes) * rows, std::memory_order_relaxed);
    g_copies.fetch_add(1, std::memory_order_relaxed);
    if (kTrace) {
        std::fprintf(stderr,
                     "[rt] xg_xs trace: slot 8 movie-plane rows: %u rows of %u B at the layout's %u B pitch (source "
                     "row pitch %u B)\n",
                     rows, row_bytes, linear.pitch_bytes, row_pitch);
    }
    (void)had_source;
    return UploadAnswer::kDone;
}

template <size_t Slot>
HRESULT __stdcall computer_slot(void*) {
    note_call(g_slots[Slot], "texture computer slot");
    const uint64_t n = g_slots[Slot].load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: texture computer slot%zu call #%llu (caller %s)\n", Slot,
                     static_cast<unsigned long long>(n), caller_text(_ReturnAddress()));
    }
    return kNotImpl;
}

// Slot 2 is GetResourceLayout: one out pointer to the whole 0x1B40-byte record. In the linear layout the record is
// filled from the description the computer was created with; a description that cannot be laid out refuses and writes
// nothing (its callers check XGCreateTextureComputer, so they do not reach here with one).
HRESULT __stdcall computer_slot2(void* self, void* out) {
    note_call(g_slots[2], "texture computer slot2");
    const uint64_t n = g_slots[2].load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: texture computer slot2 call #%llu out %p (caller %s)\n",
                     static_cast<unsigned long long>(n), out, caller_text(_ReturnAddress()));
    }
    if (layout() != Layout::kLinear) return kNotImpl;
    auto* computer = static_cast<Computer*>(self);
    census_note_record(computer ? computer->desc : nullptr);
    if (!out) return E_POINTER;
    LinearLayout linear{};
    const char* why = "computer (/description)";
    if (!computer || !linear_layout(computer->desc, linear, &why)) {
        static std::atomic<uint32_t> refused{0};
        warn_refusal(refused.fetch_add(1, std::memory_order_relaxed) + 1, "GetResourceLayout", why,
                     _ReturnAddress());
        return kNotImpl;
    }
    if (!range_ok(out, kLayoutBytes, true)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[rt] xg_xs: GetResourceLayout: the out pointer is not %zu committed writable bytes; "
                         "refusing (caller %s)\n",
                         kLayoutBytes, caller_text(_ReturnAddress()));
        }
        return kNotImpl;
    }
    fill_linear_layout(linear, out);
    if (kDump && n <= 8) dump_layout_record(static_cast<const uint8_t*>(out), caller_text(_ReturnAddress()));
    return S_OK;
}

// Slot 10 is GetResourceTiling: D3D12's seven-argument shape, decoded from game+0x3441F0 (total tiles u32* out,
// packed-mip-desc out, tile shape out, subresource-tiling count in/out, first subresource, tiling list). A tile list
// only exists for a 64 KiB-tiled resource, and the runtime creates none, so it refuses by name and writes nothing:
// filling a 64 KiB tile index would be fabricated. The caller reads 12-byte entries (WidthInTiles u32 @0,
// HeightInTiles u16 @4, DepthInTiles u16 @6, StartTileIndexInOverallResource u32 @8).
HRESULT __stdcall computer_slot10(void* /*self*/, uint32_t* count, void* tiles, void* packed, uint32_t* inout,
                                  uint32_t first, void* list) {
    note_call(g_slots[10], "texture computer slot10");
    const uint64_t n = g_slots[10].load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr,
                     "[rt] xg_xs trace: texture computer slot10 (GetResourceTiling) call #%llu total_tiles %p "
                     "packed_mip_desc %p tile_shape %p tiling_count_in %u first %u list %p (caller %s)\n",
                     static_cast<unsigned long long>(n), count, tiles, packed, inout ? *inout : 0, first, list,
                     caller_text(_ReturnAddress()));
    }
    if (kDump && n <= 8) {
        std::fprintf(stderr,
                     "[rt] xg_xs   tile list request: %u subresource tiling(s) from %u, 12-byte entries; no 64 KiB "
                     "tile layout on the PC, nothing written\n",
                     inout ? *inout : 0, first);
    }
    return kNotImpl;
}

// Slot 8 is CopyIntoSubresource: (destination resource base, 0, subresource, linear source for that subresource,
// measured word 5, measured word 6). In the linear layout the destination subresource sits at the same offset as the
// row-major source's own subresource, so the copy is source - recorded source base, at the destination base. The
// measured word 6 is the subresource's byte count; it is capped by the largest a resource of this description can be
// so a wrong word cannot run the copy past the resource, and both ranges must be committed and accessible.
HRESULT __stdcall computer_slot8(void* self, void* destination, uint32_t arg2, uint32_t subresource, const void* source,
                                 uint32_t arg5, uint32_t arg6) {
    note_call(g_slots[8], "texture computer slot8");
    const uint64_t n = g_slots[8].load(std::memory_order_relaxed);
    auto* computer = static_cast<Computer*>(self);
    const bool first_source = computer && !computer->have_source.load(std::memory_order_relaxed);
    const bool negative_source =
        !first_source && source && computer &&
        static_cast<const uint8_t*>(source) < static_cast<const uint8_t*>(computer->linear_source_base);
    census_note_copy(computer ? computer->desc : nullptr, arg2, subresource, arg5, arg6, !first_source,
                     negative_source);
    if (kTrace && n <= 8) {
        std::fprintf(stderr,
                     "[rt] xg_xs trace: texture computer slot8 call #%llu dst %p arg2 %u subresource %u src %p "
                     "arg5 0x%x (%u) arg6 0x%x (%u) (caller %s)\n",
                     static_cast<unsigned long long>(n), destination, arg2, subresource, source, arg5, arg5, arg6,
                     arg6, caller_text(_ReturnAddress()));
    }
    if (layout() != Layout::kLinear) return kNotImpl;
    if (!computer || !destination || !source) return kNotImpl;
    // The movie-plane upload rule (opt-in, docs/peer_port_movie.md section 12): a description the linear model lays
    // out is placed row by row at the layout's padded row pitch. A description it does not lay out keeps the tree's
    // flat path below, byte for byte, so the switch only changes what its model covers.
    if (rt::xg_xs::movie_plane_upload()) {
        LinearLayout linear{};
        const char* why = "description (null)";
        if (linear_layout(computer->desc, linear, &why)) {
            const UploadAnswer answer = upload_linear_rows(computer, linear, destination, arg2, subresource, source,
                                                           arg5, arg6, n, _ReturnAddress());
            return answer == UploadAnswer::kDone ? S_OK : kNotImpl;
        }
    }
    if (!computer->have_source.exchange(true, std::memory_order_relaxed)) computer->linear_source_base = source;

    const uint8_t* source_base = static_cast<const uint8_t*>(computer->linear_source_base);
    const uint8_t* source_at = static_cast<const uint8_t*>(source);
    if (source_at < source_base) {
        if (kTrace && !computer->warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[rt] xg_xs: slot 8 source offset is negative (subresource %u); refusing the copy, the "
                         "linear source layout is not the measured one\n",
                         subresource);
        }
        return kNotImpl;
    }
    const size_t offset = static_cast<size_t>(source_at - source_base);
    // Largest a subresource of the recorded description can be: every format the title asks about is at most 16
    // bytes per texel, and a linear resource holds width x height x depth of them.
    uint64_t width = 0, height = 0, depth = 0, mips = 0;
    std::memcpy(&width, computer->desc + 0x10, sizeof(width));
    std::memcpy(&height, computer->desc + 0x18, sizeof(uint32_t));
    std::memcpy(&depth, computer->desc + 0x1c, sizeof(uint16_t));
    std::memcpy(&mips, computer->desc + 0x1e, sizeof(uint16_t));
    if (mips == 0) mips = 1;
    const uint64_t mip = subresource % mips;
    const uint64_t slice_count = depth > 1 ? (depth >> mip) + ((depth >> mip) == 0 ? 1 : 0) : 1;
    const uint64_t largest = (width ? width : 1) * (height ? height : 1) * slice_count * 16;
    size_t bytes = arg6;
    if (largest && bytes > largest) bytes = static_cast<size_t>(largest);
    if (bytes == 0) return kNotImpl;
    if (!range_ok(static_cast<uint8_t*>(destination) + offset, bytes, true) ||
        !range_ok(source_at, bytes, false)) {
        if (kTrace && !computer->warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[rt] xg_xs: slot 8 range not committed/accessible (offset 0x%zx, %zu B, subresource %u); "
                         "refusing the copy\n",
                         offset, bytes, subresource);
        }
        return kNotImpl;
    }
    if (kTrace && n <= 3) dump_words("slot8 source", source_at, bytes < 32 ? bytes : 32);
    std::memcpy(static_cast<uint8_t*>(destination) + offset, source_at, bytes);
    g_copied_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_copies.fetch_add(1, std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: slot 8 copied %zu B to offset 0x%zx (subresource %u, mip %llu)\n",
                     bytes, offset, subresource, static_cast<unsigned long long>(mip));
        if (n <= 3) dump_words("slot8 destination", static_cast<uint8_t*>(destination) + offset, bytes < 32 ? bytes : 32);
    }
    return S_OK;
}

template <size_t Slot>
constexpr uintptr_t slot_address() {
    if constexpr (Slot == 0) return reinterpret_cast<uintptr_t>(&computer_add_ref);
    else if constexpr (Slot == 1) return reinterpret_cast<uintptr_t>(&computer_release);
    else if constexpr (Slot == 2) return reinterpret_cast<uintptr_t>(&computer_slot2);
    else if constexpr (Slot == 8) return reinterpret_cast<uintptr_t>(&computer_slot8);
    else if constexpr (Slot == 10) return reinterpret_cast<uintptr_t>(&computer_slot10);
    else return reinterpret_cast<uintptr_t>(&computer_slot<Slot>);
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{reinterpret_cast<void*>(slot_address<Slot>())...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});
Computer g_computers[kMaxComputers] = {};
std::atomic<uint32_t> g_computer_seq{0};

Computer* take_computer(const void* desc) {
    const uint32_t index = g_computer_seq.fetch_add(1, std::memory_order_relaxed) % kMaxComputers;
    Computer* computer = &g_computers[index];
    computer->vtable = kVtable.data();
    computer->linear_source_base = nullptr;
    computer->have_source.store(false, std::memory_order_relaxed);
    computer->warned.store(false, std::memory_order_relaxed);
    std::memset(computer->desc, 0, sizeof(computer->desc));
    if (desc) std::memcpy(computer->desc, desc, kDescBytes);
    return computer;
}

HRESULT __stdcall XGCreateTextureComputer(const void* desc, void** out) {
    note_call(g_create, "XGCreateTextureComputer");
    const uint64_t n = g_create.load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: XGCreateTextureComputer call #%llu desc %p out %p (caller %s)\n",
                     static_cast<unsigned long long>(n), desc, out, caller_text(_ReturnAddress()));
        if (n <= 2) dump_words("computer desc", desc, kDescBytes);
    }
    if (kDump && n <= 8) dump_words("create desc", desc, desc ? kDescBytes : 0);
    census_register(static_cast<const uint8_t*>(desc), true);
    if (!out) return E_POINTER;
    *out = take_computer(desc);
    if (layout() == Layout::kLinear) {
        LinearLayout linear{};
        const char* why = "description (null)";
        if (linear_layout(static_cast<const uint8_t*>(desc), linear, &why)) {
            if (kTrace) {
                std::fprintf(stderr,
                             "[rt] xg_xs: XGCreateTextureComputer: linear layout %ux%u, one level (S_OK)\n",
                             linear.width, linear.height);
            }
            return S_OK;
        }
        // The layout cache's callers check this HRESULT and take their failure path, which is what keeps an unwritten
        // slot-2 record out of that cache. The upload loop's callers do not check it and use the computer anyway.
        static std::atomic<uint32_t> refused{0};
        warn_refusal(refused.fetch_add(1, std::memory_order_relaxed) + 1, "XGCreateTextureComputer", why,
                     _ReturnAddress());
        return kNotImpl;
    }
    static std::atomic<bool> warned{false};
    warn_once(warned, "XGCreateTextureComputer", -1, _ReturnAddress());
    return kNotImpl;  // see the top of the file: two measured callers use *out without checking the HRESULT
}

HRESULT __stdcall XGSuggestSwizzleMode(const void* desc, uint32_t flags, uint32_t* mode) {
    note_call(g_suggest, "XGSuggestSwizzleMode");
    const uint64_t n = g_suggest.load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: XGSuggestSwizzleMode call #%llu desc %p flags 0x%x mode %p mode_in 0x%x "
                     "(caller %s)\n",
                     static_cast<unsigned long long>(n), desc, flags, mode, mode ? *mode : 0,
                     caller_text(_ReturnAddress()));
        if (n <= 2) {
            dump_words("suggest desc", desc, kDescBytes);
            dump_words("suggest mode field", reinterpret_cast<const uint8_t*>(desc) + 0x2c, 4);
        }
    }
    if (kDump && n <= 8) dump_words("suggest desc", desc, desc ? kDescBytes : 0);
    if (layout() == Layout::kLinear) {
        // 0x100: the explicit linear layout the title's own description helper writes for a known texture. The
        // computer answers a linear layout whatever mode the title names, because the backend's textures are
        // row-major; a console swizzle mode would make the title upload console-tiled bytes the GPU reads wrongly.
        if (!mode) return E_POINTER;
        *mode = kLinearLayoutField;
        if (kTrace) {
            std::fprintf(stderr, "[rt] xg_xs: XGSuggestSwizzleMode: suggested 0x100 (explicit linear layout, S_OK)\n");
        }
        return S_OK;
    }
    static std::atomic<bool> warned{false};
    warn_once(warned, "XGSuggestSwizzleMode", -1, _ReturnAddress());
    return kNotImpl;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
