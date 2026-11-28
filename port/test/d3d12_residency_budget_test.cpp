// CPU-only synthetic tests for d3d12_residency_budget.h; no Vulkan device or game data.
#include "../src/d3d12_residency_budget.h"

#include <cstdio>
#include <limits>

using rt::d3d12_residency::BudgetSet;
using rt::d3d12_residency::PageableId;
using rt::d3d12_residency::PageableSpec;
using rt::d3d12_residency::Refusal;
using rt::d3d12_residency::Status;
using rt::d3d12_residency::refusal_name;

namespace {
int checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)
}  // namespace

int main() {
    CHECK(BudgetSet::switch_value_enabled("1"));
    CHECK(!BudgetSet::switch_value_enabled(nullptr));
    CHECK(!BudgetSet::switch_value_enabled(""));
    CHECK(!BudgetSet::switch_value_enabled("0"));
    CHECK(!BudgetSet::switch_value_enabled("true"));
    CHECK(refusal_name(Refusal::over_budget) == "over_budget");

    // Disabled is a strict bypass: even malformed calls do not mutate state.
    BudgetSet disabled(64, false);
    auto result = disabled.register_pageable(PageableSpec{1, 10, 64});
    CHECK(result.status == Status::bypassed && result.refusal == Refusal::none);
    result = disabled.make_resident(nullptr, 2, true);
    CHECK(result.status == Status::bypassed && !result.changed);
    result = disabled.evict(nullptr, 1);
    CHECK(result.status == Status::bypassed);
    auto state = disabled.snapshot();
    CHECK(!state.enabled && state.pageables == 0 && state.allocations == 0 && state.resident_bytes == 0);

    BudgetSet set(100, true);
    CHECK(set.register_pageable(PageableSpec{0, 0, 0}).refusal == Refusal::invalid_pageable_id);
    CHECK(set.register_pageable(PageableSpec{5, 0, 8}).refusal == Refusal::allocation_identity_missing);
    CHECK(set.register_pageable(PageableSpec{5, 50, 0}).refusal == Refusal::zero_allocation_bytes);

    result = set.register_pageable(PageableSpec{1, 100, 64});
    CHECK(result.status == Status::accepted && result.changed);
    CHECK(set.register_pageable(PageableSpec{1, 100, 64}).status == Status::accepted); // idempotent
    CHECK(set.register_pageable(PageableSpec{1, 101, 64}).refusal == Refusal::pageable_identity_conflict);
    CHECK(set.register_pageable(PageableSpec{2, 100, 64}).status == Status::accepted); // same heap backing
    CHECK(set.register_pageable(PageableSpec{3, 100, 65}).refusal == Refusal::allocation_size_conflict);
    CHECK(set.register_pageable(PageableSpec{3, 300, 48}).status == Status::accepted); // committed allocation
    CHECK(set.register_pageable(PageableSpec{4, 0, 0}).status == Status::accepted); // non-memory pageable

    // Two placed views plus a duplicate request charge the shared heap once.
    const PageableId aliases[] = {1, 2, 1, 4};
    result = set.make_resident(aliases, 4, true);
    CHECK(result.status == Status::accepted && result.changed);
    CHECK(result.additional_bytes == 64 && result.resident_bytes == 64);
    state = set.snapshot();
    CHECK(state.resident_allocations == 1 && state.pageables == 4);

    const PageableId already_resident[] = {1, 1};
    result = set.make_resident(already_resident, 2, true);
    CHECK(result.status == Status::accepted && !result.changed && result.additional_bytes == 0);
    CHECK(result.resident_bytes == 64);

    // DENY_OVERBUDGET is atomic; no object in a refused batch becomes resident.
    const PageableId over_budget_batch[] = {3, 4};
    result = set.make_resident(over_budget_batch, 2, true);
    CHECK(result.status == Status::refused && result.refusal == Refusal::over_budget);
    CHECK(!result.changed && result.resident_bytes == 64);
    const PageableId invalid_batch[] = {3, 999};
    result = set.make_resident(invalid_batch, 2, false);
    CHECK(result.status == Status::refused && result.refusal == Refusal::unknown_pageable);
    CHECK(result.resident_bytes == 64);
    result = set.make_resident(nullptr, 1, false);
    CHECK(result.status == Status::refused && result.refusal == Refusal::invalid_array);

    const PageableId committed[] = {3};
    result = set.make_resident(committed, 1, false);
    CHECK(result.status == Status::accepted_over_budget && result.changed);
    CHECK(result.additional_bytes == 48 && result.resident_bytes == 112);

    // Evicting one alias keeps the backing resident until the last alias leaves.
    const PageableId first_alias[] = {1};
    result = set.evict(first_alias, 1);
    CHECK(result.status == Status::accepted && result.changed && result.resident_bytes == 112);
    result = set.evict(first_alias, 1);
    CHECK(result.status == Status::accepted && !result.changed && result.resident_bytes == 112); // idempotent
    const PageableId invalid_evict_batch[] = {3, 999};
    result = set.evict(invalid_evict_batch, 2);
    CHECK(result.status == Status::refused && result.refusal == Refusal::unknown_pageable);
    CHECK(result.resident_bytes == 112); // valid prefix was not partially evicted
    result = set.unregister_pageable(2);
    CHECK(result.status == Status::accepted && result.changed && result.resident_bytes == 48);
    state = set.snapshot();
    CHECK(state.resident_allocations == 1);
    result = set.evict(committed, 1);
    CHECK(result.status == Status::accepted && result.resident_bytes == 0);
    CHECK(set.unregister_pageable(3).status == Status::accepted);
    const PageableId no_memory[] = {4};
    CHECK(set.make_resident(no_memory, 1, true).status == Status::accepted);
    CHECK(set.evict(no_memory, 1).changed);
    CHECK(set.unregister_pageable(4).status == Status::accepted);
    CHECK(set.evict(no_memory, 1).refusal == Refusal::unknown_pageable);

    BudgetSet exact_budget(100, true);
    CHECK(exact_budget.register_pageable(PageableSpec{31, 31, 40}).status == Status::accepted);
    CHECK(exact_budget.register_pageable(PageableSpec{32, 32, 60}).status == Status::accepted);
    CHECK(exact_budget.register_pageable(PageableSpec{33, 33, 1}).status == Status::accepted);
    const PageableId exact_fit[] = {31, 32};
    result = exact_budget.make_resident(exact_fit, 2, true);
    CHECK(result.status == Status::accepted && result.resident_bytes == 100);
    const PageableId one_over[] = {33};
    result = exact_budget.make_resident(one_over, 1, true);
    CHECK(result.status == Status::refused && result.refusal == Refusal::over_budget);
    CHECK(result.resident_bytes == 100);

    // A missing budget is named only when the caller requests a denial decision.
    BudgetSet unknown_budget(0, true);
    CHECK(unknown_budget.register_pageable(PageableSpec{10, 10, 7}).status == Status::accepted);
    const PageableId one[] = {10};
    result = unknown_budget.make_resident(one, 1, true);
    CHECK(result.status == Status::refused && result.refusal == Refusal::budget_unavailable);
    CHECK(result.resident_bytes == 0);
    result = unknown_budget.make_resident(one, 1, false);
    CHECK(result.status == Status::accepted && result.resident_bytes == 7);
    CHECK(!unknown_budget.snapshot().budget_known);

    // Arithmetic overflow is refused without changing the resident set.
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    BudgetSet overflow(maximum, true);
    CHECK(overflow.register_pageable(PageableSpec{20, 20, maximum - 3}).status == Status::accepted);
    CHECK(overflow.register_pageable(PageableSpec{21, 21, 4}).status == Status::accepted);
    const PageableId huge[] = {20};
    CHECK(overflow.make_resident(huge, 1, true).status == Status::accepted);
    const PageableId would_wrap[] = {21};
    result = overflow.make_resident(would_wrap, 1, false);
    CHECK(result.status == Status::refused && result.refusal == Refusal::accounting_overflow);
    CHECK(result.resident_bytes == maximum - 3);

    std::printf("d3d12_residency_budget_test: %d checks passed\n", checks);
    return 0;
}
