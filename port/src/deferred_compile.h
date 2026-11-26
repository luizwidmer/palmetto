#pragma once

// Off-thread pipeline compilation and the caller-side timing counters (docs/deferred_compile.md).
//
// The layer creates a VkPipeline synchronously inside the console pipeline-state call today. That call runs on the
// title's own task-pool threads, and the driver's compiler (nvgpucomp64) can hold a thread for a long time, so the
// pool that should run the title's work items is starved. CompileQueue moves that driver call onto the layer's own
// worker threads: the pipeline state is registered (layout, shader module and code base are cheap and stay on the
// creating thread) and the worker builds the VkPipeline; the submission that first names the pipeline waits for it,
// on the thread that submits, never at record time and never on the title's pool.
//
// CompileQueue is deliberately free of Vulkan, Windows and device dependencies: the caller owns the build closure
// and the returned handle, so the queue's state machine (queued -> building -> done/failed, the bound, the
// synchronous fallback signal, waiting and shutdown) is tested without a GPU (port/test/deferred_compile_test.cpp).
//
// PipelineCallStats answers the measurement question the slice starts from: how many threads enter pipeline
// creation, how long each spends inside the driver's vkCreate*Pipelines call, how many are inside it at once, and
// the duration histogram. It is off unless a caller constructs it (the device's switch is
// PALMETTO_PIPELINE_CALL_STATS), and it is GPU-free and testable too.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace palmetto {

// A stable 64-bit id for the calling thread (std::thread::id's hash), for the per-thread counters and the boot logs.
inline std::uint64_t thread_hash() {
    static thread_local const std::uint64_t id =
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return id;
}

// A bounded pool of compile workers. One entry point for both the deferred path and its fallback: `submit` returns
// nullptr when the switch is off, the pool was never started, or the queue is at its bound; the caller then takes the
// synchronous path, so no request is ever dropped.
class CompileQueue {
public:
    using Clock = std::chrono::steady_clock;

    class Task {
    public:
        enum class State { queued, building, done, failed, cancelled };

        Task() = default;
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        State state() const {
            std::lock_guard lock(mutex_);
            return state_;
        }
        bool finished() const {
            std::lock_guard lock(mutex_);
            return state_ == State::done || state_ == State::failed || state_ == State::cancelled;
        }
        std::uint64_t build_us() const {
            std::lock_guard lock(mutex_);
            return build_us_;
        }

    private:
        friend class CompileQueue;
        std::function<bool()> build_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        State state_ = State::queued;
        std::uint64_t build_us_ = 0;
    };

    // bound: the most tasks allowed outstanding (queued + building) before submit refuses. 0 means "no bound"; the
    // device always passes a finite one so a burst cannot grow without limit.
    // newest_first (docs/vertex_buffer_va.md section 13): the workers take the newest queued build instead of the
    // oldest. A title creates a burst of compute PSOs and submits a pipeline shortly after creating it; oldest-first
    // leaves that pipeline behind the whole burst and the submission waits for it, newest-first compiles the
    // just-created pipelines first, so the pipeline the upcoming submission names is ready before the title reaches
    // ExecuteCommandLists (the prewarm switch). Off by default: the default is the measured FIFO order.
    explicit CompileQueue(std::size_t bound = 256, bool newest_first = false)
        : bound_(bound), newest_first_(newest_first) {}
    ~CompileQueue() { stop(); }
    CompileQueue(const CompileQueue&) = delete;
    CompileQueue& operator=(const CompileQueue&) = delete;

    // Starts `workers` threads (1..64). Returns false when the queue already started or the count is unusable. Called
    // once, on the first deferred creation; the worker body calls `on_worker_start` (the device marks the thread in
    // PipelineCallStats with it).
    bool start(unsigned workers, std::function<void()> on_worker_start = {}) {
        std::lock_guard lock(mutex_);
        if (started_ || workers == 0 || workers > 64) return false;
        started_ = true;
        worker_start_ = std::move(on_worker_start);
        worker_count_ = workers;
        for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
        return true;
    }

    // Queues a build. nullptr when not started or at the bound (the caller builds synchronously); the counters say
    // which one happened.
    std::shared_ptr<Task> submit(std::function<bool()> build) {
        std::shared_ptr<Task> task;
        {
            std::lock_guard lock(mutex_);
            if (!started_ || stopping_) {
                ++rejected_;
                return nullptr;
            }
            std::size_t outstanding = outstanding_.load(std::memory_order_relaxed);
            if (bound_ && outstanding >= bound_) {
                ++rejected_;
                return nullptr;
            }
            task = std::make_shared<Task>();
            task->build_ = std::move(build);
            queue_.push_back(task);
            outstanding_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t peak = outstanding_.load(std::memory_order_relaxed);
            if (peak > peak_outstanding_.load(std::memory_order_relaxed))
                peak_outstanding_.store(peak, std::memory_order_relaxed);
            ++submitted_;
        }
        cv_.notify_one();
        return task;
    }

