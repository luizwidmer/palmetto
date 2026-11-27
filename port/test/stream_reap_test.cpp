// Synthetic tests for the pure ordered-completion policy in port/src/stream_reap.h.
// No Windows API, GPU, title boot, game data, files, or peer code is involved.
#include "../src/stream_reap.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

using palmetto::stream_reap::OrderedRetirer;
using palmetto::stream_reap::Refusal;
using palmetto::stream_reap::Ticket;

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

struct Completion {
    enum class Kind { Request, Status, Signal } kind = Kind::Request;
    int id = 0;
    int hr = 0;
};

void test_switch_and_default_off() {
    using palmetto::stream_reap::parse_opt_in;

    const auto unset = parse_opt_in(nullptr);
    check(!unset.enabled && unset.refusal == Refusal::disabled, "unset switch is disabled");
    const auto empty = parse_opt_in("");
    check(!empty.enabled && empty.refusal == Refusal::disabled, "empty switch is disabled");
    for (const char* text : {"0", "OFF", "No"}) {
        const auto result = parse_opt_in(text);
        check(!result.enabled && result.refusal == Refusal::disabled, "explicit negative switch is disabled");
    }
    for (const char* text : {"1", "on", "YES", "True"}) {
        const auto result = parse_opt_in(text);
        check(result.enabled && result.refusal == Refusal::none, "explicit affirmative switch enables the policy");
    }
    const auto typo = parse_opt_in("oen");
    check(!typo.enabled && typo.refusal == Refusal::invalid_switch_value,
          "unknown switch spelling is refused without enabling the policy");
    check(std::strcmp(palmetto::stream_reap::refusal_name(typo.refusal), "invalid_switch_value") == 0,
          "invalid switch refusal has a stable name");

    OrderedRetirer<int, 2> off_by_default;
    check(!off_by_default.enabled(), "default constructor is off");
    const auto issue = off_by_default.issue();
    check(issue.refusal == Refusal::disabled && off_by_default.outstanding() == 0,
          "disabled issue changes no state");
    check(off_by_default.complete(Ticket{0}, 7) == Refusal::disabled && off_by_default.outstanding() == 0,
          "disabled completion changes no state");
    const auto reap = off_by_default.take_ready();
    check(reap.refusal == Refusal::disabled && !reap.value && off_by_default.outstanding() == 0,
          "disabled reap changes no state");
}

void test_out_of_order_work_retires_in_order() {
    constexpr int kOk = 0;
    constexpr int kEarlierFailure = -11;
    constexpr int kLaterFailure = -17;
    OrderedRetirer<Completion, 4> retire(true);

    const auto request0 = retire.issue();
    const auto request1 = retire.issue();
    const auto status = retire.issue();
    const auto signal = retire.issue();
    check(request0 && request1 && status && signal, "all synthetic queue entries receive tickets");
    check(request0.ticket.value == 0 && request1.ticket.value == 1 && status.ticket.value == 2 &&
              signal.ticket.value == 3,
          "tickets preserve submission sequence");

    // Deliberately complete the later control entries and request 1 before request 0. None may publish early.
    check(retire.complete(signal.ticket, {Completion::Kind::Signal, 4, kOk}) == Refusal::none,
          "later signal completion is accepted");
    check(retire.complete(status.ticket, {Completion::Kind::Status, 3, kOk}) == Refusal::none,
          "status completion may arrive before its earlier requests");
    check(retire.complete(request1.ticket, {Completion::Kind::Request, 2, kLaterFailure}) == Refusal::none,
          "later request failure is accepted");
    const auto blocked = retire.take_ready();
    check(blocked.refusal == Refusal::waiting_for_earlier && retire.outstanding() == 4,
          "status and signal stay held behind the first unfinished request");

    check(retire.complete(request0.ticket, {Completion::Kind::Request, 1, kEarlierFailure}) == Refusal::none,
          "front request failure is accepted");
    std::array<int, 4> retired_ids{};
    std::size_t retired_count = 0;
    int first_failure_since_status = kOk;
    int observed_status = 12345;
    std::size_t signal_position = 0;
    while (retired_count < retired_ids.size()) {
        auto ready = retire.take_ready();
        check(static_cast<bool>(ready), "a contiguous completed ticket is reaped");
        if (!ready) break;
        const Completion& value = *ready.value;
        retired_ids[retired_count++] = value.id;
        if (value.kind == Completion::Kind::Request && first_failure_since_status == kOk && value.hr != kOk)
            first_failure_since_status = value.hr;
        if (value.kind == Completion::Kind::Status) {
            observed_status = first_failure_since_status;
            first_failure_since_status = kOk;
        }
        if (value.kind == Completion::Kind::Signal) signal_position = retired_count;
    }
    check(retired_count == 4, "all four entries drain once the head completes");
    check(retired_ids == std::array<int, 4>{1, 2, 3, 4}, "reaped payloads stay in submission order");
    check(observed_status == kEarlierFailure,
          "status folding reports the earlier enqueue failure, not the later finish-order failure");
    check(signal_position == 4, "fence/event-style side effect remains after its preceding status entry");
    check(retire.outstanding() == 0 && retire.take_ready().refusal == Refusal::empty,
          "drained policy reports empty");
}

