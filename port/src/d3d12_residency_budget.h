#pragma once

// Optional, CPU-only accounting for D3D12 pageable residency requests.
// This is a decision model, not a Vulkan memory manager: it never unmaps,
// frees, or physically evicts GPU memory. Callers must serialize operations.
// It is deliberately not wired into the runtime until a trustworthy budget
// source and pageable lifetime callbacks are measured.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace rt::d3d12_residency {

using PageableId = std::uint64_t;
using AllocationId = std::uint64_t;

inline constexpr char kBudgetSwitch[] = "PALMETTO_D3D12_RESIDENCY_BUDGET";

enum class Status : std::uint8_t {
    bypassed,
    accepted,
    accepted_over_budget,
    refused,
};

enum class Refusal : std::uint8_t {
    none,
    invalid_pageable_id,
    invalid_array,
    allocation_identity_missing,
    zero_allocation_bytes,
    pageable_identity_conflict,
    allocation_size_conflict,
    unknown_pageable,
    budget_unavailable,
    over_budget,
    accounting_overflow,
    corrupt_accounting_state,
};

inline constexpr std::string_view refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::invalid_pageable_id: return "invalid_pageable_id";
    case Refusal::invalid_array: return "invalid_array";
    case Refusal::allocation_identity_missing: return "allocation_identity_missing";
    case Refusal::zero_allocation_bytes: return "zero_allocation_bytes";
    case Refusal::pageable_identity_conflict: return "pageable_identity_conflict";
    case Refusal::allocation_size_conflict: return "allocation_size_conflict";
    case Refusal::unknown_pageable: return "unknown_pageable";
    case Refusal::budget_unavailable: return "budget_unavailable";
    case Refusal::over_budget: return "over_budget";
    case Refusal::accounting_overflow: return "accounting_overflow";
    case Refusal::corrupt_accounting_state: return "corrupt_accounting_state";
    }
    return "unknown_refusal";
}

struct PageableSpec {
    PageableId pageable = 0;
    // Multiple pageable views may name the same allocation. Their backing
    // byte count must agree, and that allocation is charged only once while
    // at least one registered view is resident. Use 0/0 for no GPU backing.
    AllocationId allocation = 0;
    std::uint64_t allocation_bytes = 0;
};

struct Outcome {
    Status status = Status::accepted;
    Refusal refusal = Refusal::none;
    std::uint64_t resident_bytes = 0;
    std::uint64_t additional_bytes = 0;
    bool changed = false;
};

struct Snapshot {
    bool enabled = false;
    bool budget_known = false;
    std::uint64_t budget_bytes = 0;
    std::uint64_t resident_bytes = 0;
    std::size_t pageables = 0;
    std::size_t allocations = 0;
    std::size_t resident_allocations = 0;
};

// Logical set/budget contract for synthetic tests and future explicit
// callers. A zero budget means unavailable, not unlimited. When disabled,
// every operation bypasses validation and leaves this object untouched.
class BudgetSet final {
public:
    static bool switch_value_enabled(const char* value) noexcept {
        return value && value[0] == '1' && value[1] == '\0';
    }

    static bool switch_enabled_from_environment() noexcept {
        return switch_value_enabled(std::getenv(kBudgetSwitch));
    }

    explicit BudgetSet(std::uint64_t budget_bytes, bool enabled) noexcept
        : budget_bytes_(budget_bytes), enabled_(enabled) {}

    Outcome register_pageable(PageableSpec spec) {
        if (!enabled_) return bypassed();
        if (spec.pageable == 0) return refused(Refusal::invalid_pageable_id);
        if (spec.allocation == 0 && spec.allocation_bytes != 0)
            return refused(Refusal::allocation_identity_missing);
        if (spec.allocation != 0 && spec.allocation_bytes == 0)
            return refused(Refusal::zero_allocation_bytes);

        const auto current = pageables_.find(spec.pageable);
        if (current != pageables_.end()) {
            if (current->second.allocation != spec.allocation ||
                current->second.allocation_bytes != spec.allocation_bytes) {
                return refused(Refusal::pageable_identity_conflict);
            }
            return accepted(false);
        }

        if (spec.allocation != 0) {
            const auto allocation = allocations_.find(spec.allocation);
            if (allocation != allocations_.end() && allocation->second.bytes != spec.allocation_bytes)
                return refused(Refusal::allocation_size_conflict);
        }

        if (spec.allocation != 0) {
            auto allocation = allocations_.find(spec.allocation);
            if (allocation == allocations_.end()) {
                allocation = allocations_.emplace(spec.allocation, Allocation{spec.allocation_bytes, 0, 0}).first;
            }
            ++allocation->second.pageable_refs;
        }
        pageables_.emplace(spec.pageable, Pageable{spec.allocation, spec.allocation_bytes, false});
        return accepted(true);
    }

