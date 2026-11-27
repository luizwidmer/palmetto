// Advanced DirectStorage request mechanisms for the console shim in dstorage.cpp, header-only so the same code is
// compiled into `palmetto` and into the GPU-free `dstorage_test`.
//
// The console DirectStorage API wants four things of a request that the zlib-only shim does not do: a bounded staging
// area for the transforms, a destination that is pinned while the worker runs (the console pins the write-watched
// destination pages), 64 KiB physical page lists instead of a flat buffer, and the BCPack/unshuffle decompression
// stages with the locally calibrated predictor tables. The reference's mechanisms are summarised in
// docs/directstorage_advanced.md; this header is our re-derivation, with the parts that our runtime can run correctly
// behind opt-in switches and the parts that it cannot run refused by a named limitation.
//
// Everything here is either pure arithmetic over caller-provided memory (staging, page plan/copy, unshuffle, predictor
// validation) or a bounded Win32 range operation (pin). No file data, package path or game content is read or stored.
#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace palmetto::dstorage {

// ---- Environment switches -----------------------------------------------------------------------------------------
//
// Read once per process. The default for every one is "off" so a boot without them is byte-identical: the same lines,
// the same allocations and the same HRESULTs as before the slice. The values are documented in
// docs/directstorage_advanced.md and named there for the docs environment-variable check.

// A switch text as it is set: on unless it is unset/empty, "0", "off"/"OFF"/"Off" or "no"/"NO"/"No". The project's
// spelling convention (docs/peer_port_streaming.md section 12.2): a typo must never enable a path the measurement did
// not choose, so only these spellings are off. Pure, so the GPU-free test can check every spelling of a switch.
inline bool switch_text_on(const char* value) {
    if (!value || !*value) return false;
    return std::strcmp(value, "0") != 0 && _stricmp(value, "off") != 0 && _stricmp(value, "no") != 0;
}

// A switch is on for any value except the empty string, "0", "off"/"OFF" and "no"/"NO".
inline bool env_switch(const char* name) {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
    if (n == 0 || n >= sizeof value) return false;
    return switch_text_on(value);
}

// A byte count: decimal with an optional K/M/G suffix (64K, 1M), 0 when unset or unparsable.
inline std::size_t env_size(const char* name) {
    char value[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
    if (n == 0 || n >= sizeof value) return 0;
    char* end = nullptr;
    const unsigned long long number = _strtoui64(value, &end, 10);
    if (end == value) return 0;
    unsigned long long scale = 1;
    if (*end == 'k' || *end == 'K') {
        scale = 1024;
        ++end;
    } else if (*end == 'm' || *end == 'M') {
        scale = 1024 * 1024;
        ++end;
    } else if (*end == 'g' || *end == 'G') {
        scale = 1024ull * 1024 * 1024;
        ++end;
    }
    if (*end != '\0') return 0;
    const unsigned long long bytes = number * scale;
    if (number != 0 && bytes / number != scale) return 0;  // overflow
    return static_cast<std::size_t>(bytes);
}

// ---- Loading-record trace spec (measurement only; docs/decomp_stage.md) -------------------------------------------
//
// The session-3 loading/streaming record trace in dstorage.cpp samples the title's record at image+0x78AC9C8 and the
// three state words. Its switch is PALMETTO_DS_RECORD: unset, empty, "0" or unparsable is off; "1" is the documented
// default (500 ms samples for 300 s); otherwise the text is <interval_ms>[,<seconds>], the interval clamped to 50..10000
// ms and the duration to at most 600 s. Pure, so the test checks the grammar without a boot.
struct RecordTraceSpec {
    bool on = false;
    unsigned interval_ms = 500;
    double seconds = 300.0;
};

inline RecordTraceSpec parse_record_trace_spec(const char* text) {
    RecordTraceSpec spec;
    if (!text || *text == '\0' || std::strcmp(text, "0") == 0) return spec;
    if (std::strcmp(text, "1") == 0) {
        spec.on = true;
        return spec;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (end == text) return spec;
    spec.interval_ms = parsed >= 50 ? static_cast<unsigned>(std::min<unsigned long>(parsed, 10000ul)) : 50u;
    if (*end == ',') {
        const double seconds = std::strtod(end + 1, nullptr);
        if (seconds > 0.0) spec.seconds = std::min(seconds, 600.0);
    }
    spec.on = true;
    return spec;
}

// ---- Bounded staging ----------------------------------------------------------------------------------------------
//
// One reusable buffer per transform stage, with a hard cap: a request above the cap falls back to an owned allocation
// and is counted, so a huge or malformed request cannot grow the staging area. The console reference keeps a staging
// pool across queues with a fixed limit; here the pool is per queue worker (one thread, so no lock) and the cap is
// whatever PALMETTO_DS_STAGING names.

struct StagingStats {
    std::uint64_t borrows = 0;         // calls to borrow(), including a zero-size request
    std::uint64_t hits = 0;            // served from the reusable buffer
    std::uint64_t fallbacks = 0;       // over the cap or the allocation failed: the caller allocates
    std::uint64_t fallback_bytes = 0;  // bytes that bypassed staging
    std::uint64_t bytes = 0;           // bytes served from staging
    std::size_t peak = 0;              // largest single staging request
};

class BoundedScratch {
public:
    explicit BoundedScratch(std::size_t capacity = 0) : capacity_(capacity) {}

    bool enabled() const { return capacity_ != 0; }
    std::size_t capacity() const { return capacity_; }
    const StagingStats& stats() const { return stats_; }

    // A writable region of exactly `bytes`, reusing the previous allocation. Empty when the request is larger than the
    // cap or the allocation fails; empty for 0 bytes as well, which is not counted as a fallback.
    std::span<std::uint8_t> borrow(std::size_t bytes) {
        ++stats_.borrows;
        if (bytes == 0) return {};
        if (bytes > capacity_) {
            ++stats_.fallbacks;
            stats_.fallback_bytes += bytes;
            return {};
        }
        try {
            storage_.resize(bytes);
        } catch (...) {
            ++stats_.fallbacks;
            stats_.fallback_bytes += bytes;
            return {};
        }
        ++stats_.hits;
        stats_.bytes += bytes;
        stats_.peak = (std::max)(stats_.peak, bytes);
        return std::span<std::uint8_t>(storage_.data(), bytes);
    }

    // The reusable vector itself, for a stage that must append into a std::vector (the runtime's inflate). Reusing it
    // keeps its allocation across requests; the caller checks the cap before handing it over.
    std::vector<std::uint8_t>& vector() { return storage_; }

private:
    std::size_t capacity_ = 0;
    std::vector<std::uint8_t> storage_;
    StagingStats stats_{};
};

// ---- Pinned destination ranges ------------------------------------------------------------------------------------
//
// On the console a DirectStorage read pins the destination pages while it writes: the data path takes the pages away
// from the CPU write-watch and hands them back with the whole block published. Our PC port has no kernel write and no
// physical page table, but the same consistency question exists in one measured place: the diagnostics in
// dstorage.cpp put PAGE_GUARD on a streamed destination page after it has been written, so the title's first touch
// faults and names the consumer. A later request that writes the same destination currently takes that guard fault in
// the queue worker (the guard handler clears it and the write retries), i.e. the write is split and the consumer's
// watch is consumed by our own store. Pinning releases the guards on the range before the write and re-arms them after
// it, so the worker writes the block in one go and the title's next access is still the first touch.
//
// Residency is a second, independent property: VirtualLock keeps the destination pages in the working set for the
// duration of the write so a CPU consumer that polls the block after the status completes cannot take a page fault on
// it. It is a separate switch (PALMETTO_DS_PIN_RESIDENT) because it costs two system calls per request and the
// measurement should show that.

struct PinStats {
    std::uint64_t calls = 0;
    std::uint64_t pinned = 0;          // calls that pinned something (released a guard or locked the range)
    std::uint64_t pages = 0;           // 4 KiB pages in the pinned ranges
    std::uint64_t write_watched = 0;   // calls that found and released a PAGE_GUARD on the range
    std::uint64_t guards_released = 0; // contiguous guarded regions released
    std::uint64_t guards_rearmed = 0;  // regions re-armed by unpin()
    std::uint64_t lock_failures = 0;   // VirtualLock failed (working-set quota): the write still runs
    std::uint64_t refusals = 0;        // more guarded regions than the fixed table, or a query/protect failed
};

class DestinationPinner {
public:
    static constexpr unsigned kMaxRanges = 8;

    void set_residency(bool on) { residency_ = on; }
    const PinStats& stats() const { return stats_; }

    // Releases PAGE_GUARD on every committed, accessible page of [address, address + bytes) and remembers the original
    // protection; with residency on, also VirtualLock()s the range. False when nothing was pinned.
    bool pin(void* address, std::size_t bytes) {
        unpin();  // a pinner is reused across requests; start from nothing held
        ++stats_.calls;
        if (!address || bytes == 0) return false;
        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
        const std::uintptr_t end = start + bytes;
        if (end < start) {
            ++stats_.refusals;
            return false;
        }
        address_ = reinterpret_cast<std::uintptr_t>(address);
        bytes_ = bytes;
        bool released = false;
        for (std::uintptr_t at = start; at < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info)) {
                unpin();
                ++stats_.refusals;
                return false;
            }
            const std::uintptr_t region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            const std::uintptr_t region_end = region + info.RegionSize;
            const std::uintptr_t span_end = (std::min)(region_end, end);
            if (span_end <= at) {  // a zero-sized query answer would spin
                unpin();
                ++stats_.refusals;
                return false;
            }
            const bool guarded = info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) != 0 &&
                                 (info.Protect & PAGE_NOACCESS) == 0;
            if (guarded) {
                if (count_ == kMaxRanges) {  // fixed table full: give back what this call already released
                    unpin();
                    ++stats_.refusals;
                    return false;
                }
                DWORD old = 0;
                if (!VirtualProtect(reinterpret_cast<void*>(at), span_end - at, info.Protect & ~PAGE_GUARD, &old)) {
                    unpin();
                    ++stats_.refusals;
                    return false;
                }
                ranges_[count_].base = at;
                ranges_[count_].bytes = span_end - at;
                ranges_[count_].protection = info.Protect;
                ++count_;
                ++stats_.guards_released;
                released = true;
            }
            at = span_end;
        }
        if (released) ++stats_.write_watched;
        bool locked = false;
        if (residency_) {
            if (VirtualLock(address, bytes)) {
                locked = true;
            } else {
                ++stats_.lock_failures;
            }
        }
        locked_ = locked;
        pinned_ = released || locked;
        if (pinned_) {
            ++stats_.pinned;
            stats_.pages += (bytes + 4095) / 4096;
        }
        return pinned_;
    }

    // Re-arms the guards pin() released (with the protection each region had) and unlocks the range.
    void unpin() {
        if (locked_ && address_ && bytes_) VirtualUnlock(reinterpret_cast<void*>(address_), bytes_);
        locked_ = false;
        for (unsigned i = 0; i < count_; ++i) {
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void*>(ranges_[i].base), ranges_[i].bytes, ranges_[i].protection, &old))
                ++stats_.guards_rearmed;
        }
        count_ = 0;
        pinned_ = false;
        address_ = 0;
        bytes_ = 0;
    }

private:
    struct Range {
        std::uintptr_t base = 0;
        std::size_t bytes = 0;
        DWORD protection = 0;  // the protection the page had, PAGE_GUARD included
    };
    std::array<Range, kMaxRanges> ranges_{};
    unsigned count_ = 0;
    std::uintptr_t address_ = 0;
    std::size_t bytes_ = 0;
    bool residency_ = false;
    bool locked_ = false;
    bool pinned_ = false;
    PinStats stats_{};
};

// ---- Physical page lists (scatter/gather) -------------------------------------------------------------------------
//
// A physical-page request names its buffer as a list of 64 KiB page numbers plus a byte offset into the first page.
// The numbers are 4 KiB frame numbers of 64 KiB pages (xmem.cpp owns that convention; a number is slot * 16 and 0
// means an unmapped entry). Planning is pure arithmetic and is what this header provides; resolving a page number to a
// CPU address belongs to xmem, which does not expose its page table, so the dstorage shim refuses a live page-list
// request by a named limitation (docs/directstorage_advanced.md). The plan/copy code is still exercised by the test and
// is the seam a later xmem export would plug into.

inline constexpr std::uint64_t kPageListPageBytes = 64u * 1024u;

struct PageSpan {
    std::uint64_t page = 0;   // the page number to resolve
    std::uint32_t offset = 0; // bytes into that page
    std::uint32_t bytes = 0;  // bytes taken from that page
};

enum class PagePlanError {
    ok,
    empty,              // nothing to move
    zero_page,          // a 0 page number (an unmapped entry) where bytes are needed
    too_many_pages,     // the transfer needs more spans than the fixed table holds
    too_short,          // the list is shorter than the byte count needs
    offset_out_of_range // the offset is not inside the first 64 KiB page
};

struct PagePlan {
    static constexpr unsigned kMaxSpans = 32;
    std::array<PageSpan, kMaxSpans> spans{};
    unsigned count = 0;
    std::size_t bytes = 0;
};

inline const char* page_plan_error_name(PagePlanError error) {
    switch (error) {
        case PagePlanError::ok: return "ok";
        case PagePlanError::empty: return "empty";
        case PagePlanError::zero_page: return "zero_page";
        case PagePlanError::too_many_pages: return "too_many_pages";
        case PagePlanError::too_short: return "too_short";
        case PagePlanError::offset_out_of_range: return "offset_out_of_range";
    }
    return "?";
}

// [pages[0] + offset, ...] as spans over the 64 KiB page list. `page_count` is how many numbers the caller read;
// `bytes` how many bytes must be copied. Only the pages the transfer needs are validated; a longer list is accepted
// (the console reads only the page numbers it needs, and the reference allows the caller to keep the list live until
// the request completes).
inline PagePlanError plan_page_transfer(const std::uint64_t* pages, std::size_t page_count, std::uint16_t page_offset,
                                        std::size_t bytes, PagePlan& out) {
    out = PagePlan{};
    if (bytes == 0) return PagePlanError::empty;
    if (pages == nullptr || page_count == 0) return PagePlanError::too_short;
    if (page_offset >= kPageListPageBytes) return PagePlanError::offset_out_of_range;
    std::size_t remaining = bytes;
    std::uint64_t position = page_offset;
    unsigned used = 0;
    for (std::size_t i = 0; remaining; ++i) {
        if (i >= page_count) return PagePlanError::too_short;
        if (pages[i] == 0) return PagePlanError::zero_page;
        if (used == PagePlan::kMaxSpans) return PagePlanError::too_many_pages;
        const std::uint64_t room = kPageListPageBytes - position;
        const std::size_t take = static_cast<std::size_t>((std::min)(room, static_cast<std::uint64_t>(remaining)));
        out.spans[used].page = pages[i];
        out.spans[used].offset = static_cast<std::uint32_t>(position);
        out.spans[used].bytes = static_cast<std::uint32_t>(take);
        ++used;
        remaining -= take;
        position = 0;
    }
    out.count = used;
    out.bytes = bytes;
    return PagePlanError::ok;
}

// Resolves a page number to the CPU address of its 64 KiB page (the xmem seam). Null refuses the transfer at that
// span: the caller must build a page-list destination in a scratch area, never directly, because a partial copy into
// title memory would otherwise be visible.
using PageResolver = const std::uint8_t* (*)(void* context, std::uint64_t page_number);

// Copies the plan out of the page list into `destination`, in order. False on a null resolution; the caller writes the
// destination only after the whole plan has been copied into its own buffer.
inline bool copy_from_pages(const PagePlan& plan, PageResolver resolve, void* context, std::uint8_t* destination) {
    if (plan.count == 0 || resolve == nullptr || destination == nullptr) return false;
    std::size_t at = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        const std::uint8_t* page = resolve(context, plan.spans[i].page);
        if (page == nullptr) return false;
        std::memcpy(destination + at, page + plan.spans[i].offset, plan.spans[i].bytes);
        at += plan.spans[i].bytes;
    }
    return at == plan.bytes;
}

// Copies `source` into the page list, in order. False on a null resolution.
inline bool copy_to_pages(const PagePlan& plan, PageResolver resolve, void* context, const std::uint8_t* source) {
    if (plan.count == 0 || resolve == nullptr || source == nullptr) return false;
    std::size_t at = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        std::uint8_t* page = const_cast<std::uint8_t*>(resolve(context, plan.spans[i].page));
        if (page == nullptr) return false;
        std::memcpy(page + plan.spans[i].offset, source + at, plan.spans[i].bytes);
        at += plan.spans[i].bytes;
    }
    return at == plan.bytes;
}

// ---- A live page-list transfer through a resolver ---------------------------------------------------------------
//
// The request path resolves the whole plan before it moves a byte: every page number is looked up first, so a number
// with no live backing refuses the transfer with nothing written into title memory. A page-list destination is real
// title memory (unlike the caller's scratch buffer), so a half-applied scatter would be visible to the title; the
// ordering rule is what makes a failed request leave the pages as they were. `buffer` is the caller's scratch: the
// request path materialises the source/decoded bytes there (or gathers a source page list into it), so this step is
// only the scatter/gather. `write` says which way the bytes go.
enum class PageTransferError {
    ok,
    no_resolver,     // no page resolver installed (a build without xmem)
    null_buffer,     // no scratch buffer to move through
    unresolved_page, // a page number the plan needs has no live physical page
    empty,           // nothing to move
    zero_page,       // the plan's own refusals, same names as PagePlanError
    too_many_pages,
    too_short,
    offset_out_of_range,
};

inline const char* page_transfer_error_name(PageTransferError error) {
    switch (error) {
        case PageTransferError::ok: return "ok";
        case PageTransferError::no_resolver: return "no_resolver";
        case PageTransferError::null_buffer: return "null_buffer";
        case PageTransferError::unresolved_page: return "unresolved_page";
        case PageTransferError::empty: return "empty";
        case PageTransferError::zero_page: return "zero_page";
        case PageTransferError::too_many_pages: return "too_many_pages";
        case PageTransferError::too_short: return "too_short";
        case PageTransferError::offset_out_of_range: return "offset_out_of_range";
    }
    return "?";
}

// What one transfer did, and which page number stopped it (the refusal is by name and by number, never by bytes).
struct PageTransferResult {
    PageTransferError error = PageTransferError::ok;
    std::uint64_t page = 0;  // the unresolved page number, when error == unresolved_page
    unsigned spans = 0;      // the plan's span count
    std::size_t bytes = 0;   // the bytes the plan moves
};

inline PageTransferError plan_transfer_error(PagePlanError error) {
    switch (error) {
        case PagePlanError::ok: return PageTransferError::ok;
        case PagePlanError::empty: return PageTransferError::empty;
        case PagePlanError::zero_page: return PageTransferError::zero_page;
        case PagePlanError::too_many_pages: return PageTransferError::too_many_pages;
        case PagePlanError::too_short: return PageTransferError::too_short;
        case PagePlanError::offset_out_of_range: return PageTransferError::offset_out_of_range;
    }
    return PageTransferError::too_short;
}