void test_capacity_and_slot_reuse() {
    OrderedRetirer<int, 2> retire(true);
    const auto first = retire.issue();
    const auto second = retire.issue();
    const auto full = retire.issue();
    check(first && second, "bounded buffer admits up to its capacity");
    check(full.refusal == Refusal::capacity_full && retire.outstanding() == 2,
          "full buffer refuses another ticket by name without changing state");
    check(std::strcmp(palmetto::stream_reap::refusal_name(full.refusal), "capacity_full") == 0,
          "capacity refusal has a stable name");

    check(retire.complete(second.ticket, 20) == Refusal::none, "later slot may complete while the front is pending");
    check(retire.take_ready().refusal == Refusal::waiting_for_earlier,
          "completed later slot cannot bypass a pending front slot");
    check(retire.complete(first.ticket, 10) == Refusal::none, "front slot completion is accepted");
    auto reaped_first = retire.take_ready();
    check(reaped_first && reaped_first.ticket.value == first.ticket.value && *reaped_first.value == 10,
          "front slot becomes reusable only after its value is taken");

    const auto third = retire.issue();
    check(third && third.ticket.value == 2, "released slot admits the next monotonic ticket");
    check(retire.complete(third.ticket, 30) == Refusal::none, "reused slot accepts the new completion");
    const auto reaped_second = retire.take_ready();
    const auto reaped_third = retire.take_ready();
    check(reaped_second && *reaped_second.value == 20 && reaped_second.ticket.value == 1,
          "older buffered completion remains first after slot reuse");
    check(reaped_third && *reaped_third.value == 30 && reaped_third.ticket.value == 2,
          "new slot is reaped after the older completion");
}

void test_named_bad_completion_refusals() {
    OrderedRetirer<int, 3> retire(true);
    check(retire.complete(Ticket{99}, 99) == Refusal::unknown_ticket,
          "future ticket is refused as unknown_ticket");
    const auto ticket = retire.issue();
    check(static_cast<bool>(ticket), "test ticket is admitted");
    check(retire.complete(ticket.ticket, 5) == Refusal::none, "first completion is accepted");
    check(retire.complete(ticket.ticket, 6) == Refusal::duplicate_completion,
          "second completion is refused as duplicate_completion");
    const auto reaped = retire.take_ready();
    check(reaped && *reaped.value == 5, "the accepted payload survives a duplicate attempt");
    check(retire.complete(ticket.ticket, 7) == Refusal::unknown_ticket,
          "retired ticket is refused as unknown_ticket");
    check(std::strcmp(palmetto::stream_reap::refusal_name(Refusal::duplicate_completion), "duplicate_completion") == 0,
          "duplicate refusal has a stable name");
    check(std::strcmp(palmetto::stream_reap::refusal_name(Refusal::unknown_ticket), "unknown_ticket") == 0,
          "unknown-ticket refusal has a stable name");
}

void test_sequence_does_not_wrap() {
    const auto last_safe = std::numeric_limits<std::uint64_t>::max() - 1;
    OrderedRetirer<int, 2> retire(true, last_safe);
    const auto ticket = retire.issue();
    check(ticket && ticket.ticket.value == last_safe, "last non-wrapping ticket may be issued");
    check(retire.complete(ticket.ticket, 42) == Refusal::none, "last non-wrapping ticket may complete");
    const auto result = retire.take_ready();
    check(result && result.ticket.value == last_safe && *result.value == 42, "last ticket is reaped normally");
    const auto exhausted = retire.issue();
    check(exhausted.refusal == Refusal::sequence_exhausted && retire.outstanding() == 0,
          "ticket counter refuses wraparound by name");
    check(std::strcmp(palmetto::stream_reap::refusal_name(exhausted.refusal), "sequence_exhausted") == 0,
          "sequence refusal has a stable name");
}

}  // namespace

int main() {
    test_switch_and_default_off();
    test_out_of_order_work_retires_in_order();
    test_capacity_and_slot_reuse();
    test_named_bad_completion_refusals();
    test_sequence_does_not_wrap();
    std::printf("stream_reap_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
