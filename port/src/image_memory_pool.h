#pragma once

// Memory for textures, handed out as ranges of larger blocks instead of one driver allocation each.
//
// Each vkAllocateMemory/vkFreeMemory pair is a kernel-backed operation, and the retail title streams textures in
// bursts: one allocation per streamed image is a cost paid and a Vulkan object (vulkan spec: at most
// maxMemoryAllocationCount live allocations, 4096 on many drivers) spent per image. A pool turns the per-image cost
// into a range search, and lets a title that frees a texture and makes the next one reuse the same driver memory.
//
// This header has no Vulkan, Windows or device dependency: the caller owns the blocks through the two callbacks it
// passes in, so the policy can be checked without a GPU (port/test/d3d12_vulkan_device_test.cpp). Every range of a
// block is an optimal-tiled image, so VkPhysicalDeviceLimits::bufferImageGranularity, which separates linear from
// optimal resources, does not apply inside a block.
//
// Delivered memory is not zeroed: `reused()` says whether the bytes were handed out before, and the caller keeps
// D3D12's committed-resource semantics by zero-filling such an image before a shader reads it (vulkan_replay.h,
// initialize).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace palmetto {

// The free space of one block: runs of unallocated bytes, kept sorted by offset. allocate() takes the smallest run
// that holds the request at its alignment (best fit) and splits it; release() puts a run back and merges it with its
// neighbours, so the fragmentation of a title's create/free cycle stays bounded. One mutex per pool serializes the
// runs; each operation is a short scan over the block's runs (a handful in practice).
class ImageMemoryRuns {
public:
    explicit ImageMemoryRuns(std::uint64_t bytes = 0) : bytes_(bytes) {
        if (bytes_) runs_.push_back({0, bytes});
    }

    // False when no free run holds `bytes` at `alignment`. `offset` receives the aligned start on success.
    bool allocate(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t& offset) {
        if (!bytes || !alignment || bytes > bytes_) return false;
        std::size_t best = runs_.size();
        std::uint64_t best_start = 0;
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            const std::uint64_t start = (runs_[i].offset + alignment - 1) / alignment * alignment;
            if (start < runs_[i].offset) continue;  // the alignment would wrap
            const std::uint64_t lead = start - runs_[i].offset;
            if (lead > runs_[i].bytes || bytes > runs_[i].bytes - lead) continue;
            if (best == runs_.size() || runs_[i].bytes < runs_[best].bytes) {
                best = i;
                best_start = start;
            }
        }
        if (best == runs_.size()) return false;
        const Run taken = runs_[best];
        runs_.erase(runs_.begin() + std::ptrdiff_t(best));
        const std::uint64_t lead = best_start - taken.offset;
        if (lead) runs_.insert(runs_.begin() + std::ptrdiff_t(best), Run{taken.offset, lead});
        const std::uint64_t tail = taken.bytes - lead - bytes;
        if (tail)
            runs_.insert(runs_.begin() + std::ptrdiff_t(best) + (lead ? 1 : 0), Run{best_start + bytes, tail});
        in_use_ += bytes;
        offset = best_start;
        return true;
    }

    // Gives back a run allocate() returned: it goes back in offset order and is merged with the free runs it touches.
    void release(std::uint64_t offset, std::uint64_t bytes) {
        if (!bytes || offset >= bytes_ || bytes > bytes_ - offset) return;  // not a run this block handed out
        auto at = std::lower_bound(runs_.begin(), runs_.end(), offset,
                                   [](const Run& run, std::uint64_t value) { return run.offset < value; });
        if (at != runs_.end() && at->offset == offset) {  // a double release: the bytes are already free
            return;
        }
        in_use_ -= std::min(in_use_, bytes);
        if (at != runs_.begin()) {
            auto previous = std::prev(at);
            if (previous->offset + previous->bytes == offset) {  // merge with the run before
                offset = previous->offset;
                bytes += previous->bytes;
                at = runs_.erase(previous);
            }
        }
        if (at != runs_.end() && offset + bytes == at->offset) {  // merge with the run after
            bytes += at->bytes;
            at = runs_.erase(at);
        }
        runs_.insert(at, Run{offset, bytes});
    }

    bool empty() const { return in_use_ == 0; }
    std::uint64_t bytes() const { return bytes_; }
    std::uint64_t in_use() const { return in_use_; }
    // The largest free run, so a block that cannot hold the request is passed over without a search.
    std::uint64_t largest() const {
        std::uint64_t largest = 0;
        for (const Run& run : runs_) largest = std::max(largest, run.bytes);
        return largest;
    }
    std::size_t runs() const { return runs_.size(); }

