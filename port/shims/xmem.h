#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::xmem {

bool commit_physical_range(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

// The allocation probe (docs/vertex_buffer_va.md section 15): whether `address` lies in a range an XMemVirtualAlloc
// answer recorded while PALMETTO_ALLOC_WATCH is set, with the raw title caller address and the entry point that served
// it (1 = XMemVirtualAlloc). False when the switch is off or no recorded answer contains the address. The registry is
// append-only and bounded, so a range freed later still answers yes: the probe asks where the bytes came from.
bool served_range(const void* address, std::uint64_t* caller, std::uint8_t* kind);

// ---- The opt-in arena-layout probe and arms (docs/vertex_buffer_va.md section 19) --------------------------------
//
// The title's classed heap lives in memory this shim's XMemVirtualAlloc answered. The measured boot put the title's own
// allocator object, the stage-3 command object (the job) and the constant table's zero-byte block in one MEM_PRIVATE
// committed region at a host-chosen base, 0x81174e0 bytes into it (region 0x212c9000 bytes; section 19.3). The console's
// allocator layout is not observable, so where that region sits - its base, its alignment and the address space in
// front of it - is a choice this runtime makes. The arms below make one such choice explicit and record what the
// title's own allocator then does with it. Off by default: with neither switch set the default path is unchanged.
//
// PALMETTO_XMEM_LAYOUT=<mode>[:<bytes>]:
//   log           record every XMemVirtualAlloc answer of at least 1 MiB (request, answer, caller, VirtualQuery)
//   prefix:<hex>  serve the target call `bytes` further into a larger reservation (the prefix stays reserved)
//   align:<hex>   require the target answer to be aligned to `bytes` (a power of two, at least 4 KiB)
//   guard:<hex>   allocate `bytes` before serving the target answer (an address-space guard)
// PALMETTO_XMEM_LAYOUT_CALLER=<hex rva> is the measured title caller of the answer a prefix, align or guard arm targets;
// a targeted mode without a usable caller records nothing, and `log` needs no caller.
enum class XmemLayoutMode : std::uint8_t {
    none = 0,
    log = 1,
    prefix = 2,
    align = 3,
    guard = 4,
};

struct XmemLayoutPlan {
    XmemLayoutMode mode = XmemLayoutMode::none;
    std::uint64_t argument = 0;    // bytes: the prefix/guard length, or the required alignment
    std::uint64_t caller_rva = 0;  // the title module RVA the arm targets; 0 when unset
    bool enabled() const { return mode != XmemLayoutMode::none; }
    bool targeted() const {
        return mode == XmemLayoutMode::prefix || mode == XmemLayoutMode::align || mode == XmemLayoutMode::guard;
    }
    bool targets(std::uint64_t rva) const { return targeted() && caller_rva != 0 && rva == caller_rva; }
};

// Parse the two switch values (either may be null). Anything unrecognised is XmemLayoutMode::none, which is the
// default path; a targeted mode without a usable caller is also none. Pure, so the GPU-free xmem test checks it.
XmemLayoutPlan layout_plan_from(const char* layout, const char* caller);

// The process's plan, parsed once. One relaxed load per XMemVirtualAlloc call when no switch is set.
const XmemLayoutPlan& layout_plan();

// ---- The GPU-view reservation pair (docs/peer_port_movie.md, the implemented rank-5 port) ------------------------
//
// MEASURED in this title (docs/peer_port_movie.md section 11.1): XMemVirtualAlloc reserves the same span of 64 KiB
// pages twice, back to back - a CPU-readable PAGE_READWRITE window first (caller game+0x1e1d00f, 0x600000000 bytes,
// attribute word 0x21) and a PAGE_GRAPHICS_NOACCESS | PAGE_NOACCESS window of the same size immediately after it
// (caller game+0x1e1d03e, attribute word 0x3), contiguous with it in the measured boot. On the console the second is a
// GPU-only view of the first's physical pages; this shim maps the title's pages into the first only, so a placed
// resource whose console VA falls in the second has its bytes at the same offset in the first. The peers that name the
// same arrangement are XCL's `kernelx::mirrors` (the reservation pair, its GPU-view test and `source`) and Five Star's
// `XMemReservation.CpuView` ("the CPU reservation of the same size made just before it") - read as evidence for the
// mechanism, never as a code source.
//
// The pure half is here and unit-tested by port/test/xmem_test.cpp: the switch text, the measured pair rule, the
// bounded record of the reservations and pairs, and the translation of a GPU-view VA to its CPU-view address. The
// impure half is in port/shims/xmem.cpp (record at each reserve-only 64 KiB window; answer the resolve hook) and the
// consumer is the title-placed texture import in port/shims/d3d12_vulkan_device.cpp (its pages are read through the
// answer, and the per-submission refresh compares the same view). The default is OFF: with PALMETTO_GPU_VIEW_PAIR
// unset nothing is recorded, the hook answers "unavailable", and the import reads the console placement VA exactly as
// before this port.
enum class GpuViewPairMode : std::uint8_t {
    off = 0,
    on = 1,
};

struct GpuViewPairPlan {
    GpuViewPairMode mode = GpuViewPairMode::off;
    bool trace = false;  // PALMETTO_GPU_VIEW_PAIR_TRACE: the record/resolve lines, the path unchanged
    bool enabled() const { return mode == GpuViewPairMode::on; }
};

// The pure parse. `pair` accepts `on`/`1`/`yes`/`true` (case-insensitive) as the switch's on value; unset, empty, `0`,
// `off` and any unknown word are off, never a guess. `trace` is any value other than unset/empty/`0`.
GpuViewPairPlan gpu_view_pair_plan_from(const char* pair, const char* trace);

// One reservation the title made: a reserve-only 64 KiB-page window, with its CPU protection after the console
// graphics bits are dropped (port/shims/xmem.cpp's host_protect).
struct GpuViewReservation {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t protect = 0;
};

inline constexpr std::uint32_t gpu_view_protect_noaccess = 0x01;   // PAGE_NOACCESS
inline constexpr std::uint32_t gpu_view_protect_readonly = 0x02;   // PAGE_READONLY
inline constexpr std::uint32_t gpu_view_protect_readwrite = 0x04;  // PAGE_READWRITE
inline constexpr std::uint64_t gpu_view_page_bytes = 0x10000;      // the console's 64 KiB page

// The two shapes the measured pair is made of. A reservation with any other protection is not recorded: a committed
// allocation is CPU-addressable in this shim and needs no pairing, and an execute/guard protection is not the pair.
inline bool gpu_view_shaped(const GpuViewReservation& reservation) {
    return reservation.base && reservation.size &&
           reservation.base % gpu_view_page_bytes == 0 && reservation.size % gpu_view_page_bytes == 0;
}

inline bool gpu_view_is_gpu_only(const GpuViewReservation& reservation) {
    return gpu_view_shaped(reservation) && (reservation.protect & 0xffu) == gpu_view_protect_noaccess;
}

inline bool gpu_view_is_cpu_readable(const GpuViewReservation& reservation) {
    const std::uint32_t cpu = reservation.protect & 0xffu;
    return gpu_view_shaped(reservation) &&
           (cpu == gpu_view_protect_readonly || cpu == gpu_view_protect_readwrite);
}

// The measured pair rule: a CPU-readable reservation made just before a GPU-only reservation of the same size, with
// the two ranges contiguous (`cpu.base + cpu.size == gpu.base` - the measured boot's arrangement, and what says the
// two were reserved as one span on the console). False for anything else; the caller records the GPU-only reservation
// as unpaired so the resolve names the refusal instead of reading a window that holds no pages.
inline bool gpu_view_pair_rule(const GpuViewReservation& cpu, const GpuViewReservation& gpu) {
    return gpu_view_is_cpu_readable(cpu) && gpu_view_is_gpu_only(gpu) && cpu.size == gpu.size &&
           cpu.base <= UINT64_MAX - cpu.size && cpu.base + cpu.size == gpu.base;
}

// Translate a console VA inside the GPU-view reservation to the same offset in the paired CPU reservation. `bytes`
// must fit inside the reservation: a texture that reaches past its end cannot be read through the pair (the caller
// refuses it by name).
inline bool gpu_view_translate(std::uint64_t gpu_va, std::uint64_t bytes, const GpuViewReservation& gpu,
                               const GpuViewReservation& cpu, std::uint64_t& cpu_va) {
    if (!bytes || gpu_va < gpu.base) return false;
    const std::uint64_t offset = gpu_va - gpu.base;
    if (offset >= gpu.size || bytes > gpu.size - offset) return false;
    cpu_va = cpu.base + offset;
    return true;
}

// What the resolve hook answers for one request.
enum class GpuViewResolve : std::uint8_t {
    // No pairing is on, or the VA is not in a GPU-view reservation this table knows: the caller keeps its pre-port
    // source (the console placement VA), so the default path is byte-identical.
    unavailable = 0,
    // The VA is inside a paired GPU-view reservation and `cpu_va` holds the same offset in its CPU view.
    paired = 1,
    // The VA is inside a GPU-view reservation with no usable pair. Reading it would read a window with no pages (or
    // with pages mapped nowhere), so the caller refuses by name rather than importing what is there.
    gpu_only = 2,
};

// The bounded record. Sized to the measured population (one pair in every boot read) with room for a re-created
// span: eight CPU-view candidates, eight pairs and eight unpaired GPU-view reservations, newest candidates kept. The
// record is append-only for the process's lifetime because the shim never sees the title's VirtualFree of these
// windows; that is why it is bounded (docs/peer_port_movie.md section 11.5 names it a limit).
class GpuViewPairTable {
public:
    static constexpr std::size_t max_views = 8;
    static constexpr std::size_t max_pairs = 8;
    static constexpr std::size_t max_gpu_only = 8;

    enum class Recorded : std::uint8_t {
        ignored = 0,   // not the shape of the pair (a committed allocation, an execute protection, ...)
        cpu_view = 1,  // a CPU-readable reservation: a candidate for the GPU view made just after it
        paired = 2,    // a GPU-only reservation paired with a candidate: the mechanism is now armed for its VAs
        gpu_only = 3,  // a GPU-only reservation with no pair: its VAs resolve to `gpu_only` (a named refusal)
        refused = 4,   // a pair or an entry a bound dropped: the caller logs it once
    };

    struct Pair {
        GpuViewReservation cpu;
        GpuViewReservation gpu;
    };

    // Record one reservation, in creation order. A duplicate CPU-readable reservation is `ignored` (a window is
    // recorded once per XMemVirtualAlloc answer).
    Recorded record(const GpuViewReservation& reservation) {
        if (gpu_view_is_cpu_readable(reservation)) {
            for (std::size_t i = 0; i < view_count_; ++i) {
                if (views_[i].base == reservation.base && views_[i].size == reservation.size) return Recorded::ignored;
            }
            if (view_count_ == max_views) {  // keep the newest: the pair is made together, so the newest candidates matter
                for (std::size_t i = 1; i < max_views; ++i) {
                    views_[i - 1] = views_[i];
                    claimed_[i - 1] = claimed_[i];
                }
                --view_count_;
                ++refused_;
            }
            views_[view_count_] = reservation;
            claimed_[view_count_] = false;
            ++view_count_;
            return Recorded::cpu_view;
        }
        if (!gpu_view_is_gpu_only(reservation)) return Recorded::ignored;
        // Newest first: the measured pair is the two reservations made together.
        for (std::size_t i = view_count_; i-- > 0;) {
            if (claimed_[i] || !gpu_view_pair_rule(views_[i], reservation)) continue;
            if (pair_count_ == max_pairs) {
                ++refused_;
                return Recorded::refused;
            }
            claimed_[i] = true;
            pairs_[pair_count_].cpu = views_[i];
            pairs_[pair_count_].gpu = reservation;
            ++pair_count_;
            return Recorded::paired;
        }
        if (gpu_only_count_ == max_gpu_only) {
            ++refused_;
            return Recorded::refused;
        }
        gpu_only_[gpu_only_count_++] = reservation;
        return Recorded::gpu_only;
    }

    // The CPU-view address behind `gpu_va` when it is inside a pair, or the answer that the VA is inside an unpaired
    // GPU-view reservation. A request inside a pair's GPU window that does not fit the window is `gpu_only` too: the
    // caller must refuse it, not fall back to reading the window itself. The resolve is a scan of at most eight pairs
    // and eight unpaired ranges.
    GpuViewResolve resolve(std::uint64_t gpu_va, std::uint64_t bytes, std::uint64_t& cpu_va) const {
        if (!gpu_va) return GpuViewResolve::unavailable;
        for (std::size_t i = 0; i < pair_count_; ++i) {
            const GpuViewReservation& gpu = pairs_[i].gpu;
            if (gpu_va < gpu.base || gpu_va - gpu.base >= gpu.size) continue;
            // Inside this pair's GPU window: either the whole request translates, or it cannot be read through the pair.
            if (gpu_view_translate(gpu_va, bytes, gpu, pairs_[i].cpu, cpu_va)) return GpuViewResolve::paired;
            return GpuViewResolve::gpu_only;
        }
        for (std::size_t i = 0; i < gpu_only_count_; ++i) {
            if (gpu_va >= gpu_only_[i].base && gpu_va - gpu_only_[i].base < gpu_only_[i].size)
                return GpuViewResolve::gpu_only;
        }
        return GpuViewResolve::unavailable;
    }

    // Whether [va, va + bytes) is inside the CPU half of a pair: the boot's measure of the pages the title maps into the
    // view the GPU samples (port/shims/xmem.cpp counts them; docs/peer_port_movie.md section 11.4).
    bool is_pair_cpu_span(std::uint64_t va, std::uint64_t bytes) const {
        for (std::size_t i = 0; i < pair_count_; ++i) {
            const GpuViewReservation& cpu = pairs_[i].cpu;
            if (va < cpu.base || !bytes) continue;
            const std::uint64_t offset = va - cpu.base;
            if (offset < cpu.size && bytes <= cpu.size - offset) return true;
        }
        return false;
    }

    std::size_t pairs() const { return pair_count_; }
    std::size_t gpu_only_ranges() const { return gpu_only_count_; }
    std::size_t refused() const { return refused_; }
    const Pair& pair(std::size_t index) const { return pairs_[index]; }

    void clear() {
        for (std::size_t i = 0; i < max_views; ++i) {
            views_[i] = GpuViewReservation{};
            claimed_[i] = false;
        }
        for (std::size_t i = 0; i < max_pairs; ++i) pairs_[i] = Pair{};
        for (std::size_t i = 0; i < max_gpu_only; ++i) gpu_only_[i] = GpuViewReservation{};
        view_count_ = pair_count_ = gpu_only_count_ = refused_ = 0;
    }

private:
    GpuViewReservation views_[max_views]{};
    bool claimed_[max_views]{};
    std::size_t view_count_ = 0;
    Pair pairs_[max_pairs]{};
    std::size_t pair_count_ = 0;
    GpuViewReservation gpu_only_[max_gpu_only]{};
    std::size_t gpu_only_count_ = 0;
    std::size_t refused_ = 0;
};

// The runtime's hooks, filled in by port/shims/xmem.cpp; a GPU-free binary that links no xmem keeps them null and the
// layer's placed-texture import reads the console placement VA exactly as it did before this port. `cpu_va` is
// written only for `paired`.
using GpuViewResolveFn = GpuViewResolve (*)(std::uint64_t gpu_va, std::uint64_t bytes, std::uint64_t* cpu_va);
using GpuViewCountFn = std::uint64_t (*)();
inline GpuViewResolveFn gpu_view_resolve = nullptr;
inline GpuViewCountFn gpu_view_pairs = nullptr;             // pairs recorded
inline GpuViewCountFn gpu_view_resolved = nullptr;          // resolved requests (a placed texture read through the pair)
inline GpuViewCountFn gpu_view_refused = nullptr;           // requests inside an unpaired GPU-view reservation
inline GpuViewCountFn gpu_view_cpu_map_pages = nullptr;     // 64 KiB pages the title mapped into a paired CPU view

// Test seam (port/test/xmem_test.cpp): overrides the environment parse for the rest of the process and returns the plan
// now in effect. The production path never calls it; xmem.cpp implements it.
GpuViewPairPlan set_gpu_view_pair_plan(GpuViewPairPlan plan);

// Console queue slot 31 (CopyPageMappingsBatchX, port/shims/d3d12_xs_device.cpp): the command-object proxy is also
// compiled into GPU-free test binaries that link no xmem, so the mapping primitive is a hook this file's
// implementation fills in and a test can replace. Each call gives `pages` 64 KiB destination pages a second view of
// the physical pages already mapped at `source` (or the one page at `source` repeated for every destination page),
// so both addresses are the one memory; a source page with nothing mapped leaves its destination page unmapped, and
// a null `source` unmaps them all. False, with the last error set, for an unaligned address or a destination outside
// a physical-page window. A null hook keeps the proxy's honest "past the public chain" refusal.
using CopyPageMappingsFn = bool (*)(void* destination, const void* source, std::uint64_t pages, bool repeat_first);
inline CopyPageMappingsFn copy_page_mappings_fn = nullptr;

// The DirectStorage page-list resolver (port/shims/dstorage.cpp, docs/page_resolver.md): the writable CPU address of the
// 64 KiB backing page `page_number`, or null for a number that is not a live physical page (0, never allocated, freed).
// The backing is committed before it is returned, so a copy through the view needs no first touch; a title mapping of
// the same page sees the bytes because a physical page here has exactly one backing (xmem maps that backing at the
// console VA, there is no second/paired view - docs/cpu_view_mapping.md section 2). The address is valid while the page
// number is live, which the console contract requires of a request's page list until its status completes. A null hook
// makes the dstorage path keep its named refusal rather than touch a page table it cannot see; a GPU-free test replaces
// the hook, and xmem.cpp fills it in for the runtime (the same shape as the slot-31 hook above).
using ResolvePhysicalPageFn = std::uint8_t* (*)(std::uint64_t page_number);
inline ResolvePhysicalPageFn resolve_physical_page_fn = nullptr;

// ---- The physical-page destination write (port/shims/dstorage.cpp, docs/peer_port_streaming.md section 13) ----------
//
// A console page-list request's destination is a list of 64 KiB physical pages: the console's DMA writes the frames, so
// every title mapping of a frame sees the bytes. Here a frame is a pagefile-section page with exactly one backing view
// (docs/page_resolver.md section 3), so writing the backing is the delivery and every mapping already sees it. What the
// backing write cannot do is trip a host guard the page-watch tracker armed on a title mapping of that page: a
// PAGE_GUARD is per view (docs/card_copies.md), and [title_memory_model.md] section 3.4 item 4 measured that the layer's
// dstorage writes go through the backing view, which no title-VA guard sees. The peers answer the same host problem by
// recording the write at the address the frame is mapped at (Five Star Recomp's XMemPhysicalMemory.WriteFrames invokes
// its write observer for each written frame's last mapped address, and its device marks the textures there stale); this
// hook is that answer in this tree's structures. The write goes into the pages, and every armed page-watch range that
// maps one of them is written through and records its written 4 KiB pages, exactly as a guarded store would leave them.
// So the tracker's next poll answers `changed` and the plane refresh re-uploads a streamed texture instead of keeping
// stale bytes (docs/peer_port_movie.md section 8; the gap page_resolver.md section 6 named as its limit).
//
// All-or-nothing: every span's page is located and committed before the first byte moves, so a page with no live backing
// refuses the whole write with `failed_page` set and no page changed. Bounded: the request path's page plan holds at most
// 32 spans and this hook accepts at most 64; a span lies inside one 64 KiB page, so a span is not a page list and the
// console's page-number rule (0 = unmapped) is already resolved by the caller's plan. A null hook keeps the dstorage
// shim's pre-port path (the resolver scatter of docs/page_resolver.md). A GPU-free test replaces the hook and xmem.cpp
// fills it in for the runtime (the shape of the two hooks above).
struct PhysicalPageWrite {
    std::uint64_t page = 0;    // the page number, a live 64 KiB physical page
    std::uint32_t offset = 0;  // bytes into that page (below 64 KiB)
    std::uint32_t bytes = 0;   // bytes written at that offset (the range stays inside the page)
};

// What one write did. `watch_views` counts the armed title mappings the bytes were repeated through and `watch_pages` the
// 4 KiB pages the tracker recorded; both stay 0 when no page-watch range maps a written page.
struct PhysicalPageWriteStats {
    std::uint32_t spans = 0;
    std::uint64_t bytes = 0;
    std::uint32_t watch_views = 0;
    std::uint32_t watch_pages = 0;
};

// Writes the spans in `data` order into their pages. False with `*failed_page` naming the first page that is not live
// (nothing written), or with `*failed_page` left 0 for a malformed input (null spans or data, more than 64 spans, a span
// outside its page).
using WritePhysicalPagesFn = bool (*)(const PhysicalPageWrite* spans, std::size_t count, const std::uint8_t* data,
                                      PhysicalPageWriteStats* stats, std::uint64_t* failed_page);
inline WritePhysicalPagesFn write_physical_pages = nullptr;

// The guard-page write tracker (docs/card_copies.md). The layer keeps a GPU-visible mirror of the title's CPU
// descriptor-heap bytes and can only tell whether the mirror still matches them by comparing all of them - a
// measured 3.4-4.4 ms pass over the 39.8 MB heap per submission on the page that first measured it, and 8.4-9.5 ms per
// refresh in this slice's A/B boot (docs/card_copies.md section 2). The OS's own
// write watch cannot answer for that memory: the measured heap is pagefile-section views and GetWriteWatch returns
// ERROR_INVALID_PARAMETER for them (measured, docs/card_copies.md section 2). So the tracker uses the peers' documented
// mechanism instead - a PAGE_GUARD on every 64 KiB page of a watched range. The first access to a guarded page raises
// EXCEPTION_GUARD_PAGE and the system clears that page's guard; the handler records: a write means the mirror is stale,
// a read (the layer's own scan) means only that the page must be guarded again. The change signal is a page, not a
// byte, and it is a lower bound on what changed - a range a poll reports unchanged has had no write since the previous
// poll, which is exactly the condition a mirror refresh needs.
//
// The hooks are function pointers filled in by xmem.cpp (the same shape as the page-mapping hooks above), so a GPU-free
// binary that includes this header but links no xmem gets null hooks and simply keeps today's path.
enum class WriteWatchPoll : std::uint8_t {
    unavailable = 0,  // no range is armed, a page could not be armed, or the trip record overflowed: full work needed
    unchanged = 1,    // no watched page was written since the previous poll
    changed = 2,      // at least one watched page was written since the previous poll
};

// Guards every 64 KiB page of [address, address + bytes). True when every page is now guarded, false with nothing left
// armed when any page cannot be (not committed, not writable, or a guard call fails): the caller must then keep doing
// the full work for that range. `address` and `bytes` are rounded down/up to 64 KiB.
using PageWatchArmFn = bool (*)(const void* address, std::size_t bytes);
// Opt-in alias-expanding arm. In addition to the requested range, this guards every live xmem view of each physical page
// mapped in it; map/unmap changes refresh that alias set. Capacity or protection failures refuse the arm, and a failed
// live refresh makes the shared poll unavailable until that owner is forgotten, so callers must use their full compare.
using PageWatchArmAliasesFn = bool (*)(const void* address, std::size_t bytes);
// PALMETTO_XMEM_ALIAS_WATCH uses the same unset/empty/0-off, any other non-empty-on convention as the runtime switches.
inline bool page_watch_alias_switch_on(const char* value) { return value && *value && *value != '0'; }
// Stops watching the range whose start is `address`; restores the protection of every page it still holds.
using PageWatchForgetFn = void (*)(const void* address);
// The verdict for all armed ranges, and the re-arm of the pages a trip took out of the guard state. Called once per
// submission before the work the verdict gates.
using PageWatchPollFn = WriteWatchPoll (*)();
// Re-arms the pages a trip took out of the guard state without touching the change flag: called after the full work
// that the verdict let run, because that work's own reads of the title bytes trip every guard.
using PageWatchRearmFn = void (*)();
// Suspends and resumes the guard state around the full work: a full scan of an armed heap costs one exception per
// 4 KiB page (measured ~35 ms for 39.8 MB, docs/card_copies.md section 2), so the caller that must scan takes the
// guards off first. A write that lands inside the suspended window is not recorded - the same race the refresh has
// with a title writing descriptors for work already in flight (docs/page_address_space.md section 5), and the reason
// an enabled gate also forces a periodic full refresh.
using PageWatchSuspendFn = void (*)();
using PageWatchResumeFn = void (*)();
// Bounded diagnostic counters for the boot's report: guard exceptions recorded as writes and as reads-or-executes
// since the process started, and the 4 KiB pages currently guarded.
using PageWatchCountFn = std::uint64_t (*)();
inline PageWatchArmFn page_watch_arm = nullptr;
inline PageWatchArmAliasesFn page_watch_arm_aliases = nullptr;
inline PageWatchForgetFn page_watch_forget = nullptr;
inline PageWatchPollFn page_watch_poll = nullptr;
inline PageWatchRearmFn page_watch_rearm = nullptr;
inline PageWatchSuspendFn page_watch_suspend = nullptr;
inline PageWatchResumeFn page_watch_resume = nullptr;
inline PageWatchCountFn page_watch_write_trips = nullptr;
inline PageWatchCountFn page_watch_read_trips = nullptr;
inline PageWatchCountFn page_watch_pages = nullptr;

// The measured console page batch: a batch is 24 bytes {UINT64 destination; UINT32 range count; const PageRange*
// ranges} and a range is 12 bytes {UINT32 kind, start, count} (our copy of the retail executable's builder stores them
// at those strides, game+0xf2c709 / game+0xf2c6c4). Kind 0 maps pool pages start, start + 1, ... at the destination
// pages one after another; 1 maps pool page `start` at every destination page; 2 unmaps the destination pages; 3
// leaves them as they are. The proxy decodes this, the hook does the page mapping, and the tests share the layout here.
struct PageRange {
    std::uint32_t kind, start, count;
};
struct PageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const PageRange* ranges;
};
static_assert(sizeof(PageRange) == 12 && sizeof(PageBatch) == 24, "the measured console page batch layout");

}  // namespace rt::xmem
