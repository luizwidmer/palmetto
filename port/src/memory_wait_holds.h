#pragma once

// The console's cross-list memory wait and its GPU-ordered bottom-of-pipe writes (docs/peer_port_streaming.md, the
// implemented port 4-5 section; docs/bottom_of_pipe.md is the measured shape). This header is the pure part of the
// port: the switch values, the measured comparison, the wait-hold records with their bounded release, and the
// segment plan that a batch of split lists becomes. It has no Vulkan, Windows, clock or thread dependency: the
// shim (shims/d3d12_xs_device.cpp) is the runtime user, the caller passes its own clock, and the GPU-free proxy test
// pins the state machine.
//
// The measured situation this policy answers (docs/bottom_of_pipe.md, docs/peer_port_streaming.md section 5.2): a
// console list records Wait64BitValueX(address, comparison, qword, flags); the GPU does not go on with the rest of
// the list until the cell compares true, and the writer of the cell can be a later list ("the state store may sit
// behind a GPU-ordered cell"). Our model evaluates the wait at record time and never holds. The port here splits the
// list at an unmet wait, replays the state the list had set into the segment after it, posts the queue's own Wait on
// a fence before that segment, and a watcher thread CPU-signals the fence once the cell holds (or at the bound). The
// fence is the layer's existing Wait-before-Signal hold (queue_holds.h), so the release path is the one the layer
// already owns; this header is only the policy and the bookkeeping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace palmetto::memory_waits {

// ---- The measured comparison ---------------------------------------------------------------------------------------
//
// The slot-74 call site passes comparison 7 (D3D12_COMPARISON_FUNC_GREATER_EQUAL) and flags 1, and the whole 64 bits
// are compared (docs/bottom_of_pipe.md section 2.5). No other comparison is measured at any call site, so any other
// shape is refused by name rather than guessed.
inline constexpr std::uint64_t comparison_greater_equal = 7;

inline bool comparison_met(std::uint64_t comparison, std::uint64_t current, std::uint64_t target) {
    return comparison == comparison_greater_equal && current >= target;
}

// One Wait64BitValueX as recorded, after the shape checks.
struct Wait {
    std::uint64_t address = 0;
    std::uint64_t comparison = 0;
    std::uint64_t value = 0;

    bool empty() const { return address == 0; }
};

// ---- The switches (docs/peer_port_streaming.md section 11.3) ------------------------------------------------------
//
// Both default off. PALMETTO_XS_MEMORY_WAIT_HOLD turns the cross-list hold on, PALMETTO_XS_BOTTOM_OF_PIPE_GPU turns
// the GPU-ordered write form on; either is usable alone, and with both unset the measured path is the model of
// docs/bottom_of_pipe.md (record-time write, record-time evaluation, no thread, no split).
struct Settings {
    bool hold = false;
    bool gpu_writes = false;
    std::uint64_t bound_ms = 5000;  // the peers' bound; a wait that does not hold by then is released and counted
    std::size_t max_holds = 16;     // concurrent held segments (each costs a fence and a queue hold)
};

// Every "off" spelling a boot script or a test may use: unset, empty, "0", off/OFF, false/no.
inline bool bool_switch_on(const char* value) {
    if (!value || !*value) return false;
    const char* at = value;
    while (*at == ' ' || *at == '\t') ++at;
    if (*at == '\0') return false;
    if (*at == '0' && at[1] == '\0') return false;
    char word[8] = {};
    std::size_t n = 0;
    for (; at[n] && n + 1 < sizeof(word); ++n) word[n] = at[n];
    for (std::size_t i = 0; i < n; ++i)
        if (word[i] >= 'A' && word[i] <= 'Z') word[i] = static_cast<char>(word[i] - 'A' + 'a');
    return !(n == 2 && word[0] == 'n' && word[1] == 'o') && !(n >= 3 && word[0] == 'o' && word[1] == 'f' && word[2] == 'f') &&
           !(n == 5 && word[0] == 'f' && word[1] == 'a' && word[2] == 'l' && word[3] == 's' && word[4] == 'e');
}

