// FramePacer (src/frame_pacer.h) on a fake clock: the interval, the origin event with an offset, late waits, timeouts,
// the latency limit, tokens and their validation, statistics, and suspend/resume with re-registration. No GPU.
#include "../src/frame_pacer.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>

using namespace palmetto;

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr std::uint32_t period = 16000;  // us; round numbers keep the arithmetic readable

struct Result {
    std::int32_t hr;
    std::uint64_t token;
};

// Runs a wait on its own thread.
std::future<Result> wait_async(FramePacer& pacer, std::uint32_t timeout_ms) {
    return std::async(std::launch::async, [&pacer, timeout_ms] {
        Result r{0, 0};
        r.hr = pacer.wait_frame_event(frame_event_origin, timeout_ms, 0, &r.token);
        return r;
    });
}

// True when the wait is still blocked after real time has had a chance to let it through.
bool blocked(std::future<Result>& f) {
    return f.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout;
}
// The same for the flip wait, which returns an HRESULT and no token.
bool blocked_flip(std::future<std::int32_t>& f) {
    return f.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout;
}

bool setup(FramePacer& pacer, std::uint32_t latency, std::uint32_t offset = 0) {
    return pacer.set_frame_interval(period, latency, 0) == pacer_hr::ok &&
           pacer.schedule_frame_event(frame_event_origin, offset, 0) == pacer_hr::ok;
}

}  // namespace