private:
    struct Run {
        std::uint64_t offset = 0;
        std::uint64_t bytes = 0;
    };

    std::vector<Run> runs_;
    std::uint64_t bytes_ = 0;
    std::uint64_t in_use_ = 0;
};

// Blocks of device memory split into ranges, with one list of blocks per memory type. Thread-safe: a title creates
// textures on its own threads while queues retire them on theirs.
//
// The policy: allocate() reuses the range in any block of the type that fits (best fit across blocks, one block's
// largest free run as a prefilter); a request that fits no block gets a new block of `block_bytes`, unless the driver
// has no room, in which case allocate() returns null and the caller makes its own dedicated allocation. A released
// range costs a scan and a coalesce; a block whose last range is released is freed, except one empty block per type,
// so a title that frees and remakes the same textures does not ask the driver each time. After a failed block
// allocation, the type skips new blocks for a bounded number of requests, because a full heap stays full until
// something is given back and a failed vkAllocateMemory is expensive.
template<typename Memory>
class ImageMemoryPool {
private:
    struct Block;

public:
    // A driver allocation of `type` and `bytes`, or a null Memory when the driver has no room. Called under the
    // pool's lock, so a callback must not allocate another range of the same pool.
    using AllocateBlock = std::function<Memory(std::uint32_t type, std::uint64_t bytes)>;
    // Frees a block this pool allocated. Called under the pool's lock for a block the policy drops; the destructor
    // and shutdown() call it outside the lock.
    using FreeBlock = std::function<void(Memory)>;

    // One image's range. Destroying it gives the bytes back to its block, so it is destroyed when no submission that
    // used the image is running (DeferredDeletionQueue: the device pushes a token that drops the Range).
    class Range {
    public:
        Range(const Range&) = delete;
        Range& operator=(const Range&) = delete;
        ~Range();

        Memory memory() const { return memory_; }
        std::uint64_t offset() const { return offset_; }
        std::uint64_t bytes() const { return bytes_; }
        // True when the bytes were handed out before, so they may hold an earlier image's data.
        bool reused() const { return reused_; }

    private:
        friend class ImageMemoryPool;
        Range(ImageMemoryPool& pool, Block& block, std::uint64_t offset, std::uint64_t bytes, bool reused)
            : pool_(pool), block_(&block), memory_(block.memory), offset_(offset), bytes_(bytes), reused_(reused) {}

        ImageMemoryPool& pool_;
        Block* block_ = nullptr;
        Memory memory_{};
        std::uint64_t offset_ = 0;
        std::uint64_t bytes_ = 0;
        bool reused_ = false;
    };

    struct Stats {
        std::uint64_t blocks = 0;       // live driver allocations
        std::uint64_t block_bytes = 0;  // their total size
        std::uint64_t ranges = 0;       // live ranges in them
        std::uint64_t range_bytes = 0;  // their total size
        std::uint64_t reused_ranges = 0;
    };

    ImageMemoryPool(AllocateBlock allocate, FreeBlock free_block)
        : allocate_(std::move(allocate)), free_(std::move(free_block)) {}
    ImageMemoryPool(const ImageMemoryPool&) = delete;
    ImageMemoryPool& operator=(const ImageMemoryPool&) = delete;
    ~ImageMemoryPool();

