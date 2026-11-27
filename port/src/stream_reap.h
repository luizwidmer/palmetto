// Pure, bounded policy for retiring completions in submission order.
//
// This does not start workers, touch files, publish status arrays, signal fences, or own title memory. A caller may use it
// after doing work concurrently: issue tickets in queue order, complete them in any order, and publish only values returned
// by take_ready(). The caller must serialize these operations and keep publication single-threaded/in-order. The helper is
// opt-in; its default instance is disabled and has no mutable state until explicitly enabled.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace palmetto::stream_reap {

enum class Refusal : std::uint8_t {
    none,
    disabled,
    invalid_switch_value,
    capacity_full,
    sequence_exhausted,
    unknown_ticket,
    duplicate_completion,
    waiting_for_earlier,
    empty,
    internal_slot_conflict,
};

constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
        case Refusal::none: return "none";
        case Refusal::disabled: return "disabled";
        case Refusal::invalid_switch_value: return "invalid_switch_value";
        case Refusal::capacity_full: return "capacity_full";
        case Refusal::sequence_exhausted: return "sequence_exhausted";
        case Refusal::unknown_ticket: return "unknown_ticket";
        case Refusal::duplicate_completion: return "duplicate_completion";
        case Refusal::waiting_for_earlier: return "waiting_for_earlier";
        case Refusal::empty: return "empty";
        case Refusal::internal_slot_conflict: return "internal_slot_conflict";
    }
    return "unknown_refusal";
}

struct SwitchDecision {
    bool enabled = false;
    Refusal refusal = Refusal::disabled;
};

namespace detail {

constexpr unsigned char ascii_fold(unsigned char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

constexpr bool equals_ascii(const char* value, const char* expected) noexcept {
    if (!value || !expected) return false;
    while (*value && *expected) {
        if (ascii_fold(static_cast<unsigned char>(*value)) != ascii_fold(static_cast<unsigned char>(*expected)))
            return false;
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

}  // namespace detail

// Strict opt-in parser for PALMETTO_DS_REAP_ORDER. Only explicit affirmative values enable it; unknown spellings fail
// closed with a named refusal. In particular, an unset/empty value remains off.
constexpr SwitchDecision parse_opt_in(const char* value) noexcept {
    if (!value || *value == '\0' || detail::equals_ascii(value, "0") || detail::equals_ascii(value, "off") ||
        detail::equals_ascii(value, "no"))
        return {false, Refusal::disabled};
    if (detail::equals_ascii(value, "1") || detail::equals_ascii(value, "on") || detail::equals_ascii(value, "yes") ||
        detail::equals_ascii(value, "true"))
        return {true, Refusal::none};
    return {false, Refusal::invalid_switch_value};
}

struct Ticket {
    std::uint64_t value = 0;
};

struct IssueResult {
    Refusal refusal = Refusal::disabled;
    Ticket ticket{};

    constexpr explicit operator bool() const noexcept { return refusal == Refusal::none; }
};

template <typename T>
struct ReapResult {
    Refusal refusal = Refusal::empty;
    Ticket ticket{};
    std::optional<T> value;

    explicit operator bool() const noexcept { return refusal == Refusal::none && value.has_value(); }
};

// Fixed-capacity reorder buffer for values whose work can finish out of order but whose observable completion must not.
// Capacity is compile-time and no allocation occurs. This class is deliberately not thread-safe: callers must protect
// issue()/complete()/take_ready() with the same synchronization and must publish a drained batch before another caller
// can publish later tickets.
template <typename T, std::size_t Capacity>
class OrderedRetirer {
    static_assert(Capacity > 0, "OrderedRetirer needs a non-zero capacity");
    static_assert(std::is_nothrow_move_constructible_v<T>, "retired values must be nothrow-movable");

    struct Slot {
        std::uint64_t ticket = 0;
        bool occupied = false;
        bool completed = false;
        std::optional<T> value;
    };

public:
    explicit constexpr OrderedRetirer(bool enabled = false, std::uint64_t first_ticket = 0) noexcept
        : enabled_(enabled), next_issue_(first_ticket), next_retire_(first_ticket) {}

    constexpr bool enabled() const noexcept { return enabled_; }
    constexpr std::size_t outstanding() const noexcept { return outstanding_; }

    IssueResult issue() noexcept {
        if (!enabled_) return {Refusal::disabled, {}};
        if (next_issue_ == std::numeric_limits<std::uint64_t>::max())
            return {Refusal::sequence_exhausted, {}};
        if (outstanding_ >= Capacity) return {Refusal::capacity_full, {}};

        Slot& slot = slots_[slot_index(next_issue_)];
        if (slot.occupied) return {Refusal::internal_slot_conflict, {}};
        const Ticket ticket{next_issue_};
        slot.ticket = next_issue_++;
        slot.occupied = true;
        slot.completed = false;
        slot.value.reset();
        ++outstanding_;
        return {Refusal::none, ticket};
    }

    Refusal complete(Ticket ticket, T value) noexcept {
        if (!enabled_) return Refusal::disabled;
        if (ticket.value < next_retire_ || ticket.value >= next_issue_) return Refusal::unknown_ticket;

        Slot& slot = slots_[slot_index(ticket.value)];
        if (!slot.occupied || slot.ticket != ticket.value) return Refusal::unknown_ticket;
        if (slot.completed) return Refusal::duplicate_completion;
        slot.value.emplace(std::move(value));
        slot.completed = true;
        return Refusal::none;
    }

    // Returns the next value only when every earlier ticket has been retired. `waiting_for_earlier` means the queue is
    // non-empty but its front job has not completed; `empty` means no tickets are outstanding.
    ReapResult<T> take_ready() noexcept {
        if (!enabled_) return {Refusal::disabled, {}, std::nullopt};
        if (outstanding_ == 0) return {Refusal::empty, {}, std::nullopt};

        Slot& slot = slots_[slot_index(next_retire_)];
        if (!slot.occupied || slot.ticket != next_retire_) return {Refusal::internal_slot_conflict, {}, std::nullopt};
        if (!slot.completed) return {Refusal::waiting_for_earlier, {next_retire_}, std::nullopt};

        T value = std::move(*slot.value);
        const Ticket ticket{next_retire_};
        slot.value.reset();
        slot.completed = false;
        slot.occupied = false;
        ++next_retire_;  // issue() refuses UINT64_MAX, so this cannot wrap.
        --outstanding_;
        return {Refusal::none, ticket, std::optional<T>(std::move(value))};
    }

private:
    static constexpr std::size_t slot_index(std::uint64_t ticket) noexcept {
        return static_cast<std::size_t>(ticket % Capacity);
    }

    const bool enabled_;
    std::uint64_t next_issue_ = 0;
    std::uint64_t next_retire_ = 0;
    std::size_t outstanding_ = 0;
    std::array<Slot, Capacity> slots_{};
};

}  // namespace palmetto::stream_reap
