// The off-thread compile queue and the pipeline-call counters without a Vulkan device: the queued/building/done state
// machine, the bound with its synchronous-fallback signal, waiting, cancellation and shutdown, and the caller-thread
// timing report. The builds are synthetic closures that set flags and wait on gates; no Vulkan loader, no device and no
// game files are used (docs/deferred_compile.md, port/src/deferred_compile.h).
#include "deferred_compile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dc = palmetto;

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

static int checks = 0;
#define CHECK_COUNT(condition) do {                                  \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
    ++checks;                                                        \
} while (0)

// A one-shot gate: a build blocks on it until the test opens it, so "queued" and "building" are observable states.
struct Gate {
    void wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return open; });
    }
    void open_now() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
};

static int test_unstarted() {
    dc::CompileQueue queue(4);
    CHECK(!queue.started());
    CHECK(queue.submit([] { return true; }) == nullptr);
    CHECK(queue.rejected() == 1);
    CHECK(queue.start(0) == false);  // an unusable worker count starts nothing
    CHECK(queue.start(2));
    CHECK(!queue.start(1));  // already started
    CHECK(queue.workers() == 2);
    queue.stop();
    CHECK(queue.submit([] { return true; }) == nullptr);  // stopped: the caller takes the synchronous path
    return 0;
}

static int test_state_machine() {
    dc::CompileQueue queue(4);
    CHECK(queue.start(1));
    Gate gate;
    std::atomic<bool> ran{false};
    const auto task = queue.submit([&] {
        gate.wait();
        ran = true;
        return true;
    });
    CHECK(task != nullptr);
    CHECK(!task->finished());  // queued or building, not finished
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!task->finished());  // still blocked in the build
    gate.open_now();
    CHECK(queue.wait(task));
    CHECK(task->finished());
    CHECK(task->state() == dc::CompileQueue::Task::State::done);
    CHECK(ran);
    CHECK(queue.submitted() == 1);
    CHECK(queue.built() == 1);
    CHECK(queue.failed() == 0);
    CHECK(queue.waits() == 1);

    // A build that reports failure is a failed task, and waiting says so.
    const auto failed = queue.submit([] { return false; });
    CHECK(failed != nullptr);
    CHECK(!queue.wait(failed));
    CHECK(failed->state() == dc::CompileQueue::Task::State::failed);
    CHECK(queue.failed() == 1);
    queue.stop();
    return 0;
}

static int test_bound_and_fallback() {
    dc::CompileQueue queue(2);
    CHECK(queue.start(1));
    Gate gate;
    const auto first = queue.submit([&] {
        gate.wait();
        return true;
    });
    CHECK(first != nullptr);
    const auto second = queue.submit([] { return true; });
    CHECK(second != nullptr);  // 2 outstanding: at the bound, still accepted
    const auto third = queue.submit([] { return true; });
    CHECK(third == nullptr);  // past the bound: the caller builds synchronously, nothing is dropped
    CHECK(queue.rejected() == 1);
    CHECK(queue.peak_outstanding() == 2);
    gate.open_now();
    CHECK(queue.wait(first));
    CHECK(queue.wait(second));
    CHECK(queue.built() == 2);
    // Once the queue drains, a later submit is accepted again.
    const auto fourth = queue.submit([] { return true; });
    CHECK(fourth != nullptr);
    CHECK(queue.wait(fourth));
    queue.stop();
    return 0;
}

