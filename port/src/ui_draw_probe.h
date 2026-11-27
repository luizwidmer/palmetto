// Measured UI 2D-batch entry sites and the pure byte-shape refusal rule for
// PALMETTO_UI_PROBE=draw (docs/decomp_first_ui.md, sections 6 and 19).
// The signatures are short instruction prefixes from the retail image; the
// installer patches only a site whose RVA is in the mapped image and whose
// prefix still matches. The two first sites share the same measured 10-byte
// prologue; their distinct measured RVAs identify the separate call targets.
#pragma once

#include <cstddef>
#include <cstdint>
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

}  // namespace palmetto::ui_draw_probe
