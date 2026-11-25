#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::xmem {

bool commit_physical_range(const void* address, std::size_t size);
bool commit_physical_pages(const std::uint64_t* page_array, std::size_t count);

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