// A decimal count/interval with a floor and a ceiling; a value that does not parse keeps the default.
inline std::uint64_t number_switch(const char* value, std::uint64_t fallback, std::uint64_t floor,
                                   std::uint64_t ceiling) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (end == value) return fallback;
    if (parsed < floor) return floor;
    if (parsed > ceiling) return ceiling;
    return parsed;
}

inline Settings settings_from(const char* hold, const char* bound_ms, const char* max_holds, const char* gpu_writes) {
    Settings settings;
    settings.hold = bool_switch_on(hold);
    settings.gpu_writes = bool_switch_on(gpu_writes);
    settings.bound_ms = number_switch(bound_ms, 5000, 1, 600000);
    settings.max_holds = static_cast<std::size_t>(number_switch(max_holds, 16, 1, 256));
    return settings;
}

// ---- The hold records ---------------------------------------------------------------------------------------------
//
// One hold per split boundary of one submission: the queue whose later work is held, the list that recorded the wait,
// the wait itself and a deadline. The runtime watcher polls the cell and calls observe(); a later recorded
// bottom-of-pipe write calls wrote(); expire() turns a hold that outlived the bound into a timed-out release. Holds
// of one queue release in submission order (a later boundary never overtakes an earlier one), which is the order the
// layer's held deque gives the work itself.
struct Hold {
    enum class State : std::uint8_t { pending, satisfied, timed_out, cancelled, dropped };

    std::uint64_t token = 0;  // 1-based, submission order across the process
    std::uint64_t queue = 0;  // the queue proxy whose submission is held
    std::uint64_t list = 0;   // the list proxy that recorded the wait
    Wait wait;
    std::uint64_t deadline_ms = 0;
    State state = State::pending;
    bool released = false;  // the watcher has CPU-signalled the hold's fence (or the hold was dropped)
};

class Holds {
public:
    static constexpr std::size_t capacity = 256;

    // Adds a hold. 0 when the bound is reached: the wait then stays the record-time evaluation (the bounded
    // fallback), which the caller counts by name. The caller's clock is steady-clock milliseconds.
    std::uint64_t add(std::uint64_t queue, std::uint64_t list, const Wait& wait, std::uint64_t now_ms,
                      std::uint64_t bound_ms, std::size_t max_holds) {
        if (count_ >= capacity || max_holds == 0) return 0;
        std::size_t holds = 0;
        for (const Hold& hold : holds_)
            if (hold.state == Hold::State::pending && !hold.released) ++holds;
        if (holds >= max_holds) return 0;
        Hold hold;
        hold.token = next_token_++;
        hold.queue = queue;
        hold.list = list;
        hold.wait = wait;
        hold.deadline_ms = now_ms > UINT64_MAX - bound_ms ? UINT64_MAX : now_ms + bound_ms;
        holds_.push_back(hold);
        ++count_;
        ++recorded_;
        return hold.token;
    }

    // The cell holds `current`: every pending hold for that address whose comparison the value meets is satisfied.
    // Returns how many became satisfied. `written_by_later_list` counts the holds the caller has already reported as
    // met by a later list's recorded write (wrote()).
    std::size_t observe(std::uint64_t address, std::uint64_t current) {
        std::size_t met = 0;
        for (Hold& hold : holds_) {
            if (hold.state != Hold::State::pending || hold.wait.address != address) continue;
            if (!comparison_met(hold.wait.comparison, current, hold.wait.value)) continue;
            hold.state = Hold::State::satisfied;
            ++met;
            ++satisfied_;
        }
        return met;
    }

    // A bottom-of-pipe write recorded on `writer_list` after a hold was recorded: the measured cross-list case when
    // the writer is a different list. The write's value satisfies a hold when the comparison holds. Cross-list
    // dependencies are counted whether or not the hold was met, because that is the measurement this port answers.
    std::size_t wrote(std::uint64_t address, std::uint64_t value, std::uint64_t writer_list) {
        std::size_t met = 0;
        for (Hold& hold : holds_) {
            if (hold.state != Hold::State::pending || hold.wait.address != address) continue;
            if (!comparison_met(hold.wait.comparison, value, hold.wait.value)) continue;
            ++written_met_;
            if (hold.list != writer_list) ++cross_list_;
            else ++same_list_;
            hold.state = Hold::State::satisfied;
            ++met;
            ++satisfied_;
        }
        return met;
    }

