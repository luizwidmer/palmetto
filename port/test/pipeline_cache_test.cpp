// The persistent pipeline cache file path without a Vulkan device: header parsing, the compatibility rule, the bounded
// load and its failure statuses, and the atomic replace with its bounded and invalid-blob refusals.
//
// The blobs here are synthetic: a hand-built VkPipelineCacheHeaderVersionOne followed by arbitrary managed payload
// bytes. No Vulkan loader, no device and no game files are used (docs/pipeline_cache.md).
#include "pipeline_cache.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pc = palmetto::pipeline_cache;

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

static void put_u32(std::vector<char>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

static pc::Identity identity(std::uint32_t vendor = 0x10de, std::uint32_t device = 0x2684) {
    pc::Identity id;
    id.vendor = vendor;
    id.device = device;
    for (std::size_t i = 0; i < sizeof(id.uuid); ++i) id.uuid[i] = static_cast<std::uint8_t>(i + 1);
    return id;
}

// A blob a driver could have written: the 32-byte header the identity names, then `payload` managed bytes.
static std::vector<char> blob(const pc::Identity& id, std::size_t payload = 64, char fill = 'p') {
    std::vector<char> data;
    put_u32(data, 32);  // headerSize
    put_u32(data, 1);   // headerVersion
    put_u32(data, id.vendor);
    put_u32(data, id.device);
    data.insert(data.end(), std::begin(id.uuid), std::end(id.uuid));
    data.resize(data.size() + payload, fill);
    return data;
}

static std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

static void write_bytes(const fs::path& path, const std::vector<char>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

static int test_header() {
    const auto id = identity();
    const auto bytes = blob(id, 8, 'h');
    pc::Header header;
    CHECK(!pc::parse_header(nullptr, 0, header));
    CHECK(!pc::parse_header(bytes.data(), pc::header_bytes - 1, header));
    CHECK(pc::parse_header(bytes.data(), bytes.size(), header));
    CHECK(header.header_size == 32);
    CHECK(header.header_version == 1);
    CHECK(header.vendor == id.vendor);
    CHECK(header.device == id.device);
    CHECK(std::memcmp(header.uuid, id.uuid, sizeof(id.uuid)) == 0);
    CHECK(pc::compatible(header, id));
    CHECK(!pc::compatible(header, identity(id.vendor, id.device + 1)));
    CHECK(!pc::compatible(header, identity(id.vendor + 1, id.device)));
    auto other_uuid = id;
    other_uuid.uuid[15] ^= 0xff;
    CHECK(!pc::compatible(header, other_uuid));

    // Another header layout or version is not a cache this device may be handed.
    auto wrong_size = bytes;
    wrong_size[0] = 16;
    CHECK(!pc::parse_header(wrong_size.data(), wrong_size.size(), header));
    auto wrong_version = bytes;
    wrong_version[4] = 2;
    CHECK(!pc::parse_header(wrong_version.data(), wrong_version.size(), header));
    CHECK(std::string(pc::status_name(pc::Status::ok)) == "usable");
    return 0;
}

static int test_load(const fs::path& dir) {
    const auto id = identity();
    const std::size_t max = 1u << 20;
    // No file: a first run, not an error.
    CHECK(pc::load_file(dir / "absent.bin", max, id).status == pc::Status::missing);
    // Empty and short files.
    write_bytes(dir / "empty.bin", {});
    CHECK(pc::load_file(dir / "empty.bin", max, id).status == pc::Status::too_small);
    write_bytes(dir / "short.bin", std::vector<char>(pc::header_bytes - 1, 'x'));
    CHECK(pc::load_file(dir / "short.bin", max, id).status == pc::Status::too_small);
    // A usable cache comes back byte for byte.
    const auto good = blob(id, 128, 'g');
    write_bytes(dir / "good.bin", good);
    {
        const auto loaded = pc::load_file(dir / "good.bin", max, id);
        CHECK(loaded.status == pc::Status::ok);
        CHECK(loaded.data == good);
    }
    // A corrupt file is incompatible, not fatal.
    auto corrupt = good;
    corrupt[0] = 0;
    write_bytes(dir / "corrupt.bin", corrupt);
    CHECK(pc::load_file(dir / "corrupt.bin", max, id).status == pc::Status::incompatible);
    // A cache from another GPU or driver.
    write_bytes(dir / "other_device.bin", blob(identity(0x1002, 0x73ff), 32));
    CHECK(pc::load_file(dir / "other_device.bin", max, id).status == pc::Status::incompatible);
    // A cache over the caller's bound is refused rather than read into memory.
    CHECK(pc::load_file(dir / "good.bin", pc::header_bytes - 1, id).status == pc::Status::too_large);
    // A directory where the file should be is unreadable, not missing.
    CHECK(fs::create_directory(dir / "adirectory"));
    CHECK(pc::load_file(dir / "adirectory", max, id).status == pc::Status::unreadable);
    return 0;
}

static int test_write(const fs::path& dir) {
    const auto id = identity();
    const std::size_t max = 1u << 20;
    // The parent directory does not exist yet: it is created.
    const auto nested = dir / "a" / "b" / "cache.bin";
    const auto first = blob(id, 256, '1');
    CHECK(!pc::write_file(nested, first.data(), first.size(), max));
    CHECK(fs::is_regular_file(nested));
    CHECK(contents(nested) == std::string(first.begin(), first.end()));
    {
        const auto loaded = pc::load_file(nested, max, id);
        CHECK(loaded.status == pc::Status::ok && loaded.data == first);
    }
    // A second, shorter blob replaces the first in one step.
    const auto second = blob(id, 8, '2');
    CHECK(!pc::write_file(nested, second.data(), second.size(), max));
    CHECK(contents(nested) == std::string(second.begin(), second.end()));
    // Refusals: over the bound, shorter than a header, and no version-one header.
    CHECK(pc::write_file(nested, first.data(), first.size(), pc::header_bytes - 1) ==
          std::make_error_code(std::errc::file_too_large));
    CHECK(pc::write_file(nested, first.data(), 4, max) == std::make_error_code(std::errc::invalid_argument));
    auto corrupt = first;
    corrupt[4] = 9;
    CHECK(pc::write_file(nested, corrupt.data(), corrupt.size(), max) ==
          std::make_error_code(std::errc::invalid_argument));
    CHECK(contents(nested) == std::string(second.begin(), second.end()));
    // A failed replace leaves the destination and no temp file. An existing directory is not replaceable.
    CHECK(fs::create_directory(dir / "replace_dir"));
    CHECK(pc::write_file(dir / "replace_dir", second.data(), second.size(), max));
    CHECK(fs::is_directory(dir / "replace_dir"));
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++entries;
    }
    // empty, short, good, corrupt, other_device (test_load) + a, replace_dir (test_write) + adirectory: no temp leftover.
    CHECK(entries == 8);
    return 0;
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / ("palmetto_pipeline_cache_validation_" + std::to_string(stamp));
    CHECK(fs::create_directory(dir));
    int result = test_header();
    if (!result) result = test_load(dir);
    if (!result) result = test_write(dir);
    fs::remove_all(dir);
    if (result) return result;
    std::printf("pipeline cache validation: ok\n");
    return 0;
}