    // Blocks the calling thread until the task finished (the submission thread only). False when the build failed or
    // was cancelled. A null task is a failure, so a caller that lost its task does not wait on nothing.
    bool wait(const std::shared_ptr<Task>& task) {
        if (!task) return false;
        const auto started = Clock::now();
        const bool ok = wait_finished(task);
        record_wait(started, false);
        return ok;
    }

    // The submission's wait, for a caller that needs *that* pipeline rather than the whole queue ahead of it: when the
    // task is still queued the caller builds it on its own thread and returns; only a task a worker already owns is
    // waited for. The plain wait above is correct but FIFO: a boot that defers thousands of compute pipelines (docs/
    // deferred_compile.md section 3.1 bounds it at 4,096, the measured arm uses that) makes the submission that names
    // one wait for every older queued build, which the title's own 30-pass hang watchdog cannot outlast (docs/
    // 3d_textures.md section 11). Claiming the task bounds the submit stall to one driver build. The counters still
    // record the call as one submit wait, so the report keeps its meaning.
    bool wait_or_build(const std::shared_ptr<Task>& task) {
        if (!task) return false;
        const auto started = Clock::now();
        bool claimed = false;
        {
            std::lock_guard lock(mutex_);
            if (!stopping_) {
                const auto it = std::find(queue_.begin(), queue_.end(), task);
                if (it != queue_.end()) {
                    queue_.erase(it);
                    claimed = true;
                }
            }
        }
        const bool ok = claimed ? build_task(task) : wait_finished(task);
        record_wait(started, claimed);
        return ok;
    }

    // Cancels a queued task the caller no longer needs (its PSO died): a worker that has not started it skips the
    // build and marks it cancelled. A build already in flight runs to completion; the caller owns the handle then.
    void cancel(const std::shared_ptr<Task>& task) {
        if (!task) return;
        {
            std::lock_guard lock(task->mutex_);
            if (task->state_ != Task::State::queued) return;
            task->state_ = Task::State::cancelled;
        }
        ++cancelled_;
        task->cv_.notify_all();
    }

    // Stops accepting work and joins the workers. A task already queued still runs (its closure owns a pipeline
    // state's reference, so dropping it would leak that reference); the device only reaches this with an empty queue,
    // because a queued task holds a device reference and the device cannot be destroyed while it exists.
    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable()) worker.join();
        workers_.clear();
    }

    bool started() const {
        std::lock_guard lock(mutex_);
        return started_;
    }
    unsigned workers() const {
        std::lock_guard lock(mutex_);
        return worker_count_;
    }
    std::size_t bound() const { return bound_; }
    std::size_t pending() const { return outstanding_.load(std::memory_order_relaxed); }

    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t built() const { return built_; }
    std::uint64_t failed() const { return failed_; }
    std::uint64_t rejected() const { return rejected_; }
    std::uint64_t cancelled() const { return cancelled_; }
    std::uint64_t peak_outstanding() const { return peak_outstanding_.load(std::memory_order_relaxed); }
    std::uint64_t builds_us() const { return builds_us_.load(std::memory_order_relaxed); }
    std::uint64_t longest_build_us() const { return longest_build_us_.load(std::memory_order_relaxed); }
    std::uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }
    std::uint64_t wait_us() const { return wait_us_.load(std::memory_order_relaxed); }
    std::uint64_t longest_wait_us() const { return wait_us_max_.load(std::memory_order_relaxed); }
    // The submit waits split by who did the work (docs/peer_port_streaming.md): a wait the submitting thread spent in a
    // driver build it claimed itself (`wait_or_build` on a still-queued task) is the build-on-the-title-thread case the
    // non-blocking hold removes; the rest are waits for a worker's build already in flight.
    std::uint64_t wait_builds() const { return wait_builds_.load(std::memory_order_relaxed); }
    std::uint64_t wait_build_us() const { return wait_build_us_.load(std::memory_order_relaxed); }
    std::uint64_t longest_wait_build_us() const { return wait_build_us_max_.load(std::memory_order_relaxed); }

    // One line per thread that made a submit wait: which thread, how many, how long, and how many of them it built
    // itself. The device appends these to its report so a boot names the thread the expectation is about.
    std::string wait_threads_text() const {
        std::lock_guard lock(mutex_);
        std::string out;
        for (std::size_t i = 0; i < wait_thread_count_; ++i) {
            const WaitThread& thread = wait_threads_[i];
            char line[256] = {};
            std::snprintf(line, sizeof line,
                          "[rt] deferred compile: submit waits on tid 0x%llx: %llu, total %.1f ms, max %.1f ms; "
                          "built-on-submit %llu, %.1f ms, max single %.1f ms\n",
                          ull(thread.thread), ull(thread.waits), static_cast<double>(thread.us) / 1000.0,
                          static_cast<double>(thread.us_max) / 1000.0, ull(thread.builds),
                          static_cast<double>(thread.build_us) / 1000.0,
                          static_cast<double>(thread.build_us_max) / 1000.0);
            out += line;
        }
        return out;
    }