    // Every pending hold whose deadline has passed becomes timed_out. Returns how many.
    std::size_t expire(std::uint64_t now_ms) {
        std::size_t expired = 0;
        for (Hold& hold : holds_) {
            if (hold.state != Hold::State::pending || now_ms < hold.deadline_ms) continue;
            hold.state = Hold::State::timed_out;
            ++expired;
            ++timed_out_;
        }
        return expired;
    }

    // The oldest unreleased hold of a queue; false when the queue holds nothing. The watcher polls its cell.
    bool front(std::uint64_t queue, Hold& out) const {
        for (const Hold& hold : holds_) {
            if (hold.queue != queue || hold.released) continue;
            out = hold;
            return true;
        }
        return false;
    }

    // The hold that may be released now: the queue's oldest unreleased hold when its state is satisfied or timed_out.
    // 0 when it is still pending (waiting for its cell or its bound).
    std::uint64_t releasable(std::uint64_t queue) const {
        for (const Hold& hold : holds_) {
            if (hold.queue != queue || hold.released) continue;
            if (hold.state == Hold::State::satisfied || hold.state == Hold::State::timed_out)
                return hold.token;
            return 0;
        }
        return 0;
    }

    // The shim has CPU-signalled the hold's fence. The layer's held segment runs when the signal is applied.
    bool release(std::uint64_t token) {
        for (Hold& hold : holds_) {
            if (hold.token != token || hold.released) continue;
            hold.released = true;
            ++released_;
            return true;
        }
        return false;
    }

    // Every queue with an unreleased hold, most recently added last; the watcher's poll set.
    std::size_t queues(std::uint64_t* out, std::size_t room) const {
        std::size_t n = 0;
        for (const Hold& hold : holds_) {
            if (hold.released) continue;
            bool seen = false;
            for (std::size_t i = 0; i < n; ++i) seen = seen || out[i] == hold.queue;
            if (seen) continue;
            if (n == room) break;
            out[n++] = hold.queue;
        }
        return n;
    }

    // The queue is gone: its holds are dropped (the layer reports the dropped batches at queue teardown).
    std::size_t cancel_queue(std::uint64_t queue) {
        std::size_t cancelled = 0;
        for (Hold& hold : holds_) {
            if (hold.queue != queue || hold.released) continue;
            hold.state = Hold::State::cancelled;
            hold.released = true;
            ++cancelled;
            ++released_;
        }
        cancelled_ += cancelled;
        return cancelled;
    }

    // A device loss or shutdown drops every hold.
    std::size_t cancel_all() {
        std::size_t cancelled = 0;
        for (Hold& hold : holds_) {
            if (hold.released) continue;
            if (hold.state == Hold::State::pending) hold.state = Hold::State::cancelled;
            hold.released = true;
            ++cancelled;
            ++released_;
        }
        cancelled_ += cancelled;
        return cancelled;
    }

    std::size_t pending() const {
        std::size_t n = 0;
        for (const Hold& hold : holds_)
            if (!hold.released) ++n;
        return n;
    }

    // The next deadline among pending holds, 0 when none is pending: the watcher sleeps no longer than this.
    std::uint64_t next_deadline() const {
        std::uint64_t next = 0;
        for (const Hold& hold : holds_) {
            if (hold.released || hold.state != Hold::State::pending) continue;
            if (!next || hold.deadline_ms < next) next = hold.deadline_ms;
        }
        return next;
    }

    std::size_t capacity_used() const { return holds_.size(); }

    struct Counts {
        std::uint64_t recorded = 0, satisfied = 0, timed_out = 0, released = 0, cancelled = 0;
        std::uint64_t written_met = 0, cross_list = 0, same_list = 0;
    };
    Counts counts() const {
        return {recorded_, satisfied_, timed_out_, released_, cancelled_, written_met_, cross_list_, same_list_};
    }