static int test_worker_concurrency() {
    dc::CompileQueue queue(8);
    CHECK(queue.start(2));
    std::atomic<int> active{0}, peak{0};
    Gate gate;
    const auto build = [&] {
        const int now = active.fetch_add(1) + 1;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        gate.wait();
        active.fetch_sub(1);
        return true;
    };
    const auto first = queue.submit(build);
    const auto second = queue.submit(build);
    CHECK(first && second);
    // Both workers reach the build before the gate opens: two compiles run at once.
    for (int i = 0; i < 2000 && active.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(active.load() == 2);
    gate.open_now();
    CHECK(queue.wait(first));
    CHECK(queue.wait(second));
    CHECK(peak.load() == 2);
    CHECK(queue.workers() == 2);
    queue.stop();
    return 0;
}

static int test_cancel_and_shutdown() {
    dc::CompileQueue queue(4);
    CHECK(queue.start(1));
    Gate gate;
    std::atomic<bool> ran_first{false};
    const auto first = queue.submit([&] {
        gate.wait();
        ran_first = true;
        return true;
    });
    std::atomic<bool> ran_second{false};
    const auto second = queue.submit([&] {
        ran_second = true;
        return true;
    });
    CHECK(first && second);
    queue.cancel(second);  // still queued behind the blocked build: the worker must skip it
    CHECK(!queue.wait(second));
    CHECK(second->finished());
    CHECK(queue.cancelled() >= 1);
    gate.open_now();
    CHECK(queue.wait(first));
    CHECK(ran_first);
    CHECK(!ran_second);  // a cancelled task's closure is never run (the cancel notified its waiter above)
    // stop() joins the workers and refuses later work.
    queue.stop();
    CHECK(queue.submit([] { return true; }) == nullptr);
    return 0;
}

static int test_stats() {
    dc::PipelineCallStats stats;
    CHECK_COUNT(stats.caller_threads() == 0);
    {
        // The creating thread records its call and its driver time, as the device does around a synchronous build.
        dc::PipelineCallStats::Scope creation(&stats, /*graphics=*/false, dc::PipelineCallStats::Kind::creation);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        {
            dc::PipelineCallStats::Scope driver(&stats, false, dc::PipelineCallStats::Kind::driver);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        dc::PipelineCallStats::Scope graphics(&stats, true, dc::PipelineCallStats::Kind::creation);
        dc::PipelineCallStats::Scope graphics_driver(&stats, true, dc::PipelineCallStats::Kind::driver);
    }
    CHECK_COUNT(stats.creation_calls(false) == 1);
    CHECK_COUNT(stats.creation_calls(true) == 1);
    CHECK_COUNT(stats.driver_calls(false) == 1);
    CHECK_COUNT(stats.driver_calls(true) == 1);
    CHECK_COUNT(stats.driver_us(false) >= 1500);  // 2 ms sleep, generously bounded below
    CHECK_COUNT(stats.caller_threads() == 1);
    CHECK_COUNT(stats.driver_threads() == 1);
    CHECK_COUNT(stats.concurrent_creation_max() == 2);  // the graphics creation below starts while the compute one is open
    CHECK_COUNT(stats.concurrent_driver_max() == 1);
    // One compute driver call lands in exactly one duration bucket (the timer's granularity decides which).
    const std::uint64_t histogram_count = stats.histogram(false, dc::PipelineCallStats::Histogram::under_1ms) +
                                          stats.histogram(false, dc::PipelineCallStats::Histogram::under_10ms) +
                                          stats.histogram(false, dc::PipelineCallStats::Histogram::under_100ms) +
                                          stats.histogram(false, dc::PipelineCallStats::Histogram::under_1s);
    CHECK_COUNT(histogram_count == 1);
    const std::string report = stats.report();
    CHECK_COUNT(report.find("[rt] pipeline calls at") != std::string::npos);
    CHECK_COUNT(report.find("concurrent creation max 2") != std::string::npos);
    CHECK_COUNT(report.find("compute 1") != std::string::npos);

    // Two caller threads are counted separately, and a compile-worker thread names itself in the report.
    dc::PipelineCallStats shared;
    std::vector<std::thread> callers;
    for (int i = 0; i < 2; ++i) {
        callers.emplace_back([&shared] {
            dc::PipelineCallStats::Scope creation(&shared, false, dc::PipelineCallStats::Kind::creation);
            dc::PipelineCallStats::Scope driver(&shared, false, dc::PipelineCallStats::Kind::driver);
        });
    }
    for (auto& caller : callers) caller.join();
    std::thread marker_thread([&shared] { shared.note_compile_worker(); });
    marker_thread.join();
    CHECK_COUNT(shared.caller_threads() == 2);
    CHECK_COUNT(shared.driver_threads() == 2);
    CHECK_COUNT(shared.threads_seen() == 3);
    CHECK_COUNT(shared.report().find("[compile worker]") != std::string::npos);

    // A null stats pointer makes the scopes no-ops: the device's default path.
    dc::PipelineCallStats::Scope none(nullptr, false, dc::PipelineCallStats::Kind::driver);
    (void)none;
    return 0;
}

static int test_queue_report_extra() {
    dc::CompileQueue queue(4);
    CHECK(queue.start(1));
    const auto task = queue.submit([] { return true; });
    CHECK(queue.wait(task));
    // The device's periodic line appends the queue's counters.
    const std::string extra = "[rt] deferred compile: " + std::to_string(queue.built()) + " built\n";
    dc::PipelineCallStats stats;
    CHECK(stats.report([&extra] { return extra; }).find("deferred compile: 1 built") != std::string::npos);
    queue.stop();
    return 0;
}

// The submission's wait builds a deferred task it needs itself instead of waiting for the FIFO queue ahead of it: with
// one worker blocked in an older build, a task still queued is claimed and built on the calling thread (docs/
// 3d_textures.md section 11 measures why: a deferred boot queues thousands of compute pipelines, and the FIFO wait at a
// submission blocks past the title's hang watchdog).
static int test_submit_builds_queued_task() {
    dc::CompileQueue queue(8);
    CHECK(queue.start(1));
    Gate gate;
    std::atomic<bool> first_ran{false};
    const auto first = queue.submit([&] {
        gate.wait();
        first_ran = true;
        return true;
    });
    CHECK(first != nullptr);
    // The worker has claimed the first build and is blocked in it: any later task stays queued.
    for (int i = 0; i < 2000 && first->state() == dc::CompileQueue::Task::State::queued; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(first->state() == dc::CompileQueue::Task::State::building);
    std::atomic<bool> second_ran{false};
    const auto caller = std::this_thread::get_id();
    std::thread::id second_built_on;
    const auto second = queue.submit([&] {
        second_built_on = std::this_thread::get_id();
        second_ran = true;
        return true;
    });
    const auto third = queue.submit([] { return true; });
    CHECK(second && third);
    CHECK(second->state() == dc::CompileQueue::Task::State::queued);
    // The calling thread builds the task it names; the older blocked build and the later queued one are untouched.
    CHECK(queue.wait_or_build(second));
    CHECK(second_ran);
    CHECK(second_built_on == caller);
    CHECK(second->state() == dc::CompileQueue::Task::State::done);
    CHECK(queue.built() == 1);
    CHECK(queue.waits() == 1);
    CHECK(!first_ran);  // the worker is still in the older build
    CHECK(third->state() == dc::CompileQueue::Task::State::queued);
    // A task that has already finished is not built again.
    CHECK(queue.wait_or_build(second));
    CHECK(queue.built() == 1);
    CHECK(queue.waits() == 2);
    // A cancelled queued task is claimed and skipped: the caller is told it did not build.
    const auto fourth = queue.submit([] { return true; });
    CHECK(fourth != nullptr);
    queue.cancel(fourth);
    CHECK(!queue.wait_or_build(fourth));
    CHECK(fourth->state() == dc::CompileQueue::Task::State::cancelled);
    // A null task is a failure, as wait()'s contract says.
    CHECK(!queue.wait_or_build(nullptr));
    gate.open_now();
    // A build the worker owns is waited for normally (the worker finishes it once the gate opens).
    CHECK(queue.wait_or_build(first));
    CHECK(first_ran);
    CHECK(queue.wait(third));
    queue.stop();
    return 0;
}

int main() {
    int result = test_unstarted();
    if (!result) result = test_state_machine();
    if (!result) result = test_bound_and_fallback();
    if (!result) result = test_worker_concurrency();
    if (!result) result = test_cancel_and_shutdown();
    if (!result) result = test_submit_builds_queued_task();
    if (!result) result = test_stats();
    if (!result) result = test_queue_report_extra();
    if (result) return result;
    std::printf("deferred compile: ok (%d check(s))\n", checks);
    return 0;
}
