#pragma once

// A hard cap on the number of live driver pipelines a boot keeps, with least-recently-used eviction and a rebuild path.
//
// docs/memory_growth.md measures a 120 s boot at ~40 GB of process private bytes whose owner is the NVIDIA driver's
// per-pipeline host memory (nvoglv64.dll / nvgpucomp64.dll stacks) because the title creates 51,786 graphics pipeline
// states at load and the layer builds a VkPipeline for every one of them eagerly, while the frame loop uses a handful.
// This policy is the bound: a pipeline is built when a command list binds it, marked most-recently-used on every bind,
// and when the number of live pipelines passes the cap the least-recently-used one is handed back to the caller to
// destroy (the pipeline state keeps its recipe, shader modules and layout, so a later bind rebuilds it).
//
// GRACE: a pipeline bound within the last `grace` other binds is never chosen. A command list records a bind and may
// be submitted after another thread binds something else, so evicting a pipeline that was just bound could destroy a
// VkPipeline an unsubmitted recording still names. This title records and submits within one frame (the delay is one
// submission), so a grace of `kDefaultGrace` binds is orders of magnitude more than the window; the caller still
// destroys through the device's deferred-deletion queue, which waits for the submissions already given to the GPU.
//
// No Vulkan, Windows or device dependency: the caller owns the pipeline ids and the destruction, so the policy is
// tested without a GPU (port/test/pipeline_recipe_cache_test.cpp).

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <unordered_map>

namespace palmetto {

class PipelineBudget {
public:
    using Id = std::uint64_t;

    static constexpr std::size_t kDefaultCap = 16;
    static constexpr std::size_t kDefaultGrace = 8;

    PipelineBudget(std::size_t cap = kDefaultCap, std::size_t grace = kDefaultGrace) : cap_(cap), grace_(grace) {}

    // A pipeline is live and about to be used: it becomes the most recently used one. Returns the id to evict to stay
    // inside the cap, or 0 when nothing has to go. At most one pipeline is evicted per call: a cap that is only
    // approached by one build at a time needs no batch, and a caller that lowers the cap by hand can call again.
    Id touch(Id id) {
        if (!id || !cap_) return 0;
        const auto it = where_.find(id);
        if (it != where_.end()) {
            order_.splice(order_.end(), order_, it->second);
            return 0;
        }
        order_.push_back(id);
        where_.emplace(id, std::prev(order_.end()));
        ++built_;
        // The grace counts *other* live pipelines newer than the candidate: with at most `grace` newer entries the
        // candidate was bound too recently to be safe to release.
        while (order_.size() > cap_) {
            const auto oldest = order_.begin();
            const std::size_t newer = order_.size() - 1;
            if (newer < grace_) return 0;
            const Id victim = *oldest;
            where_.erase(victim);
            order_.erase(oldest);
            ++evicted_;
            return victim;
        }
        return 0;
    }

    // The pipeline state died (or its pipeline failed to build): forget it without counting an eviction.
    void remove(Id id) {
        const auto it = where_.find(id);
        if (it == where_.end()) return;
        order_.erase(it->second);
        where_.erase(it);
    }

    std::size_t live() const { return order_.size(); }
    std::size_t built() const { return built_; }
    std::size_t evicted() const { return evicted_; }
    std::size_t cap() const { return cap_; }
    std::size_t grace() const { return grace_; }
    bool empty() const { return order_.empty(); }
    Id least_recently_used() const { return order_.empty() ? 0 : order_.front(); }

private:
    std::size_t cap_ = kDefaultCap;
    std::size_t grace_ = kDefaultGrace;
    std::list<Id> order_;  // front: least recently used
    std::unordered_map<Id, std::list<Id>::iterator> where_;
    std::size_t built_ = 0;
    std::size_t evicted_ = 0;
};

}  // namespace palmetto