// Moves `bytes` between the caller's `buffer` and the page list: the request's page-array snapshot (`pages`,
// `page_count` entries) and the byte offset into its first page. Read direction fills `buffer`, write direction
// scatters it. Every page the plan names is resolved before the first copy, so a resolver miss returns
// `unresolved_page` with the page number and leaves the pages and the buffer untouched.
inline PageTransferResult transfer_page_list(const std::uint64_t* pages, std::size_t page_count,
                                             std::uint16_t page_offset, std::size_t bytes, PageResolver resolve,
                                             void* context, std::uint8_t* buffer, bool write) {
    PageTransferResult result;
    if (bytes == 0) {
        result.error = PageTransferError::empty;
        return result;
    }
    if (resolve == nullptr) {
        result.error = PageTransferError::no_resolver;
        return result;
    }
    if (buffer == nullptr) {
        result.error = PageTransferError::null_buffer;
        return result;
    }
    PagePlan plan;
    result.error = plan_transfer_error(plan_page_transfer(pages, page_count, page_offset, bytes, plan));
    result.spans = plan.count;
    result.bytes = plan.bytes;
    if (result.error != PageTransferError::ok) return result;
    const std::uint8_t* views[PagePlan::kMaxSpans] = {};
    for (unsigned i = 0; i < plan.count; ++i) {
        views[i] = resolve(context, plan.spans[i].page);
        if (views[i] == nullptr) {
            result.error = PageTransferError::unresolved_page;
            result.page = plan.spans[i].page;
            return result;
        }
    }
    std::size_t at = 0;
    for (unsigned i = 0; i < plan.count; ++i) {
        // The resolver hands back the page's backing, which the write direction writes; the const in the shared
        // resolver type is what copy_from_pages needs for its read path.
        if (write) {
            std::memcpy(const_cast<std::uint8_t*>(views[i]) + plan.spans[i].offset, buffer + at, plan.spans[i].bytes);
        } else {
            std::memcpy(buffer + at, views[i] + plan.spans[i].offset, plan.spans[i].bytes);
        }
        at += plan.spans[i].bytes;
    }
    return result;
}

// ---- Macro unshuffle (DirectStorage swizzle modes 1-4) ------------------------------------------------------------
//
// A swizzle request writes BCn data that the console stored in a page-striped layout; the runtime has to put each
// stripe back into its place in every 16 KiB or 32 KiB group. The stripe table below is the layout the upstream port
// documents as coming from the public Xbox-GDK-Samples `DirectStorageTextureShuffling` example
// (the upstream checkout's runtime/src/dstorage_swizzle.h); the NDA "DirectStorage Shuffle Formats" page is not
// public, so this is re-derived from that reference and cross-checked against it over synthetic buffers rather than
// measured from our package (docs/directstorage_advanced.md owns the provenance and the credit).
//
// Each entry is the number of consecutive 4 KiB pages one stripe takes in a group. A group is 16 KiB for mode 1 and
// 32 KiB for modes 2-4; the source is the stripes stored one after another over all groups, and the destination is the
// groups with each stripe's pages in their in-group position.

struct SwizzleLayout {
    static constexpr unsigned kMaxStripes = 5;
    std::array<unsigned, kMaxStripes> pages{};  // 4 KiB pages per stripe
    unsigned stripes = 0;
    std::size_t group_bytes = 0;
};

inline bool swizzle_layout(unsigned mode, SwizzleLayout& layout) {
    static constexpr unsigned kPages[5][SwizzleLayout::kMaxStripes] = {
        {}, {1, 1, 2}, {1, 1, 6}, {1, 3, 1, 1, 2}, {1, 3, 1, 3}};
    static constexpr unsigned kStripes[5] = {0, 3, 3, 5, 4};
    layout = SwizzleLayout{};
    if (mode == 0 || mode > 4) return false;
    layout.stripes = kStripes[mode];
    unsigned total_pages = 0;
    for (unsigned i = 0; i < layout.stripes; ++i) {
        layout.pages[i] = kPages[mode][i];
        total_pages += layout.pages[i];
    }
    layout.group_bytes = static_cast<std::size_t>(total_pages) * 4096u;
    return total_pages != 0;
}

// mode 1: 1+1+2 pages = 16 KiB; modes 2, 3, 4: 1+1+6, 1+3+1+1+2 and 1+3+1+3 pages = 32 KiB.
inline bool unshuffle(unsigned mode, const std::uint8_t* source, std::size_t bytes, std::uint8_t* destination,
                      std::size_t capacity) {
    SwizzleLayout layout;
    if (!swizzle_layout(mode, layout)) return false;
    if (source == nullptr || destination == nullptr) return false;
    if (bytes == 0 || bytes % layout.group_bytes != 0 || capacity < bytes) return false;
    const std::size_t groups = bytes / layout.group_bytes;
    std::size_t read = 0;
    std::size_t offset = 0;
    for (unsigned stripe = 0; stripe < layout.stripes; ++stripe) {
        const std::size_t stripe_bytes = static_cast<std::size_t>(layout.pages[stripe]) * 4096u;
        for (std::size_t group = 0; group < groups; ++group) {
            std::memcpy(destination + group * layout.group_bytes + offset, source + read, stripe_bytes);
            read += stripe_bytes;
        }
        offset += stripe_bytes;
    }
    return read == bytes && offset == layout.group_bytes;
}

// ---- BCPack predictor file ----------------------------------------------------------------------------------------
//
// The BCPack decoder needs the 256-entry initial endpoint/index history tables for BC6H and BC7, and the console keeps
// them in a locally calibrated sidecar rather than in the runtime: they are recovered from the player's own package by
// tooling/bcpack_history_calib.py and must stay outside the repository (docs/directstorage_advanced.md, "Predictor
// file policy"). The loader below reads the tool's JSON (a directory with <codec>.json, a combined file with codec
// sections, or one codec file) with a bounded parser and validates the ranges the decoder relies on. It never keeps
// the values: the BCPack block decoder is not in this slice, so a request that needs it is refused with the predictor's
// state named (loaded, missing or invalid).

enum class PredictorError {
    ok,
    missing,           // no file at the path
    unreadable,        // present but not readable
    too_large,         // over the bounded file size
    malformed,         // not the JSON shape the tool writes
    no_codec_section,  // neither bc6h nor bc7 in it
    out_of_range,      // a value or an entry index outside the decoder's table
    bad_length,        // an entry has the wrong number of integers
};

inline const char* predictor_error_name(PredictorError error) {
    switch (error) {
        case PredictorError::ok: return "ok";
        case PredictorError::missing: return "missing";
        case PredictorError::unreadable: return "unreadable";
        case PredictorError::too_large: return "too_large";
        case PredictorError::malformed: return "malformed";
        case PredictorError::no_codec_section: return "no_codec_section";
        case PredictorError::out_of_range: return "out_of_range";
        case PredictorError::bad_length: return "bad_length";
    }
    return "?";
}

struct PredictorReport {
    bool bc6h = false;
    bool bc7 = false;
    unsigned bc6h_endpoint_rows = 0;
    unsigned bc6h_index_rows = 0;
    unsigned bc7_endpoint_rows = 0;
    unsigned bc7_index_rows = 0;
    // The BC1/BC3/BC4/BC5 sections are optional; a confirmed row count of 0 is a codec the file has no rows for.
    unsigned bc1_rows = 0;
    unsigned bc3_rows = 0;
    unsigned bc4_rows = 0;
    unsigned bc5_rows = 0;
    std::uint64_t file_bytes = 0;
    unsigned source_files = 0;  // a directory can supply more than one
};

// One codec's initial history, retained from the local predictor file for the block decoder.
//
// `class` is the calibration tool's confidence: 0 = unknown (no row), 1 = fitted/provisional, 3 = confirmed against
// an exact reference. The block decoder is strict: a block that references anything other than class 3 (and, for the
// 16-bit codecs, a row whose confirmed-bit depth does not cover every field the mode reads) is refused by name, never
// decoded from a guess. The BC1-BC5 endpoint/history values are per-channel integers in the codec's own bit depth;
// BC6H endpoints are 16-bit components and BC7 endpoints are stored bytes, exactly as `tooling/bcpack_history_calib.py`
// writes them. Nothing in this structure is ever written into the repository: it lives in the process, loaded from the
// env-named local file (docs/bcpack_runtime.md).
struct BcpackHistory {
    // BC1..BC5: 256 possible built-in entries (index 0 is unreferenced, 256 - d for d <= 255).
    std::array<std::array<std::uint8_t, 6>, 256> bc1{};
    std::array<std::array<std::uint8_t, 8>, 256> bc3{};
    std::array<std::array<std::uint8_t, 2>, 256> bc4{};
    std::array<std::array<std::uint8_t, 4>, 256> bc5{};
    std::array<std::uint8_t, 256> bc1_class{}, bc3_class{}, bc4_class{}, bc5_class{};
    // BC6H: 256 endpoint rows (3 channels x 4 components) and 256 index rows (16 pixels), plus confidence classes and
    // leading confirmed-bit depths (endpoint per channel 16/10/6/6; index 0..4 per pixel).
    std::array<std::array<std::array<std::uint16_t, 4>, 3>, 256> bc6h_endpoint{};
    std::array<std::array<std::uint8_t, 16>, 256> bc6h_index{};
    std::array<std::uint8_t, 256> bc6h_endpoint_class{}, bc6h_index_class{};
    std::array<std::array<std::array<std::uint8_t, 4>, 3>, 256> bc6h_endpoint_bits{};
    std::array<std::array<std::uint8_t, 16>, 256> bc6h_index_bits{};
    // BC7: 256 endpoint rows (24 stored bytes), 256 index rows (32 nibbles: 16 primary + 16 secondary) and the same
    // confidence/bit-depth split the calibration tool records.
    std::array<std::array<std::uint8_t, 24>, 256> bc7_endpoint{}, bc7_endpoint_bits{}, bc7_endpoint_confirmed{};
    std::array<std::array<std::uint8_t, 32>, 256> bc7_index{}, bc7_index_bits{}, bc7_index_confirmed{};
    bool bc1_present = false;
    bool bc3_present = false;
    bool bc4_present = false;
    bool bc5_present = false;
    bool bc6h_present = false;
    bool bc7_present = false;
};

namespace detail {

// The predictor JSON is integers only, so the parser is a bounded integer/array/object reader rather than a general
// JSON implementation. Caps keep a malformed file from turning into an allocation: 8 MiB read, depth 8, 200,000
// integers, 64 keys per object.
struct JsonValue {
    enum class Type { null_value, boolean, number, string, array, object } type = Type::null_value;
    long long number = 0;
    std::string text;               // string value (only the short codec keys are ever read out of it)
    std::vector<JsonValue> items;  // array items, or object values
    std::vector<std::string> keys; // object keys, in step with items
    const JsonValue* member(const char* name) const {
        if (type != Type::object) return nullptr;
        for (std::size_t i = 0; i < keys.size() && i < items.size(); ++i)
            if (keys[i] == name) return &items[i];
        return nullptr;
    }
};

class JsonReader {
public:
    static constexpr unsigned kMaxDepth = 8;
    static constexpr std::size_t kMaxItems = 200000;
    static constexpr std::size_t kMaxKeys = 64;

    JsonReader(const char* text, std::size_t size) : text_(text), size_(size) {}

    bool parse(JsonValue& out) {
        skip_space();
        if (!value(out, 0)) return false;
        skip_space();
        return at_ == size_;
    }

private:
    bool fail() { return false; }
    void skip_space() {
        while (at_ < size_) {
            const char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++at_;
        }
    }
    bool literal(const char* word) {
        const std::size_t length = std::strlen(word);
        if (at_ + length > size_ || std::memcmp(text_ + at_, word, length) != 0) return fail();
        at_ += length;
        return true;
    }
    bool string(std::string& out) {
        if (at_ >= size_ || text_[at_] != '"') return fail();
        ++at_;
        out.clear();
        while (at_ < size_) {
            const char c = text_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail();
            if (c == '\\') {
                if (at_ >= size_) return fail();
                const char escape = text_[at_++];
                switch (escape) {
                    case '"': case '\\': case '/': out.push_back(escape); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (at_ + 4 > size_) return fail();
                        at_ += 4;  // the keys we match are ASCII; a \u escape cannot produce one
                        out.push_back('?');
                        break;
                    default: return fail();
                }
            } else {
                out.push_back(c);
            }
            if (out.size() > 128) return fail();
        }
        return fail();
    }
    bool number(long long& out) {
        bool negative = false;
        if (at_ < size_ && text_[at_] == '-') {
            negative = true;
            ++at_;
        }
        if (at_ >= size_ || text_[at_] < '0' || text_[at_] > '9') return fail();
        long long value = 0;
        unsigned digits = 0;
        while (at_ < size_ && text_[at_] >= '0' && text_[at_] <= '9') {
            if (++digits > 10) return fail();
            value = value * 10 + (text_[at_] - '0');
            ++at_;
        }
        out = negative ? -value : value;
        return true;
    }
    bool value(JsonValue& out, unsigned depth) {
        if (depth > kMaxDepth) return fail();
        if (at_ >= size_) return fail();
        const char c = text_[at_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            if (!string(out.text)) return false;
            out.type = JsonValue::Type::string;
            return true;
        }
        if (c == 't') {
            if (!literal("true")) return false;
            out.type = JsonValue::Type::boolean;
            out.number = 1;
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            out.type = JsonValue::Type::boolean;
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            out.type = JsonValue::Type::null_value;
            return true;
        }
        out.type = JsonValue::Type::number;
        return number(out.number);
    }
    bool array(JsonValue& out, unsigned depth) {
        ++at_;  // '['
        out.type = JsonValue::Type::array;
        skip_space();
        if (at_ < size_ && text_[at_] == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            if (out.items.size() >= kMaxItems) return fail();
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            skip_space();
            if (at_ >= size_) return fail();
            if (text_[at_] == ',') {
                ++at_;
                skip_space();
                continue;
            }
            if (text_[at_] == ']') {
                ++at_;
                return true;
            }
            return fail();
        }
    }
    bool object(JsonValue& out, unsigned depth) {
        ++at_;  // '{'
        out.type = JsonValue::Type::object;
        skip_space();
        if (at_ < size_ && text_[at_] == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            if (out.keys.size() >= kMaxKeys) return fail();
            std::string key;
            if (!string(key)) return false;
            skip_space();
            if (at_ >= size_ || text_[at_] != ':') return fail();
            ++at_;
            skip_space();
            out.keys.push_back(std::move(key));
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            skip_space();
            if (at_ >= size_) return fail();
            if (text_[at_] == ',') {
                ++at_;
                skip_space();
                continue;
            }
            if (text_[at_] == '}') {
                ++at_;
                return true;
            }
            return fail();
        }
    }

    const char* text_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
};

// A whole number in a table, appended when it is inside the decoder's range.
inline PredictorError push_integer(const JsonValue& value, long long low, long long high,
                                   std::vector<long long>& out) {
    if (value.type != JsonValue::Type::number) return PredictorError::malformed;
    if (value.number < low || value.number > high) return PredictorError::out_of_range;
    out.push_back(value.number);
    return PredictorError::ok;
}

// A sparse table key: a table index 0..255 with no other characters.
inline bool index_key(const std::string& key, unsigned& index) {
    if (key.empty() || key.size() > 3) return false;
    unsigned value = 0;
    for (const char c : key) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 255) return false;
    index = value;
    return true;
}

// Flattens a nested BPTC history entry ([3][2][4], [2][16] or a flat list) to exactly `expected` integers.
inline PredictorError flatten(const JsonValue& value, std::size_t expected, long long low, long long high,
                              std::vector<long long>& out) {
    out.clear();
    if (value.type != JsonValue::Type::array) return PredictorError::bad_length;
    PredictorError error = PredictorError::ok;
    const auto push = [&](const JsonValue& leaf) {
        if (error == PredictorError::ok) error = push_integer(leaf, low, high, out);
    };
    for (const JsonValue& item : value.items) {
        if (item.type != JsonValue::Type::array) {
            push(item);
        } else {
            for (const JsonValue& nested : item.items) {
                if (nested.type != JsonValue::Type::array) {
                    push(nested);
                } else {
                    for (const JsonValue& leaf : nested.items) push(leaf);
                }
            }
        }
        if (error != PredictorError::ok) return error;
        if (out.size() > expected) return PredictorError::bad_length;
    }
    return out.size() == expected ? PredictorError::ok : PredictorError::bad_length;
}

// How many rows a validated table holds (0 when it is absent). A list must be the full 256; a sparse object holds only
// the rows it names.
inline unsigned table_rows(const JsonValue* table) {
    if (table == nullptr) return 0;
    if (table->type == JsonValue::Type::array) return 256;
    return static_cast<unsigned>((std::min)(table->items.size(), table->keys.size()));
}

