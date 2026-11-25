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
