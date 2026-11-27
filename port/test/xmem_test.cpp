// Checks the registered xmem.dll shim against the measured Xbox ABI.
//
// Every function is resolved through rt::shims::find, so the test exercises the same shim table and calling
// convention the runtime installs for xmem.dll imports instead of a private copy of the implementation. The checks
// cover what the measured call sites use: 64-byte-aligned allocations, XMemFree, working-set statistics with the
// size at +8, and XMemVirtualQuery's MEMORY_BASIC_INFORMATION prefix inside a zeroed 64-byte output.
// XMemTransferMemory follows the call shape in Microsoft's public samples: TRUE for any direction and size, with
// nothing committed, and one trace event per distinct (type, size). XMemVirtualAlloc accepts attribute words and console
// graphics protection bits and commits 2 MiB guest pages; the placeholder flags pass through; physical pages keep their
// contents across unmap and remap, alias when mapped twice, and scatter-map per address. The DirectStorage page-list
// resolver turns a page number into the CPU view of its backing and is driven through the page-plan transfer seam
// (dstorage_advanced.h) against real pages and a real title window (docs/page_resolver.md).
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "xmem.h"
#include "plane_refresh.h"  // docs/peer_port_movie.md: the pure half of the per-submission texture refresh
#include "dstorage_advanced.h"  // the page-list transfer seam the dstorage request path runs (docs/page_resolver.md)

namespace {

using XMemAllocFn = void*(__stdcall*)(size_t, uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, uint64_t);
using XMemVirtualQueryFn = SIZE_T(__stdcall*)(HANDLE, const void*, void*);
// BOOL XMemGetWorkingSetStatistics(UINT32 kind, stats*): the FSR DLL passes kind 0x20000001 at every call site.
using XMemGetWorkingSetStatisticsFn = BOOL(__stdcall*)(uint32_t, void*);
using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemGetWriteWatchFn = UINT(WINAPI*)(DWORD, void*, SIZE_T, void**, ULONG_PTR*, ULONG*);
using XMemResetWriteWatchFn = UINT(WINAPI*)(void*, SIZE_T);
// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE, SIZE_T): the shape Microsoft's public NetRumble and NativeAOT-Xbox
// samples call (see port/shims/xmem.cpp).
using XMemTransferMemoryFn = BOOL(WINAPI*)(uint32_t, SIZE_T);
// The shapes measured at a title's call sites (see port/shims/xmem.cpp).
using XMemIsToolsMemAvailableFn = BOOL(WINAPI*)();
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, const uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
using XMemMapPhysicalPagesScatterFn = BOOL(WINAPI*)(void* const*, uint32_t, const uint64_t*);
constexpr uint32_t kMeasuredKind = 0x20000001;
constexpr DWORD kXmemLargePages = 0x20000000;
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr SIZE_T kPage64k = 0x10000;
constexpr SIZE_T kPage2m = 0x200000;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool aligned_64(const void* p) { return p && (reinterpret_cast<uintptr_t>(p) & 63u) == 0; }

void test_alloc(XMemAllocFn alloc, XMemFreeFn free_mem) {
    void* const zero = alloc(0, 0);
    check(zero != nullptr, "XMemAlloc(0, 0) returns non-null");
    check(aligned_64(zero), "XMemAlloc(0, 0) returns a 64-byte aligned block");

    bool zero_readable = false;
    if (zero) {
        auto* bytes = static_cast<uint8_t*>(zero);
        bytes[0] = 0xa5;
        zero_readable = bytes[0] == 0xa5;
    }
    check(zero_readable, "XMemAlloc(0, 0) block is writable and readable");

    void* const big = alloc(4096, 0x5e50000);
    check(big != nullptr, "XMemAlloc(4096, 0x5e50000) returns non-null");
    check(aligned_64(big), "XMemAlloc(4096, 0x5e50000) returns a 64-byte aligned block");

    bool pattern_ok = big != nullptr;
    if (big) {
        auto* bytes = static_cast<uint8_t*>(big);
        for (size_t i = 0; i < 4096; ++i) bytes[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
        for (size_t i = 0; i < 4096; ++i) {
            if (bytes[i] != static_cast<uint8_t>((i * 37u + 11u) & 0xffu)) {
                pattern_ok = false;
                break;
            }
        }
    }
    check(pattern_ok, "XMemAlloc(4096, 0x5e50000) block holds a write/read pattern");

    free_mem(zero, 0);
    free_mem(big, 0x5e50000);
    check(true, "XMemFree accepts both blocks (no crash)");
}

void test_working_set(XMemGetWorkingSetStatisticsFn get_stats) {
    alignas(64) uint8_t stats[64];
    std::memset(stats, 0xa5, sizeof(stats));
    // The pointer is the second argument: the first (0x20000001 here, as the FSR DLL passes it) must never be written
    // through, which is what the one-argument form did.
    const BOOL ok = get_stats(kMeasuredKind, stats);
    check(ok != FALSE, "XMemGetWorkingSetStatistics(kind, 64-byte out) returns TRUE");

    uint64_t total_memory = 0;
    std::memcpy(&total_memory, stats + 0, sizeof(total_memory));
    check(ok != FALSE && total_memory != 0, "XMemGetWorkingSetStatistics writes a non-zero total memory at +0");

    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        check(ok != FALSE && total_memory == ms.ullTotalPhys,
              "XMemGetWorkingSetStatistics total memory (+0) matches GlobalMemoryStatusEx ullTotalPhys");
    } else {
        check(ok != FALSE && total_memory == 12ULL * 1024 * 1024 * 1024,
              "XMemGetWorkingSetStatistics total memory (+0) falls back to 12 GiB title limit");
    }

    uint64_t working_set = 0;
    std::memcpy(&working_set, stats + 8, sizeof(working_set));
    check(ok != FALSE && working_set != 0, "XMemGetWorkingSetStatistics writes a non-zero size at +8");

    check(ok != FALSE && total_memory >= working_set,
          "XMemGetWorkingSetStatistics total memory (+0) >= working set (+8)");

    bool tail_untouched = true;
    for (size_t i = 16; i < sizeof(stats); ++i) {
        if (stats[i] != 0xa5) {
            tail_untouched = false;
            break;
        }
    }
    check(ok != FALSE && tail_untouched, "XMemGetWorkingSetStatistics leaves bytes 16..63 untouched");

    check(get_stats(kMeasuredKind, nullptr) == FALSE,
          "XMemGetWorkingSetStatistics(kind, nullptr) returns FALSE without crashing");
}

void test_virtual_query(XMemVirtualQueryFn query) {
    uint8_t local = 0;
    const void* const local_address = &local;

    alignas(64) uint8_t out[64];
    std::memset(out, 0xa5, sizeof(out));
    const SIZE_T written = query(GetCurrentProcess(), local_address, out);
    check(written != 0, "XMemVirtualQuery(current process, stack local) returns non-zero");

    MEMORY_BASIC_INFORMATION info{};
    std::memcpy(&info, out, sizeof(info));
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t address = reinterpret_cast<uintptr_t>(local_address);
    const bool contains_local =
        base != 0 && info.RegionSize != 0 && address >= base && address - base < info.RegionSize;
    check(written != 0 && contains_local,
          "XMemVirtualQuery fills MEMORY_BASIC_INFORMATION with a region containing the local");

    bool tail_zero = true;
    for (size_t i = sizeof(info); i < sizeof(out); ++i) {
        if (out[i] != 0) {
            tail_zero = false;
            break;
        }
    }
    check(written != 0 && tail_zero, "XMemVirtualQuery leaves the 64-byte output tail zero");

    alignas(64) uint8_t bad_out[64];
    std::memset(bad_out, 0xa5, sizeof(bad_out));
    // Address 1 is queryable (it falls in the free region starting at 0), so use a non-canonical address that
    // VirtualQueryEx rejects.
    const void* const unmappable = reinterpret_cast<const void*>(~static_cast<uintptr_t>(0));
    const SIZE_T bad = query(GetCurrentProcess(), unmappable, bad_out);
    check(bad == 0, "XMemVirtualQuery on an invalid address returns 0 without crashing");
}

void test_virtual_alloc(XMemVirtualAllocFn virtual_alloc, XMemGetWriteWatchFn get_write_watch,
                       XMemResetWriteWatchFn reset_write_watch) {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T page_size = system_info.dwPageSize;
    check(page_size != 0, "GetSystemInfo reports a non-zero page size");
    if (page_size == 0) return;

    const SIZE_T region_size = page_size * 3;
    void* const base = virtual_alloc(nullptr, region_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 0,
                                     PAGE_READWRITE);
    check(base != nullptr, "XMemVirtualAlloc creates a synthetic write-watch region");
    if (!base) return;

    void* watched[4]{};
    ULONG_PTR count = 4;
    ULONG granularity = 0;
    UINT status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 0, "new write-watch region starts with no dirty pages");
    check(status == NO_ERROR && granularity == page_size, "write-watch reports the host page granularity");

