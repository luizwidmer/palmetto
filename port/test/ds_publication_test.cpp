// Synthetic tests for the pure ordered-publication policy in port/src/ds_publication.h.
//
// The falsifier comes from docs/peer_port_streaming.md section 17.4: request 1 fails before request 0 completes
// and the later status/signal entries also complete early. No later entry may publish before request 0, and the
// status entry must report the first failed request in *enqueue* order, not the first in finish order.
//
// No Windows API beyond the compiler, no GPU, no title boot, no files, no game data and no peer code.
#include "../src/ds_publication.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using palmetto::ds_publication::ConfigRefusal;
using palmetto::ds_publication::EntryKind;
using palmetto::ds_publication::FailureFold;
using palmetto::ds_publication::OrderedPublisher;
using palmetto::ds_publication::Refusal;
using palmetto::ds_publication::Ticket;

int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

bool refusal_is(Refusal refusal, const char* name) {
    return std::strcmp(palmetto::stream_reap::refusal_name(refusal), name) == 0;
}

struct Payload {
    EntryKind kind = EntryKind::request;
    int id = 0;
    std::int32_t hr = 0;
};

// The publisher's drain callback in these tests applies the same publication rule the queue wires in
// (port/shims/dstorage.cpp): the fold is updated per kind and a status entry observes the folded answer.
struct RecordingPublisher {
    std::vector<int> published_ids;
    std::vector<std::int32_t> published_status;
    FailureFold fold;

    void operator()(Payload&& value, Ticket) {
        const std::int32_t answered = palmetto::ds_publication::note_entry(fold, value.kind, value.hr);
        published_ids.push_back(value.id);
        if (value.kind == EntryKind::status) published_status.push_back(answered);
    }
};

void test_switch_and_option_gate() {
    using palmetto::ds_publication::resolve;

    const auto unset = resolve(nullptr, false);
    check(!unset.ordered && unset.refusal == ConfigRefusal::switch_off && !unset.switch_enabled,
          "an unset switch keeps the serial path");
    check(unset.switch_refusal == Refusal::disabled, "an unset switch parses as disabled");
    check(unset.lanes == 1 && unset.capacity == 0, "the serial path starts one lane and no retirement buffer");

    const auto empty = resolve("", false);
    check(!empty.ordered && empty.refusal == ConfigRefusal::switch_off, "an empty switch keeps the serial path");
    for (const char* text : {"0", "OFF", "No"}) {
        const auto negative = resolve(text, false);
        check(!negative.ordered && negative.refusal == ConfigRefusal::switch_off,
              "an explicit negative keeps the serial path");
    }
    for (const char* text : {"1", "on", "YES", "True"}) {
        const auto positive = resolve(text, false);
        check(positive.ordered && positive.refusal == ConfigRefusal::none && positive.switch_enabled,
              "an explicit affirmative enables the ordered path");
        check(positive.lanes == palmetto::ds_publication::kReapLanes &&
                  positive.capacity == palmetto::ds_publication::kReapCapacity,
              "the ordered path starts the bounded lane count and retirement capacity");
        check(positive.lanes > 1 && positive.capacity > positive.lanes,
              "the ordered path is genuinely concurrent and its buffer is deeper than its lanes");
    }

    const auto typo = resolve("oen", false);
    check(!typo.ordered && typo.refusal == ConfigRefusal::switch_invalid &&
              typo.switch_refusal == Refusal::invalid_switch_value,
          "an unknown spelling is the named invalid switch refusal and stays off");
    check(!resolve("1 ", false).ordered && resolve("1 ", false).refusal == ConfigRefusal::switch_invalid,
          "a trailing space is not the exact opt-in and is refused by name");

    // The option gate: the staging scratch is per-queue single-worker state, so two lanes must not share it.
    const auto staging = resolve("1", true);
    check(!staging.ordered && staging.refusal == ConfigRefusal::staging_scratch_shared && staging.lanes == 1,
          "the ordered path is refused by name when the single-worker staging scratch is enabled");
    const auto staging_without_switch = resolve(nullptr, true);
    check(!staging_without_switch.ordered && staging_without_switch.refusal == ConfigRefusal::switch_off,
          "staging alone does not enable the ordered path");

    check(std::strcmp(palmetto::ds_publication::config_refusal_name(ConfigRefusal::none), "none") == 0 &&
              std::strcmp(palmetto::ds_publication::config_refusal_name(ConfigRefusal::switch_off), "switch_off") == 0 &&
              std::strcmp(palmetto::ds_publication::config_refusal_name(ConfigRefusal::switch_invalid),
                          "switch_invalid") == 0 &&
              std::strcmp(palmetto::ds_publication::config_refusal_name(ConfigRefusal::staging_scratch_shared),
                          "staging_scratch_shared") == 0 &&
              std::strcmp(palmetto::ds_publication::config_refusal_name(ConfigRefusal::allocation_failed),
                          "allocation_failed") == 0,
          "every config refusal has a stable printable name");
}

