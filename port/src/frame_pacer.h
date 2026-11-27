#pragma once

// Frame pacing for the Xbox presentation path (docs/d3d12_vulkan.md section 4.5): what SetFrameIntervalX,
// ScheduleFrameEventX, WaitFrameEventX and GetFrameStatisticsX do, and the token check and flip bookkeeping that
// PresentX needs. No Vulkan and no Windows headers: time comes from an injectable FrameClock, so the tests drive it
// with a fake clock and the real runtime uses a steady high-resolution timer (a host vblank source can replace it).
//
// What is known and what is ours. From the public GDKX samples (section 4.5): the call order, the argument counts, that
// the interval call takes an allowed frame latency (buffer count - 1 in one sample, 1 in another), that the origin
// event is scheduled once and waited on every frame, that the wait yields a token that PresentX carries, and that
// frame events must be registered again after ResumeX. Everything below the call shapes is our own model, chosen to
// be deterministic and to behave like the samples expect; none of it is measured on the console:
//   * An interval is a period in microseconds (the real enum values are unmeasured).
//   * Vblanks happen at epoch + n * period (n = 1, 2, ...), the epoch being the time of SetFrameIntervalX. The
//     first origin after ScheduleFrameEventX is the next vblank. The origin event of vblank n is signalled
//     offset microseconds after it (ScheduleFrameEventX's offset).
//   * A frame is "in flight" from PresentX until the vblank that shows it. Frames show one per vblank, in order.
//     WaitFrameEventX does not return while `latency` frames are in flight. A frame presented while the origin event
//     is not armed is not paced (PresentX does not hold it for a vblank): it shows at once and is never in flight.
//   * A wait that comes late (after the next origin already passed) returns at once with the newest passed origin;
//     the missed origins are skipped, not queued.
//   * Tokens are 1, 2, 3, ... (0 is the null token), one per successful wait, never reused, across suspend/resume.
//   * A frame is due at the vblank after the origin that handed out its token; a PresentX after that vblank is late by
//     the time since it (present_timing), and ImmediateThresholdPercent decides whether that lateness is shown at once
//     (decide_present_sync).
//
// Uncapped (set_uncapped; PALMETTO_PRESENT_UNCAP in the shim): the registration calls keep working and waits keep
// handing out one token each, but a wait never blocks — no vblank grid, no latency throttle — and a presented
// frame counts as shown at once. The frame rate is then the title's own; the error surface (nothing registered,
// suspended) is unchanged.
//
// Present-driven (set_present_driven; PALMETTO_PRESENT_DRIVEN in the shim): the clock has no independent vblank grid;
// the present is the clock. The peer evidence (docs/peer_port_first_ui.md sections 2-4 and 6: a token per origin and a
// blocking flip wait before a present, a plane's token used up by a present with held waits released at a present, a
// present marking the frame token flipped, a vsync present pacing the title) describes the console handshake this mode
// models, in our own terms:
//   * A wait hands out token 1, 2, 3, ... (one per successful wait, never reused). Before the first present there is
//     no displayed frame to follow, so waits return at once: the layer cannot wait for a clock that does not exist yet.
//   * The present that carries a token is its flip: commit_present_token marks the record presented and displayed at
//     the present's time, and frees one of the `latency` in-flight slots. A wait that was held because `latency`
//     frames were in flight is released exactly at that present (the notification carries no time of its own), and a
//     wait that arrives after the flip finds the slot free and returns on that acquire.
//   * The cadence is the one measured from the presents, not the declared interval alone: the next frame's origin is
//     one observed display interval after the last present. The observed interval is the time between successive
//     tokened presents, smoothed (1/8 weight) and clamped to the declared period's range; the declared interval seeds
//     it, so a display that keeps up gives the title's own rate and a display that falls behind drags the title with
//     it (the "vsync present paces the title" behaviour). A blank present (a frame with no token) shows on the same
//     clock and sets the phase but does not contribute a cadence sample.
//   * A wait held for an in-flight slot that no present releases is let go by a cadence dead-man after four observed
//     intervals (counted in present_clock_fallbacks()), so a title that waits without presenting cannot hang the boot.
//     The wait's own timeout is honoured first; the error surface is unchanged.
//   * A frame's lateness (present_timing) is measured against the origin it was due at: the wait's time plus the
//     observed cadence at the moment it was issued.
//
// Per-origin opt-in (set_per_origin_pacing; PALMETTO_PRESENT_PER_ORIGIN in the shim, accepted only with the present-driven
// clock):
//   * Each issued token remains a distinct flight until a successful PresentX names that token; flipping a newer token
//     cannot erase an older token's flight. The per-origin queue is bounded at 64 entries, matching the statistics ring;
//     overflow drops its oldest entry and is counted/reported.
//   * The two supported planes may carry different issued tokens. Validation compares each token against its plane's
//     high-water mark, the commit receives plane_count, and GetFrameStatisticsX reports the last plane that named that
//     token (one record per token, so the last plane wins when a token is repeated across planes).
//   * Each plane smooths its own successful-present interval with the same 1/8 weight and declared-period clamp. The
//     shared origin gate uses the slowest active plane cadence; a four-cadence waiter dead-man still releases a new token
//     without silently consuming any other origin. Before the first displayed present, waits preserve the old immediate
//     token behavior.
//   * The environment option is strict: unset/blank/off spellings leave the legacy single-watermark branch unchanged;
//     an unknown spelling or an enabled option without PALMETTO_PRESENT_DRIVEN is refused by name.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <string_view>

