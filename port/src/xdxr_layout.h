// XDXR BVH4 layout: the byte layout of a structure from its element count alone. Pure arithmetic, no Vulkan, so
// GPU-free tests can use it: xdxr_build.h builds it on the GPU, xdxr_route.h answers the builder-shaped prebuild
// from it, and d3d12_vulkan_device.cpp uses it for the D3D12 API paths. docs/bvh.md "Building runtime BVHs" has
// the derivation of every number here.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace palmetto {

struct XdxrLayout {
    static constexpr std::uint32_t header_size = 64;

    bool tlas = false;
    bool box16 = false;                // inner nodes are BOX16
    std::uint32_t node_size = 128;     // BOX32 128 bytes, BOX16 64
    std::uint32_t elements = 0;        // triangles (BLAS) or instances (TLAS)
    std::uint32_t element_size = 64;   // a leaf, or a 128-byte instance record
    std::uint32_t nodes = 0;           // the most BOX32 nodes a build can write
    std::uint32_t parents_off = header_size;
    std::uint32_t elements_off = 0;
    std::uint32_t nodes_off = 0;
    std::uint32_t size = 0;            // result data size in bytes, a multiple of 64
    std::uint32_t parent_entries = 0;  // one per 64-byte unit from elements_off on
    std::uint32_t sort_size = 0;       // keys the radix sort orders: one per element
    static constexpr std::uint32_t radix_tile = 256;  // xdxr_build.comp RADIX_TILE
    static constexpr std::uint32_t radix_bins = 16;
    static constexpr std::uint32_t radix_passes = 8;
    static constexpr std::uint32_t small_sort_max = 1024;  // xdxr_build.comp SMALL_SORT_MAX: one-workgroup sort
    std::uint64_t scratch = 0;         // scratch bytes

    // False when the count would overflow the 32-bit offsets of the format. `half_boxes`: BOX16 inner nodes.
    bool compute(std::uint32_t count, bool top_level, bool half_boxes = false) {
        *this = XdxrLayout{};
        tlas = top_level;
        box16 = half_boxes;
        node_size = box16 ? 64u : 128u;
        elements = count;
        element_size = tlas ? 128u : 64u;
        // A kept inner node has two children when both of its binary children are elements, which takes two
        // elements of its own, and three or four otherwise. With K nodes and a of the first kind, the child slots
        // count K - 1 + N >= 2a + 3(K - a), and a <= N / 2, so K <= (3N - 2) / 4. An empty structure or a single
        // element keeps one root.
        nodes = count <= 1 ? 1u : std::uint32_t((3ull * count - 2) / 4);
        // BOX16: one spare 64-byte unit after the last node a build writes (xdxr_build.comp MODE_HEADER zeroes it
        // and the header's size counts it). The translator's image_bvh_intersect_ray loads a box node's 112 bytes of
        // BOX32 bounds before it knows the type, so without it the last BOX16 node (64 bytes) would be read 48 bytes
        // past the structure, a compacted one too.
        const std::uint64_t pad = box16 ? 64u : 0u;
        const std::uint64_t body_units =
            (std::uint64_t(count) * element_size + std::uint64_t(nodes) * node_size + pad) / 64;
        const std::uint64_t parent_bytes = (4 * body_units + 63) / 64 * 64;
        const std::uint64_t e_off = header_size + parent_bytes;
        const std::uint64_t n_off = e_off + std::uint64_t(count) * element_size;
        const std::uint64_t end = n_off + std::uint64_t(nodes) * node_size + pad;
        // node pointers hold the 64-byte unit index in 29 bits
        if (end > (std::uint64_t(1) << 32) - 64 || (end >> 6) >= (std::uint64_t(1) << 29)) return false;
        elements_off = std::uint32_t(e_off);
        nodes_off = std::uint32_t(n_off);
        size = std::uint32_t(end);
        parent_entries = std::uint32_t(body_units);
        sort_size = count;
        // centroid bounds and the node count, keys and element indices twice, digit counts per tile and their prefix
        // sums, a box per element, a box accumulator per node, then per inner node of the binary tree its two
        // children, its parent, its kept flag and its node index, a kept count per tile with its prefix sum, and
        // each element's pointer in the tree
        scratch = 64 + 16ull * sort_size + 8ull * radix_bins * tiles() + 24ull * count + 32ull * nodes +
                  20ull * inner() + 8ull * kept_tiles() + 4ull * count;
        return true;
    }
    std::uint32_t tiles() const { return (sort_size + radix_tile - 1) / radix_tile; }
    std::uint32_t inner() const { return elements > 1 ? elements - 1 : 0; }   // binary tree inner nodes
    std::uint32_t kept_tiles() const { return (inner() + radix_tile - 1) / radix_tile; }
};

}  // namespace palmetto