int main() {
    // The per-origin option is strict: unset/blank/off spellings keep the legacy path, known on spellings opt in, and
    // an unknown value is a named refusal (the runtime leaves the mode off).
    {
        using palmetto::PresentPerOriginSwitch;
        const char* const off_values[] = {nullptr, "", "   ", "0", "off", "FALSE", " no "};
        const char* const on_values[] = {"1", "on", "TRUE", " yes "};
        const char* const refused_values[] = {"2", "auto", "banana", "on 1"};
        for (const char* off : off_values)
            EXPECT(palmetto::parse_present_per_origin_switch(off) == PresentPerOriginSwitch::off);
        for (const char* on : on_values)
            EXPECT(palmetto::parse_present_per_origin_switch(on) == PresentPerOriginSwitch::on);
        for (const char* refused : refused_values)
            EXPECT(palmetto::parse_present_per_origin_switch(refused) == PresentPerOriginSwitch::invalid);
        ManualFrameClock clock;
        FramePacer gated(clock);
        gated.set_per_origin_pacing(true);
        EXPECT(!gated.per_origin_pacing());  // setting the request alone cannot bypass the present-driven prerequisite
        gated.set_present_driven(true);
        EXPECT(gated.per_origin_pacing());
    }

    // Arguments and state errors.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);  // nothing set
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::invalid_state);      // no interval
        EXPECT(pacer.set_frame_interval(0, 1, 0) == pacer_hr::invalid_arg);
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::ok);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);  // not scheduled
        EXPECT(pacer.schedule_frame_event(7, 0, 0) == pacer_hr::invalid_arg);                         // unknown event
        EXPECT(pacer.schedule_frame_event(frame_event_origin, period, 0) == pacer_hr::invalid_arg);   // offset >= period
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::ok);
        EXPECT(pacer.wait_frame_event(3, 0, 0, &token) == pacer_hr::invalid_arg);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, nullptr) == pacer_hr::invalid_arg);
        EXPECT(pacer.registered());
        // Setting the interval again keeps the scheduled event armed (it is re-based, not dropped).
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::ok);
        EXPECT(pacer.registered());
    }

    // The origin event: each wait returns at the next vblank, with the next token.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        auto first = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(first));
        clock.advance_us(period - 1);
        EXPECT(blocked(first));
        clock.advance_us(1);
        Result r = first.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 1);
        auto second = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(second));
        clock.advance_us(period);
        r = second.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 2);
        EXPECT(pacer.tokens_issued() == 2);
    }

    // An offset delays the event inside the period: origin n comes at n * period + offset.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1, 5000));
        auto wait = wait_async(pacer, infinite_timeout_ms);
        clock.advance_us(period);
        EXPECT(blocked(wait));
        clock.advance_us(4999);
        EXPECT(blocked(wait));
        clock.advance_us(1);
        EXPECT(wait.get().hr == pacer_hr::ok);
        std::uint32_t count = 1;
        FrameStatistic stat;
        EXPECT(pacer.get_frame_statistics(1, stat_frame_event, &count, &stat) == pacer_hr::ok);
        EXPECT(count == 1 && stat.type == stat_frame_event && stat.time_us == period + 5000);
    }

    // Scheduling later than the interval was set makes the next vblank the first origin, not the missed ones.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::ok);
        clock.advance_us(period * 5 + 100);
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::ok);
        auto wait = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(wait));
        clock.advance_us(period - 100);
        EXPECT(wait.get().token == 1);
    }

    // A late wait returns at once with the newest passed origin; the missed ones are skipped. Tokens still count
    // one per wait.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        clock.advance_us(period * 3 + period / 2);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        std::uint32_t count = 1;
        FrameStatistic stat;
        EXPECT(pacer.get_frame_statistics(1, stat_frame_event, &count, &stat) == pacer_hr::ok);
        EXPECT(stat.time_us == period * 3);  // origin 3, not 1
        auto next = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(next));
        clock.advance_us(period / 2);
        EXPECT(next.get().token == 2);
    }

    // Timeouts: 0 polls, a finite one counts fake time, the token is left alone, and a later wait still works.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        std::uint64_t token = 99;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::timeout && token == 99);
        auto wait = wait_async(pacer, 5);  // 5 ms = 5000 us of fake time
        EXPECT(blocked(wait));
        clock.advance_us(4999);
        EXPECT(blocked(wait));
        clock.advance_us(1);
        Result r = wait.get();
        EXPECT(r.hr == pacer_hr::timeout && r.token == 0);
        EXPECT(pacer.tokens_issued() == 0);
        auto again = wait_async(pacer, 100);
        EXPECT(blocked(again));
        clock.advance_us(period - 5000);  // the vblank, 11 ms after the first wait gave up
        r = again.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 1);
        // Poll with the origin due.
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 2);
    }

    // Latency 1: a frame presented late is still in flight at the next origin, so the wait holds until it is shown.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        clock.advance_us(period);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        clock.advance_us(period + period / 2);  // frame 1 is presented 1.5 vblanks late: now = 2.5 periods
        EXPECT(pacer.check_present_token(1) == pacer_hr::ok);
        pacer.commit_present_token(1, 1);
        EXPECT(pacer.frames_in_flight() == 1);
        auto wait = wait_async(pacer, infinite_timeout_ms);  // origin 2 has passed, but one frame is pending
        EXPECT(blocked(wait));
        clock.advance_us(period / 2 - 1);
        EXPECT(blocked(wait));
        clock.advance_us(1);  // vblank 3: frame 1 shows, the wait goes through with the origin of that vblank
        EXPECT(wait.get().token == 2);
        EXPECT(pacer.frames_in_flight() == 0);
        std::uint32_t count = 2;
        FrameStatistic stats[2];
        EXPECT(pacer.get_frame_statistics(1, stat_present | stat_display, &count, stats) == pacer_hr::ok);
        EXPECT(count == 2 && stats[0].type == stat_present && stats[0].time_us == period * 5 / 2 &&
               stats[1].type == stat_display && stats[1].time_us == period * 3);
    }

    // Latency 2 lets two frames queue; a third wait is held until the oldest has been shown.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 2));
        std::uint64_t tokens[3] = {};
        for (int i = 0; i < 2; ++i) {
            clock.advance_us(period);
            EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &tokens[i]) == pacer_hr::ok);
        }
        // Now = 2 periods; the app is slow and presents both frames at 3.5 periods: they show at vblanks 4 and 5.
        clock.advance_us(period * 3 / 2);
        EXPECT(pacer.check_present_token(tokens[0]) == pacer_hr::ok);
        pacer.commit_present_token(tokens[0], 1);
        EXPECT(pacer.check_present_token(tokens[1]) == pacer_hr::ok);
        pacer.commit_present_token(tokens[1], 1);
        EXPECT(pacer.frames_in_flight() == 2);
        auto wait = wait_async(pacer, infinite_timeout_ms);  // origin 3 has passed, but both slots are taken
        EXPECT(blocked(wait));
        clock.advance_us(period / 2);  // vblank 4 shows frame 1: one in flight, the wait goes through
        Result r = wait.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 3);
        EXPECT(pacer.frames_in_flight() == 1);
    }

    // Tokens: only one that was never handed out is refused. Null, stale and repeated ones are accepted and reported
    // as such, so a title that presents with Token = 0, twice with one token, or out of order does not freeze.
    {
        using Issue = FramePacer::TokenIssue;
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 4));
        std::uint64_t t[3] = {};
        for (auto& token : t) {
            clock.advance_us(period);
            EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok);
        }
        EXPECT(t[0] == 1 && t[1] == 2 && t[2] == 3);
        Issue issue = Issue::stale;
        EXPECT(pacer.check_present_token(4, &issue) == pacer_hr::invalid_arg);     // never issued
        EXPECT(pacer.check_present_token(1000, &issue) == pacer_hr::invalid_arg);
        EXPECT(pacer.check_present_token(0, &issue) == pacer_hr::ok && issue == Issue::null_token);
        EXPECT(pacer.check_present_token(2, &issue) == pacer_hr::ok && issue == Issue::none);  // 1 is skipped
        pacer.commit_present_token(2, 1);
        EXPECT(pacer.check_present_token(2, &issue) == pacer_hr::ok && issue == Issue::repeated);
        EXPECT(pacer.check_present_token(1, &issue) == pacer_hr::ok && issue == Issue::stale);
        EXPECT(pacer.check_present_token(3, &issue) == pacer_hr::ok && issue == Issue::none);
        // Presenting a stale or null token does not move the last presented token back.
        pacer.commit_present_token(1, 1);
        pacer.commit_present_token(0, 1);
        EXPECT(pacer.check_present_token(2, &issue) == pacer_hr::ok && issue == Issue::repeated);
        EXPECT(pacer.check_present_token(3, nullptr) == pacer_hr::ok);
    }

    // Changing the interval while the origin event is armed and a waiter is blocked: the event stays armed, the grid
    // is re-based on the change, and the waiter comes back at the first origin of the new grid.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        auto waiter = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(waiter));
        clock.advance_us(period / 2);
        EXPECT(pacer.set_frame_interval(2 * period, 1, 0) == pacer_hr::ok);  // now = period / 2; new grid from here
        EXPECT(pacer.registered() && pacer.period_us() == 2 * period);
        clock.advance_us(period);  // the old grid's first origin has passed: still blocked
        EXPECT(blocked(waiter));
        clock.advance_us(period - 1);
        EXPECT(blocked(waiter));
        clock.advance_us(1);  // period / 2 + 2 * period: the first origin of the new grid
        Result r = waiter.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 1);
        std::uint32_t count = 1;
        FrameStatistic stat;
        EXPECT(pacer.get_frame_statistics(1, stat_frame_event, &count, &stat) == pacer_hr::ok);
        EXPECT(stat.time_us == period / 2 + 2 * period);
        // An offset larger than the new, shorter period is cut to fit.
        EXPECT(pacer.schedule_frame_event(frame_event_origin, period + 1000, 0) == pacer_hr::ok);
        EXPECT(pacer.set_frame_interval(period / 2, 1, 0) == pacer_hr::ok);
        EXPECT(pacer.registered());
    }

    // Scheduling inside a period, before the offset has passed: that period's origin is still to come.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::ok);
        clock.advance_us(period * 2 + 2000);
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 5000, 0) == pacer_hr::ok);
        auto wait = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(wait));
        clock.advance_us(2999);
        EXPECT(blocked(wait));
        clock.advance_us(1);  // 2 * period + 5000
        EXPECT(wait.get().token == 1);
        std::uint32_t count = 1;
        FrameStatistic stat;
        EXPECT(pacer.get_frame_statistics(1, stat_frame_event, &count, &stat) == pacer_hr::ok);
        EXPECT(stat.time_us == period * 2 + 5000);
    }

    // Statistics: unknown tokens and types the pacer cannot give report S_FALSE and count 0.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        clock.advance_us(period);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok);
        std::uint32_t count = 4;
        FrameStatistic stats[4];
        EXPECT(pacer.get_frame_statistics(77, stat_frame_event, &count, stats) == pacer_hr::false_ && count == 0);
        count = 4;
        EXPECT(pacer.get_frame_statistics(token, stat_render | stat_frame_interval, &count, stats) == pacer_hr::false_ &&
               count == 0);
        // Not presented yet: the event is there, present and display are not.
        count = 4;
        EXPECT(pacer.get_frame_statistics(token, stat_frame_event | stat_present | stat_display, &count, stats) ==
                   pacer_hr::false_ && count == 1 && stats[0].type == stat_frame_event);
        count = 0;
        EXPECT(pacer.get_frame_statistics(token, stat_frame_event, nullptr, stats) == pacer_hr::invalid_arg);
        // A null array only counts.
        count = 4;
        EXPECT(pacer.get_frame_statistics(token, stat_frame_event, &count, nullptr) == pacer_hr::ok && count == 1);
        // Presented but not shown: display missing. Then shown.
        pacer.commit_present_token(token, 1);
        count = 4;
        EXPECT(pacer.get_frame_statistics(token, stat_present | stat_display, &count, stats) == pacer_hr::false_ &&
               count == 1 && stats[0].type == stat_present);
        clock.advance_us(period);
        count = 4;
        EXPECT(pacer.get_frame_statistics(token, stat_present | stat_display, &count, stats) == pacer_hr::ok &&
               count == 2);
        // Too little room: written up to the room, S_FALSE.
        count = 1;
        EXPECT(pacer.get_frame_statistics(token, stat_present | stat_display, &count, stats) == pacer_hr::false_ &&
               count == 1);
    }

    // Suspend: a blocked waiter fails, presents are refused, registration is gone after resume and must be redone;
    // tokens keep counting.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1));
        clock.advance_us(period);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        auto waiter = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(waiter));
        pacer.suspend();
        EXPECT(waiter.get().hr == pacer_hr::invalid_state);
        EXPECT(pacer.suspended());
        EXPECT(pacer.check_present_token(1) == pacer_hr::invalid_state);
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::invalid_state);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);
        pacer.resume();
        EXPECT(!pacer.suspended() && !pacer.registered());
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);  // not re-registered
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::invalid_state);       // interval too
        EXPECT(setup(pacer, 1));
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 2);
        EXPECT(pacer.check_present_token(2) == pacer_hr::ok);
        EXPECT(pacer.check_present_token(1) == pacer_hr::ok);  // never presented, so not stale
    }

    // Destroying a pacer with a blocked waiter lets the waiter go.
    {
        ManualFrameClock clock;
        std::future<Result> waiter;
        {
            FramePacer pacer(clock);
            EXPECT(setup(pacer, 1));
            waiter = wait_async(pacer, infinite_timeout_ms);
            EXPECT(blocked(waiter));
        }
        EXPECT(waiter.get().hr == pacer_hr::invalid_state);
    }

    // ImmediateThresholdPercent: the pure decision (late_by, interval, threshold -> mode).
    {
        const auto sync = [](std::uint64_t late, std::uint32_t interval, float threshold) {
            return decide_present_sync(late, interval, threshold);
        };
        constexpr PresentSync v = PresentSync::vblank, i = PresentSync::immediate;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        EXPECT(sync(0, period, 100.0f) == v);                  // on time: nothing to tear
        EXPECT(sync(100, period, 0.0f) == v);                  // threshold 0 disables tearing
        EXPECT(sync(100, period, -5.0f) == v);
        EXPECT(sync(100, period, nan) == v);
        EXPECT(sync(100, 0, 100.0f) == v);                     // unpaced
        EXPECT(sync(1, period, 1.0f) == i);                    // 0.00625% scanned, within 1%
        EXPECT(sync(159, period, 1.0f) == i);                  // 0.99375%
        EXPECT(sync(160, period, 1.0f) == i);                  // exactly 1%: the threshold itself still tears
        EXPECT(sync(161, period, 1.0f) == v);                  // just past it
        EXPECT(sync(8000, period, 49.0f) == v);                // 50% scanned
        EXPECT(sync(8000, period, 50.0f) == i);
        EXPECT(sync(8000, period, 51.0f) == i);
        EXPECT(sync(15999, period, 100.0f) == i);              // 100: anywhere in the scan
        EXPECT(sync(15999, period, 250.0f) == i);              // above 100 is clamped
        EXPECT(sync(15999, period, 99.0f) == v);
        EXPECT(sync(period + 4000, period, 25.0f) == i);       // a whole interval and a quarter late: the scan is at 25%
        EXPECT(sync(period + 4000, period, 24.0f) == v);
        EXPECT(sync(3 * period, period, 10.0f) == i);          // late by whole intervals: the scan is at the top
        EXPECT(scan_percent(8000, period) == 50.0f);
        EXPECT(scan_percent(0, period) == 0.0f && scan_percent(5, 0) == 0.0f);
    }

    // The same through the pacer: lateness against the vblank the token was due at, and what a tear does to the frame.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 2));
        clock.advance_us(period);  // origin 1; its frame is due at vblank 2 (t = 2 * period)
        std::uint64_t token1 = 0, token2 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok);
        FramePacer::PresentTiming timing = pacer.present_timing(token1);
        EXPECT(timing.period_us == period && timing.late_us == 0);  // before the vblank it is due at
        EXPECT(pacer.present_timing(0).late_us == 0 && pacer.present_timing(99).late_us == 0);  // null, unknown
        clock.advance_us(period);  // exactly at its vblank: not late
        EXPECT(pacer.present_timing(token1).late_us == 0);
        clock.advance_us(8000);    // half an interval late
        timing = pacer.present_timing(token1);
        EXPECT(timing.late_us == 8000);
        const PresentSync mode = decide_present_sync(timing.late_us, timing.period_us, 60.0f);
        EXPECT(mode == PresentSync::immediate);
        pacer.commit_present_token(token1, 1, mode);
        EXPECT(pacer.immediate_presents() == 1);
        EXPECT(pacer.present_timing(token1).late_us == 0);  // presented already: a repeated token never tears
        EXPECT(pacer.frames_in_flight() == 0);  // shown at once, not queued for the next vblank
        std::uint32_t count = 4;
        FrameStatistic stats[4];
        EXPECT(pacer.get_frame_statistics(token1, stat_present | stat_display, &count, stats) == pacer_hr::ok && count == 2);
        EXPECT(stats[0].type == stat_present && stats[1].type == stat_display);
        EXPECT(stats[1].time_us == stats[0].time_us && stats[1].time_us == 2 * period + 8000);
        EXPECT(stats[1].percent_scanned == 50.0f && stats[0].percent_scanned == 0.0f);

        // The next frame, late by the same amount but with a threshold that is too small: waits for its vblank.
        clock.advance_us(period - 8000);  // t = 3 * period: origin 3
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token2) == pacer_hr::ok);
        clock.advance_us(period + 8000);  // due at 4 * period, now 4.5
        timing = pacer.present_timing(token2);
        EXPECT(timing.late_us == 8000);
        EXPECT(decide_present_sync(timing.late_us, timing.period_us, 40.0f) == PresentSync::vblank);
        pacer.commit_present_token(token2, 1, PresentSync::vblank);
        EXPECT(pacer.immediate_presents() == 1 && pacer.frames_in_flight() == 1);
        EXPECT(pacer.get_frame_statistics(token2, stat_display, &count, stats) == pacer_hr::false_);  // not shown yet
        clock.advance_us(period);  // vblank at 5 * period
        count = 4;
        EXPECT(pacer.get_frame_statistics(token2, stat_display, &count, stats) == pacer_hr::ok && count == 1);
        EXPECT(stats[0].time_us == 5 * period && stats[0].percent_scanned == 0.0f);
    }

    // Rescheduling changes future origins but not the vblank an issued token was due at.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 1, period / 4));
        clock.advance_us(period + period / 4);
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok);
        EXPECT(pacer.schedule_frame_event(frame_event_origin, period / 2, 0) == pacer_hr::ok);

        clock.advance_us(3 * period / 4);  // vblank 2, still the deadline of the original token
        EXPECT(pacer.present_timing(token).late_us == 0);
        clock.advance_us(period / 4);
        const auto timing = pacer.present_timing(token);
        EXPECT(timing.late_us == period / 4);
        const auto sync = decide_present_sync(timing.late_us, timing.period_us, 30.0f);
        EXPECT(sync == PresentSync::immediate);
        pacer.commit_present_token(token, 1, sync);

        FrameStatistic display{};
        std::uint32_t count = 1;
        EXPECT(pacer.get_frame_statistics(token, stat_display, &count, &display) == pacer_hr::ok);
        EXPECT(count == 1 && display.percent_scanned == 25.0f);
    }

    // A frame behind one that still waits for its vblank cannot be shown at once (it would overtake it): not late.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(setup(pacer, 2));
        std::uint64_t token1 = 0, token2 = 0;
        clock.advance_us(period);      // origin 1
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok);
        clock.advance_us(period);      // origin 2 (nothing is in flight yet, so the second token is handed out)
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token2) == pacer_hr::ok);
        clock.advance_us(period + 1600);  // 3.1 periods: token1 is presented, a whole interval late, queued for vblank 4
        pacer.commit_present_token(token1, 1);
        EXPECT(pacer.frames_in_flight() == 1);
        clock.advance_us(1600);        // 3.2: token2 was due at vblank 3 and is late, but token1 is still queued
        EXPECT(pacer.present_timing(token2).late_us == 0 && pacer.present_timing(token2).period_us == period);
        clock.advance_us(period);      // 4.2: token1 was shown at vblank 4; token2 is late and nothing is queued
        EXPECT(pacer.present_timing(token2).late_us == 19200);  // 4.2 periods now, due at 3
        EXPECT(pacer.immediate_presents() == 0);
    }

    // Presents made after SetFrameIntervalX but before the origin event is scheduled are unpaced (PresentX does not
    // hold them for a vblank), so they are shown at once and are not frames in flight: however many there were, the
    // first wait returns at the first origin.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(pacer.set_frame_interval(period, 1, 0) == pacer_hr::ok);
        EXPECT(!pacer.registered());
        for (int i = 0; i < 1000; ++i) pacer.note_untokened_present();  // blank presents of a loading screen
        pacer.commit_present_token(0, 1);                               // and one with the null token
        EXPECT(pacer.frames_in_flight() == 0);
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::ok);
        auto wait = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(wait));
        clock.advance_us(period);
        if (wait.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            pacer.suspend();  // lets the waiter go, so that the failure below ends the test and does not hang it
            EXPECT(!"the first wait returns one period after the event is armed");
        }
        Result r = wait.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 1);
        // Once the event is armed, a present is a frame in flight again.
        pacer.note_untokened_present();
        EXPECT(pacer.frames_in_flight() == 1);
    }

    // Uncapped (PALMETTO_PRESENT_UNCAP): a wait hands a token out at once instead of at the next interval origin,
    // and a presented frame counts as shown at once. Registration and suspension keep their errors.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_uncapped(true);
        EXPECT(pacer.uncapped());
        std::uint64_t token = 99;
        // Still invalid_state before registration, after suspend, and after resume until re-registered.
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token) == pacer_hr::invalid_state &&
               token == 99);
        EXPECT(setup(pacer, 1));
        // Tokens at once, no clock advance; as many in a row as asked (the latency limit does not throttle),
        // each from a wait that would still be blocked without the flag.
        for (std::uint64_t i = 1; i <= 3; ++i) {
            auto wait = wait_async(pacer, infinite_timeout_ms);
            Result r = wait.get();
            EXPECT(r.hr == pacer_hr::ok && r.token == i);
        }
        pacer.commit_present_token(1, 1);
        pacer.commit_present_token(2, 1);
        pacer.commit_present_token(3, 1);
        EXPECT(pacer.frames_in_flight() == 0);  // nothing waits for a vblank
        std::uint32_t count = 2;
        FrameStatistic stats[2];
        EXPECT(pacer.get_frame_statistics(3, stat_present | stat_display, &count, stats) == pacer_hr::ok);
        EXPECT(count == 2 && stats[0].type == stat_present && stats[0].time_us == 0 &&
               stats[1].type == stat_display && stats[1].time_us == 0);
        // An untokened (blank) present is not queued on the grid either.
        pacer.note_untokened_present();
        EXPECT(pacer.frames_in_flight() == 0);
        // An uncapped frame is due at once: present_timing reports no lateness.
        const FramePacer::PresentTiming timing = pacer.present_timing(3);
        EXPECT(timing.late_us == 0 && timing.period_us == 0);
        pacer.suspend();
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);
        pacer.resume();
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::invalid_state);
        EXPECT(setup(pacer, 1));
        clock.advance_us(1234);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 4);
        // The flag survives suspend/resume (set once, by the environment).
        pacer.suspend();
        pacer.resume();
        EXPECT(pacer.uncapped());
        EXPECT(setup(pacer, 1));
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 5);
        // Turning the cap back on restores the grid: the next wait blocks until the next origin (the grid was
        // re-based at the last SetFrameIntervalX, so a whole period is the next origin).
        pacer.set_uncapped(false);
        auto wait = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(wait));
        clock.advance_us(period);
        EXPECT(wait.get().token == 6);
    }

    // ---- Present-driven clock (PALMETTO_PRESENT_DRIVEN; frame_pacer.h "Present-driven") ---------------------------
    // The present is the clock: a wait hands out token 1 at once (there is no displayed frame to follow before the
    // first present), the present that carries a token marks it flipped and frees an in-flight slot, the blocking flip
    // wait is released exactly at that present, and the next origin is one observed display interval after the last
    // flip. A repeated or stale token is presented again but carries no new frame, so the clock does not move.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(!pacer.present_driven());
        pacer.set_present_driven(true);
        EXPECT(pacer.present_driven());
        EXPECT(setup(pacer, 1));
        std::uint64_t token = 0;
        // Before the first present there is no displayed frame to follow: token 1 at once, no clock advance.
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        EXPECT(pacer.present_clock_fallbacks() == 0 && pacer.flipped_token() == 0);
        // Before any present the flip wait has nothing to hold: it returns at once.
        EXPECT(pacer.wait_flip(infinite_timeout_ms) == pacer_hr::ok);
        // The first present anchors the phase: the next origin is one declared period after it (the seed cadence).
        clock.advance_us(1000);
        pacer.commit_present_token(1, 1);
        EXPECT(pacer.flipped_token() == 1 && pacer.frames_in_flight() == 0);
        EXPECT(pacer.display_cadence_us() == period);
        // The frame's flip is its display time: present and display are both the present's time.
        std::uint32_t count = 2;
        FrameStatistic stats[2];
        EXPECT(pacer.get_frame_statistics(1, stat_present | stat_display, &count, stats) == pacer_hr::ok &&
               count == 2 && stats[0].time_us == 1000 && stats[1].time_us == 1000);
        // The next origin is the present's time plus the cadence, exactly.
        auto held = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(held));
        clock.advance_us(period - 1);
        EXPECT(blocked(held));
        clock.advance_us(1);
        Result r = held.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 2);
        // One present consumes one token: presenting token 2 flips it at this present's time (17000 us).
        clock.advance_us(0);  // (the wait returned at the origin; token 2 is presented now)
        pacer.commit_present_token(2, 1);
        EXPECT(pacer.flipped_token() == 2 && pacer.display_cadence_us() == period);
        count = 2;
        EXPECT(pacer.get_frame_statistics(2, stat_present | stat_display, &count, stats) == pacer_hr::ok &&
               count == 2 && stats[0].time_us == period + 1000 && stats[1].time_us == stats[0].time_us);
        // A zero-timeout poll before the next origin times out and leaves the token alone; a poll after it returns a
        // token on the acquire. That is the title's measured event-0 poll (timeout 0, flag 2).
        token = 999;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::timeout && token == 999);
        EXPECT(pacer.last_issued_token() == 2);
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 3);
        pacer.commit_present_token(3, 1);
        EXPECT(pacer.flipped_token() == 3 && pacer.frames_in_flight() == 0);
        // A stale and the null token are presented again (a title does that between origins) but carry no new frame.
        const std::uint64_t cadence_before = pacer.display_cadence_us();
        clock.advance_us(10);
        pacer.commit_present_token(1, 1);
        pacer.commit_present_token(0, 1);
        EXPECT(pacer.flipped_token() == 3 && pacer.display_cadence_us() == cadence_before);
    }

    // The blocking flip wait (the title's event 2) is released exactly at the present that flips an in-flight frame,
    // and a timeout is honoured while nothing presents.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        EXPECT(setup(pacer, 1));
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        clock.advance_us(period);
        pacer.commit_present_token(1, 1);  // the first present anchors the phase at 2 * period
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 2);
        EXPECT(pacer.frames_in_flight() == 1);  // token 2 is issued but not presented: the latency slot is used
        auto flip = std::async(std::launch::async, [&pacer] { return pacer.wait_flip(infinite_timeout_ms); });
        EXPECT(blocked_flip(flip));
        clock.advance_us(1);                    // time passing does not release the flip wait; only the present does
        EXPECT(blocked_flip(flip));
        pacer.commit_present_token(2, 1);       // the present: the held flip wait is released exactly here
        EXPECT(flip.get() == pacer_hr::ok);
        EXPECT(pacer.frames_in_flight() == 0);
        // A flip wait with a timeout times out when nothing presents and the latency is used up.
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::timeout);  // not due yet (0 timeout)
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 3);
        auto timed = std::async(std::launch::async, [&pacer] { return pacer.wait_flip(1); });
        EXPECT(blocked_flip(timed));
        clock.advance_us(1000);                 // the only 1 ms of the timeout passing
        EXPECT(timed.get() == pacer_hr::timeout);
        pacer.commit_present_token(3, 1);
    }

    // The held token wait released by the cadence dead-man when no present ever comes (a title that waits without
    // presenting), so a boot cannot hang on the handshake; the fallback is counted.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        EXPECT(setup(pacer, 1));
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        clock.advance_us(1);
        pacer.commit_present_token(1, 1);  // the clock starts
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 2);
        EXPECT(pacer.frames_in_flight() == 1);
        auto choked = wait_async(pacer, infinite_timeout_ms);  // the latency slot is used and nothing presents
        EXPECT(blocked(choked));
        clock.advance_us(4 * period);  // the dead-man: four observed intervals
        Result r = choked.get();
        EXPECT(r.hr == pacer_hr::ok && r.token == 3);
        EXPECT(pacer.present_clock_fallbacks() == 1);
    }

    // The observed display cadence replaces the declared one: a present that comes late widens it, and the next origin
    // follows the widened cadence (the title is paced by the display it actually has).
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        EXPECT(setup(pacer, 2));
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        pacer.commit_present_token(1, 1);  // present at 0: the phase anchors at period
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 2);
        clock.advance_us(14000);  // the next present comes 30 ms after the first instead of 16 ms
        pacer.commit_present_token(2, 1);
        // (period = 16000: observed 30000, smoothed (7 * 16000 + 30000) / 8 = 17750.)
        EXPECT(pacer.display_cadence_us() == 17750);
        // The origin token 3 was already due at the pre-adaptation phase (32000); the one after it is a widened
        // cadence (17750 us) after token 3's origin, so the display's own rate is what the title is paced by.
        clock.advance_us(32000 - 30000);
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 3);
        auto next = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(next));
        clock.advance_us(17749);
        EXPECT(blocked(next));
        clock.advance_us(1);
        EXPECT(next.get().token == 4);
    }

    // Suspension: a held present-driven wait fails, and the flag survives (as the cap flag does).
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        EXPECT(setup(pacer, 1));
        std::uint64_t token = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token) == pacer_hr::ok && token == 1);
        pacer.commit_present_token(1, 1);
        auto waiter = wait_async(pacer, infinite_timeout_ms);  // waits for the next origin
        EXPECT(blocked(waiter));
        pacer.suspend();
        EXPECT(waiter.get().hr == pacer_hr::invalid_state);
        EXPECT(pacer.suspended());
        pacer.resume();
        EXPECT(pacer.present_driven() && !pacer.registered());
        EXPECT(setup(pacer, 1));
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token) == pacer_hr::ok && token == 2);
    }

    // Measured legacy gap: a flip of token 3 advances the single watermark over outstanding token 2, reducing the
    // reported flight count from two to zero. This is retained when the new switch is off.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        EXPECT(!pacer.per_origin_pacing());
        EXPECT(setup(pacer, 2));
        std::uint64_t token1 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok && token1 == 1);
        pacer.commit_present_token(token1, 1);
        clock.advance_us(period);
        std::uint64_t token2 = 0, token3 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token2) == pacer_hr::ok && token2 == 2);
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token3) == pacer_hr::ok && token3 == 3);
        EXPECT(pacer.frames_in_flight() == 2);
        pacer.commit_present_token(token3, 1);  // legacy high-watermark semantics collapse token 2 as well
        EXPECT(pacer.flipped_token() == 3 && pacer.frames_in_flight() == 0);
    }

    // Per-origin mode tracks exact token flights rather than treating a higher flip watermark as completion of every
    // earlier origin. Plane tokens may differ; each record keeps the last plane that carried it, and cadence samples are
    // isolated per plane before the slowest active cadence gates the shared origin stream.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(!pacer.per_origin_pacing());  // switch-off default remains the legacy branch
        pacer.set_present_driven(true);
        pacer.set_per_origin_pacing(true);
        EXPECT(pacer.per_origin_pacing());
        EXPECT(setup(pacer, 2));

        std::uint64_t token1 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok && token1 == 1);
        pacer.commit_present_tokens_for_planes(&token1, 1);  // plane 0 starts its cadence at t=0
        EXPECT(pacer.frames_in_flight() == 0 && pacer.display_cadence_us() == period);

        clock.advance_us(period);
        std::uint64_t token2 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token2) == pacer_hr::ok && token2 == 2);
        clock.advance_us(period);
        std::uint64_t token3 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token3) == pacer_hr::ok && token3 == 3);
        EXPECT(pacer.frames_in_flight() == 2);
        pacer.commit_present_tokens_for_planes(&token3, 1);  // flip 3 first; origin 2 must remain in flight
        EXPECT(pacer.flipped_token() == 3 && pacer.frames_in_flight() == 1);
        EXPECT(pacer.display_cadence_us() == period + period / 8);  // plane 0 observed 2 * period

        clock.advance_us(period);
        std::uint64_t token4 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token4) == pacer_hr::ok && token4 == 4);
        const std::uint64_t cross_plane_tokens[2] = {token4, token2};
        pacer.commit_present_tokens_for_planes(cross_plane_tokens, 2);  // new plane-0 token 4 and late plane-1 token 2
        EXPECT(pacer.frames_in_flight() == 0 && pacer.flipped_token() == 4);
        std::uint32_t count = 2;
        FrameStatistic stats[2];
        EXPECT(pacer.get_frame_statistics(token2, stat_present | stat_display, &count, stats) == pacer_hr::ok && count == 2 &&
               stats[0].plane_index == 1 && stats[1].plane_index == 1 && stats[0].time_us == 3 * period &&
               stats[1].time_us == 3 * period);
        count = 2;
        EXPECT(pacer.get_frame_statistics(token4, stat_present | stat_display, &count, stats) == pacer_hr::ok && count == 2 &&
               stats[0].plane_index == 0 && stats[1].plane_index == 0);
        FramePacer::TokenIssue issue{};
        EXPECT(pacer.check_present_token_for_plane(token2, 1, &issue) == pacer_hr::ok &&
               issue == FramePacer::TokenIssue::repeated);
        EXPECT(pacer.check_present_token_for_plane(token4, max_present_planes, &issue) == pacer_hr::invalid_arg);
        EXPECT(pacer.check_present_token_for_plane(token4 + 1, 0, &issue) == pacer_hr::invalid_arg);

        clock.advance_us(period + period / 8);  // the shared origin stream follows its slowest observed cadence
        std::uint64_t token5 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token5) == pacer_hr::ok && token5 == 5);
        clock.advance_us(3 * period - period / 8);  // plane 1 sees a four-period gap from its own first origin
        const std::uint64_t token5_on_plane1[2] = {0, token5};
        pacer.commit_present_tokens_for_planes(token5_on_plane1, 2);
        EXPECT(pacer.display_cadence_us() == period + 3 * period / 8);  // plane 1's independent 22 ms sample wins
        count = 2;
        EXPECT(pacer.get_frame_statistics(token5, stat_present | stat_display, &count, stats) == pacer_hr::ok && count == 2 &&
               stats[0].plane_index == 1 && stats[1].plane_index == 1);
    }

    // PresentX's single tear decision considers both plane origins: a late secondary token is not masked by a current
    // primary token.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        pacer.set_per_origin_pacing(true);
        EXPECT(setup(pacer, 2));
        std::uint64_t token1 = 0, token2 = 0, token3 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok && token1 == 1);
        pacer.commit_present_tokens_for_planes(&token1, 1);
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token2) == pacer_hr::ok && token2 == 2);
        clock.advance_us(period);
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token3) == pacer_hr::ok && token3 == 3);
        clock.advance_us(period);
        const std::uint64_t mixed[2] = {token3, token2};
        const FramePacer::PresentTiming timing = pacer.present_timing_for_planes(mixed, 2);
        EXPECT(timing.period_us == period && timing.late_us == period);  // token 2 is one interval late, token 3 is due now
        pacer.commit_present_tokens_for_planes(mixed, 2);
        EXPECT(pacer.frames_in_flight() == 0);
    }

    // Dead-man release does not erase unrelated origins: with latency 1, the fallback token can flip out of order but
    // the held event-2 wait stays held until the older token itself is presented.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        pacer.set_per_origin_pacing(true);
        EXPECT(setup(pacer, 1));
        std::uint64_t token1 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, 0, 0, &token1) == pacer_hr::ok && token1 == 1);
        pacer.commit_present_tokens_for_planes(&token1, 1);
        clock.advance_us(period);
        std::uint64_t token2 = 0;
        EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token2) == pacer_hr::ok && token2 == 2);
        auto held_origin = wait_async(pacer, infinite_timeout_ms);
        EXPECT(blocked(held_origin));
        clock.advance_us(4 * period);  // the waiter dead-man releases a new origin, without forgetting token 2
        const Result fallback = held_origin.get();
        EXPECT(fallback.hr == pacer_hr::ok && fallback.token == 3);
        EXPECT(pacer.present_clock_fallbacks() == 1 && pacer.frames_in_flight() == 2);
        pacer.commit_present_tokens_for_planes(&fallback.token, 1);  // token 3 flips before token 2
        EXPECT(pacer.flipped_token() == 3 && pacer.frames_in_flight() == 1);
        auto held_flip = std::async(std::launch::async, [&pacer] { return pacer.wait_flip(infinite_timeout_ms); });
        EXPECT(blocked_flip(held_flip));
        pacer.commit_present_tokens_for_planes(&token2, 1);
        EXPECT(held_flip.get() == pacer_hr::ok && pacer.frames_in_flight() == 0);
    }

    // The token queue and statistics ring share a 64-origin retention bound; overflow is counted and evicts the oldest
    // unresolved origin rather than growing process memory without limit.
    {
        ManualFrameClock clock;
        FramePacer pacer(clock);
        pacer.set_present_driven(true);
        pacer.set_per_origin_pacing(true);
        EXPECT(setup(pacer, 1));
        for (std::uint64_t expected = 1; expected <= 65; ++expected) {
            std::uint64_t token = 0;
            EXPECT(pacer.wait_frame_event(frame_event_origin, infinite_timeout_ms, 0, &token) == pacer_hr::ok &&
                   token == expected);
        }
        EXPECT(pacer.frames_in_flight() == 64 && pacer.per_origin_queue_overflows() == 1);
    }

    // Real clock: five 10 ms vblanks take about 50 ms.
    {
        SteadyFrameClock clock;
        FramePacer pacer(clock);
        EXPECT(pacer.set_frame_interval(10000, 1, 0) == pacer_hr::ok);
        EXPECT(pacer.schedule_frame_event(frame_event_origin, 0, 0) == pacer_hr::ok);
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t token = 0, last = 0;
        for (int i = 0; i < 5; ++i) {
            EXPECT(pacer.wait_frame_event(frame_event_origin, 2000, 0, &token) == pacer_hr::ok);
            EXPECT(token == last + 1);
            last = token;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::printf("real clock: 5 origins at 10 ms took %lld ms\n", static_cast<long long>(ms.count()));
        EXPECT(ms.count() >= 45 && ms.count() < 1000);
    }

    std::printf("frame_pacer_test: all cases pass\n");
    return 0;
}