    // A range of a block of `type`: the existing block whose largest free run wastes the least, or a new block of
    // `block_bytes`. Null when no block holds it and the driver has no room for a new one.
    std::unique_ptr<Range> allocate(std::uint32_t type, std::uint64_t bytes, std::uint64_t alignment,
                                    std::uint64_t block_bytes) {
        if (!bytes || !alignment || !block_bytes) return nullptr;
        std::lock_guard lock(mutex_);
        Type& pool = types_[type];
        // Best fit across blocks on each block's largest free run, so a nearly empty block is preferred and a full
        // heap is passed over without touching its runs; the chosen block then best-fits within its own runs.
        if (bytes <= block_bytes) {
            Block* best = nullptr;
            for (const auto& block : pool.blocks) {
                const std::uint64_t largest = block->runs.largest();
                if (largest < bytes) continue;
                if (!best || largest < best->runs.largest()) best = block.get();
            }
            if (best) {
                std::uint64_t offset = 0;
                if (best->runs.allocate(bytes, alignment, offset)) return make_range(*best, offset, bytes);
            }
        }
        if (bytes > block_bytes || pool.skip) return nullptr;
        Memory memory = allocate_(type, block_bytes);
        if (!memory) {
            pool.skip = kRetryAfter;
            return nullptr;
        }
        auto block = std::make_unique<Block>(type, memory, block_bytes);
        std::uint64_t offset = 0;
        if (!block->runs.allocate(bytes, alignment, offset)) {
            free_(memory);  // cannot happen for a block no smaller than the request; keep the pool honest
            return nullptr;
        }
        pool.blocks.push_back(std::move(block));
        return make_range(*pool.blocks.back(), offset, bytes);
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        Stats result;
        result.ranges = range_count_;
        result.range_bytes = range_bytes_;
        result.reused_ranges = reused_count_;
        for (const auto& [type, pool] : types_) {
            for (const auto& block : pool.blocks) {
                ++result.blocks;
                result.block_bytes += block->runs.bytes();
            }
        }
        return result;
    }

    // Frees every block. The device calls it after the deferred queue has released the ranges and before its Vulkan
    // objects go; the destructor does the same for a pool that was never shut down. Ranges must be gone by then.
    void shutdown() {
        std::vector<Memory> blocks;
        {
            std::lock_guard lock(mutex_);
            for (auto& [type, pool] : types_)
                for (const auto& block : pool.blocks) blocks.push_back(block->memory);
            types_.clear();
        }
        for (Memory memory : blocks) free_(memory);
    }

private:
    static constexpr std::uint32_t kRetryAfter = 64;

    struct Block {
        Block(std::uint32_t t, Memory m, std::uint64_t bytes) : type(t), memory(m), runs(bytes) {}
        std::uint32_t type = 0;
        Memory memory{};
        ImageMemoryRuns runs;
        std::uint64_t touched = 0;  // bytes below this were handed out before, so they may hold an earlier image
    };
    struct Type {
        std::vector<std::unique_ptr<Block>> blocks;
        std::uint32_t skip = 0;  // new-block requests still skipped after a failed allocation
    };

    std::unique_ptr<Range> make_range(Block& block, std::uint64_t offset, std::uint64_t bytes) {
        ++range_count_;
        range_bytes_ += bytes;
        const bool reused = offset < block.touched;
        if (reused) ++reused_count_;
        block.touched = std::max(block.touched, offset + bytes);
        return std::unique_ptr<Range>(new Range(*this, block, offset, bytes, reused));
    }

    // Called by ~Range: the bytes go back to their block, and the block is freed when it is the second empty one of
    // its type (one empty block is kept, so a free/make cycle does not call the driver each time).
    void deliver(Range& range) {
        std::vector<Memory> freed;
        {
            std::lock_guard lock(mutex_);
            Block& block = *range.block_;
            Type& pool = types_[block.type];
            block.runs.release(range.offset_, range.bytes_);
            --range_count_;
            range_bytes_ -= std::min(range_bytes_, range.bytes_);
            pool.skip = 0;
            std::size_t empty = 0;
            for (const auto& candidate : pool.blocks)
                if (candidate->runs.empty()) ++empty;
            if (block.runs.empty() && empty > 1) {
                freed.push_back(block.memory);
                std::erase_if(pool.blocks, [&](const auto& candidate) { return candidate.get() == &block; });
            }
        }
        for (Memory memory : freed) free_(memory);
    }

    mutable std::mutex mutex_;
    AllocateBlock allocate_;
    FreeBlock free_;
    std::unordered_map<std::uint32_t, Type> types_;
    std::uint64_t range_count_ = 0;
    std::uint64_t range_bytes_ = 0;
    std::uint64_t reused_count_ = 0;
};

template<typename Memory>
ImageMemoryPool<Memory>::~ImageMemoryPool() {
    shutdown();
}

template<typename Memory>
ImageMemoryPool<Memory>::Range::~Range() {
    pool_.deliver(*this);
}

}  // namespace palmetto