private:
    // Blocks until the task has finished, whichever thread built it.
    bool wait_finished(const std::shared_ptr<Task>& task) {
        std::unique_lock lock(task->mutex_);
        task->cv_.wait(lock, [&] { return task->state_ != Task::State::queued && task->state_ != Task::State::building; });
        return task->state_ == Task::State::done;
    }

    // Books one submit-path wait (both waits above call it, so the report's "submit waits" is the submission's own
    // stall whether it was a queue wait or a build on the submitting thread). `built_here` splits out the waits the
    // submitting thread spent inside its own driver build.
    void record_wait(Clock::time_point started, bool built_here) {
        const std::uint64_t us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        ++waits_;
        wait_us_.fetch_add(us, std::memory_order_relaxed);
        bump_max(wait_us_max_, us);
        if (built_here) {
            ++wait_builds_;
            wait_build_us_.fetch_add(us, std::memory_order_relaxed);
            bump_max(wait_build_us_max_, us);
        }
        const std::uint64_t thread = thread_id();
        std::lock_guard lock(mutex_);
        std::size_t slot = 0;
        while (slot < wait_thread_count_ && wait_threads_[slot].thread != thread) ++slot;
        if (slot == wait_thread_count_) {
            if (slot >= kMaxWaitThreads) return;  // counted in the totals, not followed per thread
            wait_threads_[slot].thread = thread;
            ++wait_thread_count_;
        }
        WaitThread& record = wait_threads_[slot];
        ++record.waits;
        record.us += us;
        if (us > record.us_max) record.us_max = us;
        if (built_here) {
            ++record.builds;
            record.build_us += us;
            if (us > record.build_us_max) record.build_us_max = us;
        }
    }

    static void bump_max(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
        std::uint64_t longest = counter.load(std::memory_order_relaxed);
        while (value > longest && !counter.compare_exchange_weak(longest, value, std::memory_order_relaxed)) {
        }
    }

    static unsigned long long ull(std::uint64_t value) { return static_cast<unsigned long long>(value); }

    // A stable 64-bit id for the calling thread: std::thread::id's hash, which the same thread returns every call.
    static std::uint64_t thread_id() { return thread_hash(); }

    // One build, whichever thread runs it (a compile worker, or the submitting thread from wait_or_build). The caller
    // has taken the task out of the queue; this marks it building, runs the closure, books the counters and wakes the
    // waiters. A task cancelled between the claim and the mark is skipped, which is what the worker always did.
    bool build_task(const std::shared_ptr<Task>& task) {
        bool skipped = false;
        {
            std::lock_guard lock(task->mutex_);
            if (task->state_ == Task::State::cancelled) skipped = true;
            else task->state_ = Task::State::building;
        }
        if (skipped) {
            task->cv_.notify_all();
            outstanding_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        const auto started = Clock::now();
        bool ok = false;
        if (task->build_) {
            // The closure owns the device call. A driver that throws is a failed pipeline, not a dead worker.
            try {
                ok = task->build_();
            } catch (...) {
                ok = false;
            }
        }
        const std::uint64_t us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        task->build_ = nullptr;  // release whatever the closure captured before the caller is woken
        {
            std::lock_guard lock(task->mutex_);
            task->state_ = ok ? Task::State::done : Task::State::failed;
            task->build_us_ = us;
        }
        if (ok) ++built_;
        else ++failed_;
        builds_us_.fetch_add(us, std::memory_order_relaxed);
        std::uint64_t longest = longest_build_us_.load(std::memory_order_relaxed);
        while (us > longest && !longest_build_us_.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
        }
        outstanding_.fetch_sub(1, std::memory_order_relaxed);
        task->cv_.notify_all();
        return ok;
    }

    void worker_loop() {
        {
            std::function<void()> mark;
            {
                std::lock_guard lock(mutex_);
                mark = worker_start_;
            }
            if (mark) mark();
        }
        for (;;) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                if (newest_first_ && queue_.size() > 1) {
                    // The device's prewarm order: the newest creation is the one a submission is closest to naming.
                    task = std::move(queue_.back());
                    queue_.pop_back();
                } else {
                    task = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            build_task(task);
        }
    }

    const std::size_t bound_;
    const bool newest_first_;
    // The per-thread submit-wait records (bounded; a thread past the bound still moves the totals above).
    static constexpr std::size_t kMaxWaitThreads = 32;
    struct WaitThread {
        std::uint64_t thread = 0;
        std::uint64_t waits = 0;
        std::uint64_t us = 0;
        std::uint64_t us_max = 0;
        std::uint64_t builds = 0;
        std::uint64_t build_us = 0;
        std::uint64_t build_us_max = 0;
    };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Task>> queue_;
    std::vector<std::thread> workers_;
    std::function<void()> worker_start_;
    bool started_ = false;
    bool stopping_ = false;
    unsigned worker_count_ = 0;
    std::atomic<std::size_t> outstanding_{0};
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> built_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> cancelled_{0};
    std::atomic<std::uint64_t> peak_outstanding_{0};
    std::atomic<std::uint64_t> builds_us_{0};
    std::atomic<std::uint64_t> longest_build_us_{0};
    std::atomic<std::uint64_t> waits_{0};
    std::atomic<std::uint64_t> wait_us_{0};
    std::atomic<std::uint64_t> wait_us_max_{0};
    std::atomic<std::uint64_t> wait_builds_{0};
    std::atomic<std::uint64_t> wait_build_us_{0};
    std::atomic<std::uint64_t> wait_build_us_max_{0};
    WaitThread wait_threads_[kMaxWaitThreads] = {};
    std::size_t wait_thread_count_ = 0;
};