    auto* const bytes = static_cast<volatile uint8_t*>(base);
    bytes[0] = 0xa1;
    bytes[page_size * 2] = 0xb2;

    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    bool saw_first_page = false;
    bool saw_third_page = false;
    bool all_addresses_are_page_aligned = count <= 4;
    for (ULONG_PTR i = 0; i < count && i < 4; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(watched[i]);
        const uintptr_t start = reinterpret_cast<uintptr_t>(base);
        all_addresses_are_page_aligned &= address >= start && (address - start) % page_size == 0;
        saw_first_page |= address == start;
        saw_third_page |= address == start + page_size * 2;
    }
    check(status == NO_ERROR && count == 2 && saw_first_page && saw_third_page,
          "write-watch reports each written page by its relative address");
    check(all_addresses_are_page_aligned, "write-watch addresses are page aligned and within the region");

    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 2, "query-only write-watch leaves dirty pages recorded");

    status = reset_write_watch(base, region_size);
    check(status == NO_ERROR, "XMemResetWriteWatch clears the synthetic region");
    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 0, "reset write-watch query returns no dirty pages");

    bytes[page_size] = 0xc3;
    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 1 &&
              reinterpret_cast<uintptr_t>(watched[0]) == reinterpret_cast<uintptr_t>(base) + page_size,
          "write-watch records writes again after reset");

    check(VirtualFree(base, 0, MEM_RELEASE), "release the synthetic write-watch region");

    // 2 MiB guest pages: the region is 2 MiB-aligned, rounded up to whole 2 MiB pages and committed.
    void* const large_page = virtual_alloc(nullptr, kPage2m + page_size, MEM_RESERVE | MEM_COMMIT | kXmemLargePages,
                                           0x183, PAGE_READWRITE | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_COHERENT);
    check(large_page != nullptr, "XMemVirtualAlloc commits 2 MiB guest pages");
    if (large_page) {
        check((reinterpret_cast<uintptr_t>(large_page) & (kPage2m - 1)) == 0, "a 2 MiB-page region is 2 MiB-aligned");
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(large_page, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.RegionSize >= 2 * kPage2m && info.Protect == PAGE_READWRITE,
              "a 2 MiB-page region is rounded up to whole pages, committed read-write without the graphics bits");
        static_cast<volatile uint8_t*>(large_page)[2 * kPage2m - 1] = 0x77;
        check(VirtualFree(large_page, 0, MEM_RELEASE), "release the 2 MiB-page region");
    }

    // The attribute word is accepted (every measured call passes a nonzero one) and the protection loses its graphics
    // bits, in the 64 KiB page mode as well.
    void* const attributed = virtual_alloc(nullptr, kPage64k, MEM_RESERVE | MEM_COMMIT | kXmem64kPages, 0x181,
                                           PAGE_READWRITE | PAGE_GRAPHICS_READWRITE);
    check(attributed != nullptr, "XMemVirtualAlloc accepts a nonzero attribute word and console protection bits");
    if (attributed) {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(attributed, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
              "the region is committed PAGE_READWRITE (graphics bits dropped)");
        static_cast<volatile uint8_t*>(attributed)[0] = 0x42;
        check(VirtualFree(attributed, 0, MEM_RELEASE), "release the attributed region");
    }

    // The retail executable's GPU pools: the attribute word is recorded, and the PAGE_GRAPHICS_* bits are dropped.
    MEMORY_BASIC_INFORMATION mbi{};
    void* const pool = virtual_alloc(nullptr, SIZE_T{8} << 30, MEM_RESERVE | 0x20400000, 0x21, PAGE_READWRITE);
    check(pool != nullptr, "attributes 0x21 with 64 KiB pages reserves 8 GiB");
    if (pool) {
        check(VirtualQuery(pool, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE, "the 8 GiB pool is reserve-only");
        check(VirtualFree(pool, 0, MEM_RELEASE), "release the 8 GiB pool");
    }

    void* const noaccess = virtual_alloc(nullptr, 0x10000, MEM_RESERVE | 0x20400000, 0x3, 0x801);
    check(noaccess != nullptr, "attributes 0x3 with protect PAGE_GRAPHICS_NOACCESS|PAGE_NOACCESS succeeds");
    if (noaccess) {
        check(VirtualQuery(noaccess, &mbi, sizeof mbi) && mbi.AllocationProtect == PAGE_NOACCESS,
              "protect 0x801 becomes PAGE_NOACCESS");
        check(VirtualFree(noaccess, 0, MEM_RELEASE), "release the PAGE_NOACCESS pool");
    }

    void* const gpu = virtual_alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x183, 0x22004);
    check(gpu != nullptr, "attributes 0x183 with protect 0x22004 succeeds");
    if (gpu) {
        check(VirtualQuery(gpu, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE, "protect 0x22004 becomes PAGE_READWRITE");
        static_cast<volatile uint8_t*>(gpu)[0] = 0x5a;
        check(static_cast<volatile uint8_t*>(gpu)[0] == 0x5a, "the 0x183 allocation is writable");
        check(VirtualFree(gpu, 0, MEM_RELEASE), "release the 0x183 allocation");
    }

    SetLastError(ERROR_SUCCESS);
    void* const replace_without_address =
        virtual_alloc(nullptr, kPage64k, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0, PAGE_READWRITE);
    check(replace_without_address == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "MEM_REPLACE_PLACEHOLDER without an address is rejected");
}

// XMemAllocDefault / XMemFreeDefault share XMemAlloc's heap: a block from either is freed by either.
void test_default_alloc(XMemAllocFn alloc, XMemFreeFn free_mem, XMemAllocFn alloc_default,
                        XMemFreeFn free_default) {
    void* const a = alloc_default(100, 0x5e50000);
    check(aligned_64(a), "XMemAllocDefault returns a 64-byte aligned block");
    void* const zero = alloc_default(0, 0);
    check(zero != nullptr, "XMemAllocDefault(0) returns non-null");
    void* const b = alloc(100, 0);
    if (a) std::memset(a, 0x11, 100);
    if (b) std::memset(b, 0x22, 100);
    free_mem(a, 0x5e50000);
    free_default(b, 0);
    free_default(zero, 0);
    check(true, "XMemFree releases an XMemAllocDefault block and XMemFreeDefault an XMemAlloc block");
}

uint32_t read_u32(const void* base, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
    return value;
}

void fill_page(void* page, uint32_t tag) {
    auto* words = static_cast<uint32_t*>(page);
    for (size_t i = 0; i < kPage64k / sizeof(uint32_t); i += 1024) words[i] = tag + static_cast<uint32_t>(i);
}

bool page_holds(const void* page, uint32_t tag) {
    for (size_t i = 0; i < kPage64k / sizeof(uint32_t); i += 1024) {
        if (read_u32(page, i * sizeof(uint32_t)) != tag + static_cast<uint32_t>(i)) return false;
    }
    return true;
}

bool reserved_only(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_RESERVE;
}

struct PhysicalApi {
    XMemVirtualAllocFn virtual_alloc;
    XMemAllocatePhysicalPagesFn allocate;
    XMemFreePhysicalPagesFn free_pages;
    XMemMapPhysicalPagesFn map;
    XMemMapPhysicalPagesScatterFn scatter;
};

// Physical pages in a 64 KiB-page window, as the measured callers use them: reserve a window, allocate pages, map them,
// remap them elsewhere and in another order, unmap, free.
void test_physical_pages(const PhysicalApi& api) {
    constexpr uint64_t kPages = 4;
    auto* const window = static_cast<uint8_t*>(api.virtual_alloc(
        nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE | PAGE_GRAPHICS_READWRITE));
    check(window != nullptr, "XMemVirtualAlloc reserves a 64 KiB-page window");
    auto* const second = static_cast<uint8_t*>(api.virtual_alloc(
        nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(second != nullptr, "XMemVirtualAlloc reserves a second window");
    if (!window || !second) return;
    check(reserved_only(window), "the window is reserved, not committed");

    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages,
          "XMemAllocatePhysicalPages allocates every page asked for and keeps the count");
    bool numbers_ok = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        numbers_ok &= pages[i] != 0 && pages[i] % 16 == 0 && pages[i] < (uint64_t{1} << 25);
        for (uint64_t j = 0; j < i; ++j) numbers_ok &= pages[i] != pages[j];
    }
    check(numbers_ok, "page numbers are distinct non-zero multiples of 16 below 2^25");

    check(api.map(window, kPages, pages) == window, "XMemMapPhysicalPages returns the address it mapped");
    for (uint64_t i = 0; i < kPages; ++i) fill_page(window + i * kPage64k, 0x1000u * static_cast<uint32_t>(i + 1));
    {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(window, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.Type == MEM_MAPPED && info.Protect == PAGE_READWRITE,
              "a mapped page is a read-write view with the window's protection");
    }

    check(api.map(window, kPages, nullptr) == window, "XMemMapPhysicalPages with no page array unmaps");
    check(reserved_only(window) && reserved_only(window + 3 * kPage64k), "unmapped pages are reserved again");

    // Remap in reverse order into the other window: each page keeps its contents.
    uint64_t reversed[kPages];
    for (uint64_t i = 0; i < kPages; ++i) reversed[i] = pages[kPages - 1 - i];
    check(api.map(second, kPages, reversed) == second, "the pages map into another window");
    bool kept = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        kept &= page_holds(second + i * kPage64k, 0x1000u * static_cast<uint32_t>(kPages - i));
    }
    check(kept, "a physical page keeps its contents across unmap and remap");

    // One page mapped at two addresses is one memory.
    check(api.map(window, 1, &pages[0]) == window, "a page already mapped elsewhere maps again");
    static_cast<volatile uint32_t*>(static_cast<void*>(window))[1] = 0xfeedf00d;
    check(read_u32(second + 3 * kPage64k, 4) == 0xfeedf00d, "both mappings of a page see the same memory");

    // Mapping over a mapped page replaces it; a 0 entry unmaps one page.
    const uint64_t replace[2] = {pages[1], 0};
    check(api.map(window, 2, replace) == window, "mapping over mapped pages replaces them");
    check(page_holds(window, 0x2000u) && reserved_only(window + kPage64k),
          "the first page now shows page 1 and a 0 entry left the second unmapped");

    // Scatter: entry i maps page i at address i, out of order; 0 unmaps.
    void* const addresses[3] = {window + 3 * kPage64k, window + 2 * kPage64k, window};
    const uint64_t scattered[3] = {pages[2], pages[3], 0};
    check(api.scatter(addresses, 3, scattered) == TRUE, "XMemMapPhysicalPagesScatter maps each page at its address");
    check(page_holds(window + 3 * kPage64k, 0x3000u) && page_holds(window + 2 * kPage64k, 0x4000u) &&
              reserved_only(window),
          "scattered pages land at their own addresses and a 0 entry unmaps");
    check(api.scatter(addresses, 3, nullptr) == TRUE && reserved_only(window + 3 * kPage64k),
          "XMemMapPhysicalPagesScatter with no page array unmaps");

    uint8_t outside = 0;
    SetLastError(ERROR_SUCCESS);
    check(api.map(reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(&outside) + 0xffff) & ~uintptr_t{0xffff}), 1,
                  pages) == nullptr,
          "mapping outside any window fails");
    check(api.map(window + 1, 1, pages) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "mapping at an address that is not 64 KiB-aligned fails");
    const uint64_t bogus = 0x1fffff0;  // the highest page number, far above the few this test allocates
    check(api.map(window, 1, &bogus) == nullptr, "mapping a page number that was never allocated fails");

    check(api.map(second, kPages, nullptr) == second, "unmap the second window");
    check(api.free_pages(kPages, pages) == TRUE, "XMemFreePhysicalPages frees the pages");
    check(api.free_pages(1, pages) == FALSE, "freeing a page twice fails");

    // Freed numbers are reused.
    uint64_t again[kPages]{};
    count = kPages;
    check(api.allocate(0x180, &count, again) == TRUE && count == kPages, "pages can be allocated again");
    bool reused = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        bool found = false;
        for (uint64_t j = 0; j < kPages; ++j) found |= again[i] == pages[j];
        reused &= found;
    }
    check(reused, "freed page numbers are reused");
    check(api.map(window, 1, again) == window && read_u32(window, 0) == 0, "a fresh page maps and reads as zero");
    api.map(window, 1, nullptr);
    api.free_pages(kPages, again);

    uint64_t zero_count = 0;
    check(api.allocate(0x280, &zero_count, nullptr) == TRUE, "allocating zero pages succeeds");
    check(api.allocate(0x280, nullptr, pages) == FALSE, "a null count fails");
    // The windows were split into one placeholder per page; the process exit releases them.
}

