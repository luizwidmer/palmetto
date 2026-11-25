// The persistent VkPipelineCache file: what the driver's cache blob must look like to be reused, how it is loaded
// with a size bound, and the atomic replace that writes it back.
//
// The blob is opaque to this layer, but its first 32 bytes are VkPipelineCacheHeaderVersionOne (Vulkan 1.3, spec
// chapter 10.8): headerSize, headerVersion, vendorID, deviceID, pipelineCacheUUID[16], all little-endian. Vulkan also
// defines the compatibility rule this file enforces: a cache whose header names another vendor, device or
// pipelineCacheUUID is incompatible and must start empty (VkPipelineCacheCreateInfo pInitialData). Nothing else in
// the blob is read here, and the header builds without the Vulkan headers so the test can link without a loader.
//
// All helpers are pure stdlib/runtime: no device, no Vulkan instance, no game files. The runtime's save path in
// d3d12_vulkan_device.cpp uses them (docs/pipeline_cache.md).
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace palmetto::pipeline_cache {

// sizeof(VkPipelineCacheHeaderVersionOne); static_asserted against the Vulkan header where one is available.
inline constexpr std::size_t header_bytes = 32;
inline constexpr std::uint32_t header_version_one = 1;

// The header layout without <vulkan/vulkan_core.h>: those 32 bytes, in one struct.
struct Header {
    std::uint32_t header_size = 0;
    std::uint32_t header_version = 0;
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    std::uint8_t uuid[16] = {};
};
static_assert(sizeof(Header) == header_bytes, "the pipeline cache header is 32 bytes");

// What the physical device reports, as VkPhysicalDeviceProperties' vendorID/deviceID and pipelineCacheUUID. The driver
// version is not part of the check: pipelineCacheUUID changes when the driver or its configuration does, and that is
// the identifier Vulkan defines for cache compatibility.
struct Identity {
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    std::uint8_t uuid[16] = {};
};

inline std::uint32_t read_u32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

// Parses the 32-byte cache header. False for a blob shorter than the header or one that names another header layout
// or version; those caches cannot be handed to vkCreatePipelineCache.
inline bool parse_header(const void* data, std::size_t size, Header& out) {
    if (!data || size < header_bytes) return false;
    const auto* p = static_cast<const unsigned char*>(data);
    Header header;
    header.header_size = read_u32(p);
    header.header_version = read_u32(p + 4);
    header.vendor = read_u32(p + 8);
    header.device = read_u32(p + 12);
    std::memcpy(header.uuid, p + 16, sizeof(header.uuid));
    if (header.header_size != sizeof(Header) || header.header_version != header_version_one) return false;
    out = header;
    return true;
}

inline bool compatible(const Header& header, const Identity& identity) {
    return header.vendor == identity.vendor && header.device == identity.device &&
           std::memcmp(header.uuid, identity.uuid, sizeof(header.uuid)) == 0;
}

enum class Status {
    ok,            // a header this device can use
    missing,       // no file: a first run
    unreadable,    // exists, but cannot be read (permissions, a directory, I/O)
    too_small,     // shorter than its own header
    too_large,     // over the caller's bound
    incompatible,  // names another device/driver, or not a version-one cache header
};

inline const char* status_name(Status status) {
    switch (status) {
        case Status::ok: return "usable";
        case Status::missing: return "missing";
        case Status::unreadable: return "unreadable";
        case Status::too_small: return "too small";
        case Status::too_large: return "over the size bound";
        case Status::incompatible: return "incompatible";
    }
    return "unknown";
}

struct Loaded {
    Status status = Status::missing;
    std::vector<char> data;  // only set when status is ok
};

// Reads the cache file with the whole blob held in memory, so the caller can hand the bytes to vkCreatePipelineCache.
// Every failure is a Status, never an exception: the runtime falls back to an empty (or build-seeded) cache.
inline Loaded load_file(const std::filesystem::path& path, std::size_t max_bytes, const Identity& identity) {
    Loaded loaded;
    if (path.empty()) return loaded;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
        // A missing file is the normal first run; anything else (a denied handle, a broken mount) is unreadable.
        loaded.status = error && error != std::errc::no_such_file_or_directory ? Status::unreadable : Status::missing;
        return loaded;
    }
    if (!std::filesystem::is_regular_file(status)) {
        loaded.status = Status::unreadable;  // a directory where the cache should be
        return loaded;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        loaded.status = Status::unreadable;
        return loaded;
    }
    if (file_size < header_bytes) {
        loaded.status = Status::too_small;
        return loaded;
    }
    if (file_size > max_bytes) {
        loaded.status = Status::too_large;
        return loaded;
    }
    std::vector<char> bytes(static_cast<std::size_t>(file_size));
    {
        std::FILE* file = nullptr;
#ifdef _WIN32
        const std::wstring wide = path.wstring();
        file = _wfopen(wide.c_str(), L"rb");
#else
        file = std::fopen(path.c_str(), "rb");
#endif
        if (!file) {
            loaded.status = Status::unreadable;
            return loaded;
        }
        const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
        const bool failed = read != bytes.size() || std::ferror(file) != 0;
        std::fclose(file);
        if (failed) {
            loaded.status = Status::unreadable;
            return loaded;
        }
    }
    Header header;
    if (!parse_header(bytes.data(), bytes.size(), header)) {
        loaded.status = Status::incompatible;
        return loaded;
    }
    if (!compatible(header, identity)) {
        loaded.status = Status::incompatible;
        return loaded;
    }
    loaded.status = Status::ok;
    loaded.data = std::move(bytes);
    return loaded;
}