// Who enters pipeline creation and how long they spend in the driver. The switch constructs one instance
// (PALMETTO_PIPELINE_CALL_STATS), and the device records a creation scope per Create*PipelineState call and a driver
// scope around each vkCreate{Graphics,Compute}Pipelines call. A scope with a null stats pointer is a no-op, so the
// default path pays one branch and nothing else.
class PipelineCallStats {
public:
    using Clock = std::chrono::steady_clock;

    enum class Kind { creation, driver };
    enum class Histogram : std::size_t { under_1ms, under_10ms, under_100ms, under_1s, under_10s, at_least_10s, count };

    class Scope {
    public:
        Scope(PipelineCallStats* stats, bool graphics, Kind kind,
              Clock::time_point started = Clock::now())
            : stats_(stats), graphics_(graphics), kind_(kind), started_(started) {
            if (stats_) stats_->begin(graphics, kind);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() {
            if (stats_) stats_->end(graphics_, kind_, us_since(started_));
        }

    private:
        PipelineCallStats* stats_;
        bool graphics_;
        Kind kind_;
        Clock::time_point started_;
    };

    explicit PipelineCallStats(std::size_t max_threads = 64) : max_threads_(max_threads) {}

    // Called by a compile-worker thread on entry, before it builds anything: the report then names the thread as the
    // deferred compiler instead of leaving it in the caller list like a title worker.
    void note_compile_worker() {
        std::lock_guard lock(mutex_);
        thread_record(thread_id()).compile_worker = true;
    }

    // Called by the layer's pipeline-hold flusher thread on entry (docs/peer_port_streaming.md): a boot's report then
    // says where a budget-forced build ran, so it cannot be read as a title thread compiling.
    void note_hold_flusher() {
        std::lock_guard lock(mutex_);
        thread_record(thread_id()).hold_flusher = true;
    }

    std::uint64_t creation_calls(bool graphics) const { return creation_calls_[graphics ? 0 : 1].load(); }
    std::uint64_t driver_calls(bool graphics) const { return driver_calls_[graphics ? 0 : 1].load(); }
    std::uint64_t driver_us(bool graphics) const { return driver_us_[graphics ? 0 : 1].load(); }
    std::uint64_t driver_us_max(bool graphics) const { return driver_us_max_[graphics ? 0 : 1].load(); }
    // Threads that made at least one Create*PipelineState call / that spent time in a driver create, and the largest
    // number of creation calls and of driver calls in flight at once (the starvation measurement).
    std::uint32_t caller_threads() const {
        std::lock_guard lock(mutex_);
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < thread_count_; ++i)
            if (threads_[i].creation_calls[0] || threads_[i].creation_calls[1]) ++count;
        return count;
    }
    std::uint32_t driver_threads() const {
        std::lock_guard lock(mutex_);
        return driver_thread_count_locked();
    }
    std::uint32_t threads_seen() const {
        std::lock_guard lock(mutex_);
        return thread_count_;
    }
    std::uint64_t concurrent_creation_max() const { return concurrent_creation_max_.load(); }
    std::uint64_t concurrent_driver_max() const { return concurrent_driver_max_.load(); }
    std::uint64_t histogram(bool graphics, Histogram bucket) const {
        return histogram_[graphics ? 0 : 1][static_cast<std::size_t>(bucket)].load();
    }

