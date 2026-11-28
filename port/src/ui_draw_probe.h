// Measured UI 2D-batch entry sites and the pure byte-shape refusal rule for
// PALMETTO_UI_PROBE=draw (docs/decomp_first_ui.md, sections 6 and 19).
// The signatures are short instruction prefixes from the retail image; the
// installer patches only a site whose RVA is in the mapped image and whose
// prefix still matches. The two first sites share the same measured 10-byte
// prologue; their distinct measured RVAs identify the separate call targets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace palmetto::ui_draw_probe {

inline constexpr unsigned kSiteCount = 4;
inline constexpr std::uint32_t kStageWordRva = 0x7446510u;

enum SiteIndex : unsigned {
    kBatchBuild = 0,  // 0x517A290: builds 0x18-byte 2D batch entries
    kRectangleFill,   // 0x3A1B860: positioned/coloured rectangle from a transform
    kTransformCopy,   // 0x58C8950: copies the 16-float draw transform
    kBatchSubmit,     // 0x2AACF0: submits the batch
};

struct SiteShape {
    const char* name;
    std::uint32_t rva;
    const std::uint8_t* signature;
    std::size_t signature_length;
};

// The first two functions share the six register saves covered by this
// 10-byte prefix. Their full prologues also save RBP/RBX and then diverge at
// the stack reservation (0x28 vs 0x38); each measured RVA remains explicit.
inline constexpr std::uint8_t kBatchBuildSignature[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
};
inline constexpr std::uint8_t kRectangleFillSignature[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57,
};
inline constexpr std::uint8_t kTransformCopySignature[] = {
    0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCF,
};
inline constexpr std::uint8_t kBatchSubmitSignature[] = {
    0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20,
};

inline constexpr SiteShape kSites[kSiteCount] = {
    {"batch-build", 0x517A290u, kBatchBuildSignature, sizeof kBatchBuildSignature},
    {"rectangle-fill", 0x3A1B860u, kRectangleFillSignature, sizeof kRectangleFillSignature},
    {"transform-copy", 0x58C8950u, kTransformCopySignature, sizeof kTransformCopySignature},
    {"batch-submit", 0x2AACF0u, kBatchSubmitSignature, sizeof kBatchSubmitSignature},
};

static_assert(sizeof kSites / sizeof kSites[0] == kSiteCount, "every measured 2D-batch site is in the table");

enum class Validation {
    Ok,
    InvalidSite,
    ImageTooSmall,
    MissingBytes,
    ShortBytes,
    SignatureMismatch,
};

inline const char* validation_name(Validation result) {
    switch (result) {
        case Validation::Ok: return "validated";
        case Validation::InvalidSite: return "unknown site";
        case Validation::ImageTooSmall: return "site signature is outside the mapped image";
        case Validation::MissingBytes: return "site bytes are unreadable";
        case Validation::ShortBytes: return "site byte span is shorter than the measured signature";
        case Validation::SignatureMismatch: return "site bytes do not match the measured instruction shape";
    }
    return "unknown validation result";
}

// Pure validation seam. `bytes` starts at the site's RVA; the installer also
// checks that the mapped page is committed and executable before it reads or
// patches the instruction.
inline Validation validate_site(unsigned site, std::size_t image_size, const std::uint8_t* bytes,
                                std::size_t available) {
    if (site >= kSiteCount) return Validation::InvalidSite;
    const SiteShape& shape = kSites[site];
    if (shape.rva > image_size || shape.signature_length > image_size - shape.rva)
        return Validation::ImageTooSmall;
    if (bytes == nullptr) return Validation::MissingBytes;
    if (available < shape.signature_length) return Validation::ShortBytes;
    return std::memcmp(bytes, shape.signature, shape.signature_length) == 0 ? Validation::Ok
                                                                            : Validation::SignatureMismatch;
}

// ---- Bounded caller/return-site sampling (round 22) -----------------------------------------------
// Section 19 capped the four site counters at 4,096 hits before stage 3 but recorded entries only, so a
// hit could not be attributed to the code that called the site. Each measured site is a function entry
// reached by `call`, so the trapping thread's RSP points at the return address before any prologue
// runs. The ring below keeps at most kCallerRingCapacity distinct return addresses per site and the
// reporter renders one fixed-size line from it: the sample is bounded in entries and in logged
// characters, it allocates nothing and dumps no title memory or registers. The runtime handler owns the
// return-address read (its existing guarded read seam) and the try-lock around this state; this header
// stays pure state and pure formatting so the deterministic test can drive it with synthetic addresses.
inline constexpr unsigned kCallerRingCapacity = 16;

enum class CallerKind : unsigned {
    None,      // no return address was readable
    Image,     // return address lies inside the mapped image (an RVA is reported)
    External,  // return address lies outside the mapped image (host or shim code)
};