bool access_faults(volatile uint32_t* address, bool write) {
    __try {
        if (write) {
            *address = 1;
        } else {
            const uint32_t value = *address;
            (void)value;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}

int64_t commit_growth(const PERFORMANCE_INFORMATION& from, const PERFORMANCE_INFORMATION& to) {
    return (static_cast<int64_t>(to.CommitTotal) - static_cast<int64_t>(from.CommitTotal)) *
           static_cast<int64_t>(to.PageSize);
}

void test_physical_pages_commit_on_touch(const PhysicalApi& api) {
    PERFORMANCE_INFORMATION before{};
    if (!GetPerformanceInfo(&before, sizeof(before))) {
        check(false, "GetPerformanceInfo reads the system commit charge");
        return;
    }
    const uint64_t free_commit = static_cast<uint64_t>(before.CommitLimit - before.CommitTotal) * before.PageSize;
    const uint64_t count = (free_commit + (uint64_t{1} << 30)) / kPage64k;
    if (count >= (uint64_t{1} << 20)) {
        std::printf("SKIP physical pages past the free commit: %llu MiB of commit is free\n",
                    static_cast<unsigned long long>(free_commit >> 20));
        return;
    }
    std::vector<uint64_t> pages(static_cast<size_t>(count));
    uint64_t got = count;
    check(api.allocate(0x380, &got, pages.data()) == TRUE && got == count,
          "XMemAllocatePhysicalPages gives 1 GiB more pages than the system has commit free");
    if (got != count) return;
    PERFORMANCE_INFORMATION allocated{};
    GetPerformanceInfo(&allocated, sizeof(allocated));
    check(commit_growth(before, allocated) < static_cast<int64_t>(count * kPage64k / 8),
          "allocating those pages does not charge them");

    constexpr uint64_t kMapped = 1024;
    constexpr uint64_t kTouched = 512;
    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kMapped * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    auto* const second = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 2 * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(window != nullptr && second != nullptr, "XMemVirtualAlloc reserves windows for them");
    if (!window || !second) {
        api.free_pages(count, pages.data());
        return;
    }
    check(api.map(window, kMapped, pages.data()) == window, "1,024 of them map into one window");
    PERFORMANCE_INFORMATION mapped{};
    GetPerformanceInfo(&mapped, sizeof(mapped));
    check(commit_growth(allocated, mapped) < static_cast<int64_t>(kMapped * kPage64k / 2),
          "mapping them does not charge them");

    for (uint64_t i = 0; i < kTouched; ++i) {
        static_cast<volatile uint32_t*>(static_cast<void*>(window + i * kPage64k))[0] =
            0xc0de0000u | static_cast<uint32_t>(i);
    }
    PERFORMANCE_INFORMATION touched{};
    GetPerformanceInfo(&touched, sizeof(touched));
    const int64_t touched_growth = commit_growth(mapped, touched);
    check(touched_growth >= static_cast<int64_t>(kTouched * kPage64k / 2) &&
              touched_growth < static_cast<int64_t>(kMapped * kPage64k),
          "writing to 512 of them charges about those 512 pages");
    bool written = true;
    for (uint64_t i = 0; i < kTouched; ++i) written &= read_u32(window + i * kPage64k, 0) == (0xc0de0000u | i);
    check(written, "each touched page keeps what was written to it");
    check(read_u32(window + kTouched * kPage64k, 0) == 0, "an untouched page reads as zero");

    check(api.map(second, 2, pages.data()) == second, "two of the pages map into a second window");
    check(read_u32(second, 0) == 0xc0de0000u && read_u32(second + kPage64k, 0) == 0xc0de0001u,
          "the second window sees what was written through the first");
    static_cast<volatile uint32_t*>(static_cast<void*>(second))[1] = 0x5ca1ab1e;
    check(read_u32(window, 4) == 0x5ca1ab1e, "and the first window sees what is written through the second");

    DWORD old = 0;
    check(VirtualProtect(window, kPage64k, PAGE_READONLY, &old) != FALSE &&
              access_faults(static_cast<volatile uint32_t*>(static_cast<void*>(window)), true),
          "a write to a committed read-only page still faults to the next handler");
    VirtualProtect(window, kPage64k, PAGE_READWRITE, &old);
    check(VirtualProtect(window + 4096, 4096, PAGE_READONLY, &old) != FALSE &&
              access_faults(static_cast<volatile uint32_t*>(static_cast<void*>(window + 4096)), true),
          "a write to a read-only 4 KiB part of a read-write page still faults to the next handler");
    check(!access_faults(static_cast<volatile uint32_t*>(static_cast<void*>(window + 8192)), true),
          "the rest of that page stays writable");
    VirtualProtect(window + 4096, 4096, PAGE_READWRITE, &old);
    void* const elsewhere = VirtualAlloc(nullptr, kPage64k, MEM_RESERVE, PAGE_READWRITE);
    check(elsewhere != nullptr && access_faults(static_cast<volatile uint32_t*>(elsewhere), false),
          "a fault outside the mapped pages still reaches the next handler");
    if (elsewhere) VirtualFree(elsewhere, 0, MEM_RELEASE);

    uint8_t* const target = window + (kTouched + 1) * kPage64k;
    check(rt::xmem::commit_physical_range(target, kPage64k), "commit_physical_range commits an untouched page");
    uint32_t local = 0;
    check(rt::xmem::commit_physical_range(&local, sizeof(local)), "commit_physical_range skips memory it does not own");
    wchar_t directory[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    bool read_ok = false;
    if (GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"xpg", 0, path)) {
        std::vector<uint32_t> pattern(kPage64k / sizeof(uint32_t));
        for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = 0x7e570000u + static_cast<uint32_t>(i);
        const HANDLE out =
            CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD bytes = 0;
        const bool wrote = out != INVALID_HANDLE_VALUE &&
                           WriteFile(out, pattern.data(), static_cast<DWORD>(kPage64k), &bytes, nullptr) &&
                           bytes == kPage64k;
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        const HANDLE in = wrote ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)
                                : INVALID_HANDLE_VALUE;
        if (in != INVALID_HANDLE_VALUE) {
            read_ok = ReadFile(in, target, static_cast<DWORD>(kPage64k), &bytes, nullptr) && bytes == kPage64k &&
                      std::memcmp(target, pattern.data(), kPage64k) == 0;
            CloseHandle(in);
        }
        DeleteFileW(path);
    }
    check(read_ok, "ReadFile fills a page that commit_physical_range committed");

    check(api.map(second, 2, nullptr) == second && api.map(window, kMapped, nullptr) == window, "they unmap");
    check(api.free_pages(count, pages.data()) == TRUE, "XMemFreePhysicalPages frees them");
}

void test_commit_physical_pages(const PhysicalApi& api) {
    constexpr uint64_t kPages = 3;
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages, "three physical pages for commit by number");
    if (count != kPages) return;
    check(rt::xmem::commit_physical_pages(pages, 0), "committing no pages succeeds");
    check(!rt::xmem::commit_physical_pages(nullptr, 1), "committing from a null page array fails");
    const uint64_t unknown = 0x1fffff0;
    check(!rt::xmem::commit_physical_pages(&unknown, 1), "committing a page number that was never allocated fails");
    check(rt::xmem::commit_physical_pages(pages, 2), "commit_physical_pages commits two pages that were never mapped");

    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(window != nullptr, "XMemVirtualAlloc reserves a window for them");
    if (window) {
        check(api.map(window, kPages, pages) == window, "the three pages map");
        MEMORY_BASIC_INFORMATION info[kPages]{};
        bool queried = true;
        for (uint64_t i = 0; i < kPages; ++i)
            queried &= VirtualQuery(window + i * kPage64k, &info[i], sizeof(info[i])) == sizeof(info[i]);
        check(queried && info[0].State == MEM_COMMIT && info[1].State == MEM_COMMIT && info[2].State == MEM_RESERVE,
              "the two committed by number are committed in the window and the third is not yet");
        check(read_u32(window, 0) == 0 && read_u32(window + kPage64k, 0) == 0,
              "pages committed by number read as zero");
        api.map(window, kPages, nullptr);
    }
    check(api.free_pages(kPages, pages) == TRUE, "XMemFreePhysicalPages frees them");
}