    // The full cumulative report, one line for the counters and one per caller thread (bounded). `extra` appends a
    // device-owned line (the compile queue's counters); it is called with the report mutex free.
    std::string report(const std::function<std::string()>& extra = {}) const {
        std::lock_guard lock(mutex_);
        const std::uint64_t now = now_us();
        const std::uint64_t started = started_us_;
        const double seconds = started && now > started ? static_cast<double>(now - started) / 1e6 : 0.0;
        char line[512] = {};
        std::snprintf(line, sizeof line,
                      "[rt] pipeline calls at %.1f s: create graphics %llu compute %llu; driver graphics %llu compute "
                      "%llu; driver time graphics %.1f ms compute %.1f ms (max single graphics %.1f ms compute %.1f ms); "
                      "creation threads %u, driver threads %u, threads seen %u; concurrent creation max %llu, concurrent "
                      "driver max %llu\n",
                      seconds, ull(creation_calls_[0].load()), ull(creation_calls_[1].load()),
                      ull(driver_calls_[0].load()), ull(driver_calls_[1].load()),
                      static_cast<double>(driver_us_[0].load()) / 1000.0,
                      static_cast<double>(driver_us_[1].load()) / 1000.0,
                      static_cast<double>(driver_us_max_[0].load()) / 1000.0,
                      static_cast<double>(driver_us_max_[1].load()) / 1000.0,
                      static_cast<unsigned>(caller_thread_count_locked()),
                      static_cast<unsigned>(driver_thread_count_locked()), static_cast<unsigned>(thread_count_),
                      ull(concurrent_creation_max_.load()), ull(concurrent_driver_max_.load()));
        std::string out = line;
        char hist[384] = {};
        std::snprintf(hist, sizeof hist,
                      "[rt] pipeline calls: driver histogram graphics <1ms %llu <10ms %llu <100ms %llu <1s %llu <10s %llu "
                      ">=10s %llu; compute <1ms %llu <10ms %llu <100ms %llu <1s %llu <10s %llu >=10s %llu\n",
                      ull(histogram_[0][0].load()), ull(histogram_[0][1].load()), ull(histogram_[0][2].load()),
                      ull(histogram_[0][3].load()), ull(histogram_[0][4].load()), ull(histogram_[0][5].load()),
                      ull(histogram_[1][0].load()), ull(histogram_[1][1].load()), ull(histogram_[1][2].load()),
                      ull(histogram_[1][3].load()), ull(histogram_[1][4].load()), ull(histogram_[1][5].load()));
        out += hist;
        for (std::uint32_t i = 0; i < thread_count_; ++i) {
            const ThreadRecord& thread = threads_[i];
            char one[320] = {};
            std::snprintf(one, sizeof one,
                          "[rt] pipeline caller tid 0x%llx%s: create graphics %llu compute %llu; driver graphics %llu "
                          "compute %llu, %.1f ms, max single %.1f ms; longest create %.1f ms\n",
                          ull(thread.thread), role_text(thread),
                          ull(thread.creation_calls[0]), ull(thread.creation_calls[1]), ull(thread.driver_calls[0]),
                          ull(thread.driver_calls[1]),
                          static_cast<double>(thread.driver_us[0] + thread.driver_us[1]) / 1000.0,
                          static_cast<double>(thread.driver_us_max) / 1000.0,
                          static_cast<double>(thread.creation_us_max) / 1000.0);
            out += one;
        }
        if (extra) out += extra();
        return out;
    }

private:
    struct ThreadRecord {
        std::uint64_t thread = 0;
        std::uint64_t creation_calls[2] = {};
        std::uint64_t driver_calls[2] = {};
        std::uint64_t driver_us[2] = {};
        std::uint64_t driver_us_max = 0;
        std::uint64_t creation_us_max = 0;
        bool compile_worker = false;
        bool hold_flusher = false;
    };

    static const char* role_text(const ThreadRecord& thread) {
        if (thread.compile_worker) return " [compile worker]";
        if (thread.hold_flusher) return " [hold flusher]";
        return "";
    }

