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

// A switch is on for any value except the empty string, "0", "off"/"OFF" and "no"/"NO".
inline bool env_switch(const char* name) {
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof value);
    if (n == 0 || n >= sizeof value) return false;
    return std::strcmp(value, "0") != 0 && _stricmp(value, "off") != 0 && _stricmp(value, "no") != 0;
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
    std::uint64_t file_bytes = 0;
    unsigned source_files = 0;  // a directory can supply two
};

namespace detail {

// The predictor JSON is integers only, so the parser is a bounded integer/array/object reader rather than a general
// JSON implementation. Caps keep a malformed file from turning into an allocation: 8 MiB read, depth 8, 200,000
// integers, 64 keys per object.
struct JsonValue {
    enum class Type { null_value, boolean, number, string, array, object } type = Type::null_value;
    long long number = 0;
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
            std::string ignored;
            if (!string(ignored)) return false;
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

inline PredictorError read_predictor_section(const JsonValue& document, PredictorReport& report) {
    // A combined file carries the codec sections; a single-codec file is the section itself.
    if (document.type != JsonValue::Type::object) return PredictorError::malformed;
    const JsonValue* bc6h = document.member("bc6h");
    const JsonValue* bc7 = document.member("bc7");
    if (bc6h == nullptr && bc7 == nullptr) {
        if (document.member("endpointHistory") != nullptr) bc6h = &document;
        else if (document.member("historyEndpoints") != nullptr) bc7 = &document;
        else return PredictorError::no_codec_section;
    }
    if (bc6h != nullptr && bc6h->type != JsonValue::Type::object) return PredictorError::malformed;
    if (bc7 != nullptr && bc7->type != JsonValue::Type::object) return PredictorError::malformed;
    if (bc6h != nullptr) {
        const PredictorError error = validate_bc6h(*bc6h, report.bc6h_endpoint_rows, report.bc6h_index_rows);
        if (error != PredictorError::ok) return error;
        report.bc6h = true;
    }
    if (bc7 != nullptr) {
        const PredictorError error = validate_bc7(*bc7, report.bc7_endpoint_rows, report.bc7_index_rows);
        if (error != PredictorError::ok) return error;
        report.bc7 = true;
    }
    return PredictorError::ok;
}

}  // namespace detail

// The bounded predictor file size: the calibration tool's combined JSON for all six codecs is well under this, and a
// larger file is a wrong file rather than a larger table.
inline constexpr std::size_t kMaxPredictorFileBytes = 8u * 1024u * 1024u;

// Loads and validates the local BCPack predictor file at `path`. A directory is read as <codec>.json (the calibration
// tool's --out directory), a file is read as a combined or single-codec JSON. The values are validated and counted but
// not retained: this slice has no BCPack block decoder to consume them (docs/directstorage_advanced.md).
inline PredictorError load_dstorage_predictor(const wchar_t* path, PredictorReport& report) {
    report = PredictorReport{};
    if (path == nullptr || path[0] == L'\0') return PredictorError::missing;
    std::error_code ec;
    const std::filesystem::path root(path);
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(root, ec)) {
        for (const char* codec : {"bc6h", "bc7"}) {
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
        if ((error = detail::read_predictor_section(document, report)) != PredictorError::ok) return error;
    }
    return PredictorError::ok;
}

}  // namespace palmetto::dstorage