// One 256-row table whose rows are fixed-length integer lists (BC6H index history, BC7 endpoints/indices). A row may
// be nested or flat, as tooling/bcpack_tables_bptc.py accepts. The calibration tool writes the full 256-list; a sparse
// index-keyed object (the decoder's other accepted form) is accepted too.
inline PredictorError rows_256(const JsonValue* table, std::size_t row_length, long long low, long long high) {
    if (table == nullptr) return PredictorError::ok;  // optional table
    std::vector<long long> row;
    if (table->type == JsonValue::Type::array) {
        if (table->items.size() != 256) return PredictorError::bad_length;
        for (const JsonValue& item : table->items) {
            const PredictorError error = flatten(item, row_length, low, high, row);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    if (table->type == JsonValue::Type::object) {
        for (std::size_t i = 0; i < table->keys.size() && i < table->items.size(); ++i) {
            unsigned index = 0;
            if (!index_key(table->keys[i], index)) return PredictorError::bad_length;
            const PredictorError error = flatten(table->items[i], row_length, low, high, row);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    return PredictorError::malformed;
}

// A 256-row table of fixed-length rows with a per-channel range (the BC6H endpoint tables: 3 channels x 4 values).
inline PredictorError channel_rows_256(const JsonValue* table, const std::array<long long, 3>& channel_high) {
    if (table == nullptr) return PredictorError::ok;
    const auto one_row = [&channel_high](const JsonValue& row) -> PredictorError {
        if (row.type != JsonValue::Type::array || row.items.size() != 3) return PredictorError::bad_length;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const JsonValue& values = row.items[channel];
            if (values.type != JsonValue::Type::array || values.items.size() != 4) return PredictorError::bad_length;
            for (const JsonValue& value : values.items) {
                if (value.type != JsonValue::Type::number) return PredictorError::malformed;
                if (value.number < 0 || value.number > channel_high[channel]) return PredictorError::out_of_range;
            }
        }
        return PredictorError::ok;
    };
    if (table->type == JsonValue::Type::array) {
        if (table->items.size() != 256) return PredictorError::bad_length;
        for (const JsonValue& item : table->items) {
            const PredictorError error = one_row(item);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    if (table->type == JsonValue::Type::object) {
        for (std::size_t i = 0; i < table->keys.size() && i < table->items.size(); ++i) {
            unsigned index = 0;
            if (!index_key(table->keys[i], index)) return PredictorError::bad_length;
            const PredictorError error = one_row(table->items[i]);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    return PredictorError::malformed;
}

// A 256-entry class table: every row is one of the decoder's four confidence classes 0..3.
inline PredictorError classes_256(const JsonValue* table) {
    if (table == nullptr) return PredictorError::ok;
    const auto one = [](const JsonValue& item) -> PredictorError {
        if (item.type != JsonValue::Type::number) return PredictorError::malformed;
        if (item.number < 0 || item.number > 3) return PredictorError::out_of_range;
        return PredictorError::ok;
    };
    if (table->type == JsonValue::Type::array) {
        if (table->items.size() != 256) return PredictorError::bad_length;
        for (const JsonValue& item : table->items) {
            const PredictorError error = one(item);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    if (table->type == JsonValue::Type::object) {
        for (std::size_t i = 0; i < table->keys.size() && i < table->items.size(); ++i) {
            unsigned index = 0;
            if (!index_key(table->keys[i], index)) return PredictorError::bad_length;
            const PredictorError error = one(table->items[i]);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    return PredictorError::malformed;
}

inline PredictorError validate_bc6h(const JsonValue& section, unsigned& endpoints, unsigned& indices) {
    endpoints = 0;
    indices = 0;
    const JsonValue* endpoint_table = section.member("endpointHistory");
    const JsonValue* index_table = section.member("indexHistory");
    if (endpoint_table == nullptr && index_table == nullptr) return PredictorError::malformed;
    // endpointHistory values are 16-bit endpoint components; the confirmed-bits tables are leading-bit depths.
    const std::array<long long, 3> endpoint_high = {65535, 65535, 65535};
    const std::array<long long, 3> depth_high = {16, 10, 6};
    for (const PredictorError error : {
             channel_rows_256(endpoint_table, endpoint_high),
             rows_256(index_table, 16, 0, 15),
             classes_256(section.member("endpointClass")),
             classes_256(section.member("indexClass")),
             channel_rows_256(section.member("endpointConfirmedBits"), depth_high),
             rows_256(section.member("indexConfirmedBits"), 16, 0, 4),
         }) {
        if (error != PredictorError::ok) return error;
    }
    endpoints = table_rows(endpoint_table);
    indices = table_rows(index_table);
    return PredictorError::ok;
}

inline PredictorError validate_bc7(const JsonValue& section, unsigned& endpoints, unsigned& indices) {
    endpoints = 0;
    indices = 0;
    const JsonValue* endpoint_table = section.member("historyEndpoints");
    const JsonValue* index_table = section.member("historyIndices");
    if (endpoint_table == nullptr && index_table == nullptr) return PredictorError::malformed;
    for (const PredictorError error : {
             rows_256(endpoint_table, 24, 0, 255),
             rows_256(section.member("historyEndpointBits"), 24, 0, 8),
             rows_256(section.member("historyEndpointConfirmedBits"), 24, 0, 8),
             rows_256(index_table, 32, 0, 15),
             rows_256(section.member("historyIndexBits"), 32, 0, 4),
             rows_256(section.member("historyIndexConfirmedBits"), 32, 0, 4),
         }) {
        if (error != PredictorError::ok) return error;
    }
    const JsonValue* last_resort = section.member("historyEndpointLastResort");
    if (last_resort != nullptr) {
        if (last_resort->type != JsonValue::Type::array) return PredictorError::malformed;
        for (const JsonValue& item : last_resort->items) {
            if (item.type != JsonValue::Type::array) return PredictorError::malformed;
            if (item.items.size() != 2 && item.items.size() != 4) return PredictorError::bad_length;
            for (const JsonValue& value : item.items) {
                if (value.type != JsonValue::Type::number) return PredictorError::malformed;
                if (value.number < 0 || value.number > 65535) return PredictorError::out_of_range;
            }
        }
    }
    endpoints = table_rows(endpoint_table);
    indices = table_rows(index_table);
    return PredictorError::ok;
}

// ---- Retaining the validated rows ---------------------------------------------------------------------------------
//
// The validators above name every way a file can be wrong. Once a table has passed them the rows are copied into the
// process-local BcpackHistory the block decoder consumes. The copies repeat the range checks (a table is trusted only
// when both passes agree), and every walk is bounded by the entry count the validator has already enforced.

template <std::size_t N, typename Fn>
inline PredictorError for_each_row(const JsonValue* table, Fn&& fn) {
    if (table == nullptr) return PredictorError::ok;
    if (table->type == JsonValue::Type::array) {
        if (table->items.size() != N) return PredictorError::bad_length;
        for (std::size_t i = 0; i < N; ++i) {
            const PredictorError error = fn(i, table->items[i]);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    if (table->type == JsonValue::Type::object) {
        for (std::size_t i = 0; i < table->keys.size() && i < table->items.size(); ++i) {
            unsigned index = 0;
            if (!index_key(table->keys[i], index) || index >= N) return PredictorError::bad_length;
            const PredictorError error = fn(index, table->items[i]);
            if (error != PredictorError::ok) return error;
        }
        return PredictorError::ok;
    }
    return PredictorError::malformed;
}

// One fixed-length integer row, flattened from a nested list and checked against the per-column maxima.
template <std::size_t N>
inline PredictorError fill_row(const JsonValue& row, const std::array<long long, N>& high,
                               std::array<std::uint8_t, N>& out) {
    const long long widest = *std::max_element(high.begin(), high.end());
    std::vector<long long> flat;
    const PredictorError error = flatten(row, N, 0, widest, flat);
    if (error != PredictorError::ok) return error;
    for (std::size_t i = 0; i < N; ++i) {
        if (flat[i] < 0 || flat[i] > high[i]) return PredictorError::out_of_range;
        out[i] = static_cast<std::uint8_t>(flat[i]);
    }
    return PredictorError::ok;
}

template <std::size_t N>
inline PredictorError fill_class_table(const JsonValue* table, std::array<std::uint8_t, N>& out) {
    return for_each_row<N>(table, [&out](std::size_t index, const JsonValue& item) {
        if (item.type != JsonValue::Type::number) return PredictorError::malformed;
        if (item.number < 0 || item.number > 3) return PredictorError::out_of_range;
        out[index] = static_cast<std::uint8_t>(item.number);
        return PredictorError::ok;
    });
}

// A BC1/BC3/BC4/BC5 section: `confirmed` rows become class 3, `provisional` rows class 1 when the confirmed map does
// not already name the entry. `rows_out` may be null (validation only, the existing caller's path).
template <std::size_t N>
inline PredictorError fill_bc_section(const JsonValue& section, const std::array<long long, N>& high,
                                      std::array<std::array<std::uint8_t, N>, 256>* rows_out,
                                      std::array<std::uint8_t, 256>* classes_out, unsigned& confirmed_rows,
                                      bool& present) {
    if (section.type != JsonValue::Type::object) return PredictorError::malformed;
    present = true;
    confirmed_rows = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const PredictorError error = for_each_row<256>(
            section.member(pass == 0 ? "confirmed" : "provisional"),
            [&](std::size_t index, const JsonValue& row) {
                std::array<std::uint8_t, N> values{};
                const PredictorError row_error = fill_row(row, high, values);
                if (row_error != PredictorError::ok) return row_error;
                if (classes_out != nullptr && (*classes_out)[index] == 0) {
                    (*classes_out)[index] = pass == 0 ? 3 : 1;
                    if (rows_out != nullptr) (*rows_out)[index] = values;
                    if (pass == 0) ++confirmed_rows;
                }
                return PredictorError::ok;
            });
        if (error != PredictorError::ok) return error;
    }
    return PredictorError::ok;
}

inline PredictorError fill_bc6h(const JsonValue& section, BcpackHistory& history, unsigned& endpoints,
                                unsigned& indices) {
    PredictorError error = for_each_row<256>(
        section.member("endpointHistory"), [&history](std::size_t index, const JsonValue& row) {
            if (row.type != JsonValue::Type::array || row.items.size() != 3) return PredictorError::bad_length;
            for (std::size_t c = 0; c < 3; ++c) {
                const JsonValue& channel = row.items[c];
                if (channel.type != JsonValue::Type::array || channel.items.size() != 4)
                    return PredictorError::bad_length;
                for (std::size_t q = 0; q < 4; ++q) {
                    const JsonValue& value = channel.items[q];
                    if (value.type != JsonValue::Type::number) return PredictorError::malformed;
                    if (value.number < 0 || value.number > 65535) return PredictorError::out_of_range;
                    history.bc6h_endpoint[index][c][q] = static_cast<std::uint16_t>(value.number);
                }
            }
            return PredictorError::ok;
        });
    if (error != PredictorError::ok) return error;
    std::array<long long, 16> pixel_high{};
    pixel_high.fill(15);
    error = for_each_row<256>(section.member("indexHistory"),
                              [&history, &pixel_high](std::size_t index, const JsonValue& row) {
                                  std::array<std::uint8_t, 16> values{};
                                  const PredictorError row_error = fill_row(row, pixel_high, values);
                                  if (row_error != PredictorError::ok) return row_error;
                                  history.bc6h_index[index] = values;
                                  return PredictorError::ok;
                              });
    if (error != PredictorError::ok) return error;
    error = fill_class_table<256>(section.member("endpointClass"), history.bc6h_endpoint_class);
    if (error != PredictorError::ok) return error;
    error = fill_class_table<256>(section.member("indexClass"), history.bc6h_index_class);
    if (error != PredictorError::ok) return error;
    // Confidence depths: an explicit table replaces the class-3 full-precision default, exactly as the offline decoder
    // resolves it (tooling/bcpack_tables_bptc.py). Omitted rows in an explicit table confirm no bits.
    const std::array<std::array<long long, 4>, 3> depth_high = {{{16, 10, 6, 6}, {16, 10, 6, 6}, {16, 10, 6, 6}}};
    if (section.member("endpointConfirmedBits") != nullptr) {
        history.bc6h_endpoint_bits = {};
        error = for_each_row<256>(
            section.member("endpointConfirmedBits"),
            [&history, &depth_high](std::size_t index, const JsonValue& row) {
                if (row.type != JsonValue::Type::array || row.items.size() != 3) return PredictorError::bad_length;
                for (std::size_t c = 0; c < 3; ++c) {
                    const JsonValue& channel = row.items[c];
                    if (channel.type != JsonValue::Type::array || channel.items.size() != 4)
                        return PredictorError::bad_length;
                    for (std::size_t q = 0; q < 4; ++q) {
                        const JsonValue& value = channel.items[q];
                        if (value.type != JsonValue::Type::number) return PredictorError::malformed;
                        if (value.number < 0 || value.number > depth_high[c][q]) return PredictorError::out_of_range;
                        history.bc6h_endpoint_bits[index][c][q] = static_cast<std::uint8_t>(value.number);
                    }
                }
                return PredictorError::ok;
            });
        if (error != PredictorError::ok) return error;
    } else {
        for (std::size_t i = 0; i < 256; ++i) {
            if (history.bc6h_endpoint_class[i] != 3) continue;
            for (std::size_t c = 0; c < 3; ++c)
                for (std::size_t q = 0; q < 4; ++q)
                    history.bc6h_endpoint_bits[i][c][q] = static_cast<std::uint8_t>(depth_high[c][q]);
        }
    }
    std::array<long long, 16> index_depth_high{};
    index_depth_high.fill(4);
    if (section.member("indexConfirmedBits") != nullptr) {
        history.bc6h_index_bits = {};
        error = for_each_row<256>(section.member("indexConfirmedBits"),
                                  [&history, &index_depth_high](std::size_t index, const JsonValue& row) {
                                      std::array<std::uint8_t, 16> values{};
                                      const PredictorError row_error = fill_row(row, index_depth_high, values);
                                      if (row_error != PredictorError::ok) return row_error;
                                      history.bc6h_index_bits[index] = values;
                                      return PredictorError::ok;
                                  });
        if (error != PredictorError::ok) return error;
    } else {
        for (std::size_t i = 0; i < 256; ++i) {
            if (history.bc6h_index_class[i] != 3) continue;
            history.bc6h_index_bits[i].fill(4);
        }
    }
    endpoints = table_rows(section.member("endpointHistory"));
    indices = table_rows(section.member("indexHistory"));
    return PredictorError::ok;
}

inline PredictorError fill_bc7(const JsonValue& section, BcpackHistory& history, unsigned& endpoints,
                               unsigned& indices) {
    std::array<long long, 24> byte_high{};
    byte_high.fill(255);
    std::array<long long, 24> depth_high{};
    depth_high.fill(8);
    std::array<long long, 32> nibble_high{};
    nibble_high.fill(15);
    std::array<long long, 32> index_depth_high{};
    index_depth_high.fill(4);
    const auto rows24 = [&section](const char* name, const std::array<long long, 24>& high,
                                   std::array<std::array<std::uint8_t, 24>, 256>& out) -> PredictorError {
        return for_each_row<256>(
            section.member(name), [&high, &out](std::size_t index, const JsonValue& row) {
                std::array<std::uint8_t, 24> values{};
                const PredictorError error = fill_row(row, high, values);
                if (error != PredictorError::ok) return error;
                out[index] = values;
                return PredictorError::ok;
            });
    };
    const auto rows32 = [&section](const char* name, const std::array<long long, 32>& high,
                                   std::array<std::array<std::uint8_t, 32>, 256>& out) -> PredictorError {
        return for_each_row<256>(
            section.member(name), [&high, &out](std::size_t index, const JsonValue& row) {
                std::array<std::uint8_t, 32> values{};
                const PredictorError error = fill_row(row, high, values);
                if (error != PredictorError::ok) return error;
                out[index] = values;
                return PredictorError::ok;
            });
    };
    for (const PredictorError step : {
             rows24("historyEndpoints", byte_high, history.bc7_endpoint),
             rows24("historyEndpointBits", depth_high, history.bc7_endpoint_bits),
             rows24("historyEndpointConfirmedBits", depth_high, history.bc7_endpoint_confirmed),
             rows32("historyIndices", nibble_high, history.bc7_index),
             rows32("historyIndexBits", index_depth_high, history.bc7_index_bits),
             rows32("historyIndexConfirmedBits", index_depth_high, history.bc7_index_confirmed),
         }) {
        if (step != PredictorError::ok) return step;
    }
    // Endpoints known to 0 < d < 8 leading bits have their low bits filled with the mid-range marker, so a reader
    // taking at most d bits observes exactly the calibrated value (tooling/bcpack_tables_bptc.py).
    for (std::size_t i = 0; i < 256; ++i) {
        for (std::size_t j = 0; j < 24; ++j) {
            const unsigned depth = history.bc7_endpoint_bits[i][j];
            if (depth == 0 || depth >= 8) continue;
            const unsigned value = history.bc7_endpoint[i][j];
            history.bc7_endpoint[i][j] =
                static_cast<std::uint8_t>(((value >> (8 - depth)) << (8 - depth)) | (1u << (7 - depth)));
        }
    }
    endpoints = table_rows(section.member("historyEndpoints"));
    indices = table_rows(section.member("historyIndices"));
    return PredictorError::ok;
}
inline std::string read_text_file(const std::filesystem::path& path, std::size_t limit, PredictorError& error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = PredictorError::missing;
        return {};
    }
    if (size > limit) {
        error = PredictorError::too_large;
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = PredictorError::unreadable;
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in && !in.eof()) {
        error = PredictorError::unreadable;
        return {};
    }
    return text;
}

inline PredictorError read_predictor_section(const JsonValue& document, PredictorReport& report,
                                             BcpackHistory* history) {
    // A combined file carries codec sections by name; a single-codec file is the section itself, named either by a
    // "codec" string (what the calibration tool writes) or by the table markers the 16-bit codecs use.
    if (document.type != JsonValue::Type::object) return PredictorError::malformed;
    const JsonValue* sections[7] = {};
    const JsonValue* const named[7] = {document.member("bc1"), document.member("bc3"), document.member("bc4"),
                                       document.member("bc5"), document.member("bc6h"), document.member("bc7"),
                                       nullptr};
    for (int i = 0; i < 6; ++i) sections[i] = named[i];
    bool any = false;
    for (const JsonValue* section : sections) any = any || section != nullptr;
    if (!any) {
        const JsonValue* const codec = document.member("codec");
        const std::string key = codec != nullptr && codec->type == JsonValue::Type::string ? codec->text : std::string();
        if (key == "bc1") sections[0] = &document;
        else if (key == "bc3") sections[1] = &document;
        else if (key == "bc4") sections[2] = &document;
        else if (key == "bc5") sections[3] = &document;
        else if (key == "bc6h" || document.member("endpointHistory") != nullptr) sections[4] = &document;
        else if (key == "bc7" || document.member("historyEndpoints") != nullptr) sections[5] = &document;
        else return PredictorError::no_codec_section;
    }
    for (std::size_t i = 0; i < 6; ++i)
        if (sections[i] != nullptr && sections[i]->type != JsonValue::Type::object) return PredictorError::malformed;
    if (sections[4] != nullptr) {
        PredictorError error = validate_bc6h(*sections[4], report.bc6h_endpoint_rows, report.bc6h_index_rows);
        if (error != PredictorError::ok) return error;
        report.bc6h = true;
        if (history != nullptr) {
            history->bc6h_present = true;
            error = fill_bc6h(*sections[4], *history, report.bc6h_endpoint_rows, report.bc6h_index_rows);
            if (error != PredictorError::ok) return error;
        }
    }
    if (sections[5] != nullptr) {
        PredictorError error = validate_bc7(*sections[5], report.bc7_endpoint_rows, report.bc7_index_rows);
        if (error != PredictorError::ok) return error;
        report.bc7 = true;
        if (history != nullptr) {
            history->bc7_present = true;
            error = fill_bc7(*sections[5], *history, report.bc7_endpoint_rows, report.bc7_index_rows);
            if (error != PredictorError::ok) return error;
        }
    }
    // BC1..BC5 sections are optional confirmed/provisional endpoint maps.
    unsigned* const row_counts[4] = {&report.bc1_rows, &report.bc3_rows, &report.bc4_rows, &report.bc5_rows};
    for (std::size_t i = 0; i < 4; ++i) {
        if (sections[i] == nullptr) continue;
        unsigned rows = 0;
        bool present = false;
        PredictorError error = PredictorError::ok;
        if (i == 0) error = fill_bc_section(*sections[i], std::array<long long, 6>{32, 64, 32, 32, 64, 32},
                                            history != nullptr ? &history->bc1 : nullptr,
                                            history != nullptr ? &history->bc1_class : nullptr, rows, present);
        if (i == 1) error = fill_bc_section(*sections[i], std::array<long long, 8>{256, 256, 32, 64, 32, 32, 64, 32},
                                            history != nullptr ? &history->bc3 : nullptr,
                                            history != nullptr ? &history->bc3_class : nullptr, rows, present);
        if (i == 2) error = fill_bc_section(*sections[i], std::array<long long, 2>{256, 256},
                                            history != nullptr ? &history->bc4 : nullptr,
                                            history != nullptr ? &history->bc4_class : nullptr, rows, present);
        if (i == 3) error = fill_bc_section(*sections[i], std::array<long long, 4>{256, 256, 256, 256},
                                            history != nullptr ? &history->bc5 : nullptr,
                                            history != nullptr ? &history->bc5_class : nullptr, rows, present);
        if (error != PredictorError::ok) return error;
        *row_counts[i] = rows;
        if (history != nullptr) {
            bool* const flags[4] = {&history->bc1_present, &history->bc3_present, &history->bc4_present,
                                    &history->bc5_present};
            *flags[i] = present;
        }
    }
    return PredictorError::ok;
}

}  // namespace detail

// The bounded predictor file size: the calibration tool's combined JSON for all six codecs is well under this, and a
// larger file is a wrong file rather than a larger table.
inline constexpr std::size_t kMaxPredictorFileBytes = 8u * 1024u * 1024u;

// Loads and validates the local BCPack predictor file at `path`. A directory is read as <codec>.json (the calibration
// tool's --out directory), a file is read as a combined or single-codec JSON. With `history` non-null the validated
// rows are also retained for the block decoder (docs/bcpack_runtime.md); without it the call validates and counts only.
inline PredictorError load_dstorage_predictor(const wchar_t* path, PredictorReport& report,
                                              BcpackHistory* history = nullptr) {
    report = PredictorReport{};
    if (history != nullptr) *history = BcpackHistory{};
    if (path == nullptr || path[0] == L'\0') return PredictorError::missing;
    std::error_code ec;
    const std::filesystem::path root(path);
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(root, ec)) {
        for (const char* codec : {"bc1", "bc3", "bc4", "bc5", "bc6h", "bc7"}) {
            const std::filesystem::path candidate = root / (std::string(codec) + ".json");
            if (std::filesystem::is_regular_file(candidate, ec)) files.push_back(candidate);
        }
        if (files.empty()) return PredictorError::missing;
    } else {
        if (!std::filesystem::exists(root, ec)) return PredictorError::missing;
        files.push_back(root);
    }
    for (const std::filesystem::path& file : files) {
        PredictorError error = PredictorError::ok;
        const std::string text = detail::read_text_file(file, kMaxPredictorFileBytes, error);
        if (error != PredictorError::ok) return error;
        report.file_bytes += text.size();
        ++report.source_files;
        detail::JsonValue document;
        detail::JsonReader reader(text.data(), text.size());
        if (!reader.parse(document)) return PredictorError::malformed;
        if ((error = detail::read_predictor_section(document, report, history)) != PredictorError::ok) return error;
    }
    return PredictorError::ok;
}

// ---- BCPack block decode ------------------------------------------------------------------------------------------
//
// A BCPack chunk (DSTORAGE_BCPACK_MODE_BC1..BC7) is an interleaved 12- or 16-lane 16-bit rANS stream of block fields
// plus a per-codec block rebuilder that turns the decoded symbol tuples back into standard BCn blocks
// (docs/bcpack_runtime.md owns the format contract and its provenance; the clean-room specification is
// docs/textures.md, "Clean-room BCPack bitstream specification"). This is our runtime implementation of that format:
// the constants below are codec-level tables (public patent/BPTC specification data, generated from this repository's
// own tooling/bcpack_tables*.py modules), while every initial endpoint/index *history* value is runtime data from the
// local predictor file (PALMETTO_DS_BCPACK_PREDICTOR) and is never compiled in.
//
// The decoder is strict and fail-closed: a block that references a built-in history entry the predictor does not
// confirm, a signature table entry that is inferred only, a symbol outside its lane width, an entropy stream that does
// not end at its home state, or an anchor/p-bit inconsistency is refused with a named reason instead of decoded from a
// guess. It writes into the caller's destination only forward; callers decode into a scratch buffer and publish it on
// success (dstorage.cpp does that), so a refused request leaves no partial block stream in title memory.

namespace bcpack {

inline constexpr std::uint32_t kRansHome = 1u << 24;  // the rANS lower bound L
inline constexpr unsigned kMaxWidth = 10;             // the widest symbol width any q reduces to
inline constexpr unsigned kMaxLanes = 16;
inline constexpr unsigned kPrefetchBudget = 8;        // bytes served per scheduler cycle

// The fourteen BC6H modes: the D3D11/Khronos mode code, whether the endpoint transform applies, the base endpoint
// width, the per-channel x/y/z delta widths, the region count and the slice of the field plan the mode reads.
struct BC6Mode {
    std::int32_t code;
    std::int32_t transformed;
    std::int32_t base;
    std::int32_t delta[3];
    std::int32_t regions;
    std::int32_t first;
    std::int32_t count;
};

// ---- Codec constants (generated from this repository's clean-room spec modules) ----
inline constexpr std::uint16_t kFrequency[176][11] = {
    // width 0
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // width 1
    {32768, 32768, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {38761, 26775, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {44484, 21052, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {49818, 15718, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {54623, 10913, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {58721, 6815, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {61911, 3625, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {64036, 1500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65121, 415, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65477, 59, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65534, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65535, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65535, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65535, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65535, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {65535, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // width 2
    {16384, 16384, 16384, 0, 0, 0, 0, 0, 0, 0, 0},
    {21168, 17592, 13388, 0, 0, 0, 0, 0, 0, 0, 0},
    {26354, 18130, 10526, 0, 0, 0, 0, 0, 0, 0, 0},
    {31900, 17918, 7859, 0, 0, 0, 0, 0, 0, 0, 0},
    {37750, 16874, 5456, 0, 0, 0, 0, 0, 0, 0, 0},
    {43800, 14920, 3408, 0, 0, 0, 0, 0, 0, 0, 0},
    {49850, 12062, 1812, 0, 0, 0, 0, 0, 0, 0, 0},
    {55537, 8499, 750, 0, 0, 0, 0, 0, 0, 0, 0},
    {60308, 4812, 208, 0, 0, 0, 0, 0, 0, 0, 0},
    {63575, 1901, 30, 0, 0, 0, 0, 0, 0, 0, 0},
    {65132, 402, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {65511, 23, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {65533, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {65533, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {65533, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {65533, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    // width 3
    {8192, 8192, 8192, 8192, 0, 0, 0, 0, 0, 0, 0},
    {11073, 10095, 8796, 6694, 0, 0, 0, 0, 0, 0, 0},
    {14406, 11948, 9065, 5263, 0, 0, 0, 0, 0, 0, 0},
    {18234, 13666, 8958, 3930, 0, 0, 0, 0, 0, 0, 0},
    {22624, 15126, 8437, 2728, 0, 0, 0, 0, 0, 0, 0},
    {27657, 16143, 7460, 1704, 0, 0, 0, 0, 0, 0, 0},
    {33414, 16436, 6031, 906, 0, 0, 0, 0, 0, 0, 0},
    {39920, 15616, 4250, 375, 0, 0, 0, 0, 0, 0, 0},
    {47024, 13284, 2406, 104, 0, 0, 0, 0, 0, 0, 0},
    {54201, 9375, 950, 15, 0, 0, 0, 0, 0, 0, 0},
    {60388, 4744, 200, 1, 0, 0, 0, 0, 0, 0, 0},
    {64250, 1262, 10, 1, 0, 0, 0, 0, 0, 0, 0},
    {65456, 74, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {65529, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {65529, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {65529, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    // width 4
    {4096, 4096, 4096, 4096, 4096, 0, 0, 0, 0, 0, 0},
    {5665, 5407, 5048, 4398, 3347, 0, 0, 0, 0, 0, 0},
    {7539, 6867, 5973, 4532, 2632, 0, 0, 0, 0, 0, 0},
    {9773, 8461, 6833, 4479, 1965, 0, 0, 0, 0, 0, 0},
    {12447, 10177, 7564, 4218, 1364, 0, 0, 0, 0, 0, 0},
    {15679, 11977, 8072, 3730, 852, 0, 0, 0, 0, 0, 0},
    {19640, 13774, 8217, 3016, 453, 0, 0, 0, 0, 0, 0},
    {24559, 15361, 7808, 2124, 188, 0, 0, 0, 0, 0, 0},
    {30704, 16320, 6642, 1203, 52, 0, 0, 0, 0, 0, 0},
    {38280, 15920, 4688, 476, 7, 0, 0, 0, 0, 0, 0},
    {47169, 13219, 2372, 99, 1, 0, 0, 0, 0, 0, 0},
    {56354, 7896, 631, 4, 1, 0, 0, 0, 0, 0, 0},
    {63248, 2208, 34, 1, 1, 0, 0, 0, 0, 0, 0},
    {65465, 57, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {65521, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {65521, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    // width 5
    {2048, 2048, 2048, 2048, 2048, 2048, 0, 0, 0, 0, 0},
    {2865, 2799, 2704, 2524, 2200, 1673, 0, 0, 0, 0, 0},
    {3858, 3682, 3434, 2986, 2266, 1316, 0, 0, 0, 0, 0},
    {5063, 4709, 4230, 3418, 2240, 982, 0, 0, 0, 0, 0},
    {6536, 5912, 5088, 3782, 2109, 682, 0, 0, 0, 0, 0},
    {8366, 7314, 5988, 4036, 1865, 426, 0, 0, 0, 0, 0},
    {10688, 8952, 6888, 4108, 1507, 227, 0, 0, 0, 0, 0},
    {13713, 10847, 7680, 3904, 1062, 94, 0, 0, 0, 0, 0},
    {17758, 12946, 8160, 3320, 602, 26, 0, 0, 0, 0, 0},
    {23272, 15008, 7960, 2344, 237, 4, 0, 0, 0, 0, 0},
    {30841, 16327, 6610, 1187, 48, 1, 0, 0, 0, 0, 0},
    {41006, 15348, 3947, 316, 1, 1, 0, 0, 0, 0, 0},
    {53291, 9957, 1104, 14, 1, 1, 0, 0, 0, 0, 0},
    {63380, 2084, 22, 1, 1, 1, 0, 0, 0, 0, 0},
    {65505, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {65505, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    // width 6
    {1024, 1024, 1024, 1024, 1024, 1024, 1024, 0, 0, 0, 0},
    {1441, 1423, 1400, 1352, 1262, 1099, 837, 0, 0, 0, 0},
    {1951, 1907, 1841, 1717, 1493, 1133, 658, 0, 0, 0, 0},
    {2577, 2487, 2354, 2115, 1709, 1120, 491, 0, 0, 0, 0},
    {3350, 3186, 2956, 2544, 1890, 1055, 341, 0, 0, 0, 0},
    {4324, 4042, 3657, 2994, 2019, 932, 213, 0, 0, 0, 0},
    {5581, 5107, 4476, 3444, 2055, 754, 113, 0, 0, 0, 0},
    {7258, 6454, 5424, 3840, 1952, 531, 47, 0, 0, 0, 0},
    {9579, 8179, 6473, 4080, 1660, 301, 13, 0, 0, 0, 0},
    {12907, 10365, 7504, 3980, 1171, 119, 2, 0, 0, 0, 0},
    {17852, 12988, 8164, 3304, 594, 23, 1, 0, 0, 0, 0},
    {25441, 15565, 7673, 1974, 155, 1, 1, 0, 0, 0, 0},
    {37208, 16084, 4978, 552, 4, 1, 1, 0, 0, 0, 0},
    {53649, 9731, 1042, 4, 1, 1, 1, 0, 0, 0, 0},
    {65147, 327, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {65473, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    // width 7
    {512, 512, 512, 512, 512, 512, 512, 512, 0, 0, 0},
    {722, 718, 712, 700, 676, 632, 550, 418, 0, 0, 0},
    {981, 971, 952, 920, 858, 748, 566, 329, 0, 0, 0},
    {1300, 1276, 1244, 1178, 1058, 854, 559, 246, 0, 0, 0},
    {1696, 1654, 1593, 1478, 1272, 946, 526, 171, 0, 0, 0},
    {2198, 2126, 2022, 1828, 1498, 1009, 467, 106, 0, 0, 0},
    {2852, 2728, 2554, 2238, 1721, 1028, 376, 57, 0, 0, 0},
    {3736, 3522, 3227, 2712, 1920, 977, 266, 23, 0, 0, 0},
    {4979, 4601, 4090, 3236, 2040, 831, 151, 6, 0, 0, 0},
    {6807, 6101, 5182, 3752, 1991, 586, 59, 1, 0, 0, 0},
    {9634, 8218, 6494, 4082, 1652, 296, 11, 1, 0, 0, 0},
    {14275, 11165, 7784, 3836, 988, 74, 1, 1, 0, 0, 0},
    {22449, 14759, 8042, 2489, 272, 1, 1, 1, 0, 0, 0},
    {37625, 16023, 4866, 509, 1, 1, 1, 1, 0, 0, 0},
    {60485, 4663, 132, 1, 1, 1, 1, 1, 0, 0, 0},
    {65409, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0},
    // width 8
    {256, 256, 256, 256, 256, 256, 256, 256, 256, 0, 0},
    {362, 360, 359, 356, 350, 338, 316, 275, 209, 0, 0},
    {492, 488, 486, 476, 460, 429, 374, 284, 164, 0, 0},
    {653, 647, 638, 622, 589, 529, 426, 280, 123, 0, 0},
    {853, 843, 828, 796, 739, 636, 473, 264, 85, 0, 0},
    {1108, 1090, 1063, 1011, 914, 748, 504, 234, 53, 0, 0},
    {1442, 1410, 1364, 1277, 1120, 860, 514, 189, 28, 0, 0},
    {1895, 1841, 1760, 1614, 1356, 961, 488, 132, 12, 0, 0},
    {2538, 2442, 2300, 2045, 1618, 1021, 414, 76, 3, 0, 0},
    {3497, 3311, 3050, 2591, 1875, 996, 292, 29, 1, 0, 0},
    {5008, 4626, 4109, 3247, 2041, 826, 149, 4, 1, 0, 0},
    {7575, 6701, 5582, 3892, 1918, 494, 34, 1, 1, 0, 0},
    {12397, 10051, 7380, 4020, 1245, 129, 1, 1, 1, 0, 0},
    {22767, 14857, 8012, 2432, 240, 1, 1, 1, 1, 0, 0},
    {47343, 13141, 2332, 35, 1, 1, 1, 1, 1, 0, 0},
    {65281, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    // width 9
    {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 0},
    {181, 181, 179, 180, 178, 175, 169, 157, 137, 105, 0},
    {246, 246, 244, 243, 238, 231, 214, 187, 142, 82, 0},
    {327, 325, 324, 319, 310, 294, 265, 213, 141, 61, 0},
    {428, 424, 422, 414, 397, 370, 319, 236, 131, 43, 0},
    {557, 551, 546, 530, 506, 457, 374, 252, 116, 27, 0},
    {725, 717, 705, 683, 638, 560, 430, 256, 95, 14, 0},
    {955, 941, 920, 880, 807, 677, 481, 244, 66, 6, 0},
    {1282, 1256, 1219, 1150, 1023, 810, 510, 207, 37, 2, 0},
    {1772, 1724, 1656, 1524, 1295, 938, 498, 147, 13, 1, 0},
    {2554, 2454, 2312, 2054, 1625, 1020, 414, 74, 1, 1, 0},
    {3904, 3672, 3350, 2791, 1946, 959, 247, 14, 1, 1, 0},
    {6523, 5873, 5026, 3690, 2011, 621, 58, 1, 1, 1, 0},
    {12593, 10175, 7428, 4006, 1216, 105, 1, 1, 1, 1, 0},
    {31009, 16335, 6572, 1136, 1, 1, 1, 1, 1, 1, 0},
    {65025, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0},
    // width 10
    {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
    {91, 89, 90, 90, 90, 90, 88, 85, 79, 69, 52},
    {123, 123, 123, 123, 121, 120, 115, 108, 93, 71, 41},
    {164, 164, 162, 161, 160, 155, 148, 131, 108, 69, 31},
    {214, 214, 212, 211, 206, 199, 184, 160, 117, 67, 21},
    {279, 277, 276, 273, 265, 252, 229, 187, 126, 59, 13},
    {364, 360, 358, 352, 342, 319, 280, 215, 129, 47, 7},
    {479, 477, 470, 460, 441, 404, 339, 240, 122, 33, 3},
    {644, 638, 627, 610, 576, 511, 405, 254, 105, 18, 1},
    {892, 880, 862, 828, 763, 647, 469, 248, 73, 6, 1},
    {1290, 1264, 1227, 1156, 1028, 812, 510, 207, 34, 1, 1},
    {1982, 1922, 1836, 1674, 1396, 972, 481, 123, 4, 1, 1},
    {3347, 3177, 2936, 2513, 1846, 1004, 311, 22, 1, 1, 1},
    {6632, 5962, 5087, 3714, 2003, 609, 37, 1, 1, 1, 1},
    {17963, 13041, 8168, 3285, 506, 1, 1, 1, 1, 1, 1},
    {64513, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
};
inline constexpr unsigned kLaneWidths[8][16] = {
    {},
    {8, 8, 5, 6, 5, 5, 6, 5, 8, 8, 8, 8},
    {},
    {4, 4, 8, 6, 6, 6, 6, 5, 6, 5, 8, 8},
    {8, 8, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6},
    {4, 4, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6},
    {4, 4, 8, 8, 8, 5, 5, 5, 6, 6, 6, 8, 8, 8, 8, 5},
    {4, 4, 8, 8, 8, 8, 8, 7, 7, 5, 6, 8, 8, 8, 8, 5},
};
inline constexpr std::uint8_t kQuad1[256] = {
    170, 255, 0, 85, 250, 175, 251, 191, 254, 239, 187, 238, 223, 186, 90, 174,
    127, 234, 171, 247, 253, 165, 95, 245, 221, 119, 102, 153, 42, 138, 162, 80,
    5, 168, 17, 154, 68, 63, 166, 169, 207, 190, 252, 243, 106, 235, 126, 231,
    219, 189, 122, 167, 218, 15, 173, 87, 213, 10, 93, 117, 47, 110, 240, 143,
    248, 230, 61, 242, 185, 37, 124, 155, 59, 133, 206, 236, 88, 179, 211, 160,
    82, 222, 123, 43, 183, 94, 199, 237, 91, 178, 142, 204, 111, 25, 246, 159,
    100, 232, 229, 41, 249, 51, 70, 34, 181, 215, 45, 120, 86, 134, 136, 104,
    145, 149, 146, 210, 135, 125, 118, 103, 157, 89, 101, 217, 21, 55, 147, 92,
    198, 53, 108, 205, 57, 69, 220, 84, 115, 81, 83, 197, 46, 139, 141, 39,
    226, 31, 244, 29, 216, 241, 114, 163, 79, 116, 184, 202, 58, 172, 209, 137,
    71, 38, 98, 152, 112, 13, 161, 26, 74, 107, 27, 208, 164, 233, 7, 177,
    158, 182, 228, 78, 188, 227, 62, 203, 130, 49, 2, 40, 76, 19, 196, 32,
    14, 11, 8, 150, 128, 224, 9, 176, 105, 96, 6, 144, 35, 151, 140, 214,
    33, 109, 121, 50, 200, 72, 132, 23, 18, 212, 77, 113, 1, 54, 99, 156,
    201, 64, 4, 16, 225, 22, 148, 73, 75, 30, 180, 97, 3, 12, 192, 66,
    129, 24, 48, 36, 195, 60, 194, 65, 20, 131, 44, 56, 67, 52, 28, 193,
};
inline constexpr std::uint8_t kQuad3[256] = {
    85, 170, 255, 0, 80, 5, 175, 250, 174, 234, 171, 186, 238, 187, 254, 191,
    239, 251, 165, 90, 240, 17, 15, 68, 245, 95, 160, 10, 138, 162, 168, 42,
    127, 247, 119, 223, 253, 221, 102, 153, 136, 190, 235, 34, 204, 51, 143, 248,
    87, 117, 213, 93, 47, 242, 122, 133, 167, 218, 88, 37, 173, 82, 63, 243,
    207, 232, 252, 142, 43, 126, 106, 166, 154, 112, 7, 13, 169, 120, 128, 8,
    2, 231, 178, 32, 219, 208, 135, 94, 189, 124, 45, 229, 199, 236, 206, 91,
    179, 210, 46, 181, 59, 224, 230, 110, 104, 139, 155, 226, 185, 14, 184, 11,
    61, 211, 81, 134, 39, 69, 21, 222, 84, 141, 183, 237, 123, 176, 216, 41,
    114, 146, 125, 215, 53, 163, 197, 130, 55, 83, 40, 92, 86, 101, 118, 149,
    89, 205, 159, 58, 115, 137, 202, 172, 103, 157, 246, 217, 249, 145, 25, 220,
    152, 108, 70, 100, 111, 38, 57, 98, 198, 147, 9, 35, 6, 96, 140, 200,
    144, 50, 3, 62, 192, 19, 49, 227, 76, 158, 12, 196, 203, 188, 182, 233,
    48, 107, 31, 241, 29, 209, 71, 116, 79, 16, 64, 244, 1, 4, 161, 26,
    164, 27, 74, 177, 228, 78, 54, 201, 156, 99, 151, 113, 121, 23, 72, 105,
    33, 18, 60, 132, 131, 109, 214, 150, 195, 56, 30, 77, 225, 212, 194, 44,
    24, 129, 180, 75, 22, 97, 193, 36, 28, 73, 20, 66, 148, 65, 52, 67,
};
inline constexpr std::uint8_t kPair3[64] = {
    0, 9, 54, 36, 27, 45, 63, 49,
    14, 8, 1, 15, 57, 24, 3, 16,
    2, 18, 37, 35, 28, 44, 41, 13,
    32, 4, 26, 19, 29, 43, 46, 53,
    62, 55, 61, 52, 38, 47, 6, 48,
    20, 34, 12, 33, 5, 40, 30, 51,
    11, 21, 25, 42, 39, 60, 31, 59,
    50, 22, 10, 17, 56, 7, 58, 23,
};
inline constexpr std::uint8_t kPair4[64] = {
    9, 0, 27, 54, 36, 45, 3, 24,
    14, 49, 63, 18, 30, 51, 1, 8,
    12, 33, 11, 25, 6, 48, 20, 47,
    61, 34, 40, 5, 38, 52, 29, 43,
    4, 32, 41, 13, 35, 28, 50, 22,
    31, 59, 17, 10, 56, 7, 42, 21,
    39, 60, 44, 37, 19, 26, 16, 2,
    57, 15, 46, 53, 62, 55, 58, 23,
};
inline constexpr std::uint8_t kPair5[64] = {
    0, 9, 36, 27, 54, 63, 4, 32,
    8, 18, 1, 3, 24, 45, 2, 16,
    49, 14, 28, 57, 35, 33, 15, 6,
    48, 30, 38, 51, 19, 52, 41, 25,
    26, 37, 29, 5, 12, 55, 44, 20,
    11, 43, 13, 62, 34, 47, 39, 40,
    46, 60, 61, 7, 53, 56, 31, 59,
    21, 42, 17, 22, 10, 50, 23, 58,
};
inline constexpr std::uint32_t kPartition2[64] = {
    52428, 34952, 61166, 60616, 51328, 65260, 65224, 60544,
    51200, 65516, 65152, 59392, 65512, 65280, 65520, 61440,
    63248, 142, 28928, 2254, 140, 29456, 12544, 36046,
    2188, 12560, 26214, 13932, 6120, 4080, 29070, 14748,
    43690, 61680, 23130, 13260, 15420, 21930, 38550, 42330,
    29646, 5064, 12876, 15324, 27030, 49980, 39270, 1632,
    626, 1252, 20032, 10016, 51510, 37740, 14790, 25500,
    37686, 40134, 33150, 59160, 52464, 4044, 30532, 60962,
};
inline constexpr std::uint32_t kPartition3[64] = {
    2858963024, 1784303680, 1515864576, 1414570152, 2779054080, 2694860880, 1431675040, 1515868240,
    2857697280, 2857719040, 2863289600, 2425393296, 2492765332, 2762253476, 2846200912, 705315408,
    2777960512, 172118100, 2779096320, 1436590240, 2829603924, 1785348160, 2762231808, 437912832,
    5285028, 2862977168, 342452500, 1768494080, 2693105056, 2860651540, 1352967248, 1784283648,
    2846195712, 1351655592, 2829094992, 606348324, 11162880, 613566756, 608801316, 1352993360,
    1342874960, 2863285316, 1717960704, 2778768800, 1352683680, 1764256040, 1152035396, 1717986816,
    2856600644, 1420317864, 2508232064, 2526451200, 2824098984, 2157286784, 2853442580, 2526412800,
    2863272980, 2689618080, 2695210400, 2516582400, 1082146944, 2846402984, 2863311428, 709513812,
};
inline constexpr std::uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
inline constexpr std::uint8_t kAnchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
inline constexpr std::uint8_t kAnchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};
inline constexpr std::uint8_t kMode2Head[22] = {
    63, 59, 58, 57, 54, 53, 52, 47, 43, 42, 40,
    38, 37, 35, 34, 32, 23, 21, 18, 17, 16, 11,
};
inline constexpr std::uint8_t kInferredRanks[10] = {
    149, 151, 152, 153, 246, 248, 251, 252, 253, 254,
};
inline constexpr BC6Mode kBC6Modes[14] = {
    {0x00, 1, 10, {5, 5, 5}, 2, 0, 80},
    {0x01, 1, 7, {6, 6, 6}, 2, 80, 80},
    {0x02, 1, 11, {5, 4, 4}, 2, 160, 77},
    {0x06, 1, 11, {4, 5, 4}, 2, 237, 77},
    {0x0A, 1, 11, {4, 4, 5}, 2, 314, 77},
    {0x0E, 1, 9, {5, 5, 5}, 2, 391, 77},
    {0x12, 1, 8, {6, 5, 5}, 2, 468, 77},
    {0x16, 1, 8, {5, 6, 5}, 2, 545, 77},
    {0x1A, 1, 8, {5, 5, 6}, 2, 622, 77},
    {0x1E, 0, 6, {6, 6, 6}, 2, 699, 77},
    {0x03, 0, 10, {10, 10, 10}, 1, 776, 60},
    {0x07, 1, 11, {9, 9, 9}, 1, 836, 60},
    {0x0B, 1, 12, {8, 8, 8}, 1, 896, 60},
    {0x0F, 1, 16, {4, 4, 4}, 1, 956, 60},
};
inline constexpr std::int8_t kBC6Plan[][3] = {
    {1, 2, 4},  {2, 2, 4},  {2, 3, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},
    {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},
    {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},  {2, 0, 0},
    {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},
    {2, 0, 9},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {1, 3, 4},  {1, 2, 0},
    {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},
    {2, 3, 0},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},
    {2, 1, 3},  {2, 1, 4},  {2, 3, 1},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},
    {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},  {2, 3, 2},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},
    {0, 3, 3},  {0, 3, 4},  {2, 3, 3},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},
    {1, 2, 5},  {1, 3, 4},  {1, 3, 5},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},
    {0, 0, 5},  {0, 0, 6},  {2, 3, 0},  {2, 3, 1},  {2, 2, 4},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},
    {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {2, 2, 5},  {2, 3, 2},  {1, 2, 4},  {2, 0, 0},
    {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 3, 3},  {2, 3, 5},
    {2, 3, 4},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},  {1, 2, 0},
    {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},
    {1, 1, 5},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},
    {2, 1, 3},  {2, 1, 4},  {2, 1, 5},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},
    {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},  {0, 2, 5},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},
    {0, 3, 3},  {0, 3, 4},  {0, 3, 5},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},
    {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},
    {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},
    {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},
    {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},
    {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 0, 10},  {1, 2, 0},  {1, 2, 1},  {1, 2, 2},  {1, 2, 3},
    {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 0, 10},  {2, 3, 0},  {1, 3, 0},  {1, 3, 1},
    {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 0, 10},  {2, 3, 1},
    {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},  {0, 2, 1},  {0, 2, 2},  {0, 2, 3},
    {0, 2, 4},  {2, 3, 2},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},  {0, 3, 3},  {0, 3, 4},  {2, 3, 3},
    {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},
    {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {0, 0, 8},  {0, 0, 9},  {1, 0, 0},
    {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 0, 8},
    {1, 0, 9},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},
    {2, 0, 7},  {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 0, 10},
    {1, 3, 4},  {1, 2, 0},  {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},
    {1, 1, 3},  {1, 1, 4},  {1, 0, 10},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},
    {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 0, 10},  {2, 3, 1},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},
    {2, 2, 3},  {0, 2, 0},  {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {2, 3, 0},  {2, 3, 2},  {0, 3, 0},
    {0, 3, 1},  {0, 3, 2},  {0, 3, 3},  {1, 2, 4},  {2, 3, 3},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},
    {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},
    {0, 0, 6},  {0, 0, 7},  {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},
    {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},  {2, 0, 0},  {2, 0, 1},
    {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},  {2, 0, 9},
    {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 0, 10},  {2, 2, 4},  {1, 2, 0},  {1, 2, 1},
    {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 0, 10},  {2, 3, 0},
    {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},
    {2, 1, 4},  {2, 0, 10},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},  {0, 2, 1},
    {0, 2, 2},  {0, 2, 3},  {2, 3, 1},  {2, 3, 2},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},  {0, 3, 3},
    {2, 3, 4},  {2, 3, 3},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},
    {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {0, 0, 8},
    {2, 2, 4},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},
    {1, 0, 7},  {1, 0, 8},  {1, 2, 4},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},
    {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},  {2, 3, 4},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},
    {0, 1, 3},  {0, 1, 4},  {1, 3, 4},  {1, 2, 0},  {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},
    {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},  {2, 3, 0},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},
    {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 3, 1},  {2, 2, 0},
    {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},  {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},
    {2, 3, 2},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},  {0, 3, 3},  {0, 3, 4},  {2, 3, 3},  {-1, -1, 0},
    {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},
    {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {1, 3, 4},  {2, 2, 4},  {1, 0, 0},  {1, 0, 1},
    {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {2, 3, 2},  {1, 2, 4},
    {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},
    {2, 3, 3},  {2, 3, 4},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},
    {1, 2, 0},  {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},
    {1, 1, 4},  {2, 3, 0},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},
    {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 3, 1},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},
    {0, 2, 0},  {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},  {0, 2, 5},  {0, 3, 0},  {0, 3, 1},
    {0, 3, 2},  {0, 3, 3},  {0, 3, 4},  {0, 3, 5},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},
    {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},
    {0, 0, 7},  {2, 3, 0},  {2, 2, 4},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},
    {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 2, 5},  {1, 2, 4},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},
    {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {1, 3, 5},  {2, 3, 4},  {0, 1, 0},
    {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {1, 3, 4},  {1, 2, 0},  {1, 2, 1},  {1, 2, 2},
    {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},  {1, 1, 5},  {1, 3, 0},
    {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 1, 4},
    {2, 3, 1},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},  {0, 2, 1},  {0, 2, 2},
    {0, 2, 3},  {0, 2, 4},  {2, 3, 2},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},  {0, 3, 3},  {0, 3, 4},
    {2, 3, 3},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},
    {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {2, 3, 1},  {2, 2, 4},
    {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},
    {2, 2, 5},  {1, 2, 4},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},
    {2, 0, 6},  {2, 0, 7},  {2, 3, 5},  {2, 3, 4},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},
    {0, 1, 4},  {1, 3, 4},  {1, 2, 0},  {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},
    {1, 1, 2},  {1, 1, 3},  {1, 1, 4},  {2, 3, 0},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},
    {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 1, 5},  {2, 2, 0},  {2, 2, 1},
    {2, 2, 2},  {2, 2, 3},  {0, 2, 0},  {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},  {2, 3, 2},
    {0, 3, 0},  {0, 3, 1},  {0, 3, 2},  {0, 3, 3},  {0, 3, 4},  {2, 3, 3},  {-1, -1, 0},  {-1, -1, 1},
    {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},
    {0, 0, 5},  {1, 3, 4},  {2, 3, 0},  {2, 3, 1},  {2, 2, 4},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},
    {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 2, 5},  {2, 2, 5},  {2, 3, 2},  {1, 2, 4},  {2, 0, 0},
    {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {1, 3, 5},  {2, 3, 3},  {2, 3, 5},
    {2, 3, 4},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},  {1, 2, 0},
    {1, 2, 1},  {1, 2, 2},  {1, 2, 3},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},
    {1, 1, 5},  {1, 3, 0},  {1, 3, 1},  {1, 3, 2},  {1, 3, 3},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},
    {2, 1, 3},  {2, 1, 4},  {2, 1, 5},  {2, 2, 0},  {2, 2, 1},  {2, 2, 2},  {2, 2, 3},  {0, 2, 0},
    {0, 2, 1},  {0, 2, 2},  {0, 2, 3},  {0, 2, 4},  {0, 2, 5},  {0, 3, 0},  {0, 3, 1},  {0, 3, 2},
    {0, 3, 3},  {0, 3, 4},  {0, 3, 5},  {-1, -1, 0},  {-1, -1, 1},  {-1, -1, 2},  {-1, -1, 3},  {-1, -1, 4},
    {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},
    {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},
    {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},
    {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},
    {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},  {0, 1, 6},  {0, 1, 7},  {0, 1, 8},  {0, 1, 9},
    {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},  {1, 1, 5},  {1, 1, 6},  {1, 1, 7},
    {1, 1, 8},  {1, 1, 9},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 1, 5},
    {2, 1, 6},  {2, 1, 7},  {2, 1, 8},  {2, 1, 9},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},
    {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},
    {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},
    {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},
    {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},
    {0, 1, 6},  {0, 1, 7},  {0, 1, 8},  {0, 0, 10},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},
    {1, 1, 4},  {1, 1, 5},  {1, 1, 6},  {1, 1, 7},  {1, 1, 8},  {1, 0, 10},  {2, 1, 0},  {2, 1, 1},
    {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 1, 5},  {2, 1, 6},  {2, 1, 7},  {2, 1, 8},  {2, 0, 10},
    {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},  {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},
    {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},  {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},
    {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},  {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},
    {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},  {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},
    {0, 1, 2},  {0, 1, 3},  {0, 1, 4},  {0, 1, 5},  {0, 1, 6},  {0, 1, 7},  {0, 0, 11},  {0, 0, 10},
    {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},  {1, 1, 4},  {1, 1, 5},  {1, 1, 6},  {1, 1, 7},
    {1, 0, 11},  {1, 0, 10},  {2, 1, 0},  {2, 1, 1},  {2, 1, 2},  {2, 1, 3},  {2, 1, 4},  {2, 1, 5},
    {2, 1, 6},  {2, 1, 7},  {2, 0, 11},  {2, 0, 10},  {0, 0, 0},  {0, 0, 1},  {0, 0, 2},  {0, 0, 3},
    {0, 0, 4},  {0, 0, 5},  {0, 0, 6},  {0, 0, 7},  {0, 0, 8},  {0, 0, 9},  {1, 0, 0},  {1, 0, 1},
    {1, 0, 2},  {1, 0, 3},  {1, 0, 4},  {1, 0, 5},  {1, 0, 6},  {1, 0, 7},  {1, 0, 8},  {1, 0, 9},
    {2, 0, 0},  {2, 0, 1},  {2, 0, 2},  {2, 0, 3},  {2, 0, 4},  {2, 0, 5},  {2, 0, 6},  {2, 0, 7},
    {2, 0, 8},  {2, 0, 9},  {0, 1, 0},  {0, 1, 1},  {0, 1, 2},  {0, 1, 3},  {0, 0, 15},  {0, 0, 14},
    {0, 0, 13},  {0, 0, 12},  {0, 0, 11},  {0, 0, 10},  {1, 1, 0},  {1, 1, 1},  {1, 1, 2},  {1, 1, 3},
    {1, 0, 15},  {1, 0, 14},  {1, 0, 13},  {1, 0, 12},  {1, 0, 11},  {1, 0, 10},  {2, 1, 0},  {2, 1, 1},
    {2, 1, 2},  {2, 1, 3},  {2, 0, 15},  {2, 0, 14},  {2, 0, 13},  {2, 0, 12},  {2, 0, 11},  {2, 0, 10},
};

// The model of one (symbol width, q) pair: banded exponential frequencies with total weight 65536.
struct Model {
    const std::uint16_t* frequency = nullptr;
    unsigned width = 0;
    std::uint32_t base[kMaxWidth + 1] = {};

    void init(unsigned symbol_width, unsigned q) {
        width = symbol_width;
        frequency = kFrequency[(symbol_width << 4) | q];
        base[0] = 0;
        for (unsigned p = 1; p <= symbol_width; ++p)
            base[p] = base[p - 1] + static_cast<std::uint32_t>(frequency[p - 1]) * (p == 1 ? 1u : 1u << (p - 2));
    }

    // The symbol whose cumulative range contains the low 16 bits of the state.
    unsigned symbol(std::uint32_t state) const {
        const std::uint32_t cf = state & 0xFFFFu;
        unsigned p = width;
        while (p > 0 && cf < base[p]) --p;
        return p == 0 ? 0u : (1u << (p - 1)) + (cf - base[p]) / frequency[p];
    }

    // The rANS state after decoding `sym` (the encoder's inverse step).
    std::uint32_t advance(std::uint32_t state, unsigned sym) const {
        unsigned p = 0;
        for (unsigned value = sym; value != 0; value >>= 1) ++p;
        std::uint32_t f = frequency[p];
        std::uint32_t c = base[p] + f * (sym - (p == 0 ? 0u : 1u << (p - 1)));
        if (c == 0) {
            --f;
            ++c;
        }
        return f * (state >> 16) + (state & 0xFFFFu) - c;
    }
};

inline const Model& model_for(unsigned width, unsigned q) {
    static const auto models = [] {
        std::array<std::array<Model, 16>, kMaxWidth + 1> table{};
        for (unsigned w = 1; w <= kMaxWidth; ++w)
            for (unsigned quality = 0; quality < 16; ++quality) table[w][quality].init(w, quality);
        return table;
    }();
    return models[width][q];
}

enum class DecodeError {
    ok,
    codec,             // no block rebuilder for this codec id (BC2 has none anywhere)
    destination_size,  // not a whole number of blocks, or not larger than the source when it must be
    entropy,           // the rANS lane stream is malformed or ends outside its home state
    symbol,            // a decoded symbol is outside its lane width or an input byte runs out
    trailing,          // non-zero bytes after every lane finished
    history_unknown,   // a built-in history entry the local predictor does not name
    history_unconfirmed,  // a history entry the local predictor has only fitted, not confirmed
    history_depth,     // a confirmed history row whose confirmed bit depth does not cover the field the mode reads
    signature_unknown, // a BC7 lane-15 pair outside the signature table (BC6H: mode outside 0..13)
    signature_inferred,  // a BC7 signature rank whose partition is inferred only
    consistency,       // an anchor index, p-bit or block-bit-length inconsistency in a rebuilt block
    bounds,            // a malformed source span for the request sizes (defense in depth)
};

inline const char* decode_error_name(DecodeError error) {
    switch (error) {
        case DecodeError::ok: return "ok";
        case DecodeError::codec: return "codec";
        case DecodeError::destination_size: return "destination_size";
        case DecodeError::entropy: return "entropy";
        case DecodeError::symbol: return "symbol";
        case DecodeError::trailing: return "trailing";
        case DecodeError::history_unknown: return "history_unknown";
        case DecodeError::history_unconfirmed: return "history_unconfirmed";
        case DecodeError::history_depth: return "history_depth";
        case DecodeError::signature_unknown: return "signature_unknown";
        case DecodeError::signature_inferred: return "signature_inferred";
        case DecodeError::consistency: return "consistency";
        case DecodeError::bounds: return "bounds";
    }
    return "?";
}

struct DecodeStats {
    std::uint64_t blocks = 0;
    std::uint64_t bytes = 0;
    std::uint64_t history_reads = 0;  // block fields that came from a built-in entry instead of the chunk
};

inline int unzig(unsigned value) { return (value & 1) ? -static_cast<int>((value + 1) >> 1) : static_cast<int>(value >> 1); }

// Pairs two n-bit fields into one 2n-bit value (the inverse of the per-pixel index de-interleave).
inline unsigned interleave(unsigned a, unsigned b, unsigned bits) {
    unsigned value = 0;
    for (unsigned k = 0; k < bits; ++k) value |= (((a >> k) & 1u) | (((b >> k) & 1u) << 1)) << (2 * k);
    return value;
}

inline unsigned interleave8(unsigned a, unsigned b) { return interleave(a, b, 4); }

// The even and odd bits of an 8-bit packed index pair, as the row index delta of pixels (2k, 2k+1).
inline unsigned deint_even(unsigned value) {
    unsigned out = 0;
    for (unsigned k = 0; k < 4; ++k) out |= ((value >> (2 * k)) & 1u) << k;
    return out;
}
inline unsigned deint_odd(unsigned value) {
    unsigned out = 0;
    for (unsigned k = 0; k < 4; ++k) out |= ((value >> (2 * k + 1)) & 1u) << k;
    return out;
}

// One BC7/BC6H block is 128 bits; MSVC has no 128-bit integer, so bits are placed into two words and written out
// little-endian, which is the BCn block byte order.
struct BlockBits {
    std::uint64_t word[2] = {0, 0};
    unsigned position = 0;

    void put(std::uint64_t value, unsigned bits) {
        for (unsigned b = 0; b < bits; ++b, ++position)
            if ((value >> b) & 1u) word[(position >> 6) & 1u] |= 1ull << (position & 63u);
    }
    void bit(bool set) { put(set ? 1u : 0u, 1); }

    bool complete() const { return position == 128; }

    void write(std::uint8_t* out) const {
        for (unsigned i = 0; i < 8; ++i) {
            out[i] = static_cast<std::uint8_t>(word[0] >> (8 * i));
            out[8 + i] = static_cast<std::uint8_t>(word[1] >> (8 * i));
        }
    }
};

// ---- Lane-array entropy decoder -----------------------------------------------------------------------------------
//
// Port of the scheduler in docs/textures.md section 2: at most eight lane bytes are served per cycle in lane order, a
// lane whose byte request was not served freezes for the cycle, and one synchronized element (one symbol per lane) is
// emitted once every active lane has filled its output slot. `emit(index, symbols)` receives one element at a time, so
// the block rebuilders below stream and never allocate a lane array (a 32 MiB destination would be 64 Mi symbols).

struct LaneState {
    enum Phase { header, length, long_length, symbols } phase = header;
    const Model* model = nullptr;
    std::uint32_t state = 0;
    std::uint32_t previous = 0;  // p1: the state before the held symbol (what the next advance is computed from)
    std::uint32_t advanced = 0;  // p0: previous advanced past the held symbol, computed once per cycle
    std::size_t remaining = 0;   // symbols still to decode
    std::size_t payload = 0;     // payload bytes still in the fragment
    unsigned width = 0;
    unsigned byte = 0;
    unsigned zeros = 0;
    unsigned symbol = 0;
    unsigned slot = 0;
    bool flush = false;
    bool extended = false;
    bool buffered = false;
    bool held = false;
    bool full = false;
    bool done = false;
    bool stalled = false;
};

template <typename Emit>
inline DecodeError decode_entropy(unsigned codec, const std::uint8_t* source, std::size_t source_bytes,
                                  std::size_t elements, Emit&& emit) {
    const unsigned lanes_used = codec >= 6 ? 16u : 12u;
    const unsigned* const widths = kLaneWidths[codec];
    std::array<LaneState, kMaxLanes> lanes{};
    for (unsigned i = 0; i < lanes_used; ++i) {
        lanes[i].remaining = elements;
        lanes[i].done = elements == 0;
    }
    std::size_t position = 0;
    std::size_t emitted = 0;
    std::uint16_t symbols[kMaxLanes] = {};
    // The scheduler is monotone, but a malformed stream can still stall it; the bound is generous (each element needs
    // at least one symbol step and at most a few cycles) and makes a corrupt stream fail instead of spin.
    const std::uint64_t max_cycles = 64u + 64u * static_cast<std::uint64_t>(elements) + 8u * source_bytes;
    std::uint64_t cycles = 0;

    for (;;) {
        bool everything_done = true;
        for (unsigned i = 0; i < lanes_used; ++i)
            if (!(lanes[i].done && !lanes[i].full)) everything_done = false;
        if (everything_done) break;
        if (++cycles > max_cycles) return DecodeError::entropy;
        unsigned budget = kPrefetchBudget;
        // Phase 1: state update, prefetch arbitration, release a held symbol to the output slot.
        for (unsigned i = 0; i < lanes_used; ++i) {
            LaneState& lane = lanes[i];
            if (lane.done) {
                lane.stalled = true;
                continue;
            }
            if (lane.held && lane.full) {
                lane.stalled = true;
                continue;
            }
            lane.stalled = false;
            if (lane.phase == LaneState::symbols)
                lane.advanced = lane.held ? lane.model->advance(lane.previous, lane.symbol) : lane.previous;
            if (!lane.buffered && (lane.phase != LaneState::symbols || lane.payload > 0)) {
                if (budget == 0) {
                    lane.stalled = true;
                    continue;
                }
                if (position >= source_bytes) return DecodeError::entropy;
                lane.byte = source[position++];
                lane.buffered = true;
                --budget;
            }
            if (lane.held) {
                lane.full = true;
                lane.slot = lane.symbol;
                lane.held = false;
            }
        }
        // Phase 2: headers, byte merge, virtual zero fill on flush, and the symbol lookup.
        for (unsigned i = 0; i < lanes_used; ++i) {
            LaneState& lane = lanes[i];
            if (lane.done || lane.stalled) continue;
            if (lane.phase == LaneState::header) {
                if (!lane.buffered) continue;
                const unsigned header = lane.byte;
                lane.byte = 0;
                lane.buffered = false;
                const unsigned q = header & 15u;
                lane.flush = (header & 0x10u) != 0;
                const bool long_length = (header & 0x20u) != 0;
                const unsigned width = widths[i] - (header >> 6);
                if (width < 1 || width > kMaxWidth) return DecodeError::symbol;
                lane.model = &model_for(width, q);
                lane.width = width;
                lane.zeros = 0;
                lane.extended = long_length;
                lane.phase = LaneState::length;
            } else if (lane.phase == LaneState::length) {
                if (!lane.buffered) continue;
                lane.payload = lane.extended ? lane.byte : lane.byte + 1u;
                lane.byte = 0;
                lane.buffered = false;
                lane.phase = lane.extended ? LaneState::long_length : LaneState::symbols;
            } else if (lane.phase == LaneState::long_length) {
                if (!lane.buffered) continue;
                lane.payload = (static_cast<std::size_t>(lane.byte) << 8) + (lane.payload & 0xFFu) + 257u;
                lane.byte = 0;
                lane.buffered = false;
                if (lane.payload > source_bytes + 8u) return DecodeError::entropy;  // cannot fit in the chunk
                lane.phase = LaneState::symbols;
            } else {
                std::uint32_t state = lane.advanced;
                if (state < kRansHome) {
                    if (lane.payload > 0) {
                        if (lane.buffered) {
                            state = (state << 8) | lane.byte;
                            lane.byte = 0;
                            lane.buffered = false;
                            --lane.payload;
                            if (lane.payload == 0 && lane.flush && state != 0)
                                while (state < kRansHome && lane.zeros < 8) {
                                    state <<= 8;
                                    ++lane.zeros;
                                }
                        }
                    } else if (lane.flush) {
                        if (state == 0 || lane.zeros >= 8) return DecodeError::entropy;
                        while (state < kRansHome && state != 0 && lane.zeros < 8) {
                            state <<= 8;
                            ++lane.zeros;
                        }
                    } else {
                        lane.previous = state;
                        if (lane.remaining > 0) {
                            lane.phase = LaneState::header;
                        } else {
                            lane.done = true;
                        }
                        continue;
                    }
                }
                const bool next_symbol = state > kRansHome && lane.remaining > 0;
                if (next_symbol) {
                    lane.symbol = lane.model->symbol(state);
                    --lane.remaining;
                    if ((lane.symbol >> lane.width) != 0) return DecodeError::symbol;
                }
                lane.held = next_symbol;
                lane.previous = state;
                if (lane.payload == 0) {
                    if (lane.flush) {
                        if (state == kRansHome) {
                            if (lane.remaining > 0) {
                                lane.phase = LaneState::header;
                                lane.previous = 0;
                            } else {
                                lane.done = true;
                            }
                        } else if (state > kRansHome && !next_symbol) {
                            return DecodeError::entropy;
                        }
                    } else if (lane.remaining == 0 && !next_symbol) {
                        lane.done = true;
                    } else if (state == kRansHome) {
                        return DecodeError::entropy;
                    }
                } else if (state == kRansHome || (lane.remaining == 0 && !next_symbol && state > kRansHome)) {
                    return DecodeError::entropy;
                }
            }
        }
        // Phase 3: one synchronized element once every active lane has filled its slot.
        bool ready = true;
        bool any_full = false;
        for (unsigned i = 0; i < lanes_used; ++i) {
            if (lanes[i].full) {
                any_full = true;
            } else if (!(lanes[i].done && !lanes[i].held)) {
                ready = false;
                break;
            }
        }
        if (ready && any_full) {
            for (unsigned i = 0; i < lanes_used; ++i) {
                if (!lanes[i].full) continue;
                symbols[i] = static_cast<std::uint16_t>(lanes[i].slot);
                lanes[i].full = false;
            }
            if (emitted >= elements) return DecodeError::entropy;
            const DecodeError error = emit(emitted, symbols);
            if (error != DecodeError::ok) return error;
            ++emitted;
        }
        for (unsigned i = 0; i < lanes_used; ++i) {
            LaneState& lane = lanes[i];
            if (lane.done && lane.held && !lane.full) {
                lane.full = true;
                lane.slot = lane.symbol;
                lane.held = false;
            }
        }
    }
    if (emitted != elements) return DecodeError::entropy;
    for (; position < source_bytes; ++position)
        if (source[position] != 0) return DecodeError::trailing;
    return DecodeError::ok;
}

// ---- Block rebuilders ---------------------------------------------------------------------------------------------
//
// Each chunk element is one symbol per lane; an 8-byte format consumes one element per block and a 16-byte format two
// (the first element is the block's even-index fields, the second its odd-index ones). The rebuilt bytes are the
// standard BCn block stream: lane 0 of BC1/BC3/BC4/BC5 is a signed backward distance to the endpoint state the block
// predicts from, and the remaining lanes are zig-zag deltas or rank-table indices. Blocks are independent of each
// other except through that distance, which is at most 255 blocks back, so each rebuilder keeps a 256-block ring
// instead of a full lane/state history.

// The confidence class the calibration tool records for a built-in history row: 0 unknown, 1 fitted, 3 confirmed.
inline DecodeError history_confidence(std::uint8_t confidence) {
    if (confidence == 0) return DecodeError::history_unknown;
    if (confidence != 3) return DecodeError::history_unconfirmed;
    return DecodeError::ok;
}

inline unsigned wrap_mod(int value, unsigned modulus) {
    int result = value % static_cast<int>(modulus);
    if (result < 0) result += static_cast<int>(modulus);
    return static_cast<unsigned>(result);
}

// Resolves one BC1..BC5 endpoint reference: a distance of 0 uses the format's zero/midpoint state (handled by the
// caller); a non-zero distance is a previous block, or `256 + block - distance` in the local predictor's built-in table.
template <std::size_t Count, typename Load>
inline DecodeError resolve_bc_reference(const std::array<std::array<std::uint16_t, Count>, 256>& ring,
                                        std::size_t block, unsigned distance, const std::uint8_t* confidence,
                                        Load&& load, std::array<std::uint16_t, Count>& base) {
    if (block >= distance) {
        base = ring[(block - distance) & 255u];
        return DecodeError::ok;
    }
    const std::size_t entry = 256 + block - distance;
    if (confidence == nullptr) return DecodeError::history_unknown;
    const DecodeError confidence_error = history_confidence(confidence[entry]);
    if (confidence_error != DecodeError::ok) return confidence_error;
    return load(entry, base);
}

inline DecodeError build_bc1(std::size_t block, const std::uint16_t* symbols, const BcpackHistory* history,
                             std::array<std::array<std::uint16_t, 6>, 256>& ring, std::uint8_t* out,
                             DecodeStats& stats) {
    static constexpr unsigned kMods[6] = {32, 64, 32, 32, 64, 32};
    if (symbols[1] != 0) return DecodeError::consistency;
    const unsigned lane0 = symbols[0];
    const int distance = (lane0 & 1) ? 256 - static_cast<int>((lane0 + 1) >> 1) : static_cast<int>(lane0 >> 1);
    std::array<std::uint16_t, 6> base{};
    if (distance == 0) {
        base.fill(0);
    } else {
        const std::uint8_t* const confidence =
            history != nullptr && history->bc1_present ? history->bc1_class.data() : nullptr;
        const DecodeError error = resolve_bc_reference(
            ring, block, static_cast<unsigned>(distance), confidence,
            [history, &stats](std::size_t entry, std::array<std::uint16_t, 6>& target) {
                for (std::size_t i = 0; i < 6; ++i) target[i] = history->bc1[entry][i];
                ++stats.history_reads;
                return DecodeError::ok;
            },
            base);
        if (error != DecodeError::ok) return error;
    }
    std::array<std::uint16_t, 6> endpoints{};
    for (std::size_t i = 0; i < 6; ++i)
        endpoints[i] = static_cast<std::uint16_t>(wrap_mod(static_cast<int>(base[i]) + unzig(symbols[2 + i]), kMods[i]));
    ring[block & 255u] = endpoints;
    const unsigned c0 = (endpoints[0] << 11) | (endpoints[1] << 5) | endpoints[2];
    const unsigned c1 = (endpoints[3] << 11) | (endpoints[4] << 5) | endpoints[5];
    const unsigned a = kQuad1[symbols[8]], b = kQuad1[symbols[9]];
    const unsigned c = kQuad1[symbols[10]], d = kQuad1[symbols[11]];
    out[0] = static_cast<std::uint8_t>(c0);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    out[4] = static_cast<std::uint8_t>((a & 15u) | ((b & 15u) << 4));
    out[5] = static_cast<std::uint8_t>((a >> 4) | ((b >> 4) << 4));
    out[6] = static_cast<std::uint8_t>((c & 15u) | ((d & 15u) << 4));
    out[7] = static_cast<std::uint8_t>((c >> 4) | ((d >> 4) << 4));
    return DecodeError::ok;
}

// BC3: endpoint state (a0, a1, r0, g0, b0, r1, g1, b1); the first two are 8-bit alpha endpoints, the rest are RGB565
// components. Lane 2 carries the alpha delta, lanes 3-6 the RGB565 row ranks, lanes 7-9 the colour deltas and lanes
// 10-11 the colour quadrant ranks.
inline DecodeError build_bc3(std::size_t block, const std::uint16_t* first, const std::uint16_t* second,
                             const BcpackHistory* history, std::array<std::array<std::uint16_t, 8>, 256>& ring,
                             std::uint8_t* out, DecodeStats& stats) {
    static constexpr unsigned kMods[8] = {256, 256, 32, 64, 32, 32, 64, 32};
    static constexpr std::uint16_t kNone[8] = {128, 128, 0, 0, 0, 0, 0, 0};
    if (first[1] != 0 || second[1] != 0) return DecodeError::consistency;
    const int distance = unzig(interleave8(first[0], second[0])) & 255;
    std::array<std::uint16_t, 8> base{};
    if (distance == 0) {
        for (std::size_t i = 0; i < 8; ++i) base[i] = kNone[i];
    } else {
        const std::uint8_t* const confidence =
            history != nullptr && history->bc3_present ? history->bc3_class.data() : nullptr;
        const DecodeError error = resolve_bc_reference(
            ring, block, static_cast<unsigned>(distance), confidence,
            [history, &stats](std::size_t entry, std::array<std::uint16_t, 8>& target) {
                for (std::size_t i = 0; i < 8; ++i) target[i] = history->bc3[entry][i];
                ++stats.history_reads;
                return DecodeError::ok;
            },
            base);
        if (error != DecodeError::ok) return error;
    }
    const unsigned deltas[8] = {first[2], second[2], first[7],  first[8],
                                first[9], second[7], second[8], second[9]};
    std::array<std::uint16_t, 8> endpoints{};
    for (std::size_t i = 0; i < 8; ++i)
        endpoints[i] = static_cast<std::uint16_t>(wrap_mod(static_cast<int>(base[i]) + unzig(deltas[i]), kMods[i]));
    ring[block & 255u] = endpoints;
    std::uint64_t bits = 0;
    for (unsigned row = 0; row < 4; ++row) {
        const std::uint64_t left = kPair3[first[3 + row]];
        const std::uint64_t right = kPair3[second[3 + row]];
        bits |= (left | (right << 6)) << (12 * row);
    }
    out[0] = static_cast<std::uint8_t>(endpoints[0]);
    out[1] = static_cast<std::uint8_t>(endpoints[1]);
    for (unsigned i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    const unsigned c0 = (endpoints[2] << 11) | (endpoints[3] << 5) | endpoints[4];
    const unsigned c1 = (endpoints[5] << 11) | (endpoints[6] << 5) | endpoints[7];
    const unsigned qa = kQuad3[first[10]], qb = kQuad3[second[10]];
    const unsigned qc = kQuad3[first[11]], qd = kQuad3[second[11]];
    out[8] = static_cast<std::uint8_t>(c0);
    out[9] = static_cast<std::uint8_t>(c0 >> 8);
    out[10] = static_cast<std::uint8_t>(c1);
    out[11] = static_cast<std::uint8_t>(c1 >> 8);
    out[12] = static_cast<std::uint8_t>((qa & 15u) | ((qb & 15u) << 4));
    out[13] = static_cast<std::uint8_t>((qa >> 4) | ((qb >> 4) << 4));
    out[14] = static_cast<std::uint8_t>((qc & 15u) | ((qd & 15u) << 4));
    out[15] = static_cast<std::uint8_t>((qc >> 4) | ((qd >> 4) << 4));
    return DecodeError::ok;
}

inline DecodeError build_bc4(std::size_t block, const std::uint16_t* symbols, const BcpackHistory* history,
                             std::array<std::array<std::uint16_t, 2>, 256>& ring, std::uint8_t* out,
                             DecodeStats& stats) {
    if (symbols[1] != 0) return DecodeError::consistency;
    const unsigned lane0 = symbols[0];
    const int distance = (lane0 & 1) ? 256 - static_cast<int>((lane0 + 1) >> 1) : static_cast<int>(lane0 >> 1);
    std::array<std::uint16_t, 2> base{};
    if (distance == 0) {
        base[0] = base[1] = 128;
    } else {
        const std::uint8_t* const confidence =
            history != nullptr && history->bc4_present ? history->bc4_class.data() : nullptr;
        const DecodeError error = resolve_bc_reference(
            ring, block, static_cast<unsigned>(distance), confidence,
            [history, &stats](std::size_t entry, std::array<std::uint16_t, 2>& target) {
                for (std::size_t i = 0; i < 2; ++i) target[i] = history->bc4[entry][i];
                ++stats.history_reads;
                return DecodeError::ok;
            },
            base);
        if (error != DecodeError::ok) return error;
    }
    std::array<std::uint16_t, 2> endpoints{};
    endpoints[0] = static_cast<std::uint16_t>(wrap_mod(static_cast<int>(base[0]) + unzig(symbols[2]), 256));
    endpoints[1] = static_cast<std::uint16_t>(wrap_mod(static_cast<int>(base[1]) + unzig(symbols[3]), 256));
    ring[block & 255u] = endpoints;
    std::uint64_t bits = 0;
    for (unsigned row = 0; row < 4; ++row) {
        const std::uint64_t left = kPair4[symbols[4 + 2 * row]];
        const std::uint64_t right = kPair4[symbols[5 + 2 * row]];
        bits |= (left | (right << 6)) << (12 * row);
    }
    out[0] = static_cast<std::uint8_t>(endpoints[0]);
    out[1] = static_cast<std::uint8_t>(endpoints[1]);
    for (unsigned i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    return DecodeError::ok;
}

inline DecodeError build_bc5(std::size_t block, const std::uint16_t* first, const std::uint16_t* second,
                             const BcpackHistory* history, std::array<std::array<std::uint16_t, 4>, 256>& ring,
                             std::uint8_t* out, DecodeStats& stats) {
    if (first[1] != 0 || second[1] != 0) return DecodeError::consistency;
    const int distance = unzig(interleave8(first[0], second[0])) & 255;
    std::array<std::uint16_t, 4> base{};
    if (distance == 0) {
        base.fill(128);
    } else {
        const std::uint8_t* const confidence =
            history != nullptr && history->bc5_present ? history->bc5_class.data() : nullptr;
        const DecodeError error = resolve_bc_reference(
            ring, block, static_cast<unsigned>(distance), confidence,
            [history, &stats](std::size_t entry, std::array<std::uint16_t, 4>& target) {
                for (std::size_t i = 0; i < 4; ++i) target[i] = history->bc5[entry][i];
                ++stats.history_reads;
                return DecodeError::ok;
            },
            base);
        if (error != DecodeError::ok) return error;
    }
    const unsigned deltas[4] = {first[2], second[2], first[3], second[3]};
    std::array<std::uint16_t, 4> endpoints{};
    for (std::size_t i = 0; i < 4; ++i)
        endpoints[i] = static_cast<std::uint16_t>(wrap_mod(static_cast<int>(base[i]) + unzig(deltas[i]), 256));
    ring[block & 255u] = endpoints;
    for (unsigned channel = 0; channel < 2; ++channel) {
        std::uint64_t bits = 0;
        for (unsigned row = 0; row < 4; ++row) {
            const std::uint64_t left = kPair5[first[4 + 4 * channel + row]];
            const std::uint64_t right = kPair5[second[4 + 4 * channel + row]];
            bits |= (left | (right << 6)) << (12 * row);
        }
        out[8 * channel] = static_cast<std::uint8_t>(endpoints[2 * channel]);
        out[8 * channel + 1] = static_cast<std::uint8_t>(endpoints[2 * channel + 1]);
        for (unsigned i = 0; i < 6; ++i) out[8 * channel + 2 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    return DecodeError::ok;
}

// ---- BC6H ---------------------------------------------------------------------------------------------------------

inline unsigned bc6h_stream_w(unsigned mode, unsigned w) {
    if (mode == 13) return (w & 0x3FFu) | (((w >> 10) & 1u) << 11) | (((w >> 11) & 1u) << 10);
    if (mode == 14) {
        unsigned out = w & 0x3FFu;
        for (unsigned b = 10; b < 16; ++b) out |= ((w >> b) & 1u) << (25 - b);
        return out;
    }
    return w;
}

struct Bc6hState {
    std::array<std::array<std::array<std::uint16_t, 4>, 3>, 256> endpoint{};
    std::array<std::array<std::uint16_t, 16>, 256> index{};
    std::uint16_t first[16] = {};
    bool have_first = false;
};

inline DecodeError build_bc6h(std::size_t block, const std::uint16_t* first, const std::uint16_t* second,
                              const BcpackHistory* history, Bc6hState& state, std::uint8_t* out,
                              DecodeStats& stats) {
    const unsigned mode_symbol = first[15];
    if (mode_symbol > 13) return DecodeError::signature_unknown;
    const unsigned mode = mode_symbol + 1;
    const BC6Mode& info = kBC6Modes[mode - 1];
    const unsigned partition = second[15] < 31 ? second[15] : 31;
    const unsigned index_distance = interleave8(first[1], second[1]);
    const unsigned endpoint_distance = (index_distance + unzig(interleave8(first[0], second[0]))) & 255;
    std::array<std::array<std::uint16_t, 4>, 3> endpoint_ref{};
    if (endpoint_distance == 0) {
        for (unsigned c = 0; c < 3; ++c) endpoint_ref[c] = {0x8000, 0x200, 0x20, 0x20};
    } else if (block >= endpoint_distance) {
        endpoint_ref = state.endpoint[(block - endpoint_distance) & 255u];
    } else {
        const std::size_t entry = 256 + block - endpoint_distance;
        if (history == nullptr || !history->bc6h_present) return DecodeError::history_unknown;
        const DecodeError confidence = history_confidence(history->bc6h_endpoint_class[entry]);
        if (confidence != DecodeError::ok) return confidence;
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned delta_bits = static_cast<unsigned>(info.delta[c]);
            const unsigned need[4] = {static_cast<unsigned>(info.base), delta_bits, delta_bits, delta_bits};
            const unsigned required = info.regions == 2 ? 4u : 2u;
            for (unsigned q = 0; q < required; ++q)
                if (history->bc6h_endpoint_bits[entry][c][q] < need[q]) return DecodeError::history_depth;
            for (unsigned q = 0; q < 4; ++q) endpoint_ref[c][q] = history->bc6h_endpoint[entry][c][q];
        }
        ++stats.history_reads;
    }
    std::array<std::uint16_t, 16> index_ref{};
    if (index_distance == 0) {
        index_ref.fill(8);
    } else if (block >= index_distance) {
        index_ref = state.index[(block - index_distance) & 255u];
    } else {
        const std::size_t entry = 256 + block - index_distance;
        if (history == nullptr || !history->bc6h_present) return DecodeError::history_unknown;
        const DecodeError confidence = history_confidence(history->bc6h_index_class[entry]);
        if (confidence != DecodeError::ok) return confidence;
        const unsigned need = info.regions == 2 ? 3u : 4u;
        for (unsigned p = 0; p < 16; ++p)
            if (history->bc6h_index_bits[entry][p] < need) return DecodeError::history_depth;
        for (unsigned p = 0; p < 16; ++p) index_ref[p] = history->bc6h_index[entry][p];
        ++stats.history_reads;
    }

    const unsigned w_shift = 16 - static_cast<unsigned>(info.base);
    std::array<std::array<std::uint16_t, 4>, 3> endpoint_state{};
    std::array<std::array<unsigned, 4>, 3> field{};
    for (unsigned c = 0; c < 3; ++c) {
        const unsigned delta_bits = static_cast<unsigned>(info.delta[c]);
        const unsigned w_mask = (1u << info.base) - 1u;
        const unsigned delta_mask = (1u << delta_bits) - 1u;
        const unsigned shift = delta_bits < 6 ? 6 - delta_bits : 0;
        const int dz0 = unzig(interleave(first[2 + c], second[2 + c], 8));
        const int dz1 = unzig(interleave(first[5 + c], second[5 + c], 5));
        const int dz2 = unzig(first[8 + c]);
        const int dz3 = unzig(second[8 + c]);
        const unsigned w = ((endpoint_ref[c][0] >> w_shift) + dz0) & w_mask;
        const unsigned x = ((endpoint_ref[c][1] >> (10 - delta_bits)) + dz1) & delta_mask;
        const unsigned y = ((endpoint_ref[c][2] >> shift) + dz2) & delta_mask;
        const unsigned z = ((endpoint_ref[c][3] >> shift) + dz3) & delta_mask;
        const unsigned x10 = x << (10 - delta_bits);
        if (info.regions == 2) {
            endpoint_state[c] = {static_cast<std::uint16_t>(w << w_shift), static_cast<std::uint16_t>(x10),
                                 static_cast<std::uint16_t>(y << shift), static_cast<std::uint16_t>(z << shift)};
            field[c] = {bc6h_stream_w(mode, w), x, y, z};
        } else {
            const unsigned value =
                (w & delta_mask) ^ (info.transformed == 1 ? (1u << (delta_bits - 1)) : 0u);
            const unsigned low_y = delta_bits >= 6 ? value >> (delta_bits - 6) : value << shift;
            endpoint_state[c] = {static_cast<std::uint16_t>(w << w_shift), static_cast<std::uint16_t>(x10),
                                 static_cast<std::uint16_t>(low_y), static_cast<std::uint16_t>(x10 >> 4)};
            field[c] = {bc6h_stream_w(mode, w), x, 0, 0};
        }
    }
    state.endpoint[block & 255u] = endpoint_state;

    const unsigned index_bits = info.regions == 2 ? 3u : 4u;
    const unsigned index_mask = (1u << index_bits) - 1u;
    const unsigned index_shift = 4 - index_bits;
    unsigned indices[16] = {};
    std::array<std::uint16_t, 16> index_state{};
    for (unsigned row = 0; row < 4; ++row) {
        const unsigned sa = first[11 + row];
        const unsigned sb = second[11 + row];
        unsigned d[4];
        if (row < 3) {
            d[0] = deint_odd(sa);
            d[1] = deint_even(sa);
            d[2] = deint_odd(sb);
            d[3] = deint_even(sb);
        } else {
            d[0] = deint_even(sa);
            d[1] = deint_odd(sa);
            d[2] = deint_even(sb);
            d[3] = deint_odd(sb);
        }
        for (unsigned off = 0; off < 4; ++off) {
            const unsigned p = row * 4 + off;
            const unsigned value = ((index_ref[p] >> index_shift) + unzig(d[off])) & index_mask;
            indices[p] = value;
            index_state[p] = static_cast<std::uint16_t>(value << index_shift);
        }
    }
    state.index[block & 255u] = index_state;
    const unsigned second_anchor = info.regions == 2 ? kAnchor2[partition] : 0;
    if ((indices[0] & (info.regions == 2 ? 4u : 8u)) != 0) return DecodeError::consistency;
    if (info.regions == 2 && (indices[second_anchor] & 4u) != 0) return DecodeError::consistency;

    BlockBits bits;
    bits.put(static_cast<std::uint64_t>(info.code), info.code < 2 ? 2u : 5u);
    for (std::int32_t i = 0; i < info.count; ++i) {
        const std::int8_t* const step = kBC6Plan[info.first + i];
        if (step[0] < 0) {
            bits.bit(((partition >> step[2]) & 1u) != 0);
        } else {
            bits.bit(((field[static_cast<unsigned>(step[0])][static_cast<unsigned>(step[1])] >> step[2]) & 1u) != 0);
        }
    }
    for (unsigned p = 0; p < 16; ++p) {
        const unsigned available = (p == 0 || p == second_anchor) ? index_bits - 1u : index_bits;
        bits.put(indices[p] & ((1u << available) - 1u), available);
    }
    if (!bits.complete()) return DecodeError::consistency;
    bits.write(out);
    return DecodeError::ok;
}

// ---- BC7 ----------------------------------------------------------------------------------------------------------

struct Bc7Tables {
    std::array<std::array<std::uint8_t, 16>, 64> partition2{};
    std::array<std::array<std::uint8_t, 16>, 64> partition3{};
    std::array<std::array<std::uint8_t, 4>, 286> config{};  // mode, partition, rotation, index selection
    std::array<std::int16_t, 512> rank_of{};
    std::array<bool, 286> inferred{};
};

inline const Bc7Tables& bc7_tables() {
    static const Bc7Tables tables = [] {
        Bc7Tables result;
        for (std::size_t p = 0; p < 64; ++p) {
            for (std::size_t i = 0; i < 16; ++i) {
                result.partition2[p][i] = static_cast<std::uint8_t>((kPartition2[p] >> i) & 1u);
                result.partition3[p][i] = static_cast<std::uint8_t>((kPartition3[p] >> (2 * i)) & 3u);
            }
        }
        // Lane-15 pairs are ranked by ((a + 1) * (b + 1), a) ascending; the first 286 ranks name a signature.
        std::array<std::pair<unsigned, unsigned>, 512> cells{};
        unsigned count = 0;
        for (unsigned a = 0; a < 32; ++a)
            for (unsigned b = 0; b < 16; ++b) cells[count++] = {a, b};
        std::sort(cells.begin(), cells.end(), [](const auto& left, const auto& right) {
            const unsigned left_key = (left.first + 1) * (left.second + 1);
            const unsigned right_key = (right.first + 1) * (right.second + 1);
            return left_key != right_key ? left_key < right_key : left.first < right.first;
        });
        result.rank_of.fill(-1);
        for (unsigned rank = 0; rank < count && rank < result.config.size(); ++rank)
            result.rank_of[cells[rank].first * 16 + cells[rank].second] = static_cast<std::int16_t>(rank);
        // The signature permutation is generated from the same closed-form descending ranges the offline decoder uses.
        unsigned at = 0;
        const auto put = [&result, &at](std::uint8_t mode, std::uint8_t partition, std::uint8_t rotation,
                                        std::uint8_t selection) {
            result.config[at++] = {mode, partition, rotation, selection};
        };
        for (unsigned r = 0; r < 64; ++r) put(7, static_cast<std::uint8_t>(63 - r), 0, 0);
        put(6, 0, 0, 0);
        for (unsigned r = 0; r < 4; ++r) put(5, 0, static_cast<std::uint8_t>(3 - r), 0);
        for (unsigned r = 0; r < 8; ++r) put(4, 0, static_cast<std::uint8_t>((7 - r) >> 1), static_cast<std::uint8_t>((7 - r) & 1));
        for (unsigned r = 0; r < 64; ++r) put(3, static_cast<std::uint8_t>(63 - r), 0, 0);
        for (unsigned i = 0; i < static_cast<unsigned>(std::size(kMode2Head)); ++i) put(2, kMode2Head[i], 0, 0);
        for (unsigned r = 0; r < 64; ++r) put(1, static_cast<std::uint8_t>(63 - r), 0, 0);
        for (unsigned p : {15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u, 5u, 4u, 3u, 2u, 1u, 0u})
            put(0, static_cast<std::uint8_t>(p), 0, 0);
        bool head[64] = {};
        for (unsigned i = 0; i < static_cast<unsigned>(std::size(kMode2Head)); ++i) head[kMode2Head[i]] = true;
        for (int p = 63; p >= 0; --p)
            if (!head[p]) put(2, static_cast<std::uint8_t>(p), 0, 0);
        put(0, 7, 0, 0);
        put(0, 6, 0, 0);
        put(255, 255, 0, 0);
        for (unsigned i = 0; i < static_cast<unsigned>(std::size(kInferredRanks)); ++i)
            result.inferred[kInferredRanks[i]] = true;
        return result;
    }();
    return tables;
}

struct Bc7State {
    std::array<std::array<std::uint8_t, 24>, 256> endpoint{};
    std::array<std::array<std::uint8_t, 32>, 256> index{};
    std::uint16_t first[16] = {};
    bool have_first = false;
};

// The leading bits each endpoint byte of a mode's fields carries (per channel R, G, B, A after rotation).
inline std::array<unsigned, 4> bc7_field_bits(unsigned mode, unsigned rotation) {
    static constexpr unsigned kColourBits[8] = {4, 6, 5, 7, 5, 7, 7, 5};
    static constexpr unsigned kAlphaBits[8] = {0, 0, 0, 0, 6, 8, 7, 5};
    static constexpr unsigned kPbitKind[8] = {1, 2, 0, 1, 0, 0, 1, 1};
    std::array<unsigned, 4> widths = {kColourBits[mode], kColourBits[mode], kColourBits[mode], kAlphaBits[mode]};
    if (rotation != 0) {
        widths[rotation - 1] = kAlphaBits[mode];
        widths[3] = kColourBits[mode];
    }
    std::array<unsigned, 4> bits{};
    for (unsigned c = 0; c < 4; ++c)
        bits[c] = widths[c] == 0 ? 0 : widths[c] + ((kPbitKind[mode] != 0 && c == 0) ? 1u : 0u);
    return bits;
}

inline DecodeError build_bc7(std::size_t block, const std::uint16_t* element0, const std::uint16_t* element1,
                             const BcpackHistory* history, Bc7State& state, std::uint8_t* out, DecodeStats& stats) {
    static constexpr unsigned kSubsets[8] = {3, 2, 3, 2, 1, 1, 1, 2};
    static constexpr unsigned kColourBits[8] = {4, 6, 5, 7, 5, 7, 7, 5};
    static constexpr unsigned kAlphaBits[8] = {0, 0, 0, 0, 6, 8, 7, 5};
    static constexpr unsigned kPbitKind[8] = {1, 2, 0, 1, 0, 0, 1, 1};
    static constexpr unsigned kIndexBits[8] = {3, 3, 2, 2, 3, 2, 4, 2};
    static constexpr unsigned kPartitionBits[8] = {4, 6, 6, 6, 0, 0, 0, 6};
    static constexpr int kLaneRow[3][4] = {{2, 3, 4, 5}, {6, 7, 8, 9}, {10, 5, 9, -1}};
    static constexpr unsigned kSecondaryRow[4] = {6, 7, 8, 10};  // the mode 4/5 second index set

    const Bc7Tables& tables = bc7_tables();
    const unsigned s15a = element0[15];
    const unsigned s15b = element1[15];
    const std::int16_t rank =
        (s15a < 32 && s15b < 16) ? tables.rank_of[s15a * 16 + s15b] : static_cast<std::int16_t>(-1);
    const std::array<std::uint8_t, 4> config =
        rank >= 0 ? tables.config[static_cast<std::size_t>(rank)] : std::array<std::uint8_t, 4>{255, 255, 0, 0};
    if (rank < 0 || config[0] > 7 || config[1] == 255) return DecodeError::signature_unknown;
    if (tables.inferred[static_cast<std::size_t>(rank)]) return DecodeError::signature_inferred;
    const unsigned mode = config[0];
    const unsigned partition = config[1];
    const unsigned rotation = config[2];
    const unsigned selection = config[3];
    const unsigned subsets = kSubsets[mode];
    const unsigned index_distance = interleave8(element0[1], element1[1]);
    const unsigned endpoint_distance = (index_distance + unzig(interleave8(element0[0], element1[0]))) & 255;
    std::array<std::uint8_t, 24> endpoint_ref{};
    std::array<std::uint8_t, 32> index_ref{};
    if (endpoint_distance == 0) {
        endpoint_ref.fill(0);
    } else if (block >= endpoint_distance) {
        endpoint_ref = state.endpoint[(block - endpoint_distance) & 255u];
    } else {
        const std::size_t entry = 256 + block - endpoint_distance;
        if (history == nullptr || !history->bc7_present) return DecodeError::history_unknown;
        // A BC7 endpoint row has no class column: its confidence is its recorded bit depth against the confirmed
        // depth for every field the mode reads.
        const std::array<unsigned, 4> field = bc7_field_bits(mode, rotation);
        bool unknown = false;
        bool unconfirmed = false;
        for (unsigned sub = 0; sub < subsets; ++sub) {
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned want = field[c];
                if (want == 0) continue;
                for (unsigned e = 0; e < 2; ++e) {
                    const std::size_t slot = sub * 8 + e * 4 + c;
                    if (history->bc7_endpoint_bits[entry][slot] < want) unknown = true;
                    if (history->bc7_endpoint_confirmed[entry][slot] < want) unconfirmed = true;
                }
            }
        }
        if (unknown) return DecodeError::history_unknown;
        if (unconfirmed) return DecodeError::history_unconfirmed;
        for (std::size_t i = 0; i < 24; ++i) endpoint_ref[i] = history->bc7_endpoint[entry][i];
        ++stats.history_reads;
    }
    if (index_distance == 0) {
        index_ref.fill(0);
    } else if (block >= index_distance) {
        index_ref = state.index[(block - index_distance) & 255u];
    } else {
        const std::size_t entry = 256 + block - index_distance;
        if (history == nullptr || !history->bc7_present) return DecodeError::history_unknown;
        const unsigned want0 = kIndexBits[mode];
        const unsigned want1 = (mode == 4 || mode == 5) ? 2u : 0u;
        bool unknown = false;
        bool unconfirmed = false;
        for (unsigned st = 0; st < 2; ++st) {
            const unsigned want = st == 0 ? want0 : want1;
            if (want == 0) continue;
            for (unsigned p = 0; p < 16; ++p) {
                const std::size_t slot = st * 16 + p;
                if (history->bc7_index_bits[entry][slot] < want) unknown = true;
                if (history->bc7_index_confirmed[entry][slot] < want) unconfirmed = true;
            }
        }
        if (unknown) return DecodeError::history_unknown;
        if (unconfirmed) return DecodeError::history_unconfirmed;
        for (std::size_t i = 0; i < 32; ++i) index_ref[i] = history->bc7_index[entry][i];
        ++stats.history_reads;
    }

    unsigned colour_bits = kColourBits[mode];
    const unsigned alpha_bits = kAlphaBits[mode];
    const unsigned pbit_kind = kPbitKind[mode];
    std::array<unsigned, 4> widths = {colour_bits, colour_bits, colour_bits, alpha_bits};
    if (rotation != 0) {
        widths[rotation - 1] = alpha_bits;
        widths[3] = colour_bits;
    }
    unsigned values[3][2][4] = {};
    std::uint8_t separated[24] = {};
    unsigned pbits[3][2] = {};
    bool pbit_mismatch = false;
    for (unsigned sub = 0; sub < subsets; ++sub) {
        for (unsigned c = 0; c < 4; ++c) {
            const int lane_index = kLaneRow[sub][c];
            const unsigned total_bits = widths[c] == 0 ? 0 : widths[c] + ((pbit_kind != 0 && c == 0) ? 1u : 0u);
            if (total_bits == 0 || lane_index < 0) continue;
            const unsigned shift = 8 - total_bits;
            const unsigned mask = (1u << total_bits) - 1u;
            const unsigned v0 = ((endpoint_ref[sub * 8 + c] >> shift) + unzig(element0[lane_index])) & mask;
            const unsigned v1 = ((endpoint_ref[sub * 8 + 4 + c] >> shift) + unzig(element1[lane_index])) & mask;
            values[sub][0][c] = v0;
            values[sub][1][c] = v1;
            separated[sub * 8 + c] = static_cast<std::uint8_t>(v0 << shift);
            separated[sub * 8 + 4 + c] = static_cast<std::uint8_t>(v1 << shift);
        }
        if (pbit_kind == 1) {
            pbits[sub][0] = values[sub][0][0] & 1u;
            pbits[sub][1] = values[sub][1][0] & 1u;
        } else if (pbit_kind == 2) {
            pbits[sub][0] = values[sub][0][0] & 1u;
            pbits[sub][1] = pbits[sub][0];
            if ((values[sub][1][0] & 1u) != pbits[sub][0]) pbit_mismatch = true;
        }
    }
    for (unsigned sub = subsets; sub < 3; ++sub)
        for (std::size_t i = 0; i < 8; ++i) separated[sub * 8 + i] = separated[i];
    state.endpoint[block & 255u] = {};
    for (std::size_t i = 0; i < 24; ++i) state.endpoint[block & 255u][i] = separated[i];

    const unsigned primary_bits = kIndexBits[mode];
    const unsigned primary_shift = 4 - primary_bits;
    const unsigned primary_mask = (1u << primary_bits) - 1u;
    const bool two_index_sets = mode == 4 || mode == 5;
    unsigned primary[16] = {};
    unsigned secondary[16] = {};
    std::uint8_t six[32] = {};
    for (unsigned row = 0; row < 4; ++row) {
        const unsigned sa = element0[11 + row];
        const unsigned sb = element1[11 + row];
        unsigned d[4];
        if (row < 3) {
            d[0] = deint_odd(sa);
            d[1] = deint_even(sa);
            d[2] = deint_odd(sb);
            d[3] = deint_even(sb);
        } else {
            d[0] = deint_even(sa);
            d[1] = deint_odd(sa);
            d[2] = deint_even(sb);
            d[3] = deint_odd(sb);
        }
        for (unsigned off = 0; off < 4; ++off) {
            const unsigned p = row * 4 + off;
            const unsigned value = ((index_ref[p] >> primary_shift) + unzig(d[off])) & primary_mask;
            primary[p] = value;
            six[p] = static_cast<std::uint8_t>(value << primary_shift);
        }
        if (two_index_sets) {
            const unsigned s2a = element0[kSecondaryRow[row]];
            const unsigned s2b = element1[kSecondaryRow[row]];
            if (row < 3) {
                d[0] = deint_odd(s2a);
                d[1] = deint_even(s2a);
                d[2] = deint_odd(s2b);
                d[3] = deint_even(s2b);
            } else {
                d[0] = deint_even(s2a);
                d[1] = deint_odd(s2a);
                d[2] = deint_even(s2b);
                d[3] = deint_odd(s2b);
            }
            for (unsigned off = 0; off < 4; ++off) {
                const unsigned p = row * 4 + off;
                const unsigned value = ((index_ref[16 + p] >> 2) + unzig(d[off])) & 3u;
                secondary[p] = value;
                six[16 + p] = static_cast<std::uint8_t>(value << 2);
            }
        }
    }
    state.index[block & 255u] = {};
    for (std::size_t i = 0; i < 32; ++i) state.index[block & 255u][i] = six[i];

    bool empty = true;
    for (std::size_t i = 0; i < 24; ++i) empty = empty && separated[i] == 0;
    for (std::size_t i = 0; i < 16; ++i) empty = empty && primary[i] == 0;
    if (mode == 0 && partition == 0 && empty) {
        if (pbit_mismatch) return DecodeError::consistency;
        std::memset(out, 0, 16);
        return DecodeError::ok;
    }

    const std::array<std::uint8_t, 16>* const subsets_map =
        subsets == 2 ? &tables.partition2[partition] : (subsets == 3 ? &tables.partition3[partition] : nullptr);
    const unsigned anchors[3] = {
        0u, subsets == 2 ? static_cast<unsigned>(kAnchor2[partition])
                         : (subsets == 3 ? static_cast<unsigned>(kAnchor3a[partition]) : 0u),
        subsets == 3 ? static_cast<unsigned>(kAnchor3b[partition]) : 0u};
    const unsigned* primary_set = primary;
    unsigned primary_width = primary_bits;
    const unsigned* secondary_set = nullptr;
    unsigned secondary_width = 0;
    if (mode == 4) {
        primary_set = secondary;
        primary_width = 2;
        secondary_set = primary;
        secondary_width = 3;
    } else if (mode == 5) {
        primary_set = primary;
        primary_width = 2;
        secondary_set = secondary;
        secondary_width = 2;
    }

    // The block header starts with the mode's single set bit, then the partition, rotation and index-selection fields.
    BlockBits block_bits;
    block_bits.put(1ull << mode, mode + 1);
    const unsigned partition_bits = kPartitionBits[mode];
    if (partition_bits != 0) block_bits.put(partition, partition_bits);
    if (mode == 4 || mode == 5) block_bits.put(rotation, 2);
    if (mode == 4) block_bits.put(selection, 1);
    for (unsigned c = 0; c < 3; ++c) {
        const unsigned channel = (rotation != 0 && c == rotation - 1) ? 3u : c;
        const bool drop = pbit_kind != 0 && channel == 0;
        for (unsigned sub = 0; sub < subsets; ++sub)
            for (unsigned e = 0; e < 2; ++e) {
                const unsigned value = values[sub][e][channel];
                block_bits.put((drop ? value >> 1 : value) & ((1u << colour_bits) - 1u), colour_bits);
            }
    }
    if (alpha_bits != 0) {
        const unsigned channel = rotation != 0 ? rotation - 1 : 3;
        const bool drop = pbit_kind != 0 && channel == 0;
        for (unsigned sub = 0; sub < subsets; ++sub)
            for (unsigned e = 0; e < 2; ++e) {
                const unsigned value = values[sub][e][channel];
                block_bits.put((drop ? value >> 1 : value) & ((1u << alpha_bits) - 1u), alpha_bits);
            }
    }
    if (pbit_kind == 1) {
        for (unsigned sub = 0; sub < subsets; ++sub) {
            block_bits.bit((pbits[sub][0] & 1u) != 0);
            block_bits.bit((pbits[sub][1] & 1u) != 0);
        }
    } else if (pbit_kind == 2) {
        for (unsigned sub = 0; sub < subsets; ++sub) block_bits.bit((pbits[sub][0] & 1u) != 0);
    }
    const unsigned primary_anchor_mask = (1u << (primary_width - 1)) - 1u;
    for (unsigned p = 0; p < 16; ++p) {
        const unsigned subset = subsets_map != nullptr ? (*subsets_map)[p] : 0u;
        const bool anchor = p == anchors[subset];
        const unsigned value = primary_set[p];
        if (anchor) {
            if ((value >> (primary_width - 1)) != 0) return DecodeError::consistency;
            block_bits.put(value & primary_anchor_mask, primary_width - 1);
        } else {
            block_bits.put(value & ((1u << primary_width) - 1u), primary_width);
        }
    }
    if (secondary_set != nullptr) {
        for (unsigned p = 0; p < 16; ++p) {
            const unsigned value = secondary_set[p];
            if (p == 0) {
                if ((value >> (secondary_width - 1)) != 0) return DecodeError::consistency;
                block_bits.put(value & ((1u << (secondary_width - 1)) - 1u), secondary_width - 1);
            } else {
                block_bits.put(value & ((1u << secondary_width) - 1u), secondary_width);
            }
        }
    }
    if (pbit_mismatch) return DecodeError::consistency;
    if (!block_bits.complete()) return DecodeError::signature_unknown;
    block_bits.write(out);
    return DecodeError::ok;
}

// ---- Entry point --------------------------------------------------------------------------------------------------

inline unsigned block_bytes(unsigned codec) {
    switch (codec) {
        case 1:
        case 4: return 8;
        case 3:
        case 5:
        case 6:
        case 7: return 16;
        default: return 0;
    }
}

// Decodes one BCPack chunk into `destination` (a whole number of BCn blocks). `codec` is the request's
// DSTORAGE_BCPACK_MODE (1, 3, 4, 5, 6 or 7); BC2 has no known block decoder. `history` may be null only for chunks
// whose delta chains stay inside the chunk; any reference to a built-in history entry then refuses by name. The
// destination is written block by block in order and is unspecified on failure - dstorage.cpp decodes into scratch and
// publishes it only after `ok`, so a refused request never leaves a partial block stream in title memory.
inline DecodeError decode(unsigned codec, const std::uint8_t* source, std::size_t source_bytes,
                          std::uint8_t* destination, std::size_t destination_bytes, const BcpackHistory* history,
                          DecodeStats* stats_out) {
    DecodeStats stats{};
    const unsigned block = block_bytes(codec);
    if (block == 0) return DecodeError::codec;
    if (destination_bytes == 0) {
        if (stats_out != nullptr) *stats_out = stats;
        return source_bytes == 0 ? DecodeError::ok : DecodeError::destination_size;
    }
    if (destination == nullptr || (source == nullptr && source_bytes != 0)) return DecodeError::bounds;
    if (destination_bytes % block != 0) return DecodeError::destination_size;
    const std::size_t blocks = destination_bytes / block;
    const std::size_t elements = destination_bytes / 8;
    DecodeError error = DecodeError::ok;
    if (codec == 1) {
        std::array<std::array<std::uint16_t, 6>, 256> ring{};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   return build_bc1(index, symbols, history, ring, destination + index * 8, stats);
                               });
    } else if (codec == 4) {
        std::array<std::array<std::uint16_t, 2>, 256> ring{};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   return build_bc4(index, symbols, history, ring, destination + index * 8, stats);
                               });
    } else if (codec == 3) {
        std::array<std::array<std::uint16_t, 8>, 256> ring{};
        std::uint16_t first[16] = {};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   if ((index & 1u) == 0) {
                                       std::memcpy(first, symbols, sizeof first);
                                       return DecodeError::ok;
                                   }
                                   return build_bc3(index / 2, first, symbols, history, ring,
                                                    destination + (index / 2) * 16, stats);
                               });
    } else if (codec == 5) {
        std::array<std::array<std::uint16_t, 4>, 256> ring{};
        std::uint16_t first[16] = {};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   if ((index & 1u) == 0) {
                                       std::memcpy(first, symbols, sizeof first);
                                       return DecodeError::ok;
                                   }
                                   return build_bc5(index / 2, first, symbols, history, ring,
                                                    destination + (index / 2) * 16, stats);
                               });
    } else if (codec == 6) {
        Bc6hState state;
        std::uint16_t first[16] = {};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   if ((index & 1u) == 0) {
                                       std::memcpy(first, symbols, sizeof first);
                                       return DecodeError::ok;
                                   }
                                   return build_bc6h(index / 2, first, symbols, history, state,
                                                     destination + (index / 2) * 16, stats);
                               });
    } else if (codec == 7) {
        (void)bc7_tables();
        Bc7State state;
        std::uint16_t first[16] = {};
        error = decode_entropy(codec, source, source_bytes, elements,
                               [&](std::size_t index, const std::uint16_t* symbols) {
                                   if ((index & 1u) == 0) {
                                       std::memcpy(first, symbols, sizeof first);
                                       return DecodeError::ok;
                                   }
                                   return build_bc7(index / 2, first, symbols, history, state,
                                                    destination + (index / 2) * 16, stats);
                               });
    }
    if (error == DecodeError::ok) {
        stats.blocks = blocks;
        stats.bytes = destination_bytes;
    }
    if (stats_out != nullptr) *stats_out = stats;
    return error;
}

}  // namespace bcpack

}  // namespace palmetto::dstorage
