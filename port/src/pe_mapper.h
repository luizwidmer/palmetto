// Bounds-checked mapper for a parsed PE32+ image. Mapping never invokes image code.
#pragma once

#include "pe_image.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rt::pe::mapper {

// Values match the Win32 PAGE_* constants but keep this interface independent of windows.h.
enum class PageProtection : std::uint32_t {
    no_access = 0x01,
    read_only = 0x02,
    read_write = 0x04,
    execute = 0x10,
    execute_read = 0x20,
    execute_read_write = 0x40,
};

// A section's mapped extent and the amount of file-backed data copied into it.
struct MappedSection {
    std::string name;
    std::uint32_t rva = 0;
    std::uint32_t mapped_size = 0;
    std::uint32_t copied_size = 0;
    std::uint32_t characteristics = 0;
};

struct ProtectionRegion {
    std::string name;
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    PageProtection protection = PageProtection::no_access;
};

// The mapping is non-owning. The caller owns `base` even when map() throws after allocation and must release it.
struct MappedImage {
    std::uint8_t* base = nullptr;
    std::uint32_t size = 0;
    bool relocated = false;
    std::vector<MappedSection> sections;
    std::vector<ProtectionRegion> protect_plan;
};

// Allocation is one-shot: try the preferred base first; the implementation may return another base only when
// allow_rebase is true. It must return writable storage of `size` bytes or nullptr. map() never calls free/release.
class Allocator {
public:
    virtual ~Allocator() = default;
    // Fake storage allocators may intentionally accept arbitrary preferred addresses; Windows-like allocators opt in
    // so map_image also enforces the loader's 64 KiB ImageBase granularity.
    virtual bool enforces_windows_image_base_alignment() const noexcept { return false; }
    virtual std::uint8_t* allocate(std::uint64_t preferred_base, std::size_t size, bool allow_rebase) = 0;
};

// Protects one region after copying and relocation are complete. `false` is a named mapping refusal.
class Protector {
public:
    virtual ~Protector() = default;
    virtual bool protect(std::uint8_t* address, std::size_t size, PageProtection protection) = 0;
};

// The injected overload is deterministic and testable without reserving a foreign image address. The convenience
// overload uses VirtualAlloc/VirtualProtect on Windows.
MappedImage map_image(const Image& image, Allocator& allocator, Protector& protector);
MappedImage map_image(const Image& image);

}  // namespace rt::pe::mapper