    Outcome make_resident(const PageableId* ids, std::size_t count, bool deny_over_budget) {
        if (!enabled_) return bypassed();
        if (count != 0 && ids == nullptr) return refused(Refusal::invalid_array);

        std::set<PageableId> unique;
        std::map<AllocationId, std::uint64_t> newly_resident;
        std::uint64_t additional = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == 0) return refused(Refusal::invalid_pageable_id);
            if (!unique.insert(ids[i]).second) continue;
            const auto object = pageables_.find(ids[i]);
            if (object == pageables_.end()) return refused(Refusal::unknown_pageable);
            if (object->second.resident || object->second.allocation == 0) continue;
            const auto allocation = allocations_.find(object->second.allocation);
            if (allocation == allocations_.end()) return refused(Refusal::corrupt_accounting_state);
            if (allocation->second.resident_refs == 0) {
                newly_resident.emplace(object->second.allocation, allocation->second.bytes);
            }
        }
        for (const auto& item : newly_resident) {
            if (item.second > std::numeric_limits<std::uint64_t>::max() - additional)
                return refused(Refusal::accounting_overflow);
            additional += item.second;
        }
        if (additional > std::numeric_limits<std::uint64_t>::max() - resident_bytes_)
            return refused(Refusal::accounting_overflow);
        if (deny_over_budget && additional != 0) {
            if (budget_bytes_ == 0) return refused(Refusal::budget_unavailable);
            if (resident_bytes_ > budget_bytes_ || additional > budget_bytes_ - resident_bytes_)
                return refused(Refusal::over_budget);
        }

        bool changed = false;
        for (const PageableId id : unique) {
            auto object = pageables_.find(id);
            if (object->second.resident) continue;
            object->second.resident = true;
            changed = true;
            if (object->second.allocation == 0) continue;
            auto allocation = allocations_.find(object->second.allocation);
            if (allocation->second.resident_refs == 0) resident_bytes_ += allocation->second.bytes;
            ++allocation->second.resident_refs;
        }
        const Status status = budget_bytes_ != 0 && resident_bytes_ > budget_bytes_
            ? Status::accepted_over_budget : Status::accepted;
        return Outcome{status, Refusal::none, resident_bytes_, additional, changed};
    }

    Outcome evict(const PageableId* ids, std::size_t count) {
        if (!enabled_) return bypassed();
        if (count != 0 && ids == nullptr) return refused(Refusal::invalid_array);
        std::set<PageableId> unique;
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == 0) return refused(Refusal::invalid_pageable_id);
            if (!unique.insert(ids[i]).second) continue;
            const auto object = pageables_.find(ids[i]);
            if (object == pageables_.end()) return refused(Refusal::unknown_pageable);
            if (!object->second.resident || object->second.allocation == 0) continue;
            const auto allocation = allocations_.find(object->second.allocation);
            if (allocation == allocations_.end() || allocation->second.resident_refs == 0)
                return refused(Refusal::corrupt_accounting_state);
        }

        bool changed = false;
        for (const PageableId id : unique) {
            auto object = pageables_.find(id);
            if (!object->second.resident) continue;
            object->second.resident = false;
            changed = true;
            if (object->second.allocation == 0) continue;
            auto allocation = allocations_.find(object->second.allocation);
            --allocation->second.resident_refs;
            if (allocation->second.resident_refs == 0) resident_bytes_ -= allocation->second.bytes;
        }
        return accepted(changed);
    }

    Outcome unregister_pageable(PageableId id) {
        if (!enabled_) return bypassed();
        if (id == 0) return refused(Refusal::invalid_pageable_id);
        auto object = pageables_.find(id);
        if (object == pageables_.end()) return refused(Refusal::unknown_pageable);
        if (object->second.allocation != 0) {
            auto allocation = allocations_.find(object->second.allocation);
            if (allocation == allocations_.end() || allocation->second.pageable_refs == 0)
                return refused(Refusal::corrupt_accounting_state);
            if (object->second.resident && allocation->second.resident_refs == 0)
                return refused(Refusal::corrupt_accounting_state);
            if (object->second.resident) {
                --allocation->second.resident_refs;
                if (allocation->second.resident_refs == 0) resident_bytes_ -= allocation->second.bytes;
            }
            --allocation->second.pageable_refs;
            if (allocation->second.pageable_refs == 0) allocations_.erase(allocation);
        }
        pageables_.erase(object);
        return accepted(true);
    }

    Snapshot snapshot() const noexcept {
        Snapshot result{enabled_, budget_bytes_ != 0, budget_bytes_, resident_bytes_,
                        pageables_.size(), allocations_.size(), 0};
        for (const auto& entry : allocations_) {
            if (entry.second.resident_refs != 0) ++result.resident_allocations;
        }
        return result;
    }

private:
    struct Pageable {
        AllocationId allocation;
        std::uint64_t allocation_bytes;
        bool resident;
    };
    struct Allocation {
        std::uint64_t bytes;
        std::size_t pageable_refs;
        std::size_t resident_refs;
    };

    Outcome bypassed() const noexcept {
        return Outcome{Status::bypassed, Refusal::none, resident_bytes_, 0, false};
    }
    Outcome refused(Refusal refusal) const noexcept {
        return Outcome{Status::refused, refusal, resident_bytes_, 0, false};
    }
    Outcome accepted(bool changed) const noexcept {
        return Outcome{Status::accepted, Refusal::none, resident_bytes_, 0, changed};
    }

    std::uint64_t budget_bytes_ = 0;
    std::uint64_t resident_bytes_ = 0;
    bool enabled_ = false;
    std::map<PageableId, Pageable> pageables_;
    std::map<AllocationId, Allocation> allocations_;
};

}  // namespace rt::d3d12_residency