    static std::uint64_t now_us() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    }
    static std::uint64_t us_since(Clock::time_point started) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
    }
    static unsigned long long ull(std::uint64_t value) { return static_cast<unsigned long long>(value); }

    void begin(bool graphics, Kind kind) {
        const std::uint64_t tid = thread_id();
        std::lock_guard lock(mutex_);
        if (!started_us_) started_us_ = now_us();
        ThreadRecord& thread = thread_record(tid);
        if (kind == Kind::creation) {
            ++thread.creation_calls[graphics ? 0 : 1];
            bump_max(concurrent_creation_, concurrent_creation_max_);
        } else {
            ++thread.driver_calls[graphics ? 0 : 1];
            bump_max(concurrent_driver_, concurrent_driver_max_);
        }
    }

    void end(bool graphics, Kind kind, std::uint64_t us) {
        const std::uint64_t tid = thread_id();
        std::lock_guard lock(mutex_);
        ThreadRecord& thread = thread_record(tid);
        if (kind == Kind::creation) {
            creation_calls_[graphics ? 0 : 1].fetch_add(1);
            if (us > thread.creation_us_max) thread.creation_us_max = us;
            concurrent_creation_.fetch_sub(1);
        } else {
            ++driver_calls_[graphics ? 0 : 1];
            driver_us_[graphics ? 0 : 1].fetch_add(us);
            if (us > thread.driver_us[graphics ? 0 : 1]) thread.driver_us[graphics ? 0 : 1] = us;
            if (us > thread.driver_us_max) thread.driver_us_max = us;
            std::uint64_t longest = driver_us_max_[graphics ? 0 : 1].load();
            while (us > longest && !driver_us_max_[graphics ? 0 : 1].compare_exchange_weak(longest, us)) {
            }
            histogram_[graphics ? 0 : 1][histogram_slot(us)].fetch_add(1);
            concurrent_driver_.fetch_sub(1);
        }
    }

    static void bump_max(std::atomic<std::uint64_t>& counter, std::atomic<std::uint64_t>& maximum) {
        const std::uint64_t now = counter.fetch_add(1) + 1;
        std::uint64_t longest = maximum.load();
        while (now > longest && !maximum.compare_exchange_weak(longest, now)) {
        }
    }

    std::uint32_t caller_thread_count_locked() const {
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < thread_count_; ++i)
            if (threads_[i].creation_calls[0] || threads_[i].creation_calls[1]) ++count;
        return count;
    }

    std::uint32_t driver_thread_count_locked() const {
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < thread_count_; ++i)
            if (threads_[i].driver_calls[0] || threads_[i].driver_calls[1]) ++count;
        return count;
    }

    // The record for the calling thread, created on first sight. A thread past the record bound (or one that just
    // marks itself a worker) is tracked in a shared overflow record: its counters still move the totals, it only does
    // not get its own report line.
    ThreadRecord& thread_record(std::uint64_t tid) {
        for (std::uint32_t i = 0; i < thread_count_; ++i)
            if (threads_[i].thread == tid) return threads_[i];
        if (overflow_.thread == tid) return overflow_;
        if (thread_count_ >= max_threads_ || thread_count_ >= threads_.size()) {
            overflow_.thread = tid;
            return overflow_;
        }
        ThreadRecord& fresh = threads_[thread_count_++];
        fresh = ThreadRecord{};
        fresh.thread = tid;
        return fresh;
    }

    static std::size_t histogram_slot(std::uint64_t us) {
        if (us < 1'000) return 0;
        if (us < 10'000) return 1;
        if (us < 100'000) return 2;
        if (us < 1'000'000) return 3;
        if (us < 10'000'000) return 4;
        return 5;
    }

    // A stable 64-bit id for the calling thread: std::thread::id's hash, which the same thread returns every call.
    static std::uint64_t thread_id() {
        static thread_local const std::uint64_t id = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return id;
    }

    const std::size_t max_threads_;
    mutable std::mutex mutex_;
    std::uint64_t started_us_ = 0;
    std::array<ThreadRecord, 64> threads_{};
    std::uint32_t thread_count_ = 0;
    std::atomic<std::uint64_t> creation_calls_[2] = {};
    std::atomic<std::uint64_t> driver_calls_[2] = {};
    std::atomic<std::uint64_t> driver_us_[2] = {};
    std::atomic<std::uint64_t> driver_us_max_[2] = {};
    std::atomic<std::uint64_t> histogram_[2][6] = {};
    std::atomic<std::uint64_t> concurrent_creation_{0};
    std::atomic<std::uint64_t> concurrent_creation_max_{0};
    std::atomic<std::uint64_t> concurrent_driver_{0};
    std::atomic<std::uint64_t> concurrent_driver_max_{0};
    ThreadRecord overflow_{};
};

// ---- the non-blocking submission hold (docs/peer_port_streaming.md) -----------------------------------------------
//
// The deferred compile above moves the driver build off the title's threads, but the submission that first names a
// pipeline still waits for it on the thread that submits (VulkanQueue::execute_now -> prepare_pipelines). That thread
// is a title thread, and the peer audit (docs/peer_port_streaming.md sections 5-6) ranks "no title thread waits for a
// pipeline build" as the first port: the peers build on their own workers and never park the submitting thread. This
// section is the GPU-free half of our own version of that port: the switch parsing, and the book that tracks which
// held submission waits for which deferred pipeline ids and enforces two budgets.
//
// The queue owns the batch itself (its existing held_ deque: a copy of the lists, the pins that keep the pipeline
// states alive, and the Wait fences), so ordering and lifetime come from the machinery docs/queue_holds.md already
// covers. What is here is only the decision: may this submission be held (bound), is the submission whose token is X
// ready (all its pipeline ids finished), has it been held past its budget (force the old wait), and the counters that
// the boot report prints.
//
// The switch is PALMETTO_PIPELINE_HOLD. Unset is the current path exactly: no submission is held for a pipeline and
// every wait stays where it is. The parsers are pure so the unit test covers every spelling without an environment.

struct PipelineHoldSettings {
    bool enabled = false;               // PALMETTO_PIPELINE_HOLD
    std::size_t bound = 16;             // PALMETTO_PIPELINE_HOLD_BOUND: the most submissions held for pipelines at once
    std::uint64_t budget_us = 2'000'000;  // PALMETTO_PIPELINE_HOLD_BUDGET_MS: how long one may stay held (ms, x1000)
};

// True when the value is a switch-on spelling: set, not empty, and not 0/off/false/no (case-insensitive, blanks
// trimmed). Every other value turns the switch on, as the repository's other switches do.
inline bool pipeline_hold_switch_enabled(const char* value) {
    if (!value) return false;
    while (*value == ' ' || *value == '\t') ++value;
    if (!*value) return false;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    char text[8] = {};
    std::size_t length = 0;
    for (const char* c = value; *c && length + 1 < sizeof text; ++c) text[length++] = lower(*c);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) text[--length] = '\0';
    return !(std::strcmp(text, "0") == 0 || std::strcmp(text, "off") == 0 || std::strcmp(text, "false") == 0 ||
             std::strcmp(text, "no") == 0);
}

// The bound: unset or unusable reads the default 16; a parsed value is clamped to 1..1024 so a typo cannot turn the
// hold into an unbounded queue and a 0 cannot disable the fallback by accident.
inline std::size_t pipeline_hold_bound(const char* value) {
    if (!value || !*value) return PipelineHoldSettings{}.bound;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) return PipelineHoldSettings{}.bound;
    const unsigned long long clamped = parsed < 1 ? 1 : (parsed > 1024 ? 1024 : parsed);
    return static_cast<std::size_t>(clamped);
}