inline const char* caller_kind_name(CallerKind kind) {
    switch (kind) {
        case CallerKind::None: return "none";
        case CallerKind::Image: return "image";
        case CallerKind::External: return "external";
    }
    return "unknown caller kind";
}

// Classifies one return address against the mapped image. `rva` receives the image-relative value for
// CallerKind::Image only; an address exactly at `image_base` is image RVA 0, and the exclusive end
// `image_base + image_size` is outside.
inline CallerKind classify_return_address(std::uint64_t return_address, std::uint64_t image_base,
                                          std::size_t image_size, std::uint32_t* rva) {
    if (rva) *rva = 0;
    if (return_address == 0) return CallerKind::None;
    if (return_address < image_base) return CallerKind::External;
    const std::uint64_t offset = return_address - image_base;
    if (offset >= image_size || offset > 0xFFFFFFFFull) return CallerKind::External;
    if (rva) *rva = static_cast<std::uint32_t>(offset);
    return CallerKind::Image;
}

enum class CallerRecord : unsigned {
    Recorded,         // a new distinct return address joined the ring
    Duplicate,        // the return address is already held; the ring did not move
    RecordedEvicted,  // a new distinct return address replaced the oldest one
    Unreadable,       // no return address was available
};

struct CallerRing {
    std::uint64_t address[kCallerRingCapacity] = {};  // raw return addresses, ring slots
    unsigned count = 0;                               // occupied slots, <= kCallerRingCapacity
    unsigned oldest = 0;                              // oldest slot index once count == kCallerRingCapacity
    std::uint32_t samples = 0;                        // every record_caller() call (saturating)
    std::uint32_t evicted = 0;                        // distinct return addresses that left the ring (saturating)
    std::uint32_t unreadable = 0;                     // record_caller() calls with no address (saturating)
};

inline void saturating_increment(std::uint32_t& value) {
    if (value != 0xFFFFFFFFu) ++value;
}

inline unsigned caller_slot(const CallerRing& ring, unsigned position) {
    if (position >= ring.count) return kCallerRingCapacity;
    return ring.count == kCallerRingCapacity ? (ring.oldest + position) % kCallerRingCapacity : position;
}

// The `position`-th oldest return address currently held; 0 when the position is out of range.
inline std::uint64_t caller_at(const CallerRing& ring, unsigned position) {
    const unsigned slot = caller_slot(ring, position);
    return slot == kCallerRingCapacity ? 0 : ring.address[slot];
}

// Records one return address. The ring deduplicates what it holds: a caller that is already sampled is
// counted as a sample but cannot fill the ring, so one hot loop cannot crowd out a second caller; a new
// distinct caller replaces the oldest entry once the ring is full. Both paths are O(kCallerRingCapacity).
inline CallerRecord record_caller(CallerRing& ring, std::uint64_t return_address) {
    saturating_increment(ring.samples);
    if (return_address == 0) {
        saturating_increment(ring.unreadable);
        return CallerRecord::Unreadable;
    }
    for (unsigned position = 0; position < ring.count; ++position)
        if (caller_at(ring, position) == return_address) return CallerRecord::Duplicate;
    if (ring.count < kCallerRingCapacity) {
        ring.address[ring.count] = return_address;
        ++ring.count;
        return CallerRecord::Recorded;
    }
    ring.address[ring.oldest] = return_address;
    ring.oldest = (ring.oldest + 1) % kCallerRingCapacity;
    saturating_increment(ring.evicted);
    return CallerRecord::RecordedEvicted;
}

// One fixed-size rendering of the ring, oldest entry first: `image+0x<rva>` for an image caller and
// `ext:0x<address>` for any other readable address. Returns the number of characters the complete
// rendering needs (snprintf convention) and always NUL-terminates when `out_size > 0`; it never writes
// past `out_size`, so the reporter's line is bounded by the ring capacity.
inline std::size_t format_callers(const CallerRing& ring, std::uint64_t image_base, std::size_t image_size,
                                  char* out, std::size_t out_size) {
    std::size_t used = 0;
    const auto put = [&](const char* text) {
        for (const char* at = text; *at; ++at) {
            if (out != nullptr && used + 1 < out_size) out[used] = *at;
            ++used;
        }
    };
    put("[");
    for (unsigned position = 0; position < ring.count; ++position) {
        if (position != 0) put(" ");
        const std::uint64_t address = caller_at(ring, position);
        std::uint32_t rva = 0;
        char entry[32];
        if (classify_return_address(address, image_base, image_size, &rva) == CallerKind::Image)
            std::snprintf(entry, sizeof entry, "image+0x%x", rva);
        else
            std::snprintf(entry, sizeof entry, "ext:0x%llx", static_cast<unsigned long long>(address));
        put(entry);
    }
    put("]");
    if (out != nullptr && out_size > 0) out[used < out_size ? used : out_size - 1] = '\0';
    return used;
}

}  // namespace palmetto::ui_draw_probe
