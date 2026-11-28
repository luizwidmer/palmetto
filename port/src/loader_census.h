// Pure, bounded census formatter for the runtime PE loader.
//
// The loader can already map, bind and protect an image with no positive record outside the opt-in startup trace:
// module base/size/relocation facts and import counts are invisible in stderr, VirtualProtect results are dropped, and
// thread attach/detach events are deliberately untraced (docs/pe_loader_rewrite.md, inherited-loader audit). This layer
// turns validated loader facts into one bounded, named stderr line each, so a boot can be read from the harness
// summary alone. It formats only what the caller supplies: no host calls, no I/O, no loader policy change, and the one
// environment read is the exact switch below. With the switch off the insertion hunks perform no formatting and the
// loader's output is byte-identical to the tree without this layer.
//
// Line kinds (all prefixed '[rt] loader: census v1 '):
//   mapped <name> base=0x.. size=0x.. preferred=0x.. relocated=true|false entry=+0x..
//   bound <name> total=.. shim=.. host=.. module=.. unimplemented=..
//   tls <name> index=..|none slot=..|none ga_slot=..|none ga_sites=..
//   tls-counters attach=.. detach=.. failures=..
//   protect-failed module=<m> headers|section[i][=name] rva=0x.. size=0x.. prot=0x.. error=..
//   protect-omitted count=..
//
// Module names are bounded to kMaxNameBytes bytes, non-printable bytes are replaced with '?', and a truncated name ends
// with '...', so one caller-supplied name can never split or unbound a line. Every formatter returns the line length
// (without a newline) or 0 when the output buffer is invalid or too small; a refused call never writes at or past
// `capacity` (a truncated prefix may remain in the buffer, as std::snprintf leaves it).
//
// Header-only on purpose: the insertion hunks add one include to pe_loader.cpp, so the 22 targets that already compile
// that file need no new source file and port/CMakeLists.txt only gains the pure test target.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace rt::loader_census {

// The one switch. Only the exact value "1" enables it; unset, empty, "0", "true" and every other spelling stay off.
inline constexpr const char kSwitchName[] = "PALMETTO_LOADER_CENSUS";
inline constexpr std::size_t kMaxLineBytes = 512;
inline constexpr std::size_t kMaxNameBytes = 96;
// The insertion hunks stop after this many failed VirtualProtect calls and print one omission line instead.
inline constexpr std::uint32_t kMaxProtectFailures = 32;
// Matches rt::kNoTlsIndex in pe_loader.h without making this header depend on the loader.
inline constexpr std::uint32_t kNoTlsIndex = 0xFFFFFFFFu;

struct ModuleFacts {
    std::string_view name;
    std::uint64_t base = 0;            // the address the image was actually mapped at
    std::uint64_t preferred_base = 0;  // ImageBase from the file header
    std::uint32_t size = 0;            // SizeOfImage
    std::uint32_t entry_rva = 0;
    bool relocated = false;            // the mapped base differs from ImageBase because relocations ran
};

struct BindCounts {
    std::uint32_t shim = 0;
    std::uint32_t host = 0;
    std::uint32_t module = 0;
    std::uint32_t unimplemented = 0;
    // The IAT slots bound in total; computed, so a caller cannot print a total that disagrees with its parts.
    constexpr std::uint64_t total() const noexcept {
        return static_cast<std::uint64_t>(shim) + host + module + unimplemented;
    }
};

struct BindFacts {
    std::string_view name;
    BindCounts counts{};
};

struct TlsFacts {
    std::string_view name;
    std::uint32_t index = kNoTlsIndex;    // _tls_index / module TLS index; kNoTlsIndex prints 'none'
    std::uint32_t slot = kNoTlsIndex;     // the loader's reserved TLS-host slot; kNoTlsIndex prints 'none'
    std::uint32_t ga_slot = kNoTlsIndex;  // the /GA TlsAlloc cell; kNoTlsIndex prints 'none'
    std::uint64_t ga_sites = 0;           // rewritten gs:[58h] reads
};

// Process-wide thread-event counters: one attach/detach increment per helper DLL thread event, one failure per
// per-slot install/callback exception. Counted only while the switch is on, via Collector below.
struct TlsCounters {
    std::uint64_t attach_events = 0;
    std::uint64_t detach_events = 0;
    std::uint64_t failures = 0;
};

enum class ProtectTarget { headers, section };