    std::string report() const {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "holds recorded %llu satisfied %llu timed out %llu released %llu cancelled %llu | met by a "
                      "later write %llu (cross-list %llu, same-list %llu) | pending %zu",
                      static_cast<unsigned long long>(recorded_), static_cast<unsigned long long>(satisfied_),
                      static_cast<unsigned long long>(timed_out_), static_cast<unsigned long long>(released_),
                      static_cast<unsigned long long>(cancelled_), static_cast<unsigned long long>(written_met_),
                      static_cast<unsigned long long>(cross_list_), static_cast<unsigned long long>(same_list_),
                      pending());
        return line;
    }

private:
    std::vector<Hold> holds_;
    std::size_t count_ = 0;
    std::uint64_t next_token_ = 1;
    std::uint64_t recorded_ = 0, satisfied_ = 0, timed_out_ = 0, released_ = 0, cancelled_ = 0;
    std::uint64_t written_met_ = 0, cross_list_ = 0, same_list_ = 0;
};

// ---- The segment plan ---------------------------------------------------------------------------------------------
//
// A batch is an array of command lists. A list split at a wait becomes several parts: part 0 is everything recorded
// before the first wait, part i the commands between wait i-1 and wait i. At submission the batch becomes segments in
// this order: every part up to a wait in one ExecuteCommandLists, then the queue's Wait on the fence of that wait,
// then the parts after it - so the work before a wait is never held behind it (the peer shape: five-star ListSplits,
// victory list.rs cut). This builder is pure; the shim feeds it the part counts and waits and then replays the same
// order with its real list handles.
struct BatchPart {
    std::uint64_t list = 0;  // the list proxy's identity
    std::uint32_t part = 0;  // 0 = before the first wait
};

class BatchPlan {
public:
    static constexpr std::size_t max_parts = 64;
    static constexpr std::size_t max_waits = 63;

    // Adds one list of a batch: `part_count` parts and the `part_count - 1` waits between them (waits[j] precedes
    // part j + 1). False when the plan would exceed its bound; the caller then submits the batch unsplit (the
    // record-time fallback) and counts the refusal by name.
    bool add_list(std::uint64_t list, std::uint32_t part_count, const Wait* waits) {
        if (part_count == 0 || parts_ + part_count > max_parts) return false;
        std::size_t added_waits = 0;
        for (std::uint32_t part = 1; part < part_count; ++part)
            if (waits && !waits[part - 1].empty()) ++added_waits;
        if (waits_ + added_waits > max_waits) return false;
        for (std::uint32_t part = 0; part < part_count; ++part) {
            part_list_[parts_] = list;
            part_index_[parts_] = part;
            wait_[parts_] = (part && waits) ? waits[part - 1] : Wait{};
            ++parts_;
        }
        waits_ += added_waits;
        return true;
    }

    std::size_t parts() const { return parts_; }

    // The segments this batch becomes: one per run of parts with no wait between them.
    std::size_t segments() const {
        std::size_t segments = parts_ ? 1 : 0;
        for (std::size_t i = 1; i < parts_; ++i)
            if (!wait_[i].empty()) ++segments;
        return segments;
    }

    BatchPart part(std::size_t index) const {
        return index < parts_ ? BatchPart{part_list_[index], part_index_[index]} : BatchPart{};
    }

    // The wait that precedes parts()[index]; empty for index 0 and for a part that continues a segment.
    Wait wait_before(std::size_t index) const { return index < parts_ ? wait_[index] : Wait{}; }

    // Which segment parts()[index] runs in: 0 for the first segment, one more after each wait.
    std::size_t segment_of(std::size_t index) const {
        std::size_t segment = 0;
        for (std::size_t i = 1; i <= index && i < parts_; ++i)
            if (!wait_[i].empty()) ++segment;
        return segment;
    }

private:
    std::array<std::uint64_t, max_parts> part_list_{};
    std::array<std::uint32_t, max_parts> part_index_{};
    std::array<Wait, max_parts> wait_{};  // wait_[i]: before parts()[i]; empty when the part continues a segment
    std::size_t parts_ = 0;
    std::size_t waits_ = 0;
};

}  // namespace palmetto::memory_waits