// Console queue slot 31 (port/shims/xmem.h, docs/cpu_view_mapping.md): the proxy decodes the title's batch, and the
// hook this file installs gives each destination page a second view of the physical page already mapped at its source
// page (a null source unmaps). This is the host mapping that makes the title's page destination readable; the decode
// itself is xs_proxy_mock_test's check.
void test_copy_page_mappings(const PhysicalApi& api) {
    constexpr uint64_t kPages = 4;
    check(rt::xmem::copy_page_mappings_fn != nullptr, "xmem installs the queue slot-31 page-mapping hook");
    if (!rt::xmem::copy_page_mappings_fn) return;
    auto* const source = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    auto* const destination = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(source != nullptr && destination != nullptr, "XMemVirtualAlloc reserves source and destination windows");
    if (!source || !destination) return;
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages, "physical pages for the slot-31 copy");
    if (count != kPages) return;
    check(api.map(source, kPages, pages) == source, "the pages map into the source window");
    for (uint64_t i = 0; i < kPages; ++i) fill_page(source + i * kPage64k, 0x1000u * static_cast<uint32_t>(i + 1));

    // Kind 0's shape: source pages 0, 1 one after another at destination pages 0, 1.
    check(rt::xmem::copy_page_mappings_fn(destination, source, 2, false),
          "copy_page_mappings maps two source pages at two destination pages");
    check(page_holds(destination, 0x1000u) && page_holds(destination + kPage64k, 0x2000u),
          "the destination pages hold the source pages' bytes");
    check(reserved_only(destination + 2 * kPage64k), "a destination page the copy did not name stays reserved");

    // Kind 1's shape: source page 3 at both destination pages 2 and 3.
    check(rt::xmem::copy_page_mappings_fn(destination + 2 * kPage64k, source + 3 * kPage64k, 2, true),
          "copy_page_mappings repeats one source page over two destination pages");
    check(page_holds(destination + 2 * kPage64k, 0x4000u) && page_holds(destination + 3 * kPage64k, 0x4000u),
          "both repeated destination pages hold the one source page");

    // One physical page, three views: the destination write is visible at the source address.
    static_cast<volatile uint32_t*>(static_cast<void*>(destination))[3] = 0xfeedf00d;
    check(read_u32(source, 12) == 0xfeedf00d, "a copied destination page and its source page are one memory");

    // A source page with nothing mapped leaves its destination page unmapped (a page mapped there before is dropped).
    check(api.map(source + 2 * kPage64k, 1, nullptr) == source + 2 * kPage64k, "unmap the third source page");
    check(rt::xmem::copy_page_mappings_fn(destination, source + 2 * kPage64k, 1, false),
          "copying an unmapped source page succeeds");
    check(reserved_only(destination), "an unmapped source page leaves its destination page reserved");
    check(reserved_only(source + 2 * kPage64k), "the unmapped source page stays reserved");

    // A null source unmaps the destination pages it names.
    check(rt::xmem::copy_page_mappings_fn(destination + 2 * kPage64k, nullptr, 2, false),
          "copy_page_mappings with a null source unmaps");
    check(reserved_only(destination + 2 * kPage64k) && reserved_only(destination + 3 * kPage64k),
          "a null source left both destination pages reserved");

    // Refusals: an unaligned page address, a page count that would wrap, and a destination outside any window.
    SetLastError(ERROR_SUCCESS);
    check(!rt::xmem::copy_page_mappings_fn(destination + 1, source, 1, false) &&
              GetLastError() == ERROR_INVALID_PARAMETER,
          "copying to an address that is not 64 KiB-aligned fails");
    SetLastError(ERROR_SUCCESS);
    check(!rt::xmem::copy_page_mappings_fn(destination, source + 1, 1, false) &&
              GetLastError() == ERROR_INVALID_PARAMETER,
          "copying from an address that is not 64 KiB-aligned fails");
    check(!rt::xmem::copy_page_mappings_fn(destination, source, (uint64_t{1} << 62) / kPage64k + 1, false),
          "a page count that overflows the address space fails");
    SetLastError(ERROR_SUCCESS);
    check(!rt::xmem::copy_page_mappings_fn(destination + kPages * kPage64k, source, 1, false) &&
              GetLastError() == ERROR_INVALID_ADDRESS,
          "a destination outside a physical-page window fails");

    api.map(source, kPages, nullptr);
    api.map(destination, 1, nullptr);
    check(api.free_pages(kPages, pages) == TRUE, "XMemFreePhysicalPages frees the slot-31 pages");
}

// The DirectStorage page-list resolver (port/shims/xmem.h, docs/page_resolver.md): a page number resolves to the CPU
// view of the physical page's backing, which is one memory with every mapping of that page. The dstorage page-list path
// drives the resolver through dstorage_advanced.h's transfer_page_list, so this test runs that same seam against real
// pages and a real title window: the bytes must be readable at the mapping the title reads.
void test_resolve_physical_page(const PhysicalApi& api) {
    check(rt::xmem::resolve_physical_page_fn != nullptr, "xmem installs the DirectStorage page-list resolver hook");
    if (!rt::xmem::resolve_physical_page_fn) return;
    constexpr uint64_t kPages = 3;
    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(window != nullptr, "XMemVirtualAlloc reserves a window for the resolver");
    if (!window) return;
    uint64_t pages[kPages] = {};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages, "physical pages for the resolver");
    if (count != kPages) return;

    // A mapped page: the title's view and the resolver's view are one memory in both directions.
    check(api.map(window, 1, pages) == window, "the first resolver page maps into the window");
    fill_page(window, 0x51510000u);
    uint8_t* const view = rt::xmem::resolve_physical_page_fn(pages[0]);
    check(view != nullptr, "a live physical page number resolves to a CPU view");
    bool same = view != nullptr;
    if (view) {
        for (std::size_t i = 0; i < kPage64k / sizeof(uint32_t); i += 1024)
            same &= *reinterpret_cast<const uint32_t*>(view + i * sizeof(uint32_t)) ==
                    0x51510000u + static_cast<uint32_t>(i);
        check(same, "the resolved view holds what the title's mapping holds (one memory)");
        *reinterpret_cast<uint32_t*>(view + 0x2000) = 0xfeedc0deu;
        check(read_u32(window, 0x2000) == 0xfeedc0deu, "a write through the resolved view is visible at the mapped view");
    }

    // A page that was allocated but never touched resolves and is committed on the way out (a store into it must not
    // fault); mapping it afterwards shows the bytes the resolver wrote.
    uint8_t* const fresh = rt::xmem::resolve_physical_page_fn(pages[1]);
    check(fresh != nullptr, "an untouched physical page resolves (and is committed)");
    if (fresh) {
        std::memset(fresh, 0x5a, kPage64k);
        check(api.map(window + kPage64k, 1, pages + 1) == window + kPage64k,
              "the untouched page maps after the resolve");
        check(read_u32(window + kPage64k, 0) == 0x5a5a5a5au, "the title's mapping sees the resolver's write");
    }

    // The same seam the request path uses: a plan over two mapped pages, the transfer, and the bytes read back.
    using palmetto::dstorage::PageTransferError;
    using palmetto::dstorage::transfer_page_list;
    const palmetto::dstorage::PageResolver resolve = [](void*, std::uint64_t page_number) -> const std::uint8_t* {
        return rt::xmem::resolve_physical_page_fn ? rt::xmem::resolve_physical_page_fn(page_number) : nullptr;
    };
    constexpr std::size_t kBytes = 100000;
    std::vector<uint8_t> payload(kBytes);
    for (std::size_t at = 0; at < kBytes; ++at) payload[at] = static_cast<uint8_t>((at * 29 + 7) & 0xff);
    const auto scattered = transfer_page_list(pages, 2, 4321, kBytes, resolve, nullptr, payload.data(), true);
    bool scattered_ok = scattered.error == PageTransferError::ok && scattered.spans == 2;
    if (scattered_ok) {
        for (std::size_t at = 0; at < kBytes; ++at) {
            const std::size_t page = (4321 + at) / kPage64k;
            const std::size_t offset = (4321 + at) % kPage64k;
            if (window[page * kPage64k + offset] != payload[at]) scattered_ok = false;
        }
    }
    check(scattered_ok, "a live page-list transfer's bytes are visible through the title's mappings");
    std::vector<uint8_t> gathered(kBytes);
    const auto read_back = transfer_page_list(pages, 2, 4321, kBytes, resolve, nullptr, gathered.data(), false);
    check(read_back.error == PageTransferError::ok && gathered == payload,
          "the same page list gathers the transferred bytes back");

    // An unallocated page number refuses the whole transfer before the first byte: the mapped pages keep their bytes.
    uint64_t absent[2] = {pages[0], 0x1fffff0};  // a multiple of 16 below the slot limit that was never allocated
    const std::vector<uint8_t> before(window, window + 2 * kPage64k);
    const auto refused = transfer_page_list(absent, 2, 0, static_cast<std::size_t>(kPage64k) + 32, resolve, nullptr,
                                            payload.data(), true);
    check(refused.error == PageTransferError::unresolved_page && refused.page == absent[1],
          "an unallocated page number is an unresolved_page refusal");
    check(std::vector<uint8_t>(window, window + 2 * kPage64k) == before,
          "the refused transfer wrote nothing into the mapped pages");

    // The resolver's own refusals: page 0, a never-allocated number, and a number after its pages are freed.
    check(rt::xmem::resolve_physical_page_fn(0) == nullptr, "page number 0 does not resolve");
    check(rt::xmem::resolve_physical_page_fn(absent[1]) == nullptr, "a never-allocated page number does not resolve");
    check(api.map(window, 1, nullptr) == window, "unmap the first resolver page");
    check(api.map(window + kPage64k, 1, nullptr) == window + kPage64k, "unmap the second resolver page");
    check(api.free_pages(kPages, pages) == TRUE, "free the resolver's pages");
    check(rt::xmem::resolve_physical_page_fn(pages[2]) == nullptr, "a freed page number no longer resolves");
}