struct ProtectFailure {
    std::string_view module;
    ProtectTarget target = ProtectTarget::headers;
    std::uint32_t section_index = 0;  // meaningful for ProtectTarget::section
    std::string_view section_name;    // IMAGE_SECTION_HEADER name, up to eight bytes; empty prints no '=name'
    std::uint64_t rva = 0;            // image-relative address the protection was requested for
    std::uint64_t size = 0;
    std::uint32_t requested_protection = 0;  // the raw PAGE_* value passed to VirtualProtect
    std::uint32_t last_error = 0;            // GetLastError() after the failed call
};

// Each formatter writes the line (no trailing newline) into `output` and returns its length, or returns 0 when `output`
// is null, `capacity` is 0, or the line does not fit; see the truncation note above.
std::size_t format_mapped(const ModuleFacts& facts, char* output, std::size_t capacity) noexcept;
std::size_t format_binds(const BindFacts& facts, char* output, std::size_t capacity) noexcept;
std::size_t format_tls(const TlsFacts& facts, char* output, std::size_t capacity) noexcept;
std::size_t format_tls_counters(const TlsCounters& counters, char* output, std::size_t capacity) noexcept;
std::size_t format_protect_failure(const ProtectFailure& failure, char* output, std::size_t capacity) noexcept;
// The caller bounds failure lines at kMaxProtectFailures; this names how many it did not print.
std::size_t format_protect_omitted(std::uint64_t count, char* output, std::size_t capacity) noexcept;

// Interpret one environment value; the loader reads the switch once. Pure.
inline bool switch_enabled_from(const char* value) noexcept {
    // Intentionally exact: no aliases, whitespace or truthy spellings silently turn diagnostics on.
    return value && value[0] == '1' && value[1] == '\0';
}