void test_default_off_publisher_changes_nothing() {
    OrderedPublisher<Payload> publisher;
    check(!publisher.enabled() && publisher.outstanding() == 0, "a default publisher is disabled and empty");
    const auto admission = publisher.admit();
    check(admission.refusal == Refusal::disabled, "a disabled publisher refuses admission by name");
    check(publisher.complete(Ticket{0}, Payload{EntryKind::request, 1, 0}) == Refusal::disabled,
          "a disabled publisher refuses completion by name");
    RecordingPublisher recorder;
    std::size_t published = 99;
    check(publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published) ==
                  Refusal::disabled &&
              published == 0 && recorder.published_ids.empty(),
          "a disabled publisher drains nothing, reports disabled by name and changes no state");
}

void test_reversed_completion_publishes_in_enqueue_order() {
    constexpr std::int32_t kHeadFailure = -11;
    constexpr std::int32_t kLaterFailure = -17;
    OrderedPublisher<Payload, 8> publisher(true);
    const auto request0 = publisher.admit();
    const auto request1 = publisher.admit();
    const auto status = publisher.admit();
    const auto signal = publisher.admit();
    check(request0 && request1 && status && signal, "every synthetic entry is admitted in submission order");
    check(request0.ticket.value == 0 && request1.ticket.value == 1 && status.ticket.value == 2 &&
              signal.ticket.value == 3,
          "admission tickets preserve the enqueue sequence");

    // The later entries finish first, exactly the shape the queue's two lanes can produce.
    check(publisher.complete(signal.ticket, Payload{EntryKind::signal, 4, 0}) == Refusal::none,
          "a late signal completion is accepted");
    check(publisher.complete(status.ticket, Payload{EntryKind::status, 3, 0}) == Refusal::none,
          "a status completion may arrive before its earlier requests");
    check(publisher.complete(request1.ticket, Payload{EntryKind::request, 2, kLaterFailure}) == Refusal::none,
          "a later request failure is accepted");

    RecordingPublisher recorder;
    std::size_t published = 0;
    check(publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published) == Refusal::none,
          "a drain with the head outstanding is not an error");
    check(published == 0 && recorder.published_ids.empty(),
          "no later completion is published while the head request is outstanding");
    check(publisher.outstanding() == 4, "the buffered completions stay outstanding");

    check(publisher.complete(request0.ticket, Payload{EntryKind::request, 1, kHeadFailure}) == Refusal::none,
          "the head request completes last");
    check(publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published) == Refusal::none &&
              published == 4,
          "one drain publishes the whole contiguous completed prefix");
    check(recorder.published_ids == std::vector<int>{1, 2, 3, 4}, "publication order is enqueue order, not finish order");
    check(recorder.published_status.size() == 1 && recorder.published_status[0] == kHeadFailure,
          "the status entry reports the earlier enqueue failure, not the later finish-order failure");
    check(publisher.outstanding() == 0, "the drained publisher is empty");

    std::size_t again = 99;
    check(publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, again) == Refusal::none &&
              again == 0 && recorder.published_ids.size() == 4,
          "a second drain publishes nothing and duplicates no payload");
}

void test_failure_fold_rules() {
    using palmetto::ds_publication::note_entry;
    FailureFold fold;
    check(fold.consume() == 0, "an empty fold reports success");
    check(note_entry(fold, EntryKind::request, -5) == -5 && fold.pending() == -5,
          "a failed request becomes the pending failure");
    check(note_entry(fold, EntryKind::request, -9) == -9 && fold.pending() == -5,
          "a later failed request does not replace the pending failure");
    check(note_entry(fold, EntryKind::request, 1) == 1 && fold.pending() == -5,
          "a successful request is not a failure and keeps the fold");
    check(note_entry(fold, EntryKind::status, 0) == -5 && fold.pending() == 0,
          "a status entry consumes the folded failure and resets it");
    check(note_entry(fold, EntryKind::status, -3) == -3 && fold.pending() == 0,
          "a status entry that failed itself reports its own answer and does not consume the fold");
    check(note_entry(fold, EntryKind::request, -7) == -7 && fold.pending() == -7, "the fold restarts after a status");
    check(note_entry(fold, EntryKind::signal, 0) == 0 && fold.pending() == 0,
          "a fence signal resets the fold like the serial queue");
    check(note_entry(fold, EntryKind::event, -1) == -1 && fold.pending() == 0,
          "an event entry publishes its own answer without touching the fold");
}

