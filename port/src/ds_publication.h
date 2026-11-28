// Pure, bounded publication policy for the DirectStorage queue's opt-in ordered path.
//
// The queue's default worker executes an entry and publishes its result inline, one entry at a time, so the
// default path needs no ordering gate. With PALMETTO_DS_REAP_ORDER=1 the queue runs request work on two lanes
// and keeps only the observable publication (status array, fence signal, event set, failure record, reference
// release) in enqueue order. This header holds the parts of that seam that are testable without Windows, a GPU,
// a title, game data or a boot:
//
//   resolve()                       the exact switch parser and the option gate that keeps the serial path
//   FailureFold / note_entry()      the queue's first-failure/status/fence rule, applied in publication order
//   OrderedPublisher<T, Capacity>   admission in submission order, completion in any order, in-order drain
//
// Nothing here starts a worker, touches a file, publishes a status array, signals a fence or owns title memory.
// The publisher is deliberately not thread-safe: the owning queue calls admit/complete/drain under one
// publication mutex and performs the published side effects inside that same critical section, so two lanes can
// never interleave their observable completions.
#pragma once

#include "stream_reap.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace palmetto::ds_publication {

using stream_reap::IssueResult;
using stream_reap::OrderedRetirer;
using stream_reap::Refusal;
using stream_reap::Ticket;

// Bounded retirement buffer and request-work lane count of the opt-in path. Both are compile-time bounds: the
// buffer backpressures dispatch before a later entry can run past an earlier one, and the lanes only overlap work
// whose observable completion the single publisher serializes.
constexpr std::size_t kReapCapacity = 64;
constexpr unsigned kReapLanes = 2;

// HRESULT failure is the negative state; S_OK (0) and S_FALSE (1) are success. Kept as an integer so this header
// needs no windows.h and the test stays host-only.
constexpr bool failed(std::int32_t hr) noexcept { return hr < 0; }

// Why the ordered publication path is or is not running. `switch_*` carries the strict switch parse; `refusal`
// is the option-independent gate the runtime path reads. Every non-`none` value keeps the current serial path and
// has a stable printable spelling, so a boot log names exactly why the opt-in did nothing.
enum class ConfigRefusal : std::uint8_t {
    none,
    switch_off,             // unset, empty or an explicit negative: the default path, no line
    switch_invalid,         // an unrecognised spelling: named, and the serial path runs
    staging_scratch_shared, // PALMETTO_DS_STAGING buffers are single-worker state, so lanes are refused
    allocation_failed,      // the bounded buffer could not be allocated: the serial path runs
};

constexpr const char* config_refusal_name(ConfigRefusal refusal) noexcept {
    switch (refusal) {
        case ConfigRefusal::none: return "none";
        case ConfigRefusal::switch_off: return "switch_off";
        case ConfigRefusal::switch_invalid: return "switch_invalid";
        case ConfigRefusal::staging_scratch_shared: return "staging_scratch_shared";
        case ConfigRefusal::allocation_failed: return "allocation_failed";
    }
    return "unknown_config_refusal";
}

struct Config {
    bool ordered = false;                          // run the concurrent-lane + ordered-publication path
    bool switch_enabled = false;                   // the switch parsed as an explicit affirmative
    Refusal switch_refusal = Refusal::disabled;    // the exact parse outcome (named on an unknown spelling)
    ConfigRefusal refusal = ConfigRefusal::switch_off;
    unsigned lanes = 1;                            // 1 = the serial path
    std::size_t capacity = 0;                      // 0 = no retirement buffer
};

// Exact switch parser: stream_reap::parse_opt_in accepts only 1/on/yes/true (case-insensitive); 0/off/no and an
// unset or empty value are off; any other spelling is the named invalid_switch_value refusal and stays off.
// The option gate then refuses the ordered path when the queue's single-worker staging scratch is enabled.
constexpr Config resolve(const char* value, bool staging_scratch_enabled) noexcept {
    Config config;
    const stream_reap::SwitchDecision decision = stream_reap::parse_opt_in(value);
    config.switch_refusal = decision.refusal;
    config.switch_enabled = decision.enabled;
    if (decision.refusal == Refusal::invalid_switch_value) {
        config.refusal = ConfigRefusal::switch_invalid;
        return config;
    }
    if (!decision.enabled) {
        config.refusal = ConfigRefusal::switch_off;
        return config;
    }
    if (staging_scratch_enabled) {
        config.refusal = ConfigRefusal::staging_scratch_shared;
        return config;
    }
    config.ordered = true;
    config.refusal = ConfigRefusal::none;
    config.lanes = kReapLanes;
    config.capacity = kReapCapacity;
    return config;
}