namespace palmetto {

// HRESULT values as plain numbers, so this header stands alone.
namespace pacer_hr {
inline constexpr std::int32_t ok = 0;
inline constexpr std::int32_t false_ = 1;                                  // S_FALSE
inline constexpr std::int32_t invalid_arg = std::int32_t(0x80070057);      // E_INVALIDARG
inline constexpr std::int32_t invalid_state = std::int32_t(0x8007139F);    // HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
inline constexpr std::int32_t timeout = std::int32_t(0x80070102);          // HRESULT_FROM_WIN32(WAIT_TIMEOUT)
}  // namespace pacer_hr

inline constexpr std::uint64_t no_deadline = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t infinite_timeout_ms = 0xFFFFFFFFu;  // INFINITE

// PALMETTO_PRESENT_PER_ORIGIN is deliberately stricter than the old boolean switches: a typo must not silently
// enable a different token-validation and latency model. The caller names the refusal and leaves the option off.
enum class PresentPerOriginSwitch { off, on, invalid };
inline PresentPerOriginSwitch parse_present_per_origin_switch(const char* raw) {
    if (!raw) return PresentPerOriginSwitch::off;
    std::string_view value(raw);
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    const auto equals = [&](std::string_view expected) {
        if (value.size() != expected.size()) return false;
        for (std::size_t i = 0; i < value.size(); ++i) {
            char c = value[i];
            if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
            if (c != expected[i]) return false;
        }
        return true;
    };
    if (value.empty() || equals("0") || equals("off") || equals("false") || equals("no"))
        return PresentPerOriginSwitch::off;
    if (equals("1") || equals("on") || equals("true") || equals("yes")) return PresentPerOriginSwitch::on;
    return PresentPerOriginSwitch::invalid;
}

class FrameClock {
public:
    virtual ~FrameClock() = default;
    virtual std::uint64_t now_us() = 0;
    // Called with `lock` held on the pacer's mutex. Returns when `cv` is notified, `deadline_us` (in now_us() time;
    // no_deadline for none) has passed, or spuriously; the caller re-checks its condition.
    virtual void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t deadline_us) = 0;
    // The pacer gives a function that takes its mutex and notifies; a fake clock calls it when time moves.
    virtual void set_wake(std::function<void()> wake) { (void)wake; }
};

// Real time: std::chrono::steady_clock (QueryPerformanceCounter on Windows).
class SteadyFrameClock final : public FrameClock {
public:
    std::uint64_t now_us() override {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start_).count());
    }
    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t deadline_us) override {
        if (deadline_us == no_deadline) cv.wait(lock);
        else cv.wait_until(lock, start_ + std::chrono::microseconds(deadline_us));
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Time that only moves when the test says so. A pacer waiting on it blocks until advance_us() is called.
class ManualFrameClock final : public FrameClock {
public:
    std::uint64_t now_us() override { return now_.load(); }
    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t) override { cv.wait(lock); }
    void set_wake(std::function<void()> wake) override {
        std::lock_guard guard(wake_mutex_);
        wake_ = std::move(wake);
    }
    void advance_us(std::uint64_t microseconds) {
        now_ += microseconds;
        // Called under the lock, so a pacer that is being destroyed (set_wake({})) is never woken after it is gone.
        std::lock_guard guard(wake_mutex_);
        if (wake_) wake_();
    }

private:
    std::atomic<std::uint64_t> now_{0};
    std::mutex wake_mutex_;
    std::function<void()> wake_;
};

// Provisional definitions of ours: the real constants and layouts of the console API are unmeasured.
inline constexpr std::uint32_t frame_event_origin = 0;
inline constexpr std::uint32_t max_present_planes = 2;  // the presentation service's measured composition limit
// Statistics types (bit mask). Interval and render entries are never produced here.
inline constexpr std::uint32_t stat_frame_interval = 0x01;
inline constexpr std::uint32_t stat_frame_event = 0x02;
inline constexpr std::uint32_t stat_render = 0x04;
inline constexpr std::uint32_t stat_present = 0x08;
inline constexpr std::uint32_t stat_display = 0x10;

// How a frame that is being presented is put on the display.
enum class PresentSync {
    vblank,     // at the next vblank (FIFO)
    immediate,  // now, mid-scan (a tear)
};

// D3D12XBOX_PRESENT_PARAMETERS::ImmediateThresholdPercent (docs/d3d12_vulkan.md section 4.5): a frame that missed its
// vblank is flipped at once, mid-scan, when the scan has not got further than `threshold_percent` of the screen,
// instead of waiting a whole interval. `late_by_us` is how long after the vblank the frame was ready (0: on time),
// `interval_us` the refresh period; the vblank grid is regular, so the scan is `late_by_us % interval_us` of the way
// down. 0 (or a negative or NaN value) never tears, 100 or more tears wherever the scan is. A frame on time, or an
// interval of 0 (unpaced), is never torn. Pure, so the tests can sweep it.
inline PresentSync decide_present_sync(std::uint64_t late_by_us, std::uint32_t interval_us, float threshold_percent) {
    if (!(threshold_percent > 0.0f) || late_by_us == 0 || interval_us == 0) return PresentSync::vblank;
    const double scanned_percent = 100.0 * double(late_by_us % interval_us) / double(interval_us);
    return scanned_percent <= double(std::min(threshold_percent, 100.0f)) ? PresentSync::immediate
                                                                          : PresentSync::vblank;
}

// Where the scan of a frame that was shown mid-scan stood, in percent (0 for a frame shown at a vblank).
inline float scan_percent(std::uint64_t late_by_us, std::uint32_t interval_us) {
    return interval_us ? float(100.0 * double(late_by_us % interval_us) / double(interval_us)) : 0.0f;
}

struct FrameStatistic {
    std::uint32_t type = 0;          // one stat_* bit
    std::uint32_t plane_index = 0;
    std::uint64_t token = 0;
    std::uint64_t time_us = 0;       // pacer clock
    float percent_scanned = 0.0f;    // display entries: where the scan was when the frame came up (0 at a vblank)
};