void test_first_touch_during_remap(const PhysicalApi& api) {
    constexpr uint64_t kSlice = 16;
    constexpr uint64_t kSlices = 256;
    std::vector<uint64_t> pages(kSlice * kSlices);
    uint64_t count = pages.size();
    check(api.allocate(0x280, &count, pages.data()) == TRUE && count == pages.size(),
          "4,096 physical pages for the remap race");
    if (count != pages.size()) return;
    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kSlice * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(window != nullptr, "XMemVirtualAlloc reserves a window for the remap race");
    if (!window) {
        api.free_pages(count, pages.data());
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> touches{0};
    std::atomic<uint64_t> unmapped_faults{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t] {
            uint64_t i = static_cast<uint64_t>(t);
            while (!stop.load(std::memory_order_relaxed)) {
                auto* const word =
                    static_cast<volatile uint32_t*>(static_cast<void*>(window + (i % kSlice) * kPage64k)) + t;
                if (access_faults(word, true)) {
                    unmapped_faults.fetch_add(1, std::memory_order_relaxed);
                } else {
                    touches.fetch_add(1, std::memory_order_relaxed);
                }
                ++i;
            }
        });
    }
    const ULONGLONG started = GetTickCount64();
    bool remapped = true;
    for (uint64_t s = 0; s < kSlices; ++s) {
        remapped &= api.map(window, kSlice, pages.data() + s * kSlice) == window;
        Sleep(0);
        remapped &= api.map(window, kSlice, nullptr) == window;
    }
    remapped &= api.map(window, kSlice, pages.data() + (kSlices - 1) * kSlice) == window;
    stop.store(true);
    for (auto& writer : writers) writer.join();
    const ULONGLONG elapsed = GetTickCount64() - started;
    check(remapped, "256 slices map and unmap while 4 threads write into the window");
    check(elapsed < 30000, "first-touch faults during map and unmap finish without hanging");
    check(touches.load() > 0, "writes land while the pages are mapped");
    std::printf("remap race: %llu writes landed, %llu hit an unmapped page, %llu ms\n",
                static_cast<unsigned long long>(touches.load()),
                static_cast<unsigned long long>(unmapped_faults.load()), static_cast<unsigned long long>(elapsed));
    static_cast<volatile uint32_t*>(static_cast<void*>(window))[8] = 0x600dcafe;
    check(read_u32(window, 32) == 0x600dcafe, "the last slice is still mapped and writable");
    api.map(window, kSlice, nullptr);
    check(api.free_pages(count, pages.data()) == TRUE, "XMemFreePhysicalPages frees the remap race pages");
}

// The placeholder flags pass through: reserve a placeholder, carve windows and committed memory out of it.
void test_placeholders(const PhysicalApi& api) {
    auto* const base = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 6 * kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
    check(base != nullptr, "XMemVirtualAlloc reserves a MEM_RESERVE_PLACEHOLDER region");
    if (!base) return;

    void* const window = api.virtual_alloc(base + kPage64k, 2 * kPage64k,
                                           MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27,
                                           PAGE_READWRITE | PAGE_GRAPHICS_READONLY);
    check(window == base + kPage64k, "a reserve-only MEM_REPLACE_PLACEHOLDER returns the address it replaced");

    auto* const committed = static_cast<uint8_t*>(
        api.virtual_alloc(base + 4 * kPage64k, kPage64k,
                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE));
    check(committed == base + 4 * kPage64k, "a committing MEM_REPLACE_PLACEHOLDER returns the address it replaced");
    if (committed) {
        committed[0] = 0x5a;
        check(committed[0] == 0x5a, "the committed replacement is writable");
    }

    uint64_t pages[2]{};
    uint64_t count = 2;
    check(api.allocate(0x380, &count, pages) == TRUE && count == 2, "allocate two pages for the carved window");
    check(api.map(window, 2, pages) == window, "pages map into a window carved out of a placeholder");
    if (window) {
        fill_page(static_cast<uint8_t*>(window) + kPage64k, 0xabc0000u);
        check(page_holds(static_cast<uint8_t*>(window) + kPage64k, 0xabc0000u), "the carved window holds data");
    }
    check(api.map(window, 2, nullptr) == window, "unmap the carved window");
    check(api.free_pages(2, pages) == TRUE, "free the carved window's pages");
    check(reserved_only(base), "the rest of the placeholder is still reserved");
}

// palmetto --trace: one "xmem_attributes" event per distinct raw attribute word and API, none with the trace off.
void test_attribute_trace(XMemAllocFn alloc, XMemFreeFn free_mem, XMemVirtualAllocFn virtual_alloc) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xmem_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    void* untraced = alloc(64, 0x1111);  // trace not open yet: nothing to write, nothing to crash
    check(untraced != nullptr, "XMemAlloc with the trace off still allocates");
    free_mem(untraced, 0x1111);

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");
    void* a = alloc(4096, 0x5e50000);
    void* b = alloc(8192, 0x5e50000);  // same word again: no second event
    void* c = alloc(16, 0x99);
    free_mem(a, 0x5e50000);
    free_mem(b, 0x5e50000);  // same word again: no second event
    free_mem(c, 0x99);
    free_mem(nullptr, 0x77);  // a free-only word is recorded too
    void* const v = virtual_alloc ? virtual_alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE)
                                  : nullptr;
    if (v) VirtualFree(v, 0, MEM_RELEASE);
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    auto count = [&](const std::string& needle) {
        int n = 0;
        for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
        return n;
    };
    check(count("\"kind\":\"xmem_attributes\"") == 6, "six events: two XMemAlloc words, three XMemFree words and one XMemVirtualAlloc word");
    check(count("\"api\":\"XMemVirtualAlloc\",\"attributes\":\"0x183\",\"size\":65536") == 1,
          "XMemVirtualAlloc 0x183 is recorded once, with its size");
    check(count("\"api\":\"XMemAlloc\",\"attributes\":\"0x5e50000\",\"size\":4096") == 1,
          "XMemAlloc 0x5e50000 is recorded once, with the size of the first call");
    check(count("\"api\":\"XMemAlloc\",\"attributes\":\"0x99\",\"size\":16") == 1, "XMemAlloc 0x99 is recorded once");
    check(count("\"api\":\"XMemFree\",\"attributes\":\"0x5e50000\"") == 1, "XMemFree 0x5e50000 is recorded once");
    check(count("\"api\":\"XMemFree\",\"attributes\":\"0x77\"") == 1, "XMemFree 0x77 is recorded");
    check(count("\"attributes\":\"0x1111\"") == 0, "nothing was recorded while the trace was off");
    check(count("\"caller\":\"") == 6, "every event names its caller");
    DeleteFileW(path.c_str());
}

// PALMETTO_XMEM_CALLERS=1: one "xmem_caller" event per (API, caller), none with the switch unset. Two call sites give
// two callers per API, and a repeated call from the same site adds nothing.
void caller_site_a(XMemAllocFn alloc, XMemFreeFn free_mem) {
    void* p = alloc(32, 0x22);
    free_mem(p, 0x22);
}

void caller_site_b(XMemAllocFn alloc, XMemFreeFn free_mem) {
    void* p = alloc(48, 0x22);
    free_mem(p, 0x22);
}