// The budget in milliseconds (the report prints the value it read): unset reads 2000 ms, a parsed value is clamped to
// 1..600000 ms. The budget is the age past which a submission held only for pipelines gets the old synchronous wait,
// forced on the releasing thread, instead of staying held.
inline std::uint64_t pipeline_hold_budget_ms(const char* value) {
    constexpr std::uint64_t kDefaultMs = 2000;
    if (!value || !*value) return kDefaultMs;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) return kDefaultMs;
    return parsed < 1 ? 1 : (parsed > 600'000 ? 600'000 : parsed);
}

// The book of submissions held for pipelines. Thread-safe: a queue's submit thread adds and drops tokens, compile
// workers mark ids finished, and the hold flusher asks what is ready or expired.
class PipelineHoldPolicy {
public:
    using Clock = std::chrono::steady_clock;

    PipelineHoldPolicy() = default;
    explicit PipelineHoldPolicy(PipelineHoldSettings settings) : settings_(settings) {}

    const PipelineHoldSettings& settings() const { return settings_; }
    bool enabled() const { return settings_.enabled; }

    // Fills in the settings of a policy the device constructs disabled (the policy owns a mutex, so it is neither
    // copyable nor movable; the switch is registered before any queue exists).
    void configure(const PipelineHoldSettings& settings) {
        std::lock_guard lock(mutex_);
        settings_ = settings;
    }

    // The most submissions the budget allows held at once; past it the queue falls back to the old wait.
    bool at_bound() const {
        std::lock_guard lock(mutex_);
        return settings_.bound && entries_.size() >= settings_.bound;
    }
    std::size_t held() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    // Records submission `token` as held for `pipelines`. False when the budget is already used up (the caller then
    // falls back to the old synchronous path); the caller must not keep the token in that case.
    bool hold(std::uint64_t token, std::vector<std::uintptr_t> pipelines, Clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (settings_.bound && entries_.size() >= settings_.bound) {
            ++bound_refusals_;
            return false;
        }
        Entry entry;
        entry.pipelines = std::move(pipelines);
        entry.done.assign(entry.pipelines.size(), false);
        entry.held_at = now;
        entries_[token] = std::move(entry);
        ++held_total_;
        if (entries_.size() > peak_held_) peak_held_ = entries_.size();
        return true;
    }