// The exact switch as this process spells it. Reads the environment once; the only environment access in this layer.
// std::getenv is the tree's switch idiom (pe_loader.cpp, startup_trace.h); this layer reads one value and copies nothing.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
inline bool enabled() noexcept {
    static const bool on = [] { return switch_enabled_from(std::getenv(kSwitchName)); }();
    return on;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Optional convenience the insertion hunks use for the thread-event counters. The pure formatters above are the
// contract; a caller is free to keep its own TlsCounters instead. Every note_* is a relaxed atomic increment that
// does nothing when disabled, so the default path pays one predictable branch per thread event and formats nothing.
class Collector {
public:
    explicit Collector(bool enabled) noexcept : enabled_(enabled) {}
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    bool enabled() const noexcept { return enabled_; }
    void note_attach() noexcept {
        if (enabled_) attach_events_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_detach() noexcept {
        if (enabled_) detach_events_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_failure() noexcept {
        if (enabled_) failures_.fetch_add(1, std::memory_order_relaxed);
    }
    TlsCounters snapshot() const noexcept {
        TlsCounters counters;
        if (enabled_) {
            counters.attach_events = attach_events_.load(std::memory_order_relaxed);
            counters.detach_events = detach_events_.load(std::memory_order_relaxed);
            counters.failures = failures_.load(std::memory_order_relaxed);
        }
        return counters;
    }

private:
    const bool enabled_;
    std::atomic<std::uint64_t> attach_events_{0};
    std::atomic<std::uint64_t> detach_events_{0};
    std::atomic<std::uint64_t> failures_{0};
};

// The loader's process-wide collector, enabled from kSwitchName once.
inline Collector& process_counters() noexcept {
    static Collector collector(enabled());
    return collector;
}

namespace detail {

constexpr char kPrefix[] = "[rt] loader: census v1 ";
constexpr char kTruncationMarker[] = "...";
constexpr std::size_t kTruncationMarkerBytes = sizeof(kTruncationMarker) - 1;
// Enough for a sanitised, truncated name and its NUL.
constexpr std::size_t kNameBufferBytes = kMaxNameBytes + kTruncationMarkerBytes + 1;
// Enough for 'section[65535]=NAME', 'headers' or a decimal TLS index.
constexpr std::size_t kWhereBufferBytes = 8 + 8 + 1 + kNameBufferBytes;

// A bounded, single-line module name: non-printable bytes become '?', bytes past kMaxNameBytes are replaced by the
// truncation marker. Returns the number of bytes written before the NUL.
inline std::size_t copy_name(std::string_view name, char* output) noexcept {
    const std::size_t limit = name.size() < kMaxNameBytes ? name.size() : kMaxNameBytes;
    std::size_t written = 0;
    for (; written < limit; ++written) {
        const unsigned char c = static_cast<unsigned char>(name[written]);
        output[written] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    }
    if (name.size() > kMaxNameBytes) {
        for (std::size_t i = 0; i < kTruncationMarkerBytes; ++i) output[written++] = kTruncationMarker[i];
    }
    output[written] = '\0';
    return written;
}

// 'none' or a decimal TLS index/slot.
inline std::size_t copy_index(std::uint32_t value, char* output, std::size_t capacity) noexcept {
    if (value == kNoTlsIndex) {
        std::snprintf(output, capacity, "none");
        return 4;
    }
    const int written = std::snprintf(output, capacity, "%u", value);
    return written > 0 && static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written) : 0;
}

// std::snprintf returns the needed length, so a line that did not fit is reported as no line at all.
inline std::size_t finish(int written, char* output, std::size_t capacity) noexcept {
    if (!output || !capacity || written <= 0) return 0;
    return static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written) : 0;
}

}  // namespace detail

inline std::size_t format_mapped(const ModuleFacts& facts, char* output, std::size_t capacity) noexcept {
    char name[detail::kNameBufferBytes];
    detail::copy_name(facts.name, name);
    const int written = std::snprintf(
        output, capacity,
        "%smapped %s base=0x%llx size=0x%x preferred=0x%llx relocated=%s entry=+0x%x", detail::kPrefix, name,
        static_cast<unsigned long long>(facts.base), facts.size,
        static_cast<unsigned long long>(facts.preferred_base), facts.relocated ? "true" : "false",
        facts.entry_rva);
    return detail::finish(written, output, capacity);
}

inline std::size_t format_binds(const BindFacts& facts, char* output, std::size_t capacity) noexcept {
    char name[detail::kNameBufferBytes];
    detail::copy_name(facts.name, name);
    const BindCounts& counts = facts.counts;
    const int written = std::snprintf(
        output, capacity,
        "%sbound %s total=%llu shim=%u host=%u module=%u unimplemented=%u", detail::kPrefix, name,
        static_cast<unsigned long long>(counts.total()), counts.shim, counts.host, counts.module,
        counts.unimplemented);
    return detail::finish(written, output, capacity);
}

inline std::size_t format_tls(const TlsFacts& facts, char* output, std::size_t capacity) noexcept {
    char name[detail::kNameBufferBytes];
    detail::copy_name(facts.name, name);
    char index[12];
    char slot[12];
    char ga_slot[12];
    if (!detail::copy_index(facts.index, index, sizeof index) ||
        !detail::copy_index(facts.slot, slot, sizeof slot) ||
        !detail::copy_index(facts.ga_slot, ga_slot, sizeof ga_slot))
        return 0;
    const int written = std::snprintf(
        output, capacity, "%stls %s index=%s slot=%s ga_slot=%s ga_sites=%llu", detail::kPrefix, name, index, slot,
        ga_slot, static_cast<unsigned long long>(facts.ga_sites));
    return detail::finish(written, output, capacity);
}

inline std::size_t format_tls_counters(const TlsCounters& counters, char* output, std::size_t capacity) noexcept {
    const int written = std::snprintf(
        output, capacity, "%stls-counters attach=%llu detach=%llu failures=%llu", detail::kPrefix,
        static_cast<unsigned long long>(counters.attach_events),
        static_cast<unsigned long long>(counters.detach_events),
        static_cast<unsigned long long>(counters.failures));
    return detail::finish(written, output, capacity);
}

inline std::size_t format_protect_failure(const ProtectFailure& failure, char* output,
                                          std::size_t capacity) noexcept {
    char module[detail::kNameBufferBytes];
    detail::copy_name(failure.module, module);
    char where[detail::kWhereBufferBytes];
    if (failure.target == ProtectTarget::headers) {
        std::snprintf(where, sizeof where, "headers");
    } else {
        char section_name[detail::kNameBufferBytes];
        detail::copy_name(failure.section_name, section_name);
        if (section_name[0])
            std::snprintf(where, sizeof where, "section[%u]=%s", failure.section_index, section_name);
        else
            std::snprintf(where, sizeof where, "section[%u]", failure.section_index);
    }
    const int written = std::snprintf(
        output, capacity, "%sprotect-failed module=%s %s rva=0x%llx size=0x%llx prot=0x%x error=%u",
        detail::kPrefix, module, where, static_cast<unsigned long long>(failure.rva),
        static_cast<unsigned long long>(failure.size), failure.requested_protection, failure.last_error);
    return detail::finish(written, output, capacity);
}

inline std::size_t format_protect_omitted(std::uint64_t count, char* output, std::size_t capacity) noexcept {
    const int written = std::snprintf(output, capacity, "%sprotect-omitted count=%llu", detail::kPrefix,
                                      static_cast<unsigned long long>(count));
    return detail::finish(written, output, capacity);
}

}  // namespace rt::loader_census