void test_caller_trace(XMemAllocFn alloc, XMemFreeFn free_mem) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xmem_caller_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());
    const auto events = [&] {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    };
    const auto count = [](const std::string& hay, const std::string& needle) {
        int n = 0;
        for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
        return n;
    };

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file for the caller test");
    caller_site_a(alloc, free_mem);
    rt::startup_trace::close();
    check(count(events(), "\"kind\":\"xmem_caller\"") == 0, "no xmem_caller event while PALMETTO_XMEM_CALLERS is unset");
    DeleteFileW(path.c_str());

    _putenv_s("PALMETTO_XMEM_CALLERS", "1");
    check(rt::startup_trace::open(path.c_str()), "open the startup trace file with the caller switch on");
    caller_site_a(alloc, free_mem);
    caller_site_a(alloc, free_mem);  // the same two sites again: no more events
    caller_site_b(alloc, free_mem);
    rt::startup_trace::close();
    _putenv_s("PALMETTO_XMEM_CALLERS", "");
    const std::string log = events();
    std::vector<std::string> caller_lines;
    {
        std::istringstream lines(log);
        for (std::string line; std::getline(lines, line);) {
            if (line.find("\"kind\":\"xmem_caller\"") != std::string::npos) caller_lines.push_back(line);
        }
    }
    int alloc_lines = 0;
    int free_lines = 0;
    int host_callers = 0;
    std::set<std::string> unique_callers;
    for (const std::string& line : caller_lines) {
        if (line.find("\"api\":\"XMemAlloc\"") != std::string::npos) ++alloc_lines;
        if (line.find("\"api\":\"XMemFree\"") != std::string::npos) ++free_lines;
        const size_t at = line.find("\"caller\":\"");
        const size_t end = at == std::string::npos ? std::string::npos : line.find('"', at + 10);
        if (end == std::string::npos) continue;
        const std::string caller = line.substr(at + 10, end - (at + 10));
        if (caller.rfind("host:", 0) == 0) ++host_callers;
        unique_callers.insert(caller);
    }
    check(caller_lines.size() >= 2, "the caller trace records the allocator entry points");
    check(alloc_lines > 0 && free_lines > 0, "both XMemAlloc and XMemFree callers are recorded");
    check(host_callers == static_cast<int>(caller_lines.size()), "every event names its caller as a module");
    check(unique_callers.size() == caller_lines.size(), "no caller is recorded twice");
    DeleteFileW(path.c_str());
}

// XMemTransferMemory moves budget between the console's title and system partitions; a PC has neither, so the shim
// succeeds for every direction and size and moves (allocates) nothing. The XMEM_TRANSFER_TYPE values are not public,
// so the directions here are just the first few enum values and one far outside any plausible range.
void test_transfer_memory(XMemTransferMemoryFn transfer) {
    constexpr SIZE_T kMiB = 1024 * 1024;
    const uint32_t types[] = {0u, 1u, 2u, 0xffffffffu};
    // 2 MiB and 40 MiB are the sizes the samples pass ("Must be multiples of 2MB sizes"); 0, an odd size and SIZE_MAX
    // are the boundaries.
    const SIZE_T sizes[] = {0, 2 * kMiB, 40 * kMiB, 2 * kMiB + 1, (std::numeric_limits<SIZE_T>::max)()};

    PROCESS_MEMORY_COUNTERS_EX before{};
    const bool have_before = GetProcessMemoryInfo(GetCurrentProcess(),
                                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
    bool all_true = true;
    bool last_error_kept = true;
    for (const uint32_t type : types) {
        for (const SIZE_T size : sizes) {
            SetLastError(0x2bad);
            const BOOL result = transfer(type, size);
            if (result != TRUE) {
                all_true = false;
                std::printf("  XMemTransferMemory(%u, %zu) -> %d\n", type, static_cast<size_t>(size), result);
            }
            last_error_kept &= GetLastError() == 0x2bad;
        }
    }
    check(all_true, "XMemTransferMemory returns TRUE (1) for every direction and size, 0 and SIZE_MAX included");
    check(last_error_kept, "XMemTransferMemory leaves the thread's last error alone");

    PROCESS_MEMORY_COUNTERS_EX after{};
    const bool have_after = GetProcessMemoryInfo(GetCurrentProcess(),
                                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
    // 20 calls asked to move up to 40 MiB each; nothing may be committed for them. 1 MiB of slack covers the CRT's
    // own buffers (the one-time stderr line).
    check(have_before && have_after && after.PrivateUsage < before.PrivateUsage + kMiB,
          "XMemTransferMemory commits no memory for the transferred sizes");

    // A bool-returning declaration reads only AL: TRUE must be non-zero there as well.
    const auto as_bool = reinterpret_cast<bool(WINAPI*)(uint32_t, SIZE_T)>(reinterpret_cast<void*>(transfer));
    check(as_bool(0, 2 * kMiB), "XMemTransferMemory's result also reads as true through a bool return");

    // The enum is 32 bits: garbage in the upper half of RCX must not change anything.
    const auto wide = reinterpret_cast<BOOL(WINAPI*)(uint64_t, SIZE_T)>(reinterpret_cast<void*>(transfer));
    check(wide(0xdead000000000000ull, 2 * kMiB) == TRUE, "XMemTransferMemory ignores the upper half of RCX");
}

// palmetto --trace: one "xmem_transfer" event per distinct (type, size), none with the trace off.
void test_transfer_trace(XMemTransferMemoryFn transfer) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xmem_transfer_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    check(transfer(7, 12345) == TRUE, "XMemTransferMemory with the trace off still returns TRUE");

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");
    transfer(0, 2097152);
    transfer(0, 2097152);  // same pair again: no second event
    transfer(0, 41943040);
    transfer(1, 2097152);
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    auto count = [&](const std::string& needle) {
        int n = 0;
        for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
        return n;
    };
    check(count("\"kind\":\"xmem_transfer\"") == 3, "three xmem_transfer events: one per distinct (type, size)");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":0,\"size\":2097152") == 1,
          "type 0 / 2 MiB is recorded once although it was called twice");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":0,\"size\":41943040") == 1, "type 0 / 40 MiB is recorded");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":1,\"size\":2097152") == 1, "type 1 / 2 MiB is recorded");
    check(count("\"size\":12345") == 0, "nothing was recorded while the trace was off");
    check(count("\"caller\":\"") == 3, "every transfer event names its caller");
    DeleteFileW(path.c_str());
}

using HostVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);

// RT-217: a title's kernel32!VirtualAlloc carries the console's MEM_GRAPHICS / MEM_TITLE type bits, which the host
// rejects. The registered shim has to drop them and still allocate.
void test_title_virtual_alloc() {
    constexpr DWORD kMemGraphics = 0x10000000;
    constexpr DWORD kMemTitle = 0x40000000;
    const char* const dlls[] = {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-memory-l1-1-0.dll"};
    for (const char* dll : dlls) {
        const auto alloc = reinterpret_cast<HostVirtualAllocFn>(rt::shims::find(dll, "VirtualAlloc"));
        char what[160];
        std::snprintf(what, sizeof what, "registry exposes %s!VirtualAlloc", dll);
        check(alloc != nullptr, what);
        if (!alloc) continue;

        void* const gfx = alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x2004);  // PAGE_GRAPHICS_READWRITE
        check(gfx != nullptr, "title VirtualAlloc with protect 0x2004 succeeds");
        if (gfx) {
            MEMORY_BASIC_INFORMATION mbi{};
            check(VirtualQuery(gfx, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE,
                  "protect 0x2004 commits PAGE_READWRITE");
            VirtualFree(gfx, 0, MEM_RELEASE);
        }
        for (const DWORD bits : {kMemGraphics, kMemTitle, kMemGraphics | kMemTitle, DWORD{0}}) {
            void* const p = alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT | bits, PAGE_READWRITE);
            std::snprintf(what, sizeof what, "%s!VirtualAlloc with type bits %#lx returns memory", dll,
                          static_cast<unsigned long>(bits));
            check(p != nullptr, what);
            if (!p) continue;
            static_cast<volatile uint8_t*>(p)[0] = 0x5a;
            check(static_cast<volatile uint8_t*>(p)[0] == 0x5a, "the allocation is committed and writable");
            check(VirtualFree(p, 0, MEM_RELEASE), "release the allocation");
        }
    }
}

void print_result() {
    if (g_failures == 0) {
        std::printf("xmem_test: OK (%d checks)\n", g_checks);
    } else {
        std::printf("xmem_test: FAILED (%d checks, %d failure%s)\n", g_checks, g_failures,
                    g_failures == 1 ? "" : "s");
    }
}