    // A compile worker finished this pipeline id: every held submission naming it loses that dependency.
    void finished(std::uintptr_t id) {
        std::lock_guard lock(mutex_);
        for (auto& [token, entry] : entries_) {
            for (std::size_t i = 0; i < entry.pipelines.size(); ++i)
                if (entry.pipelines[i] == id) entry.done[i] = true;
        }
    }

    // Every pipeline this token names has been built (a failed build counts as finished: the held batch is released
    // and the replay refuses it, the same honest answer the eager path gives for an unresolved pipeline).
    bool ready(std::uint64_t token) const {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(token);
        if (it == entries_.end()) return true;
        for (const bool done : it->second.done)
            if (!done) return false;
        return true;
    }

    // The token has been held longer than its budget.
    bool expired(std::uint64_t token, Clock::time_point now) const {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(token);
        if (it == entries_.end()) return false;
        const auto age = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.held_at).count();
        return age >= static_cast<long long>(settings_.budget_us);
    }

    // The token's submission has run or been dropped. Books how long it was held.
    void release(std::uint64_t token) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(token);
        if (it == entries_.end()) return;
        const auto age = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - it->second.held_at).count();
        const std::uint64_t age_us = age < 0 ? 0 : static_cast<std::uint64_t>(age);
        if (age_us > longest_hold_us_) longest_hold_us_ = age_us;
        entries_.erase(it);
        ++released_;
    }

    // The token's held batch was dropped without running (queue teardown, a fence released under it, device loss):
    // the same bookkeeping as release, counted apart so a boot can say how much held work never ran.
    void drop(std::uint64_t token) {
        release(token);
        ++dropped_;
    }

    // Queue shutdown/teardown: the tokens are gone with the held batches.
    void clear() {
        std::lock_guard lock(mutex_);
        dropped_ += entries_.size();
        entries_.clear();
    }

    // Counters the boot report prints. `fallback` is one submission whose own submit thread took the old synchronous
    // wait because the hold budget could not take it; `forced` is a held submission completed by that wait when its
    // budget expired; `timeout` counts the expiry decision itself (one per forced release).
    void note_fallback() { ++fallbacks_; }
    void note_forced() { ++forced_; }
    void note_timeout() { ++timeouts_; }

    std::uint64_t held_total() const { return held_total_.load(std::memory_order_relaxed); }
    std::uint64_t released() const { return released_.load(std::memory_order_relaxed); }
    std::uint64_t fallbacks() const { return fallbacks_.load(std::memory_order_relaxed); }
    std::uint64_t forced() const { return forced_.load(std::memory_order_relaxed); }
    std::uint64_t timeouts() const { return timeouts_.load(std::memory_order_relaxed); }
    std::uint64_t bound_refusals() const { return bound_refusals_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t peak_held() const { return peak_held_.load(std::memory_order_relaxed); }
    std::uint64_t longest_hold_us() const { return longest_hold_us_.load(std::memory_order_relaxed); }

    // One report line (the device appends it to the deferred-compile line only when the switch is on).
    std::string report() const {
        char line[384] = {};
        std::snprintf(line, sizeof line,
                      "[rt] pipeline submit hold: bound %zu, budget %llu ms; held now %zu, peak %llu; holds %llu, "
                      "releases %llu, forced %llu, timeouts %llu, bound refusals %llu, fallback waits %llu, dropped "
                      "%llu; longest hold %.1f ms\n",
                      settings_.bound, static_cast<unsigned long long>(settings_.budget_us / 1000), held(),
                      static_cast<unsigned long long>(peak_held()), static_cast<unsigned long long>(held_total()),
                      static_cast<unsigned long long>(released()), static_cast<unsigned long long>(forced()),
                      static_cast<unsigned long long>(timeouts()),
                      static_cast<unsigned long long>(bound_refusals()),
                      static_cast<unsigned long long>(fallbacks()), static_cast<unsigned long long>(dropped()),
                      static_cast<double>(longest_hold_us()) / 1000.0);
        return line;
    }

private:
    struct Entry {
        std::vector<std::uintptr_t> pipelines;
        std::vector<bool> done;
        Clock::time_point held_at;
    };

    mutable std::mutex mutex_;
    PipelineHoldSettings settings_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::atomic<std::uint64_t> held_total_{0};
    std::atomic<std::uint64_t> released_{0};
    std::atomic<std::uint64_t> fallbacks_{0};
    std::atomic<std::uint64_t> forced_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> bound_refusals_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> peak_held_{0};
    std::atomic<std::uint64_t> longest_hold_us_{0};
};

}  // namespace palmetto