// Replaces `path` with `size` bytes from `data` in one atomic step: write a unique `path.tmp.<stamp>.<n>` beside it,
// flush and close it, then rename it over the destination (retrying the Windows sharing violation a reader of the
// previous cache can cause). Creates the parent directory. Refuses more than `max_bytes` (the caller's bound) and
// refuses a blob that is not a version-one cache header. Returns an empty error_code on success; on failure the
// destination is left as it was and the temp file is removed.
inline std::error_code write_file(const std::filesystem::path& path, const void* data, std::size_t size,
                                  std::size_t max_bytes) {
    namespace fs = std::filesystem;
    if (path.empty()) return std::make_error_code(std::errc::invalid_argument);
    if (!data || size < header_bytes) return std::make_error_code(std::errc::invalid_argument);
    if (size > max_bytes) return std::make_error_code(std::errc::file_too_large);
    Header header;
    if (!parse_header(data, size, header)) return std::make_error_code(std::errc::invalid_argument);

    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return error;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path staged;
    std::FILE* file = nullptr;
    for (int attempt = 0; attempt < 8 && !file; ++attempt) {
        staged = path;
        staged += ".tmp." + std::to_string(stamp) + "." + std::to_string(attempt);
#ifdef _WIN32
        const std::wstring wide = staged.wstring();
        file = _wfopen(wide.c_str(), L"wbx");
#else
        file = std::fopen(staged.c_str(), "wbx");
#endif
        if (!file && errno != EEXIST) break;
    }
    if (!file) return {errno ? errno : EIO, std::generic_category()};

    // Keep every individual CRT write modest even when a driver's cache is large.
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t remaining = size;
    while (remaining > 0 && !error) {
        const std::size_t chunk = (std::min)(remaining, std::size_t{16} << 20);
        if (std::fwrite(bytes, 1, chunk, file) != chunk || std::ferror(file))
            error = {errno ? errno : EIO, std::generic_category()};
        bytes += chunk;
        remaining -= chunk;
    }
    if (!error && std::fflush(file) != 0) error = {errno ? errno : EIO, std::generic_category()};
    if (std::fclose(file) != 0 && !error) error = {errno ? errno : EIO, std::generic_category()};
    if (!error) {
        fs::rename(staged, path, error);
#ifdef _WIN32
        // A process still reading the previous cache can hold it without FILE_SHARE_DELETE for a moment.
        for (int retry = 0; retry < 8 && error == std::errc::permission_denied; ++retry)
            fs::rename(staged, path, error);
#endif
    }
    if (error) {
        std::error_code ignored;
        fs::remove(staged, ignored);
    }
    return error;
}

}  // namespace palmetto::pipeline_cache