// docs/card_copies.md: the guard-page write tracker. The tracker is filled in by xmem.cpp (the same hook shape as the
// page-mapping hooks) and answers "was any watched page written since the last poll" without a scan. The checks are the
// coherence cases the mirror gate needs: an unchanged interval, a write reported once and only once, a read not
// reported as a change, detection surviving a read trip, the suspended window not reporting (and resuming), a range
// that cannot be guarded refused, and forget restoring ordinary protection.
void test_page_write_watch() {
    check(rt::xmem::page_watch_arm && rt::xmem::page_watch_forget && rt::xmem::page_watch_poll &&
              rt::xmem::page_watch_rearm && rt::xmem::page_watch_suspend && rt::xmem::page_watch_resume,
          "xmem.cpp installs the guard-page write tracker hooks");
    if (!rt::xmem::page_watch_arm || !rt::xmem::page_watch_forget || !rt::xmem::page_watch_poll ||
        !rt::xmem::page_watch_rearm || !rt::xmem::page_watch_suspend || !rt::xmem::page_watch_resume)
        return;
    constexpr SIZE_T kFourPages = 4 * 4096;
    auto* const region = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kFourPages, MEM_RESERVE | MEM_COMMIT,
                                                                 PAGE_READWRITE));
    auto* const other = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                                                PAGE_READWRITE));
    check(region != nullptr && other != nullptr, "two scratch regions for the write tracker");
    if (!region || !other) return;

    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unavailable,
          "the tracker answers unavailable before any range is watched");
    check(rt::xmem::page_watch_arm(region, kFourPages), "a committed writable range arms");
    check(rt::xmem::page_watch_arm(other, 4096), "a second range arms");
    check(rt::xmem::page_watch_pages() == 5, "the tracker reports the five guarded pages");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged,
          "a freshly armed range has no write to report");

    // A write trips the guard, the handler lets it continue, and the next poll reports it exactly once.
    region[4096 + 0x30] = 0x5a;
    check(region[4096 + 0x30] == 0x5a, "a write through a guarded page lands unchanged (coherence)");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::changed, "a guarded write is reported as changed");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged,
          "the change is reported once: the next poll is unchanged again");

    // A read is an access, not a change: it trips the guard, is re-armed, and is never reported as a change.
    volatile std::uint8_t read = region[2 * 4096 + 0x10];
    (void)read;
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged,
          "a guarded read is not reported as a change");

    // Detection survives that read trip: the other pages were never unguarded.
    region[3 * 4096] = 0x11;
    check(region[3 * 4096] == 0x11, "a second page's write lands");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::changed,
          "a write after a read-trip page is still reported");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged, "and is reported once");

    // The gate's dirty-range shape: two ranges, a write in one, the other reported unchanged until it is written.
    other[0] = 0x22;
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::changed, "a write in the second range is reported");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged, "the verdict clears");
    region[0] = 0x33;
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::changed, "a write in the first range is reported");

    // Suspended window (the full refresh scan): a write is not recorded, and resuming restores detection.
    rt::xmem::page_watch_suspend();
    region[4096 * 2] = 0x44;
    check(region[4096 * 2] == 0x44, "a write in the suspended window lands");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged,
          "a suspended write is not reported (the measured, documented window)");
    rt::xmem::page_watch_resume();
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unchanged, "resume keeps the verdict quiet");
    region[4096 * 2 + 1] = 0x55;
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::changed, "resume restores write detection");

    // A range that is reserved but never committed cannot hold a guard: the arm is refused, nothing stays armed.
    auto* const reserved = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
    if (reserved) {
        check(!rt::xmem::page_watch_arm(reserved, 4096), "a reserved (uncommitted) range is refused");
        check(rt::xmem::page_watch_pages() == 5, "a refused arm leaves the guarded page count unchanged");
        VirtualFree(reserved, 0, MEM_RELEASE);
    }
    // A range already guarded by the title is not adopted.
    auto* const own_guard = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                                                    PAGE_READWRITE | PAGE_GUARD));
    if (own_guard) {
        check(!rt::xmem::page_watch_arm(own_guard, 4096), "a page another owner already guards is not adopted");
        VirtualFree(own_guard, 0, MEM_RELEASE);
    }

    // Forget restores ordinary protection and closes the range; the tracker goes unavailable with nothing left.
    rt::xmem::page_watch_forget(other);
    rt::xmem::page_watch_forget(region);
    check(rt::xmem::page_watch_pages() == 0, "forget releases every guarded page");
    check(rt::xmem::page_watch_poll() == rt::xmem::WriteWatchPoll::unavailable,
          "the tracker is unavailable again with no range watched");
    // Coherence of the release: writing and reading the released memory must not trip a guard.
    region[4096] = 0x66;
    check(region[4096] == 0x66, "a forgotten range is ordinary writable memory again");
    // The tracker can be re-armed on the same address after a forget (the range registry is not a leak).
    check(rt::xmem::page_watch_arm(region, 4096), "a forgotten range can be armed again");
    rt::xmem::page_watch_forget(region);
    VirtualFree(other, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
}

