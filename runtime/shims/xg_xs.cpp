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
//     stack structure (2 sites), slot 8 with six arguments in a loop over subresources (1 site) and slot 10 with
//     seven arguments (1 site). No site reads the return value of slots 2, 8 or 10.
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
// The runtime has no model of the console's resource layout (runtime/src/gfx10_tiling.h covers two swizzle modes of
// one 2D level, without mips or the layout structure slot 2 fills), so nothing here computes a console layout:
// - Default (PALMETTO_XG_LAYOUT unset or "refuse"): both exports refuse with E_NOTIMPL.
//   XGSuggestSwizzleMode writes nothing, so the field keeps the 0 the caller put there; XGCreateTextureComputer still
//   stores a computer in *out (the two callers that do not check would otherwise call through a null pointer, so this
//   deliberately breaks COM's rule that *out is null on failure); every computer slot prints its index and caller
//   once and returns E_NOTIMPL without writing its arguments.
// - PALMETTO_XG_LAYOUT=linear: the honest linear answer for a backend whose textures are ordinary row-major committed
//   images. XGSuggestSwizzleMode returns S_OK and writes 0x100 - the value the title's own helper writes for a known
//   texture whose mode byte is 0; XGCreateTextureComputer returns S_OK and a computer that copies slot 8's linear
//   subresource into the linear destination at the offset the title's own source layout names (the destination
//   subresource of a row-major resource sits where the row-major source puts it). Slots 2 and 10 (the layout and tile
//   structures) still refuse by name: the structures they fill are not modelled, and fabricating tiling is forbidden.
//   The layout-cache caller does not check slot 2's HRESULT, so the S_OK create makes it consume an uninitialised
//   record; that is why this mode is an opt-in measurement switch and the default keeps the cache path closed.
// - PALMETTO_XG_TRACE=1 logs the first calls with their desc words and arguments, and a count line every 1024 calls,
//   in either mode.  The counters are also printed on the first 8 calls of each entry, so a boot killed at its
//   timeout still shows how often the title reached the API.
//
// Hypothesis, not measured: slots 0 and 1 being AddRef and Release; the meaning of slots 2, 8 and 10; that callers
// reach no slot above kSlots - 1 (the computer is also stored in an object at one site and may be called later).
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "shims.h"

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr size_t kSlots = 32;
constexpr size_t kDescBytes = 0x38;     // XG resource description the measured call sites build
constexpr size_t kMaxComputers = 64;    // static pool; the measured boot creates one per placed texture

// Behaviour: "refuse" (default; every entry and slot is the measured E_NOTIMPL) or "linear" (the honest linear
// layout a row-major backend can serve).
enum class Layout { kRefuse, kLinear };

Layout read_layout() {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_XG_LAYOUT", value, sizeof(value));
    return (n > 0 && _stricmp(value, "linear") == 0) ? Layout::kLinear : Layout::kRefuse;
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

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_suggest{0};
std::atomic<uint64_t> g_create{0};
std::atomic<uint64_t> g_slots[kSlots] = {};
std::atomic<uint64_t> g_copied_bytes{0};
std::atomic<uint64_t> g_copies{0};

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
                 "slots1 %llu, copied %llu bytes in %llu call(s)\n",
                 why, static_cast<unsigned long long>(g_suggest.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_create.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[8].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[10].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slots[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_copied_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_copies.load(std::memory_order_relaxed)));
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

// Slot 2 is the measured "one out pointer to a large stack structure" method (the layout record). The structure it
// fills is not modelled, so it refuses by name and writes nothing, as the refusal boot did.
HRESULT __stdcall computer_slot2(void* /*self*/, void* out) {
    note_call(g_slots[2], "texture computer slot2");
    const uint64_t n = g_slots[2].load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr, "[rt] xg_xs trace: texture computer slot2 call #%llu out %p (caller %s)\n",
                     static_cast<unsigned long long>(n), out, caller_text(_ReturnAddress()));
    }
    return kNotImpl;
}

// Slot 10 is the measured seven-argument method (the tile-list structure). Refused by name for the same reason.
HRESULT __stdcall computer_slot10(void* /*self*/, uint32_t* count, void* a, void* b, uint32_t* inout, uint32_t flags,
                                  void* list) {
    note_call(g_slots[10], "texture computer slot10");
    const uint64_t n = g_slots[10].load(std::memory_order_relaxed);
    if (kTrace && n <= 8) {
        std::fprintf(stderr,
                     "[rt] xg_xs trace: texture computer slot10 call #%llu count_in %u a %p b %p inout_in 0x%x flags "
                     "0x%x list %p (caller %s)\n",
                     static_cast<unsigned long long>(n), count ? *count : 0, a, b, inout ? *inout : 0, flags, list,
                     caller_text(_ReturnAddress()));
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
    if (kTrace && n <= 8) {
        std::fprintf(stderr,
                     "[rt] xg_xs trace: texture computer slot8 call #%llu dst %p arg2 %u subresource %u src %p "
                     "arg5 0x%x (%u) arg6 0x%x (%u) (caller %s)\n",
                     static_cast<unsigned long long>(n), destination, arg2, subresource, source, arg5, arg5, arg6,
                     arg6, caller_text(_ReturnAddress()));
    }
    if (layout() != Layout::kLinear) return kNotImpl;
    if (!computer || !destination || !source) return kNotImpl;
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
    if (!out) return E_POINTER;
    *out = take_computer(desc);
    if (layout() == Layout::kLinear) {
        if (kTrace) {
            std::fprintf(stderr, "[rt] xg_xs: XGCreateTextureComputer: linear layout computer handed out (S_OK)\n");
        }
        return S_OK;
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
    if (layout() == Layout::kLinear) {
        // 0x100: the value the title's own description helper writes for a known texture whose mode byte is 0. The
        // computer answers a linear layout whatever mode the title names, because the backend's textures are
        // row-major; a console swizzle mode would make the title upload console-tiled bytes the GPU reads wrongly.
        if (!mode) return E_POINTER;
        *mode = 0x100;
        if (kTrace) {
            std::fprintf(stderr, "[rt] xg_xs: XGSuggestSwizzleMode: suggested 0x100 (linear layout, S_OK)\n");
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