void test_capacity_backpressure_before_dispatch() {
    OrderedPublisher<Payload, 2> publisher(true);
    const auto first = publisher.admit();
    const auto second = publisher.admit();
    const auto third = publisher.admit();
    check(first && second, "the bounded buffer admits up to its capacity");
    check(third.refusal == palmetto::ds_publication::Refusal::capacity_full && publisher.outstanding() == 2,
          "a full buffer refuses the next admission by name, before it can be dispatched");
    check(refusal_is(third.refusal, "capacity_full"), "the capacity refusal has a stable name");

    check(publisher.complete(second.ticket, Payload{EntryKind::request, 2, 0}) == Refusal::none,
          "the later buffered entry may complete first");
    RecordingPublisher recorder;
    std::size_t published = 0;
    publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published);
    check(published == 0, "the completed later entry cannot be published past the head");
    check(publisher.complete(first.ticket, Payload{EntryKind::request, 1, 0}) == Refusal::none,
          "the head completes");
    publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published);
    check(published == 2 && recorder.published_ids == std::vector<int>{1, 2},
          "both buffered entries publish in order once the head retires");
    const auto fourth = publisher.admit();
    check(fourth && fourth.ticket.value == 2, "the freed buffer slot admits the next monotonic ticket");
}

void test_named_bad_completions_fail_closed() {
    OrderedPublisher<Payload, 4> publisher(true);
    check(publisher.complete(Ticket{9}, Payload{EntryKind::request, 9, 0}) == Refusal::unknown_ticket,
          "a future ticket is refused as unknown_ticket");
    const auto ticket = publisher.admit();
    check(static_cast<bool>(ticket), "the test ticket is admitted");
    check(publisher.complete(ticket.ticket, Payload{EntryKind::request, 1, 0}) == Refusal::none,
          "the first completion is accepted");
    check(publisher.complete(ticket.ticket, Payload{EntryKind::request, 1, -4}) == Refusal::duplicate_completion,
          "a second completion is refused as duplicate_completion");
    RecordingPublisher recorder;
    std::size_t published = 0;
    publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published);
    check(published == 1 && recorder.published_ids == std::vector<int>{1},
          "the accepted payload survives a refused duplicate and publishes once");
    check(publisher.complete(ticket.ticket, Payload{EntryKind::request, 1, 0}) == Refusal::unknown_ticket,
          "a retired ticket is refused as unknown_ticket");
    check(refusal_is(Refusal::duplicate_completion, "duplicate_completion") &&
              refusal_is(Refusal::unknown_ticket, "unknown_ticket"),
          "both bad-completion refusals keep their stable names");
}

void test_two_lanes_never_interleave_publication() {
    // The runtime contract: two lanes complete under one publication mutex, and the same critical section
    // drains and publishes. Whatever the finish order, publication is enqueue order and the status entry
    // observes the first enqueue-order failure.
    OrderedPublisher<Payload, 16> publisher(true);
    std::mutex publication;
    RecordingPublisher recorder;
    constexpr int kEntries = 8;
    std::array<Ticket, kEntries> tickets{};
    for (int i = 0; i < kEntries; ++i) {
        const auto admission = publisher.admit();
        check(static_cast<bool>(admission), "every lane-scheduled entry is admitted before dispatch");
        tickets[static_cast<std::size_t>(i)] = admission.ticket;
    }
    // Lanes run in reverse order to force the worst finish order: lane 7 completes first, lane 0 last.
    const auto lane = [&](int index) {
        std::lock_guard lock(publication);
        const std::int32_t hr = index == 0 ? -11 : (index == 3 ? -17 : 0);
        const EntryKind kind = index == 5 ? EntryKind::status : (index == 7 ? EntryKind::signal : EntryKind::request);
        const Refusal refused = publisher.complete(tickets[static_cast<std::size_t>(index)],
                                                   Payload{kind, index + 1, hr});
        check(refused == Refusal::none, "a lane completion is accepted");
        std::size_t published = 0;
        publisher.drain([&](Payload&& value, Ticket t) { recorder(std::move(value), t); }, published);
    };
    std::thread later([&] {
        for (int index = kEntries - 1; index >= 1; --index) lane(index);
    });
    later.join();
    lane(0);
    check(recorder.published_ids.size() == static_cast<std::size_t>(kEntries),
          "every entry publishes exactly once after both lanes finish");
    bool in_order = recorder.published_ids.size() == static_cast<std::size_t>(kEntries);
    for (int i = 0; in_order && i < kEntries; ++i)
        in_order = recorder.published_ids[static_cast<std::size_t>(i)] == i + 1;
    check(in_order, "the single publisher keeps the two lanes' completions in enqueue order");
    check(recorder.published_status.size() == 1 && recorder.published_status[0] == -11,
          "the status entry behind both failures reports the enqueue-order first failure");
}

}  // namespace

int main() {
    test_switch_and_option_gate();
    test_default_off_publisher_changes_nothing();
    test_reversed_completion_publishes_in_enqueue_order();
    test_failure_fold_rules();
    test_capacity_backpressure_before_dispatch();
    test_named_bad_completions_fail_closed();
    test_two_lanes_never_interleave_publication();
    std::printf("ds_publication_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