// The serial queue's first-failure rule, separated from the entry kinds so publication order can be tested.
// A failed request is remembered only while no failure is pending; a status entry consumes the pending failure
// and resets it; a fence signal resets it. This is exactly the `since_notify_` state machine the queue already
// ran inline, so a status entry reports the first failed request *in enqueue order*, not the first in finish
// order.
class FailureFold {
public:
    constexpr void note(std::int32_t hr) noexcept {
        if (failed(hr) && !failed(pending_)) pending_ = hr;
    }
    constexpr std::int32_t consume() noexcept {
        const std::int32_t value = pending_;
        pending_ = 0;
        return value;
    }
    constexpr void reset() noexcept { pending_ = 0; }
    constexpr std::int32_t pending() const noexcept { return pending_; }

private:
    std::int32_t pending_ = 0;  // S_OK
};

// The entry kinds whose publication the ordered path serializes. A request has work and a failure to fold; a
// status entry publishes the folded failure; a fence signal and an event set are side effects only.
enum class EntryKind : std::uint8_t { request, status, signal, event };

// Applies one entry's publication rule and returns the HRESULT the publisher acts on: for a status entry that is
// the folded failure (or the entry's own answer when the entry itself failed); for the others it is `hr`.
constexpr std::int32_t note_entry(FailureFold& fold, EntryKind kind, std::int32_t hr) noexcept {
    switch (kind) {
        case EntryKind::request:
            fold.note(hr);
            return hr;
        case EntryKind::status:
            return failed(hr) ? hr : fold.consume();
        case EntryKind::signal:
            fold.reset();  // the serial queue resets on a signal whether or not the fence call succeeds
            return hr;
        case EntryKind::event:
            return hr;
    }
    return hr;
}

// The bounded ordered publisher: admit() is called in submission order, complete() may arrive in any order, and
// drain() yields the contiguous completed prefix in ticket order and publishes each payload exactly once. The
// caller holds one mutex around all three and performs the side effect inside drain()'s callback.
template <typename T, std::size_t Capacity = kReapCapacity>
class OrderedPublisher {
    static_assert(Capacity > 0, "OrderedPublisher needs a non-zero capacity");

public:
    explicit constexpr OrderedPublisher(bool enabled = false) noexcept : retirer_(enabled) {}

    constexpr bool enabled() const noexcept { return retirer_.enabled(); }
    constexpr std::size_t outstanding() const noexcept { return retirer_.outstanding(); }

    // Admission before dispatch. A full or disabled buffer refuses by name; `capacity_full` means the caller
    // must wait for a retirement and retry, never run this entry past earlier work.
    IssueResult admit() noexcept { return retirer_.issue(); }

    Refusal complete(Ticket ticket, T value) noexcept { return retirer_.complete(ticket, value); }

    // Publishes the contiguous completed prefix, in ticket order, through `publish(value, ticket)`. Returns the
    // first refusal that is not an ordinary end (none) or wait (waiting_for_earlier / empty), and reports how
    // many payloads it published. A named refusal is an integration fault the caller must report: dropping or
    // reordering the remaining completions would break the queue's order guarantee.
    template <typename Publish>
    Refusal drain(Publish&& publish, std::size_t& published) {
        published = 0;
        for (;;) {
            auto ready = retirer_.take_ready();
            if (ready.refusal == Refusal::waiting_for_earlier) return Refusal::none;
            if (ready.refusal == Refusal::empty) return Refusal::none;
            if (ready.refusal != Refusal::none) return ready.refusal;
            publish(std::move(*ready.value), ready.ticket);
            ++published;
        }
    }

private:
    OrderedRetirer<T, Capacity> retirer_;
};

}  // namespace palmetto::ds_publication