// The arena-layout plan (docs/vertex_buffer_va.md section 19). The switch text is parsed as data, so an arm is a pure
// decision over one measured title caller: a typo, a missing caller or an unplaceable argument falls back to the
// default path instead of guessing at the title's allocator. This test process sets neither switch, so it also checks
// that the runtime's own plan stays off by default.
void test_layout_plan() {
    using rt::xmem::XmemLayoutMode;
    const auto none = rt::xmem::layout_plan_from(nullptr, nullptr);
    check(none.mode == XmemLayoutMode::none && !none.enabled() && !none.targeted(),
          "layout plan: unset switches leave the default path");
    check(rt::xmem::layout_plan_from("", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: an empty switch leaves the default path");
    check(rt::xmem::layout_plan_from("0", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: a zero switch leaves the default path");
    check(!rt::xmem::layout_plan().enabled(), "layout plan: the runtime's plan is off with no switch set");

    const auto log = rt::xmem::layout_plan_from("log", nullptr);
    check(log.mode == XmemLayoutMode::log && log.enabled() && !log.targeted(),
          "layout plan: log records answers and needs no caller");
    check(rt::xmem::layout_plan_from("log:0x10000", nullptr).mode == XmemLayoutMode::log,
          "layout plan: log ignores a trailing argument");

    const auto prefix = rt::xmem::layout_plan_from("prefix:0x10000", "1e1d28d");
    check(prefix.mode == XmemLayoutMode::prefix && prefix.argument == 0x10000 && prefix.caller_rva == 0x1e1d28d,
          "layout plan: prefix takes its bytes and the measured caller");
    check(prefix.targeted() && prefix.targets(0x1e1d28d) && !prefix.targets(0x1e1d28e),
          "layout plan: a targeted plan matches exactly its caller");
    const auto align = rt::xmem::layout_plan_from("align:0x200000", "1e1d28d");
    check(align.mode == XmemLayoutMode::align && align.argument == 0x200000, "layout plan: align takes its alignment");
    const auto guard = rt::xmem::layout_plan_from("guard:0x10000", "1e1d28d");
    check(guard.mode == XmemLayoutMode::guard && guard.argument == 0x10000, "layout plan: guard takes its bytes");

    // Every malformed or unplaceable form falls back to the default path rather than guessing.
    check(rt::xmem::layout_plan_from("prefix", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: prefix without bytes is the default path");
    check(rt::xmem::layout_plan_from("prefix:0x10000", nullptr).mode == XmemLayoutMode::none,
          "layout plan: a placement arm without a caller is the default path");
    check(rt::xmem::layout_plan_from("prefix:0x10000", "zzz").mode == XmemLayoutMode::none,
          "layout plan: a malformed caller is the default path");
    check(rt::xmem::layout_plan_from("prefix:0x810", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: a prefix not on a page boundary is the default path");
    check(rt::xmem::layout_plan_from("align:0x1234", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: an alignment that is not a power of two is the default path");
    check(rt::xmem::layout_plan_from("guard:0", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: a zero-length guard is the default path");
    check(rt::xmem::layout_plan_from("prefix:0x80000000", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: an over-limit argument is the default path");
    check(rt::xmem::layout_plan_from("bogus:0x10000", "1e1d28d").mode == XmemLayoutMode::none,
          "layout plan: an unknown mode is the default path");
    check(rt::xmem::layout_plan_from("prefix:0x1000", "1e1d28d").mode == XmemLayoutMode::prefix,
          "layout plan: one page is a usable prefix");
}

// docs/peer_port_movie.md, the implemented rank-4 port: the pure half of the per-submission refresh of a title-written
// texture. The switch text, the bounded compare budget (round-robin and never starving an oversized texture), the
// changed byte range and the row band it names, the step the walk takes against the guard-page tracker's verdict, and the
// report's shape. No device and no title pages: the impure half is exercised on the GPU in d3d12_vulkan_device_test.
void test_plane_refresh_plan() {
    using namespace palmetto;
    const auto off = plane_refresh_plan_from(nullptr, nullptr);
    check(off.mode == PlaneRefreshMode::off && !off.enabled() && !off.arms_tracker(),
          "plane refresh: unset switches leave the one-shot import (off)");
    check(!plane_refresh_plan_from("", "").enabled(), "plane refresh: an empty switch is off");
    check(!plane_refresh_plan_from("0", nullptr).enabled(), "plane refresh: a zero switch is off");
    check(!plane_refresh_plan_from("off", nullptr).enabled(), "plane refresh: an explicit off is off");
    check(!plane_refresh_plan_from("on", nullptr).enabled(), "plane refresh: an unknown word is off, never a guess");
    const auto hash = plane_refresh_plan_from("hash", nullptr);
    check(hash.mode == PlaneRefreshMode::hash && hash.enabled() && !hash.arms_tracker(),
          "plane refresh: hash mode compares and needs no tracker");
    check(plane_refresh_plan_from("HASH", nullptr).mode == PlaneRefreshMode::hash,
          "plane refresh: the switch words are case-insensitive");
    check(plane_refresh_plan_from("1", nullptr).mode == PlaneRefreshMode::hash,
          "plane refresh: a leading digit selects the default (hash) implementation");
    const auto watch = plane_refresh_plan_from("watch", "1");
    check(watch.mode == PlaneRefreshMode::watch && watch.arms_tracker() && watch.trace,
          "plane refresh: watch mode arms the tracker and the trace switch is read");

    // The budget: textures are admitted whole while the remaining budget covers them; an oversized texture is admitted
    // alone when nothing has been admitted yet, and nothing else fits behind it.
    {
        RefreshBudget budget(100);
        check(budget.take(60) && budget.remaining() == 40, "refresh budget: a texture that fits is admitted whole");
        check(!budget.take(60), "refresh budget: a texture that does not fit behind an admitted one is deferred");
        check(budget.take(40), "refresh budget: the rest of the budget admits the next texture");
    }
    {
        RefreshBudget budget(100);
        check(budget.take(4096) && budget.oversized() && budget.remaining() == 0,
              "refresh budget: an oversized texture is admitted alone (never starved)");
        check(!budget.take(1), "refresh budget: after an oversized admission nothing else is admitted this submission");
    }
    {
        RefreshBudget budget(0);
        check(!budget.take(1), "refresh budget: a zero budget admits nothing (the round-robin moves on)");
    }

    // The changed byte range: three cases a movie rewrite shows (nothing, one row, every byte).
    std::uint8_t before[64] = {};
    std::uint8_t after[64] = {};
    std::uint64_t first = 99, last = 99;
    check(!changed_byte_range(before, after, sizeof before, first, last),
          "changed range: identical spans report no change");
    std::memcpy(after, before, sizeof before);
    after[17] = 0x5a;
    check(changed_byte_range(before, after, sizeof before, first, last) && first == 17 && last == 18,
          "changed range: one byte in the middle reports exactly that byte");
    after[63] = 0x11;
    check(changed_byte_range(before, after, sizeof before, first, last) && first == 17 && last == 64,
          "changed range: a second byte extends the range to the last byte");
    std::memset(after, 0xff, sizeof after);
    check(changed_byte_range(before, after, sizeof before, first, last) && first == 0 && last == 64,
          "changed range: a full rewrite reports the whole span");
    check(!changed_byte_range(nullptr, after, 4, first, last), "changed range: a null span is not a change");

    // The row band a changed byte range names: byte offsets map to copy rows, and anything not narrowable is refused.
    {
        std::uint32_t top = 99, bottom = 99;
        check(changed_rows(16, 8, 32, 48, top, bottom) && top == 2 && bottom == 3,
              "changed rows: bytes 32..48 are exactly row 2 of a 16-byte-row subresource");
        check(changed_rows(16, 8, 30, 34, top, bottom) && top == 1 && bottom == 3,
              "changed rows: a range across two rows covers both");
        check(changed_rows(16, 8, 0, 1, top, bottom) && top == 0 && bottom == 1, "changed rows: the first row");
        check(changed_rows(16, 8, 127, 128, top, bottom) && top == 7 && bottom == 8, "changed rows: the last row");
        check(changed_rows(16, 8, 3, 40, top, bottom) && top == 0 && bottom == 3,
              "changed rows: a range across rows covers every row it touches");
        check(changed_rows(16, 8, 0, 128, top, bottom) && top == 0 && bottom == 8,
              "changed rows: a whole-subresource change names every row (the caller re-uploads whole)");
        check(!changed_rows(0, 8, 0, 1, top, bottom), "changed rows: no row size is not narrowable");
        check(!changed_rows(16, 0, 0, 1, top, bottom), "changed rows: no rows is not narrowable");
        check(!changed_rows(16, 8, 5, 5, top, bottom), "changed rows: an empty range is not a change");
        check(!changed_rows(16, 8, 0, 129, top, bottom), "changed rows: an offset past the subresource is refused");
    }

    // The step the walk takes: the tracker's "unchanged" is the only skip; "changed" and "unavailable" both compare, and
    // an unadmitted texture is deferred (the round-robin start moves so it is first next time).
    check(texture_refresh_step(PlaneRefreshMode::hash, rt::xmem::WriteWatchPoll::unchanged, true) ==
              TextureRefreshStep::compare,
          "refresh step: hash mode compares even when no range is armed (the tracker is not asked)");
    check(texture_refresh_step(PlaneRefreshMode::watch, rt::xmem::WriteWatchPoll::unchanged, true) ==
              TextureRefreshStep::skip_tracker_unchanged,
          "refresh step: watch mode skips the compare when the tracker says unchanged");
    check(texture_refresh_step(PlaneRefreshMode::watch, rt::xmem::WriteWatchPoll::changed, true) ==
              TextureRefreshStep::compare,
          "refresh step: a tracker write falls through to the compare (which narrows the upload)");
    check(texture_refresh_step(PlaneRefreshMode::watch, rt::xmem::WriteWatchPoll::unavailable, true) ==
              TextureRefreshStep::compare,
          "refresh step: an unavailable tracker falls back to the hash compare");
    check(texture_refresh_step(PlaneRefreshMode::hash, rt::xmem::WriteWatchPoll::changed, false) ==
              TextureRefreshStep::defer_budget,
          "refresh step: a texture the budget did not admit is deferred");

    // The report names every counter once (a boot's milestone line).
    PlaneRefreshCounters counters;
    counters.tracked = 40;
    counters.watched = 3;
    counters.compared = 12;
    counters.compare_bytes = 1 << 20;
    counters.changed = 5;
    counters.unchanged = 7;
    counters.uploaded = 5;
    counters.upload_bytes = 1 << 19;
    counters.narrow_uploads = 4;
    counters.full_uploads = 1;
    counters.tracker_skips = 28;
    counters.deferred = 1;
    counters.unreadable = 2;
    char report[512] = {};
    write_plane_refresh_report(counters, report, sizeof report);
    const std::string text(report);
    check(text.find("plane refresh:") == 0 && text.find("40 tracked") != std::string::npos &&
              text.find("3 watched") != std::string::npos && text.find("narrow 4") != std::string::npos &&
              text.find("deferred 1") != std::string::npos,
          "refresh report: one line names the tracked, compared, changed, uploaded and fallback counts");
    write_plane_refresh_report(counters, nullptr, 0);  // no destination: a no-op, never a crash
}

}  // namespace

int main() {
    std::printf("xmem_test: registered xmem.dll shim ABI\n");

    const auto alloc = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAlloc"));
    const auto free_mem = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFree"));
    const auto query = reinterpret_cast<XMemVirtualQueryFn>(rt::shims::find("xmem.dll", "XMemVirtualQuery"));
    const auto get_stats =
        reinterpret_cast<XMemGetWorkingSetStatisticsFn>(rt::shims::find("xmem.dll", "XMemGetWorkingSetStatistics"));
    const auto virtual_alloc =
        reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto get_write_watch =
        reinterpret_cast<XMemGetWriteWatchFn>(rt::shims::find("xmem.dll", "XMemGetWriteWatch"));
    const auto reset_write_watch =
        reinterpret_cast<XMemResetWriteWatchFn>(rt::shims::find("xmem.dll", "XMemResetWriteWatch"));
    const auto transfer =
        reinterpret_cast<XMemTransferMemoryFn>(rt::shims::find("xmem.dll", "XMemTransferMemory"));

    check(alloc != nullptr, "registry exposes xmem.dll!XMemAlloc");
    check(free_mem != nullptr, "registry exposes xmem.dll!XMemFree");
    check(query != nullptr, "registry exposes xmem.dll!XMemVirtualQuery");
    check(get_stats != nullptr, "registry exposes xmem.dll!XMemGetWorkingSetStatistics");
    check(virtual_alloc != nullptr, "registry exposes xmem.dll!XMemVirtualAlloc");
    check(get_write_watch != nullptr, "registry exposes xmem.dll!XMemGetWriteWatch");
    check(reset_write_watch != nullptr, "registry exposes xmem.dll!XMemResetWriteWatch");
    check(transfer != nullptr, "registry exposes xmem.dll!XMemTransferMemory");

    const auto alloc_default = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAllocDefault"));
    const auto free_default = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFreeDefault"));
    const auto tools_mem =
        reinterpret_cast<XMemIsToolsMemAvailableFn>(rt::shims::find("xmem.dll", "XMemIsToolsMemAvailable"));
    const PhysicalApi physical{
        virtual_alloc,
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages")),
        reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesScatterFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPagesScatter")),
    };
    check(alloc_default != nullptr, "registry exposes xmem.dll!XMemAllocDefault");
    check(free_default != nullptr, "registry exposes xmem.dll!XMemFreeDefault");
    check(tools_mem != nullptr, "registry exposes xmem.dll!XMemIsToolsMemAvailable");
    check(physical.allocate != nullptr, "registry exposes xmem.dll!XMemAllocatePhysicalPages");
    check(physical.free_pages != nullptr, "registry exposes xmem.dll!XMemFreePhysicalPages");
    check(physical.map != nullptr, "registry exposes xmem.dll!XMemMapPhysicalPages");
    check(physical.scatter != nullptr, "registry exposes xmem.dll!XMemMapPhysicalPagesScatter");

    if (!alloc || !free_mem || !query || !get_stats) {
        print_result();
        return 1;
    }

    test_alloc(alloc, free_mem);
    test_working_set(get_stats);
    test_virtual_query(query);
    if (virtual_alloc && get_write_watch && reset_write_watch) {
        test_virtual_alloc(virtual_alloc, get_write_watch, reset_write_watch);
    }
    test_attribute_trace(alloc, free_mem, virtual_alloc);
    test_caller_trace(alloc, free_mem);
    if (transfer) {
        test_transfer_memory(transfer);
        test_transfer_trace(transfer);
    }

    test_title_virtual_alloc();
    test_page_write_watch();
    test_layout_plan();
    test_plane_refresh_plan();  // docs/peer_port_movie.md: the pure per-submission refresh rules

    if (alloc_default && free_default) test_default_alloc(alloc, free_mem, alloc_default, free_default);
    if (tools_mem) check(tools_mem() == FALSE, "XMemIsToolsMemAvailable returns FALSE (no devkit tools memory)");
    if (virtual_alloc && physical.allocate && physical.free_pages && physical.map && physical.scatter) {
        test_physical_pages(physical);
        test_physical_pages_commit_on_touch(physical);
        test_commit_physical_pages(physical);
        test_copy_page_mappings(physical);
        test_resolve_physical_page(physical);
        test_first_touch_during_remap(physical);
        test_placeholders(physical);
    }

    print_result();
    return g_failures ? 1 : 0;
}