class FramePacer {
public:
    explicit FramePacer(FrameClock& clock) : clock_(clock) {
        clock_.set_wake([this] {
            { std::lock_guard guard(mutex_); }
            cv_.notify_all();
        });
    }
    ~FramePacer() {
        clock_.set_wake({});
        {
            std::lock_guard guard(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(mutex_);  // waiters leave before the members go
        cv_.wait(lock, [this] { return waiters_ == 0; });
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // SetFrameIntervalX(output, interval, latency, flags). `period_us` is the interval as a period; `latency` is
    // the number of frames allowed in flight (0 counts as 1). The vblank grid is re-based: its epoch is now. A
    // scheduled origin event stays armed (with its offset, kept below the new period) and its next origin is one
    // new period from now; waiters blocked on the old grid pick the new one up.
    std::int32_t set_frame_interval(std::uint32_t period_us, std::uint32_t latency, std::uint32_t flags) {
        (void)flags;
        if (period_us == 0) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        if (suspended_) return pacer_hr::invalid_state;
        period_us_ = period_us;
        latency_ = std::max(latency, 1u);
        epoch_us_ = clock_.now_us();
        interval_set_ = true;
        offset_us_ = std::min(offset_us_, period_us - 1);
        last_origin_ = 0;  // origin 0 is the epoch itself, which has passed
        if (present_driven_) {
            cadence_us_ = period_us_;  // seed only: the observed presents replace it
            if (per_origin_requested_) {
                for (std::size_t i = 0; i < plane_origins_.size(); ++i) {
                    plane_origins_[i].last_present_us = 0;
                    plane_origins_[i].last_token = plane_last_presented_[i];
                    plane_origins_[i].present_count = 0;
                    plane_origins_[i].cadence_us = period_us_;
                }
            }
        }
        cv_.notify_all();
        return pacer_hr::ok;
    }

    // ScheduleFrameEventX(event, offset, ..., flags). Only the origin event exists here.
    std::int32_t schedule_frame_event(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags) {
        (void)flags;
        if (event != frame_event_origin) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        if (suspended_ || !interval_set_) return pacer_hr::invalid_state;
        if (offset_us >= period_us_) return pacer_hr::invalid_arg;
        offset_us_ = offset_us;
        armed_ = true;
        // The first origin is the next one that has not come yet: origin n is due at epoch + n * period + offset.
        const std::uint64_t now = clock_.now_us();
        last_origin_ = now >= epoch_us_ + offset_us_ ? std::int64_t((now - epoch_us_ - offset_us_) / period_us_) : 0;
        cv_.notify_all();
        return pacer_hr::ok;
    }

    // WaitFrameEventX(event, timeout, ..., flags, &token). Blocks until the next origin, and while `latency` frames
    // are in flight. pacer_hr::timeout when `timeout_ms` passes first (infinite_timeout_ms: never); the token is left
    // alone then. invalid_state when nothing is registered, the pacer is suspended, or one of those happens while waiting.
    std::int32_t wait_frame_event(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                  std::uint64_t* token) {
        (void)flags;
        if (event != frame_event_origin || !token) return pacer_hr::invalid_arg;
        std::unique_lock lock(mutex_);
        ++waiters_;
        const std::int32_t result = wait_locked(lock, timeout_ms, token);
        if (--waiters_ == 0) cv_.notify_all();
        return result;
    }

    // The last token wait_frame_event handed out (0: none). With the origin event's flag 2 the caller may present the
    // frame it already holds while no new origin is due (the title's measured poll shape: event 0, timeout 0, flag 2).
    std::uint64_t last_issued_token() const { std::lock_guard guard(mutex_); return issued_; }

    // A pure flip wait (the title's measured event 2, INFINITE: the blocking flip wait before a frame's work): no token
    // is issued or consumed. The wait returns when an in-flight frame has been flipped by a present, so a caller that
    // keeps `latency` frames queued is released exactly at the present that flips one; on the vblank grid it returns
    // when the oldest queued frame's vblank has come. Never blocks before the first presented frame. A dead-man
    // (four observed cadences) releases a present-driven wait whose flip never comes, so a title cannot hang here;
    // `timeout_ms` is otherwise honoured. invalid_state while unregistered or suspended.
    std::int32_t wait_flip(std::uint32_t timeout_ms) {
        std::unique_lock lock(mutex_);
        ++waiters_;
        const std::int32_t result = wait_flip_locked(lock, timeout_ms);
        if (--waiters_ == 0) cv_.notify_all();
        return result;
    }

    // GetFrameStatisticsX(token, type_mask, &count, array). `count` is the room in `out` on entry and the number of
    // entries written on return. S_OK when every producible requested type was written (event, present, display),
    // S_FALSE with count 0 or fewer when they are not (there yet, or the token is unknown or has aged out of the
    // last 64). With a null `out`, count only receives how many entries are available.
    std::int32_t get_frame_statistics(std::uint64_t token, std::uint32_t type_mask, std::uint32_t* count,
                                      FrameStatistic* out) {
        if (!count) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        retire_locked(clock_.now_us());
        const Record* record = nullptr;
        for (const Record& r : records_)
            if (r.token == token) record = &r;
        FrameStatistic found[3];
        std::uint32_t n = 0, expected = 0;
        auto add = [&](std::uint32_t type, bool present, std::uint64_t time) {
            if (!(type_mask & type)) return;
            ++expected;
            if (!record || !present) return;
            found[n++] = FrameStatistic{type, per_origin_active_.load(std::memory_order_relaxed) ? record->plane_index : 0u,
                                         token, time,
                                         type == stat_display ? record->percent_scanned : 0.0f};
        };
        if (record) {
            add(stat_frame_event, true, record->origin_us);
            add(stat_present, record->presented, record->present_us);
            add(stat_display, record->displayed, record->display_us);
        } else {
            expected = ((type_mask & stat_frame_event) ? 1u : 0u) + ((type_mask & stat_present) ? 1u : 0u) +
                       ((type_mask & stat_display) ? 1u : 0u);
        }
        if (!out) {
            const std::uint32_t available = n;
            *count = available;
            return available && available == expected ? pacer_hr::ok : pacer_hr::false_;
        }
        const std::uint32_t written = std::min(n, *count);
        for (std::uint32_t i = 0; i < written; ++i) out[i] = found[i];
        *count = written;
        return written && written == expected ? pacer_hr::ok : pacer_hr::false_;
    }

    // Queue side. check_present_token before the frame is presented, commit_present_token after it was queued.
    // What real hardware does with a null, repeated or out-of-order token is unmeasured, and a title that presents
    // with Token = 0 (a loading screen), re-presents with one token, or presents from a render thread out of order
    // must not freeze. So the only refusal is a token that was never handed out (invalid_arg, which is a bug in the
    // caller); null, stale (older than the last presented) and repeated tokens return ok and say which kind they
    // were in `issue`, so the caller can log it. invalid_state while suspended.
    enum class TokenIssue { none, null_token, stale, repeated };
    std::int32_t check_present_token(std::uint64_t token, TokenIssue* issue = nullptr) {
        std::lock_guard guard(mutex_);
        if (issue) *issue = TokenIssue::none;
        if (suspended_) return pacer_hr::invalid_state;
        if (token > issued_) return pacer_hr::invalid_arg;
        if (issue) {
            if (token == 0) *issue = TokenIssue::null_token;
            else if (token == last_presented_) *issue = TokenIssue::repeated;
            else if (token < last_presented_) *issue = TokenIssue::stale;
        }
        return pacer_hr::ok;
    }
    // Opt-in per-origin validation. A token is compared with the watermark for its own plane; unlike the legacy helper,
    // completion of a newer token on another plane does not make this origin stale.
    std::int32_t check_present_token_for_plane(std::uint64_t token, std::uint32_t plane_index,
                                               TokenIssue* issue = nullptr) {
        if (issue) *issue = TokenIssue::none;
        if (plane_index >= max_present_planes) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        if (suspended_) return pacer_hr::invalid_state;
        if (token > issued_) return pacer_hr::invalid_arg;
        if (issue) {
            if (token == 0) *issue = TokenIssue::null_token;
            else if (token == plane_last_presented_[plane_index]) *issue = TokenIssue::repeated;
            else if (token < plane_last_presented_[plane_index]) *issue = TokenIssue::stale;
        }
        return pacer_hr::ok;
    }
    // How late the frame of `token` is if it is presented now (see the model above). late_us stays 0 when the frame
    // could not be shown at once anyway: unpaced, uncapped, null or unknown token, or earlier frames still waiting
    // for their vblank (a flip cannot overtake them), or the token was already presented or is older than the last
    // presented (a repeated or stale token never tears).
    struct PresentTiming {
        std::uint64_t late_us = 0;
        std::uint32_t period_us = 0;   // 0: unpaced
    };
    // The swapchain has one sync decision for the whole PresentX. Use the greatest lateness among its distinct plane
    // origins so that a late secondary plane is not hidden by an on-time primary plane.
    PresentTiming present_timing_for_planes(const std::uint64_t* plane_tokens, std::uint32_t plane_count) {
        std::lock_guard guard(mutex_);
        PresentTiming timing;
        if (!plane_tokens || plane_count == 0 || plane_count > max_present_planes || !interval_set_ || uncapped_)
            return timing;
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        timing.period_us = period_us_;
        if (!pending_.empty()) return timing;
        for (std::uint32_t i = 0; i < plane_count; ++i) {
            const std::uint64_t token = plane_tokens[i];
            if (!token) continue;
            for (const Record& r : records_)
                if (r.token == token && !r.presented && token > plane_last_presented_[i] && now > r.due_us)
                    timing.late_us = std::max(timing.late_us, now - r.due_us);
        }
        return timing;
    }
    PresentTiming present_timing(std::uint64_t token) {
        std::lock_guard guard(mutex_);
        PresentTiming timing;
        if (!interval_set_ || uncapped_ || !token) return timing;
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        timing.period_us = period_us_;
        if (!pending_.empty()) return timing;
        for (const Record& r : records_)
            if (r.token == token && !r.presented && token > last_presented_ && now > r.due_us)
                timing.late_us = now - r.due_us;
        return timing;
    }

    // `sync` immediate: the frame is shown at once (display time = present time) instead of at the next vblank, and
    // percent_scanned is the lateness measured at this commit, so it agrees with the display time. The caller got
    // `sync` from decide_present_sync and the presenter confirmed it.
    void commit_present_token(std::uint64_t token, std::uint32_t plane_count, PresentSync sync = PresentSync::vblank) {
        (void)plane_count;
        std::lock_guard guard(mutex_);
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        last_presented_ = std::max(last_presented_, token);
        if (!interval_set_ || !armed_ || uncapped_) {  // not paced or uncapped: shows at once
            for (Record& r : records_)
                if (r.token == token) { r.presented = r.displayed = true; r.present_us = r.display_us = now; }
            return;
        }
        if (present_driven_) {
            // The present is this frame's flip: it is marked displayed at the present's own time. Only a present that
            // consumes a token nobody has flipped yet advances the clock (a repeated or stale token is presented again,
            // which a title does between origins, and carries no new frame); a blank frame carries no token at all.
            for (Record& r : records_)
                if (r.token == token && token) { r.presented = true; r.present_us = now; }
            if (token && token > flipped_) advance_present_clock_locked(now, token);
            cv_.notify_all();
            return;
        }
        // The vblank after now, and not before the one after the previous queued frame.
        const std::uint64_t since = now > epoch_us_ ? now - epoch_us_ : 0;
        std::uint64_t flip = epoch_us_ + (since / period_us_ + 1) * period_us_;
        if (!pending_.empty()) flip = std::max(flip, pending_.back().flip_us + period_us_);
        float percent = 0.0f;
        if (sync == PresentSync::immediate && pending_.empty()) {
            flip = now;
            std::uint64_t late_us = 0;
            for (const Record& r : records_)
                if (r.token == token && now > r.due_us) late_us = now - r.due_us;
            percent = scan_percent(late_us, period_us_);
            ++immediate_presents_;
        }
        pending_.push_back(Pending{token, flip, percent});
        for (Record& r : records_)
            if (r.token == token) { r.presented = true; r.present_us = now; }
        retire_locked(now);
        cv_.notify_all();
    }
    // Opt-in commit for independently tokened planes. Each token removes only its own outstanding origin; the public
    // statistics record remembers the last plane that carried that origin. The legacy single-token commit above stays
    // the sole path while PALMETTO_PRESENT_PER_ORIGIN is off.
    void commit_present_tokens_for_planes(const std::uint64_t* plane_tokens, std::uint32_t plane_count,
                                          PresentSync sync = PresentSync::vblank) {
        (void)sync;  // present-driven mode records display at the successful host present, as the legacy mode does
        if (!per_origin_active_.load(std::memory_order_relaxed) || !plane_tokens || plane_count == 0 ||
            plane_count > max_present_planes)
            return;
        std::lock_guard guard(mutex_);
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);

        // Unpaced and uncapped presents still fill any available statistics, but do not enter the origin queue or move
        // the display cadence.
        if (!interval_set_ || !armed_ || uncapped_ || !present_driven_) {
            for (std::uint32_t i = 0; i < plane_count; ++i) {
                const std::uint64_t token = plane_tokens[i];
                if (token) {
                    remove_origin_flight_locked(token);
                    last_presented_ = std::max(last_presented_, token);
                    plane_last_presented_[i] = std::max(plane_last_presented_[i], token);
                }
                for (Record& r : records_)
                    if (r.token == token) {
                        r.presented = r.displayed = true;
                        r.present_us = r.display_us = now;
                        r.plane_index = i;
                    }
            }
            cv_.notify_all();
            return;
        }

        std::array<std::uint64_t, max_present_planes> unique_tokens{};
        std::array<bool, max_present_planes> newly_flipped{};
        std::size_t unique_count = 0;
        bool any_new_origin = false;
        bool plane_cadence_changed = false;
        std::uint64_t highest_new_token = flipped_;
        for (std::uint32_t i = 0; i < plane_count; ++i) {
            const std::uint64_t token = plane_tokens[i];
            std::size_t token_slot = unique_count;
            bool duplicate = false;
            for (std::size_t j = 0; j < unique_count; ++j)
                if (unique_tokens[j] == token) {
                    token_slot = j;
                    duplicate = true;
                    break;
                }
            bool new_origin = false;
            if (!duplicate) {
                unique_tokens[unique_count] = token;
                if (token) {
                    const bool queued = remove_origin_flight_locked(token);
                    bool known = false;
                    bool not_yet_displayed = false;
                    for (const Record& r : records_)
                        if (r.token == token) {
                            known = true;
                            not_yet_displayed = not_yet_displayed || !r.displayed;
                        }
                    new_origin = queued || not_yet_displayed || (!known && token > last_presented_);
                    newly_flipped[unique_count] = new_origin;
                    if (new_origin) {
                        any_new_origin = true;
                        highest_new_token = std::max(highest_new_token, token);
                    }
                }
                ++unique_count;
            } else {
                new_origin = newly_flipped[token_slot];
            }

            if (token) {
                last_presented_ = std::max(last_presented_, token);
                plane_last_presented_[i] = std::max(plane_last_presented_[i], token);
                for (Record& r : records_)
                    if (r.token == token) {
                        r.presented = true;
                        r.present_us = now;
                        r.plane_index = i;  // the final plane that names this origin is the statistic's plane
                        if (new_origin && !r.displayed) {
                            r.displayed = true;
                            r.display_us = now;
                        }
                    }

                PlaneOrigin& origin = plane_origins_[i];
                if (token > origin.last_token) {
                    if (!origin.cadence_us) origin.cadence_us = cadence_us_ ? cadence_us_ : period_us_;
                    if (origin.present_count && now > origin.last_present_us && period_us_) {
                        const std::uint64_t observed = now - origin.last_present_us;
                        const std::uint64_t low = std::max<std::uint64_t>(period_us_ / 8, 1000u);
                        const std::uint64_t high = 4ull * period_us_;
                        if (observed >= low && observed <= high) {
                            const std::uint64_t smoothed = (std::uint64_t(origin.cadence_us) * 7 + observed) / 8;
                            origin.cadence_us = std::uint32_t(std::min(smoothed, high));
                        }
                    }
                    origin.last_present_us = now;
                    origin.last_token = token;
                    ++origin.present_count;
                    plane_cadence_changed = true;
                }
            }
        }

        if (plane_cadence_changed) {
            std::uint32_t slowest_cadence = 0;
            bool has_plane_cadence = false;
            for (const PlaneOrigin& origin : plane_origins_)
                if (origin.present_count) {
                    slowest_cadence = std::max(slowest_cadence, origin.cadence_us);
                    has_plane_cadence = true;
                }
            cadence_us_ = has_plane_cadence ? slowest_cadence : period_us_;
        }
        if (any_new_origin) {
            if (present_count_ == 0) request_due_us_ = now + cadence_us_;
            last_present_us_ = now;
            ++present_count_;
            flipped_ = std::max(flipped_, highest_new_token);
        }
        if (any_new_origin || plane_cadence_changed) cv_.notify_all();
    }

    // A blank present (no planes) carries no token: it is only counted as a frame to show. With no interval set,
    // the origin event not armed or the cap off it is not paced: it shows at once and is not counted.
    void note_untokened_present() {
        std::lock_guard guard(mutex_);
        if (!interval_set_ || !armed_ || uncapped_) return;
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        if (present_driven_) {  // a blank frame carries no token: it is not a flip of a tokened frame
            return;
        }
        const std::uint64_t since = now > epoch_us_ ? now - epoch_us_ : 0;
        std::uint64_t flip = epoch_us_ + (since / period_us_ + 1) * period_us_;
        if (!pending_.empty()) flip = std::max(flip, pending_.back().flip_us + period_us_);
        pending_.push_back(Pending{0, flip, 0.0f});
        cv_.notify_all();
    }

    // SuspendX: presentation stops, the registration and the frames in flight are dropped, waiters fail.
    void suspend() {
        std::lock_guard guard(mutex_);
        suspended_ = true;
        interval_set_ = armed_ = false;
        pending_.clear();
        present_count_ = 0;  // the clock's phase goes with the registration, which suspend drops
        request_due_us_ = 0;
        if (per_origin_requested_) origins_in_flight_.clear();
        cv_.notify_all();
    }
    // ResumeX: presents work again, but the interval and the origin event must be set up again.
    void resume() {
        std::lock_guard guard(mutex_);
        suspended_ = false;
        interval_set_ = armed_ = false;
        present_count_ = 0;
        request_due_us_ = 0;
        if (per_origin_requested_) origins_in_flight_.clear();
        cv_.notify_all();
    }

    bool suspended() const { std::lock_guard guard(mutex_); return suspended_; }
    std::uint64_t tokens_issued() const { std::lock_guard guard(mutex_); return issued_; }
    std::uint32_t frames_in_flight() {
        std::lock_guard guard(mutex_);
        if (per_origin_active_.load(std::memory_order_relaxed))
            return std::uint32_t(origins_in_flight_.size());
        if (present_driven_) return std::uint32_t(issued_ >= flipped_ ? issued_ - flipped_ : 0);
        retire_locked(clock_.now_us());
        return std::uint32_t(pending_.size());
    }
    // Frames that were flipped mid-scan because of ImmediateThresholdPercent.
    std::uint64_t immediate_presents() const { std::lock_guard guard(mutex_); return immediate_presents_; }
    std::uint32_t period_us() const { std::lock_guard guard(mutex_); return period_us_; }
    bool registered() const { std::lock_guard guard(mutex_); return interval_set_ && armed_; }

    // The fps cap off (PALMETTO_PRESENT_UNCAP; see the top comment): waits never block and presents count as shown
    // at once. Registration and suspension keep meaning what they did; the flag survives suspend/resume.
    void set_uncapped(bool on) { std::lock_guard guard(mutex_); uncapped_ = on; }
    bool uncapped() const { std::lock_guard guard(mutex_); return uncapped_; }

    // The present-driven clock (PALMETTO_PRESENT_DRIVEN in the shim; see the top comment): the present is the clock.
    // The flag is set once, before registration, by the environment; turning it off restores the vblank grid, which
    // the next SetFrameIntervalX re-bases. It survives suspend/resume like the cap flag.
    void set_present_driven(bool on) {
        std::lock_guard guard(mutex_);
        if (present_driven_ == on) return;
        present_driven_ = on;
        pending_.clear();      // nothing was shown on the grid this mode is leaving
        present_count_ = 0;
        request_due_us_ = 0;
        if (on && period_us_) cadence_us_ = period_us_;
        per_origin_active_.store(per_origin_requested_ && on, std::memory_order_relaxed);
        if (per_origin_requested_) reset_per_origin_state_locked();
        cv_.notify_all();
    }
    bool present_driven() const { std::lock_guard guard(mutex_); return present_driven_; }
    // Optional independent origin accounting (`PALMETTO_PRESENT_PER_ORIGIN`): active only with the present-driven
    // clock. The off path continues through the original single-watermark state machine.
    void set_per_origin_pacing(bool on) {
        std::lock_guard guard(mutex_);
        if (per_origin_requested_ == on) return;
        per_origin_requested_ = on;
        per_origin_active_.store(on && present_driven_, std::memory_order_relaxed);
        reset_per_origin_state_locked();
        cv_.notify_all();
    }
    bool per_origin_pacing() const { return per_origin_active_.load(std::memory_order_relaxed); }
    // The observed display cadence the clock is following, in microseconds (0 before an interval is set). Seeded by
    // SetFrameIntervalX's period and replaced by the presents' own intervals as they are measured.
    std::uint32_t display_cadence_us() const { std::lock_guard guard(mutex_); return cadence_us_; }
    // The highest token whose consuming present has happened (the flip watermark).
    std::uint64_t flipped_token() const { std::lock_guard guard(mutex_); return flipped_; }
    // Waits let go by the cadence dead-man because no present came to release an in-flight slot.
    std::uint64_t present_clock_fallbacks() const {
        std::lock_guard guard(mutex_);
        return present_clock_fallbacks_;
    }
    std::uint64_t per_origin_queue_overflows() const {
        std::lock_guard guard(mutex_);
        return origin_queue_overflows_;
    }

private:
    struct Pending {
        std::uint64_t token;
        std::uint64_t flip_us;
        float percent_scanned;  // where the scan was for a frame shown mid-scan, else 0
    };
    struct Record {
        std::uint64_t token = 0, origin_us = 0, present_us = 0, display_us = 0;
        std::uint64_t due_us = 0;  // the frame's deadline: the origin's own vblank (grid), or origin + cadence (present-driven)
        float percent_scanned = 0.0f;
        std::uint32_t plane_index = 0;
        bool presented = false, displayed = false;
    };
    struct OriginFlight {
        std::uint64_t token = 0;  // one outstanding frame origin, not a high-watermark range
    };
    struct PlaneOrigin {
        std::uint64_t last_present_us = 0;
        std::uint64_t last_token = 0;
        std::uint64_t present_count = 0;
        std::uint32_t cadence_us = 0;
    };
    static constexpr std::size_t max_origin_flights = 64;  // the statistics retention bound also bounds live origin state

    void reset_per_origin_state_locked() {
        origins_in_flight_.clear();
        origin_queue_overflows_ = 0;
        plane_last_presented_.fill(last_presented_);
        for (std::size_t i = 0; i < plane_origins_.size(); ++i) {
            plane_origins_[i] = PlaneOrigin{};
            plane_origins_[i].last_token = last_presented_;
            plane_origins_[i].cadence_us = cadence_us_ ? cadence_us_ : period_us_;
        }
    }
    bool remove_origin_flight_locked(std::uint64_t token) {
        const auto it = std::find_if(origins_in_flight_.begin(), origins_in_flight_.end(),
                                     [token](const OriginFlight& flight) { return flight.token == token; });
        if (it == origins_in_flight_.end()) return false;
        origins_in_flight_.erase(it);
        return true;
    }
    void queue_origin_flight_locked(std::uint64_t token) {
        if (origins_in_flight_.size() >= max_origin_flights) {
            origins_in_flight_.pop_front();  // bounded history; a very old origin cannot wedge a long-running boot
            ++origin_queue_overflows_;
        }
        origins_in_flight_.push_back(OriginFlight{token});
    }

    // Frames whose vblank has come are shown.
    void retire_locked(std::uint64_t now) {
        while (!pending_.empty() && pending_.front().flip_us <= now) {
            for (Record& r : records_)
                if (r.token == pending_.front().token && pending_.front().token) {
                    r.displayed = true;
                    r.display_us = pending_.front().flip_us;
                    r.percent_scanned = pending_.front().percent_scanned;
                }
            pending_.pop_front();
        }
    }

    // A token's record, in the shared shape: the mode-specific fields are the origin and the deadline.
    std::int32_t record_token_locked(std::uint64_t token, std::uint64_t origin_us, std::uint64_t due_us) {
        Record record;
        record.token = token;
        record.origin_us = origin_us;
        record.due_us = due_us;
        records_.push_back(record);
        if (records_.size() > 64) records_.pop_front();
        return pacer_hr::ok;
    }

    // The present is the clock. A frame's flip (the present that consumes its token) marks it displayed at the
    // present's time and frees one of the `latency` slots; the frame's own cadence sample is the interval between two
    // successive flips of new tokens, smoothed and clamped to the declared period's range.
    void advance_present_clock_locked(std::uint64_t now, std::uint64_t token) {
        if (present_count_ > 0 && cadence_us_) {
            const std::uint64_t observed = now > last_present_us_ ? now - last_present_us_ : 0;
            const std::uint64_t low = std::max<std::uint64_t>(period_us_ / 8, 1000u);
            const std::uint64_t high = 4ull * period_us_;
            if (observed >= low && observed <= high) {
                const std::uint64_t smoothed = (std::uint64_t(cadence_us_) * 7 + observed) / 8;
                cadence_us_ = std::uint32_t(std::min(smoothed, high));
            }
        }
        if (present_count_ == 0) request_due_us_ = now + cadence_us_;  // the first present anchors the phase
        last_present_us_ = now;
        ++present_count_;
        if (token > flipped_) {
            flipped_ = token;
            for (Record& r : records_)
                if (r.token == token) { r.displayed = true; r.display_us = now; }
        }
        cv_.notify_all();
    }

    // Hand a present-driven token out at the newest origin that has passed (the missed ones are skipped, as on the
    // grid), and record the deadline as one observed interval later.
    std::int32_t issue_present_token_locked(std::uint64_t now, std::uint64_t* token) {
        if (!cadence_us_) cadence_us_ = period_us_ ? period_us_ : 1000;
        if (request_due_us_ == 0) request_due_us_ = now + cadence_us_;
        std::uint64_t origin = request_due_us_;
        if (now >= origin) origin += ((now - origin) / cadence_us_) * cadence_us_;
        request_due_us_ = origin + cadence_us_;
        *token = ++issued_;
        const std::int32_t result = record_token_locked(*token, origin, origin + cadence_us_);
        if (per_origin_active_.load(std::memory_order_relaxed)) queue_origin_flight_locked(*token);
        return result;
    }

    std::int32_t wait_present_per_origin_locked(std::unique_lock<std::mutex>& lock, std::uint64_t start,
                                               std::uint64_t give_up, std::uint64_t* token) {
        for (;;) {
            if (shutdown_ || suspended_ || !interval_set_ || !armed_) return pacer_hr::invalid_state;
            const std::uint64_t now = clock_.now_us();
            // The first origin cannot follow a display that does not exist yet; keep the legacy initial-token rule.
            if (present_count_ == 0) return issue_present_token_locked(now, token);
            const std::uint64_t budget = std::max<std::uint64_t>(latency_, 1u);
            if (origins_in_flight_.size() >= budget) {
                const std::uint64_t dead_man = start + 4ull * std::max<std::uint64_t>(cadence_us_, 1000u);
                if (now >= dead_man) {
                    ++present_clock_fallbacks_;
                    return issue_present_token_locked(now, token);
                }
                if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
                clock_.wait(lock, cv_, give_up == no_deadline ? dead_man : std::min(dead_man, give_up));
                continue;
            }
            const std::uint64_t due = request_due_us_ ? request_due_us_ : now;
            if (now >= due) return issue_present_token_locked(now, token);
            if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
            clock_.wait(lock, cv_, give_up == no_deadline ? due : std::min(due, give_up));
        }
    }

    std::int32_t wait_present_driven_locked(std::unique_lock<std::mutex>& lock, std::uint64_t start,
                                            std::uint64_t give_up, std::uint64_t* token) {
        if (per_origin_active_.load(std::memory_order_relaxed))
            return wait_present_per_origin_locked(lock, start, give_up, token);
        for (;;) {
            if (shutdown_ || suspended_ || !interval_set_ || !armed_) return pacer_hr::invalid_state;
            const std::uint64_t now = clock_.now_us();
            // Before the first present there is no displayed frame to follow: the layer cannot wait for a clock that
            // does not exist yet, and a wait must not stall the title before its first frame.
            if (present_count_ == 0) return issue_present_token_locked(now, token);
            const std::uint64_t budget = std::max<std::uint64_t>(latency_, 1u);
            const bool free_slot = issued_ >= flipped_ && issued_ - flipped_ < budget;
            if (!free_slot) {
                // A held wait: the present that flips a frame releases it (the notification), and the dead-man lets it
                // go if no present ever comes so a title that waits without presenting cannot hang.
                const std::uint64_t dead_man = start + 4ull * std::max<std::uint64_t>(cadence_us_, 1000u);
                if (now >= dead_man) {
                    ++present_clock_fallbacks_;
                    return issue_present_token_locked(now, token);
                }
                if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
                clock_.wait(lock, cv_, give_up == no_deadline ? dead_man : std::min(dead_man, give_up));
                continue;
            }
            // The cadence gate: the next frame event is one observed display interval after the previous origin.
            const std::uint64_t due = request_due_us_ ? request_due_us_ : now;
            if (now >= due) return issue_present_token_locked(now, token);
            if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
            clock_.wait(lock, cv_, give_up == no_deadline ? due : std::min(due, give_up));
        }
    }

    std::int32_t wait_flip_locked(std::unique_lock<std::mutex>& lock, std::uint32_t timeout_ms) {
        const std::uint64_t start = clock_.now_us();
        const std::uint64_t give_up = timeout_ms == infinite_timeout_ms ? no_deadline
                                                                         : start + std::uint64_t(timeout_ms) * 1000;
        const std::uint64_t budget = std::max<std::uint64_t>(latency_, 1u);
        for (;;) {
            if (shutdown_ || suspended_ || !interval_set_ || !armed_) return pacer_hr::invalid_state;
            const std::uint64_t now = clock_.now_us();
            if (present_driven_) {
                if (per_origin_active_.load(std::memory_order_relaxed)) {
                    // Each token is its own flight: a newer plane flip cannot consume an older unflipped origin.
                    if (present_count_ == 0 || origins_in_flight_.size() < budget) return pacer_hr::ok;
                    const std::uint64_t dead_man = start + 4ull * std::max<std::uint64_t>(cadence_us_, 1000u);
                    if (now >= dead_man) {
                        ++present_clock_fallbacks_;
                        return pacer_hr::ok;
                    }
                    if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
                    clock_.wait(lock, cv_, give_up == no_deadline ? dead_man : std::min(dead_man, give_up));
                    continue;
                }
                // No displayed frame yet, or an in-flight slot is free: nothing to wait for. A present that flips a
                // frame notifies the wait (`commit_present_token`), which then finds its slot and returns.
                if (present_count_ == 0 || (issued_ >= flipped_ && issued_ - flipped_ < budget)) return pacer_hr::ok;
                const std::uint64_t dead_man = start + 4ull * std::max<std::uint64_t>(cadence_us_, 1000u);
                if (now >= dead_man) {
                    ++present_clock_fallbacks_;
                    return pacer_hr::ok;
                }
                if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
                clock_.wait(lock, cv_, give_up == no_deadline ? dead_man : std::min(dead_man, give_up));
                continue;
            }
            retire_locked(now);
            if (pending_.size() < budget) return pacer_hr::ok;
            const std::uint64_t wake = pending_.front().flip_us;  // the oldest frame's vblank
            if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
            clock_.wait(lock, cv_, give_up == no_deadline ? wake : std::min(wake, give_up));
        }
    }

    std::int32_t wait_locked(std::unique_lock<std::mutex>& lock, std::uint32_t timeout_ms, std::uint64_t* token) {
        const std::uint64_t start = clock_.now_us();
        const std::uint64_t give_up = timeout_ms == infinite_timeout_ms ? no_deadline
                                                                         : start + std::uint64_t(timeout_ms) * 1000;
        for (;;) {
            if (shutdown_ || suspended_ || !interval_set_ || !armed_) return pacer_hr::invalid_state;
            const std::uint64_t now = clock_.now_us();
            retire_locked(now);
            if (uncapped_) {  // no vblank grid and no latency throttle: a token at once, due now
                *token = ++issued_;
                return record_token_locked(*token, now, now);
            }
            if (present_driven_) return wait_present_driven_locked(lock, start, give_up, token);
            const std::uint64_t first = epoch_us_ + offset_us_;  // origin 0
            const std::int64_t next = last_origin_ + 1;
            const std::uint64_t next_us = first + std::uint64_t(next) * period_us_;
            std::uint64_t wake = next_us;
            if (now >= next_us) {
                if (pending_.size() < latency_) {
                    const std::int64_t newest = std::int64_t((now - first) / period_us_);
                    const std::int64_t origin = std::max(next, newest);
                    last_origin_ = origin;
                    *token = ++issued_;
                    Record record;
                    record.token = *token;
                    record.origin_us = first + std::uint64_t(origin) * period_us_;
                    record.due_us = record.origin_us - offset_us_ + period_us_;
                    records_.push_back(record);
                    if (records_.size() > 64) records_.pop_front();
                    return pacer_hr::ok;
                }
                wake = pending_.front().flip_us;  // blocked by the latency: until the oldest frame is shown
            }
            if (give_up != no_deadline && now >= give_up) return pacer_hr::timeout;
            clock_.wait(lock, cv_, give_up == no_deadline ? wake : std::min(wake, give_up));
        }
    }

    FrameClock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;
    std::uint32_t waiters_ = 0;

    bool suspended_ = false;
    bool interval_set_ = false;
    bool armed_ = false;
    bool uncapped_ = false;
    bool present_driven_ = false;
    bool per_origin_requested_ = false;
    std::atomic<bool> per_origin_active_{false};
    std::uint32_t period_us_ = 0;
    std::uint32_t latency_ = 1;
    std::uint32_t offset_us_ = 0;
    std::uint64_t epoch_us_ = 0;
    std::int64_t last_origin_ = 0;  // origin 0 is the epoch itself, which has passed
    // Present-driven clock state (see the top comment). Only the wireframe above is shared with the grid.
    std::uint32_t cadence_us_ = 0;        // observed display cadence; seeded by the period
    std::uint64_t request_due_us_ = 0;    // the next origin a wait may hand a token out at
    std::uint64_t last_present_us_ = 0;   // the last present that advanced the clock
    std::uint64_t present_count_ = 0;     // presents that advanced the clock (0: no displayed frame yet)
    std::uint64_t flipped_ = 0;           // the highest token whose consuming present has happened
    std::uint64_t present_clock_fallbacks_ = 0;
    std::uint64_t issued_ = 0;
    std::uint64_t last_presented_ = 0;
    std::uint64_t immediate_presents_ = 0;
    std::uint64_t origin_queue_overflows_ = 0;
    std::array<PlaneOrigin, max_present_planes> plane_origins_{};
    std::array<std::uint64_t, max_present_planes> plane_last_presented_{};
    std::deque<Pending> pending_;
    std::deque<Record> records_;
    std::deque<OriginFlight> origins_in_flight_;
};

}  // namespace palmetto
