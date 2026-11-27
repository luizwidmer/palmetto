// GPU-free parity test for the IXThreadingImpl task-queue and XAsync surface (docs/task_queue_parity.md section 9,
// rank 6 of docs/peer_port_streaming.md section 6).
//
// The retail executable does not import xgameruntime.dll: its thunks ask QueryApiImpl for IXThreadingImpl and call
// one vtable slot per task-queue/XAsync function (docs/runtime_imports.md). port/shims/xgameruntime.cpp serves those
// slots by forwarding each one to vendor/xtaskqueue, Microsoft's libHttpClient implementation. This test drives every
// contract point the port plan names - the four dispatch modes, E_PENDING, the completion routine on the completion
// port, cancellation, one provider cleanup and XAsyncBlock reuse, and waiter/monitor delivery - twice: once through
// the shim's vtable slots (the measured title path) and once through the vendored library's own exported functions.
// The same named assertions run on both faces, so a later shim change (a callback trampoline, a mode translation, a
// handle wrapper) that silently diverges fails here without a boot and without a GPU. The named expectations that do
// differ are collected in scenario_expected_gaps().
//
// No game data, no GPU, no device: plain Win32 handles, the vendored queue and this repository's shim only.
#include <windows.h>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "shims.h"

// The vendored library's private test-harness begin, declared from vendor/xtaskqueue/Source/Task/XAsyncProviderPriv.h
// (STDAPI is a plain extern "C" __stdcall export). Our shim's slot 8 deliberately refuses it; scenario_expected_gaps()
// records that divergence as a named expectation.
extern "C" HRESULT WINAPI XAsyncBeginAlloc(XAsyncBlock*, const void*, const char*, XAsyncProvider*, size_t, size_t,
                                           void*);

namespace {

int g_checks = 0;
int g_failures = 0;

bool check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
    return condition;
}

template <class... Args>
bool checkf(bool condition, const char* format, Args... args) {
    char message[512];
    std::snprintf(message, sizeof message, format, args...);
    return check(condition, message);
}

using Clock = std::chrono::steady_clock;

std::int64_t elapsed_ms(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

template <class Predicate>
bool wait_until(Predicate predicate, std::uint32_t timeout_ms) {
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (predicate()) return true;
        if (Clock::now() >= deadline) return false;
        Sleep(1);
    }
}

// ---- The two faces of the same contract ----
//
// shim:     QueryApiImpl -> IXThreadingImpl -> the vtable slot for each function (the title's measured path)
// vendored: the emitted vendor/xtaskqueue exports themselves, linked in through palmetto_xtaskqueue
//
// The named-export path of the same shim (rt::shims::find("xgameruntime.dll", name)) is checked to be those same
// vendored functions at the bottom of main(); it is the path a title linked against xgameruntime.lib takes after its
// thunk resolves the module.

constexpr GUID kThreadingClass = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

using QueryFn = HRESULT(WINAPI*)(const GUID*, REFIID, void**);

void* threading_object() {
    auto query = reinterpret_cast<QueryFn>(rt::shims::find("xgameruntime.dll", "QueryApiImpl"));
    if (!query) return nullptr;
    void* object = nullptr;
    if (FAILED(query(&kThreadingClass, kThreadingClass, &object))) return nullptr;
    return object;
}

void* slot_function(void* object, int index) {
    return (*static_cast<void* const* const*>(object))[index];
}

void release_object(void* object) {
    reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(slot_function(object, 2))(object);
}

// The vtable slot signatures take `this` first, then the documented function's own arguments
// (docs/runtime_imports.md has the slot table).
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*);
using CompositeFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueuePortHandle, XTaskQueuePortHandle, XTaskQueueHandle*);
using GetPortFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, XTaskQueuePortHandle*);
using DuplicateFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueueHandle*);
using DispatchFn = bool(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, uint32_t);
using CloseFn = void(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle);
using SubmitFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, void*, XTaskQueueCallback*);
using SubmitDelayedFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, uint32_t, void*,
                                                    XTaskQueueCallback*);
using RegisterWaiterFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueuePort, HANDLE, void*,
                                                     XTaskQueueCallback*, XTaskQueueRegistrationToken*);
using UnregisterWaiterFn = void(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueueRegistrationToken);
using TerminateFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, bool, void*, XTaskQueueTerminatedCallback*);
using RegisterMonitorFn = HRESULT(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, void*, XTaskQueueMonitorCallback*,
                                                      XTaskQueueRegistrationToken*);
using UnregisterMonitorFn = void(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle, XTaskQueueRegistrationToken);
using ProcessQueueFn = bool(STDMETHODCALLTYPE*)(void*, XTaskQueueHandle*);
using GetStatusFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, bool);
using GetResultSizeFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, size_t*);
using CancelFn = void(STDMETHODCALLTYPE*)(void*, XAsyncBlock*);
using RunFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, XAsyncWork*);
using BeginFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, void*, const void*, const char*, XAsyncProvider*);
using ScheduleFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, uint32_t);
using CompleteFn = void(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, HRESULT, size_t);
using GetResultFn = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, const void*, size_t, void*, size_t*);

struct Facade {
    const char* name = "";
    std::function<HRESULT(XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*)> create;
    std::function<HRESULT(XTaskQueuePortHandle, XTaskQueuePortHandle, XTaskQueueHandle*)> create_composite;
    std::function<HRESULT(XTaskQueueHandle, XTaskQueuePort, XTaskQueuePortHandle*)> get_port;
    std::function<HRESULT(XTaskQueueHandle, XTaskQueueHandle*)> duplicate;
    std::function<bool(XTaskQueueHandle, XTaskQueuePort, uint32_t)> dispatch;
    std::function<void(XTaskQueueHandle)> close;
    std::function<HRESULT(XTaskQueueHandle, XTaskQueuePort, void*, XTaskQueueCallback*)> submit;
    std::function<HRESULT(XTaskQueueHandle, XTaskQueuePort, uint32_t, void*, XTaskQueueCallback*)> submit_delayed;
    std::function<HRESULT(XTaskQueueHandle, XTaskQueuePort, HANDLE, void*, XTaskQueueCallback*,
                          XTaskQueueRegistrationToken*)>
        register_waiter;
    std::function<void(XTaskQueueHandle, XTaskQueueRegistrationToken)> unregister_waiter;
    std::function<HRESULT(XTaskQueueHandle, bool, void*, XTaskQueueTerminatedCallback*)> terminate;
    std::function<HRESULT(XTaskQueueHandle, void*, XTaskQueueMonitorCallback*, XTaskQueueRegistrationToken*)>
        register_monitor;
    std::function<void(XTaskQueueHandle, XTaskQueueRegistrationToken)> unregister_monitor;
    std::function<bool(XTaskQueueHandle*)> get_process_queue;
    std::function<HRESULT(XAsyncBlock*, bool)> async_get_status;
    std::function<HRESULT(XAsyncBlock*, size_t*)> async_get_result_size;
    std::function<void(XAsyncBlock*)> async_cancel;
    std::function<HRESULT(XAsyncBlock*, XAsyncWork*)> async_run;
    std::function<HRESULT(XAsyncBlock*, void*, const void*, const char*, XAsyncProvider*)> async_begin;
    std::function<HRESULT(XAsyncBlock*, uint32_t)> async_schedule;
    std::function<void(XAsyncBlock*, HRESULT, size_t)> async_complete;
    std::function<HRESULT(XAsyncBlock*, const void*, size_t, void*, size_t*)> async_get_result;
};

Facade shim_face(void* object) {
    Facade f;
    f.name = "shim slot path";
    f.create = [object](XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue) {
        return reinterpret_cast<CreateFn>(slot_function(object, 12))(object, work, completion, queue);
    };
    f.create_composite = [object](XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle* queue) {
        return reinterpret_cast<CompositeFn>(slot_function(object, 13))(object, work, completion, queue);
    };
    f.get_port = [object](XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle* handle) {
        return reinterpret_cast<GetPortFn>(slot_function(object, 14))(object, queue, port, handle);
    };
    f.duplicate = [object](XTaskQueueHandle queue, XTaskQueueHandle* duplicate) {
        return reinterpret_cast<DuplicateFn>(slot_function(object, 15))(object, queue, duplicate);
    };
    f.dispatch = [object](XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeout) {
        return reinterpret_cast<DispatchFn>(slot_function(object, 16))(object, queue, port, timeout);
    };
    f.close = [object](XTaskQueueHandle queue) {
        reinterpret_cast<CloseFn>(slot_function(object, 17))(object, queue);
    };
    f.submit = [object](XTaskQueueHandle queue, XTaskQueuePort port, void* context, XTaskQueueCallback* callback) {
        return reinterpret_cast<SubmitFn>(slot_function(object, 18))(object, queue, port, context, callback);
    };
    f.submit_delayed = [object](XTaskQueueHandle queue, XTaskQueuePort port, uint32_t delay, void* context,
                                XTaskQueueCallback* callback) {
        return reinterpret_cast<SubmitDelayedFn>(slot_function(object, 19))(object, queue, port, delay, context, callback);
    };
    f.register_waiter = [object](XTaskQueueHandle queue, XTaskQueuePort port, HANDLE handle, void* context,
                                 XTaskQueueCallback* callback, XTaskQueueRegistrationToken* token) {
        return reinterpret_cast<RegisterWaiterFn>(slot_function(object, 20))(object, queue, port, handle, context, callback,
                                                                   token);
    };
    f.unregister_waiter = [object](XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
        reinterpret_cast<UnregisterWaiterFn>(slot_function(object, 21))(object, queue, token);
    };
    f.terminate = [object](XTaskQueueHandle queue, bool wait, void* context, XTaskQueueTerminatedCallback* callback) {
        return reinterpret_cast<TerminateFn>(slot_function(object, 22))(object, queue, wait, context, callback);
    };
    f.register_monitor = [object](XTaskQueueHandle queue, void* context, XTaskQueueMonitorCallback* callback,
                                  XTaskQueueRegistrationToken* token) {
        return reinterpret_cast<RegisterMonitorFn>(slot_function(object, 23))(object, queue, context, callback, token);
    };
    f.unregister_monitor = [object](XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
        reinterpret_cast<UnregisterMonitorFn>(slot_function(object, 24))(object, queue, token);
    };
    f.get_process_queue = [object](XTaskQueueHandle* queue) {
        return reinterpret_cast<ProcessQueueFn>(slot_function(object, 25))(object, queue);
    };
    f.async_get_status = [object](XAsyncBlock* block, bool wait) {
        return reinterpret_cast<GetStatusFn>(slot_function(object, 3))(object, block, wait);
    };
    f.async_get_result_size = [object](XAsyncBlock* block, size_t* size) {
        return reinterpret_cast<GetResultSizeFn>(slot_function(object, 4))(object, block, size);
    };
    f.async_cancel = [object](XAsyncBlock* block) {
        reinterpret_cast<CancelFn>(slot_function(object, 5))(object, block);
    };
    f.async_run = [object](XAsyncBlock* block, XAsyncWork* work) {
        return reinterpret_cast<RunFn>(slot_function(object, 6))(object, block, work);
    };
    f.async_begin = [object](XAsyncBlock* block, void* context, const void* identity, const char* name,
                             XAsyncProvider* provider) {
        return reinterpret_cast<BeginFn>(slot_function(object, 7))(object, block, context, identity, name, provider);
    };
    f.async_schedule = [object](XAsyncBlock* block, uint32_t delay) {
        return reinterpret_cast<ScheduleFn>(slot_function(object, 9))(object, block, delay);
    };
    f.async_complete = [object](XAsyncBlock* block, HRESULT result, size_t size) {
        reinterpret_cast<CompleteFn>(slot_function(object, 10))(object, block, result, size);
    };
    f.async_get_result = [object](XAsyncBlock* block, const void* identity, size_t size, void* buffer,
                                  size_t* used) {
        return reinterpret_cast<GetResultFn>(slot_function(object, 11))(object, block, identity, size, buffer, used);
    };
    return f;
}

Facade vendored_face() {
    Facade f;
    f.name = "vendored export";
    f.create = [](XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue) {
        return XTaskQueueCreate(work, completion, queue);
    };
    f.create_composite = [](XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle* queue) {
        return XTaskQueueCreateComposite(work, completion, queue);
    };
    f.get_port = [](XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle* handle) {
        return XTaskQueueGetPort(queue, port, handle);
    };
    f.duplicate = [](XTaskQueueHandle queue, XTaskQueueHandle* duplicate) {
        return XTaskQueueDuplicateHandle(queue, duplicate);
    };
    f.dispatch = [](XTaskQueueHandle queue, XTaskQueuePort port, uint32_t timeout) {
        return XTaskQueueDispatch(queue, port, timeout);
    };
    f.close = [](XTaskQueueHandle queue) { XTaskQueueCloseHandle(queue); };
    f.submit = [](XTaskQueueHandle queue, XTaskQueuePort port, void* context, XTaskQueueCallback* callback) {
        return XTaskQueueSubmitCallback(queue, port, context, callback);
    };
    f.submit_delayed = [](XTaskQueueHandle queue, XTaskQueuePort port, uint32_t delay, void* context,
                          XTaskQueueCallback* callback) {
        return XTaskQueueSubmitDelayedCallback(queue, port, delay, context, callback);
    };
    f.register_waiter = [](XTaskQueueHandle queue, XTaskQueuePort port, HANDLE handle, void* context,
                           XTaskQueueCallback* callback, XTaskQueueRegistrationToken* token) {
        return XTaskQueueRegisterWaiter(queue, port, handle, context, callback, token);
    };
    f.unregister_waiter = [](XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
        XTaskQueueUnregisterWaiter(queue, token);
    };
    f.terminate = [](XTaskQueueHandle queue, bool wait, void* context, XTaskQueueTerminatedCallback* callback) {
        return XTaskQueueTerminate(queue, wait, context, callback);
    };
    f.register_monitor = [](XTaskQueueHandle queue, void* context, XTaskQueueMonitorCallback* callback,
                            XTaskQueueRegistrationToken* token) {
        return XTaskQueueRegisterMonitor(queue, context, callback, token);
    };
    f.unregister_monitor = [](XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
        XTaskQueueUnregisterMonitor(queue, token);
    };
    f.get_process_queue = [](XTaskQueueHandle* queue) { return XTaskQueueGetCurrentProcessTaskQueue(queue); };
    f.async_get_status = [](XAsyncBlock* block, bool wait) { return XAsyncGetStatus(block, wait); };
    f.async_get_result_size = [](XAsyncBlock* block, size_t* size) { return XAsyncGetResultSize(block, size); };
    f.async_cancel = [](XAsyncBlock* block) { XAsyncCancel(block); };
    f.async_run = [](XAsyncBlock* block, XAsyncWork* work) { return XAsyncRun(block, work); };
    f.async_begin = [](XAsyncBlock* block, void* context, const void* identity, const char* name,
                       XAsyncProvider* provider) { return XAsyncBegin(block, context, identity, name, provider); };
    f.async_schedule = [](XAsyncBlock* block, uint32_t delay) { return XAsyncSchedule(block, delay); };
    f.async_complete = [](XAsyncBlock* block, HRESULT result, size_t size) { XAsyncComplete(block, result, size); };
    f.async_get_result = [](XAsyncBlock* block, const void* identity, size_t size, void* buffer, size_t* used) {
        return XAsyncGetResult(block, identity, size, buffer, used);
    };
    return f;
}

// ---- Shared callbacks ----

std::atomic<int> g_work_runs{0};
std::atomic<DWORD> g_work_thread{0};

HRESULT CALLBACK run_work(XAsyncBlock*) {
    ++g_work_runs;
    g_work_thread = GetCurrentThreadId();
    return S_OK;
}

struct CompletionObservation {
    const Facade* facade = nullptr;
    std::atomic<int> calls{0};
    std::atomic<DWORD> thread{0};
    std::atomic<HRESULT> status_inside{E_PENDING};
};

void CALLBACK completion_callback(XAsyncBlock* block) {
    auto* observation = static_cast<CompletionObservation*>(block->context);
    ++observation->calls;
    observation->thread = GetCurrentThreadId();
    observation->status_inside = observation->facade->async_get_status(block, false);
}

struct ThreadObservation {
    std::atomic<int> calls{0};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> canceled{false};
};

void CALLBACK counter_callback(void* context, bool canceled) {
    auto* observation = static_cast<ThreadObservation*>(context);
    ++observation->calls;
    observation->thread = GetCurrentThreadId();
    observation->canceled = canceled;
}

struct SerializedObservation {
    std::atomic<int> calls{0};
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<DWORD> first_thread{0};
};

void CALLBACK serialized_callback(void* context, bool) {
    auto* observation = static_cast<SerializedObservation*>(context);
    const int now = ++observation->in_flight;
    int previous = observation->max_in_flight.load();
    while (now > previous && !observation->max_in_flight.compare_exchange_weak(previous, now)) {}
    if (observation->first_thread.load() == 0) observation->first_thread = GetCurrentThreadId();
    Sleep(1);  // long enough for a second worker to overlap if the mode serialized nothing
    --observation->in_flight;
    ++observation->calls;
}

struct PendingProvider {
    std::atomic<int> begins{0};
    std::atomic<int> works{0};
    std::atomic<int> cancels{0};
    std::atomic<int> cleanups{0};
    std::atomic<int> get_results{0};
};

HRESULT CALLBACK pending_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* p = static_cast<PendingProvider*>(data->context);
    switch (op) {
        case XAsyncOp::Begin: ++p->begins; return S_OK;
        case XAsyncOp::DoWork: ++p->works; return E_PENDING;  // reserve the call: more work is coming
        case XAsyncOp::Cancel: ++p->cancels; return S_OK;
        case XAsyncOp::GetResult: ++p->get_results; return S_OK;
        case XAsyncOp::Cleanup: ++p->cleanups; return S_OK;
    }
    return S_OK;
}

struct CancelProvider {
    const Facade* facade = nullptr;
    XAsyncBlock* block = nullptr;
    std::atomic<int> begins{0};
    std::atomic<int> works{0};
    std::atomic<int> cancels{0};
    std::atomic<int> cleanups{0};
};

HRESULT CALLBACK cancel_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* p = static_cast<CancelProvider*>(data->context);
    switch (op) {
        case XAsyncOp::Begin:
            ++p->begins;
            return S_OK;
        case XAsyncOp::Cancel:
            // The documented provider contract for cancel: finish the call with E_ABORT.
            ++p->cancels;
            p->facade->async_complete(p->block, E_ABORT, 0);
            return S_OK;
        case XAsyncOp::DoWork:
            ++p->works;  // must never run: the call was canceled before its work was dispatched
            return S_OK;
        case XAsyncOp::Cleanup:
            ++p->cleanups;
            return S_OK;
        case XAsyncOp::GetResult:
            return S_OK;
    }
    return S_OK;
}

struct PayloadProvider {
    const Facade* facade = nullptr;
    std::atomic<int> begins{0};
    std::atomic<int> works{0};
    std::atomic<int> get_results{0};
    std::atomic<int> cleanups{0};
};

HRESULT CALLBACK payload_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* p = static_cast<PayloadProvider*>(data->context);
    switch (op) {
        case XAsyncOp::Begin:
            ++p->begins;
            return S_OK;
        case XAsyncOp::DoWork:
            ++p->works;
            p->facade->async_complete(data->async, S_OK, sizeof(std::uint32_t));
            return S_OK;
        case XAsyncOp::GetResult:
            *static_cast<std::uint32_t*>(data->buffer) = 0x1EA0;
            ++p->get_results;
            return S_OK;
        case XAsyncOp::Cleanup:
            ++p->cleanups;
            return S_OK;
        case XAsyncOp::Cancel:
            return S_OK;
    }
    return S_OK;
}

struct WaiterObservation {
    std::atomic<int> calls{0};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> canceled{false};
};

void CALLBACK waiter_callback(void* context, bool canceled) {
    auto* observation = static_cast<WaiterObservation*>(context);
    ++observation->calls;
    observation->thread = GetCurrentThreadId();
    observation->canceled = canceled;
}

struct MonitorObservation {
    std::atomic<int> calls{0};
    std::atomic<int> work{0};
    std::atomic<int> completion{0};
};

void CALLBACK monitor_callback(void* context, XTaskQueueHandle, XTaskQueuePort port) {
    auto* observation = static_cast<MonitorObservation*>(context);
    ++observation->calls;
    if (port == XTaskQueuePort::Work) ++observation->work;
    else ++observation->completion;
}

struct TerminationObservation {
    std::atomic<int> calls{0};
};

void CALLBACK terminated_callback(void* context) {
    ++static_cast<TerminationObservation*>(context)->calls;
}

struct OrderObservation {
    std::vector<int> order;
};

struct OrderBlock {
    OrderObservation* observation = nullptr;
    int index = 0;
};

void CALLBACK order_callback(XAsyncBlock* block) {
    auto* order = static_cast<OrderBlock*>(block->context);
    order->observation->order.push_back(order->index);
}

// ---- Contract points ----

// Ports and dispatch modes. All sixteen mode pairs create a queue on both faces; the measured title shape (two
// distinct ThreadPool/ThreadPool queues and an Immediate/Immediate one) and the duplicate -> Work port -> composite
// sequence with the same port for both ports (docs/task_queue_parity.md section 2) are driven too.
void scenario_dispatch_modes(const Facade& f) {
    static const XTaskQueueDispatchMode kModes[] = {XTaskQueueDispatchMode::Manual,
                                                    XTaskQueueDispatchMode::ThreadPool,
                                                    XTaskQueueDispatchMode::SerializedThreadPool,
                                                    XTaskQueueDispatchMode::Immediate};
    static const char* const kModeNames[] = {"Manual", "ThreadPool", "SerializedThreadPool", "Immediate"};
    for (int work = 0; work < 4; ++work) {
        for (int completion = 0; completion < 4; ++completion) {
            XTaskQueueHandle queue = nullptr;
            const HRESULT hr = f.create(kModes[work], kModes[completion], &queue);
            checkf(SUCCEEDED(hr) && queue != nullptr, "%s: XTaskQueueCreate(%s, %s) creates a queue", f.name,
                   kModeNames[work], kModeNames[completion]);
            if (queue) f.close(queue);
        }
    }

    XTaskQueueHandle first = nullptr;
    XTaskQueueHandle second = nullptr;
    checkf(f.create(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &first) == S_OK &&
               first != nullptr &&
               f.create(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool, &second) == S_OK &&
               second != nullptr && first != second,
           "%s: the title's two measured ThreadPool/ThreadPool creates are two distinct queues", f.name);
    if (first) f.close(first);
    if (second) f.close(second);

    XTaskQueueHandle queue = nullptr;
    XTaskQueueHandle duplicate = nullptr;
    XTaskQueueHandle composite = nullptr;
    XTaskQueuePortHandle work_port = nullptr;
    XTaskQueuePortHandle completion_port = nullptr;
    const bool created = f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                         queue != nullptr;
    checkf(created, "%s: a Manual/Manual queue for the port sequence", f.name);
    if (!created) return;
    checkf(f.duplicate(queue, &duplicate) == S_OK && duplicate != nullptr && duplicate != queue,
           "%s: XTaskQueueDuplicateHandle returns a distinct second handle", f.name);
    checkf(f.get_port(queue, XTaskQueuePort::Work, &work_port) == S_OK && work_port != nullptr &&
               f.get_port(queue, XTaskQueuePort::Completion, &completion_port) == S_OK && completion_port != nullptr,
           "%s: XTaskQueueGetPort returns both port handles", f.name);
    // The measured title call passes the same port value for the work and the completion argument; the GDK permits
    // it and the vendored implementation accepts it.
    checkf(f.create_composite(work_port, work_port, &composite) == S_OK && composite != nullptr,
           "%s: XTaskQueueCreateComposite accepts the same work port for both ports (the measured title call)", f.name);
    if (composite) f.close(composite);
    if (duplicate) f.close(duplicate);
    f.close(queue);
}

// Manual: submitted work waits for XTaskQueueDispatch on its own port; the completion port is separate.
void scenario_manual_dispatch(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue", f.name))
        return;
    ThreadObservation work;
    ThreadObservation completion;
    checkf(f.submit(queue, XTaskQueuePort::Work, &work, counter_callback) == S_OK,
           "%s: submitting a work callback to a Manual port returns S_OK", f.name);
    checkf(work.calls.load() == 0, "%s: a Manual work callback does not run during submit", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && work.calls.load() == 1,
           "%s: dispatching the Manual work port runs the callback exactly once", f.name);
    checkf(work.thread.load() == GetCurrentThreadId(),
           "%s: the Manual work callback runs on the dispatching thread", f.name);
    checkf(!work.canceled.load(), "%s: a Manual work callback sees canceled=false", f.name);
    checkf(!f.dispatch(queue, XTaskQueuePort::Work, 0) && work.calls.load() == 1,
           "%s: a second dispatch of the empty work port returns false", f.name);

    checkf(f.submit(queue, XTaskQueuePort::Completion, &completion, counter_callback) == S_OK,
           "%s: submitting a callback to the completion port returns S_OK", f.name);
    checkf(completion.calls.load() == 0 && !f.dispatch(queue, XTaskQueuePort::Work, 0),
           "%s: a completion-port callback is not run by dispatching the work port", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Completion, 0) && completion.calls.load() == 1,
           "%s: dispatching the completion port runs the completion callback once", f.name);
    checkf(completion.thread.load() == GetCurrentThreadId(),
           "%s: the Manual completion callback runs on the dispatching thread", f.name);
    f.close(queue);
}

// Immediate: the callback runs on the submitting thread before submit returns, and nothing is left queued.
void scenario_immediate(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Immediate, XTaskQueueDispatchMode::Immediate, &queue) == S_OK &&
                    queue != nullptr,
                "%s: an Immediate/Immediate queue", f.name))
        return;
    ThreadObservation work;
    const HRESULT hr = f.submit(queue, XTaskQueuePort::Work, &work, counter_callback);
    checkf(hr == S_OK && work.calls.load() == 1,
           "%s: an Immediate work callback runs during the submit call", f.name);
    checkf(work.thread.load() == GetCurrentThreadId(),
           "%s: the Immediate callback runs on the submitting thread", f.name);
    checkf(!f.dispatch(queue, XTaskQueuePort::Work, 0),
           "%s: the Immediate work port is already empty after the submit", f.name);

    CompletionObservation completion;
    completion.facade = &f;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &completion;
    block.callback = completion_callback;
    g_work_runs = 0;
    const HRESULT run = f.async_run(&block, run_work);
    checkf(run == S_OK && completion.calls.load() == 1,
           "%s: XAsyncRun on Immediate/Immediate invokes the completion routine before it returns", f.name);
    checkf(completion.thread.load() == GetCurrentThreadId(),
           "%s: that completion routine runs on the submitting thread", f.name);
    checkf(g_work_runs.load() == 1 && f.async_get_status(&block, false) == S_OK,
           "%s: the Immediate async work ran once and its status is S_OK", f.name);
    f.close(queue);
}

// ThreadPool: work runs on a pool thread without a dispatch call, on a thread other than the submitter.
void scenario_thread_pool(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a ThreadPool/Manual queue", f.name))
        return;
    ThreadObservation work;
    checkf(f.submit(queue, XTaskQueuePort::Work, &work, counter_callback) == S_OK,
           "%s: submitting to a ThreadPool work port returns S_OK", f.name);
    checkf(wait_until([&] { return work.calls.load() == 1; }, 5000),
           "%s: a ThreadPool work callback runs without any dispatch call", f.name);
    checkf(work.thread.load() != GetCurrentThreadId(),
           "%s: the ThreadPool work callback runs on a pool thread", f.name);
    f.close(queue);
}

// SerializedThreadPool: as ThreadPool, but one callback at a time.
void scenario_serialized_thread_pool(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::SerializedThreadPool, XTaskQueueDispatchMode::Manual, &queue) ==
                        S_OK &&
                    queue != nullptr,
                "%s: a SerializedThreadPool/Manual queue", f.name))
        return;
    SerializedObservation observation;
    bool submitted = true;
    for (int i = 0; i < 8; ++i) submitted = submitted && f.submit(queue, XTaskQueuePort::Work, &observation,
                                                                 serialized_callback) == S_OK;
    checkf(submitted, "%s: eight callbacks submitted to the serialized port", f.name);
    checkf(wait_until([&] { return observation.calls.load() == 8; }, 5000),
           "%s: all eight SerializedThreadPool callbacks ran", f.name);
    checkf(observation.max_in_flight.load() == 1,
           "%s: SerializedThreadPool never ran two callbacks at once (max in flight %d)", f.name,
           observation.max_in_flight.load());
    checkf(observation.first_thread.load() != GetCurrentThreadId(),
           "%s: the SerializedThreadPool callbacks run on a pool thread", f.name);
    f.close(queue);
}

// Delayed submission (slot 19 / XTaskQueueSubmitDelayedCallback): not due at once, runs inside the dispatch call
// that reaches its deadline.
void scenario_delayed(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue", f.name))
        return;
    ThreadObservation work;
    const Clock::time_point start = Clock::now();
    checkf(f.submit_delayed(queue, XTaskQueuePort::Work, 400, &work, counter_callback) == S_OK,
           "%s: a 400 ms delayed callback is submitted", f.name);
    checkf(!f.dispatch(queue, XTaskQueuePort::Work, 0) && work.calls.load() == 0,
           "%s: a delayed callback is not dispatched before its delay has elapsed", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 3000) && work.calls.load() == 1,
           "%s: dispatching with a timeout runs the delayed callback when it becomes due", f.name);
    checkf(elapsed_ms(start) >= 100, "%s: the delayed callback observed its delay (%lld ms)", f.name,
           static_cast<long long>(elapsed_ms(start)));
    f.close(queue);
}

// E_PENDING on the work port, and the schedule/complete state machine around it: a provider that returns E_PENDING
// leaves the call pending; a second XAsyncSchedule is refused; XAsyncComplete(E_PENDING) is a no-op; the terminal
// completion cleans the provider up exactly once.
void scenario_async_e_pending(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for the async contract", f.name))
        return;
    PendingProvider provider;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &provider;
    checkf(f.async_begin(&block, &provider, nullptr, "task_queue_parity_test", pending_provider) == S_OK &&
               provider.begins.load() == 1,
           "%s: XAsyncBegin starts the provider with XAsyncOp::Begin exactly once", f.name);
    checkf(f.async_get_status(&block, false) == E_PENDING,
           "%s: a begun call reports E_PENDING before its work runs", f.name);
    size_t size = 1;
    checkf(f.async_get_result_size(&block, &size) == E_PENDING && size == 0,
           "%s: XAsyncGetResultSize on a pending call reports E_PENDING and size 0", f.name);
    checkf(f.async_schedule(&block, 0) == S_OK, "%s: XAsyncSchedule queues the work on the work port", f.name);
    checkf(f.async_schedule(&block, 0) == E_UNEXPECTED,
           "%s: a second XAsyncSchedule while work is already queued returns E_UNEXPECTED", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && provider.works.load() == 1,
           "%s: dispatching the work port invokes the provider's DoWork once", f.name);
    checkf(f.async_get_status(&block, false) == E_PENDING,
           "%s: DoWork returning E_PENDING leaves the call's status E_PENDING", f.name);
    f.async_complete(&block, E_PENDING, 0);
    checkf(f.async_get_status(&block, false) == E_PENDING,
           "%s: XAsyncComplete(E_PENDING) does not complete the call", f.name);
    f.async_complete(&block, S_OK, 0);
    checkf(f.async_get_status(&block, false) == S_OK,
           "%s: completing the call sets its terminal status to S_OK", f.name);
    checkf(provider.cleanups.load() == 1,
           "%s: the provider sees XAsyncOp::Cleanup exactly once (count %d)", f.name, provider.cleanups.load());
    checkf(provider.cancels.load() == 0 && provider.get_results.load() == 0,
           "%s: an uncancelled, payload-free call sees neither Cancel nor GetResult", f.name);
    size = 1;
    checkf(f.async_get_result_size(&block, &size) == S_OK && size == 0,
           "%s: XAsyncGetResultSize after completion is S_OK with a 0-byte payload", f.name);
    f.close(queue);
}

// The completion routine runs on the queue's completion port: with a Manual completion port it runs only when that
// port is dispatched, and the call's status is already terminal before it runs (XAsync.h documents both).
void scenario_completion_port(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for completion delivery", f.name))
        return;
    CompletionObservation completion;
    completion.facade = &f;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &completion;
    block.callback = completion_callback;
    g_work_runs = 0;
    checkf(f.async_run(&block, run_work) == S_OK && g_work_runs.load() == 0 && completion.calls.load() == 0,
           "%s: XAsyncRun on a Manual work port has run neither the work nor the completion routine", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && g_work_runs.load() == 1,
           "%s: dispatching the work port runs the async work on the dispatching thread", f.name);
    checkf(f.async_get_status(&block, false) == S_OK,
           "%s: the call's status is terminal once the work has finished", f.name);
    checkf(completion.calls.load() == 0,
           "%s: the completion routine is queued to the completion port and has not run yet", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Completion, 0) && completion.calls.load() == 1,
           "%s: dispatching the completion port invokes the completion routine exactly once", f.name);
    checkf(completion.thread.load() == GetCurrentThreadId(),
           "%s: the completion routine runs on the thread that dispatched the completion port", f.name);
    checkf(completion.status_inside.load() == S_OK,
           "%s: inside the completion routine XAsyncGetStatus reports the terminal result", f.name);
    checkf(!f.dispatch(queue, XTaskQueuePort::Completion, 0),
           "%s: no second completion delivery is queued", f.name);
    f.close(queue);

    // A ThreadPool completion port delivers the same routine on a pool thread with no dispatch at all.
    XTaskQueueHandle pool_completion = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::ThreadPool, &pool_completion) ==
                        S_OK &&
                    pool_completion != nullptr,
                "%s: a Manual/ThreadPool queue", f.name))
        return;
    CompletionObservation pooled;
    pooled.facade = &f;
    XAsyncBlock pooled_block{};
    pooled_block.queue = pool_completion;
    pooled_block.context = &pooled;
    pooled_block.callback = completion_callback;
    g_work_runs = 0;
    checkf(f.async_run(&pooled_block, run_work) == S_OK && f.dispatch(pool_completion, XTaskQueuePort::Work, 0),
           "%s: the pooled-completion call's work was dispatched", f.name);
    checkf(wait_until([&] { return pooled.calls.load() == 1; }, 5000),
           "%s: a ThreadPool completion port runs the completion routine without a dispatch call", f.name);
    checkf(pooled.thread.load() != GetCurrentThreadId(),
           "%s: the ThreadPool completion routine runs on a pool thread", f.name);
    checkf(pooled.status_inside.load() == S_OK,
           "%s: the pooled completion routine sees the terminal status", f.name);
    f.close(pool_completion);
}

// Completion ordering: with an Immediate work port the calls complete in submission order and a Manual completion
// port delivers their routines in that same order.
void scenario_completion_order(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Immediate, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: an Immediate/Manual queue for ordering", f.name))
        return;
    OrderObservation observation;
    OrderBlock blocks[3];
    XAsyncBlock async_blocks[3] = {};
    bool started = true;
    for (int i = 0; i < 3; ++i) {
        blocks[i].observation = &observation;
        blocks[i].index = i;
        async_blocks[i].queue = queue;
        async_blocks[i].context = &blocks[i];
        async_blocks[i].callback = order_callback;
        started = started && f.async_run(&async_blocks[i], run_work) == S_OK;
    }
    checkf(started, "%s: three async calls submitted in order on the Immediate work port", f.name);
    checkf(observation.order.empty(),
           "%s: their completion routines are queued, not run during the submits", f.name);
    int dispatched = 0;
    while (f.dispatch(queue, XTaskQueuePort::Completion, 0)) ++dispatched;
    checkf(dispatched == 3 && observation.order.size() == 3,
           "%s: dispatching the completion port delivered all three completion routines", f.name);
    checkf(observation.order == std::vector<int>({0, 1, 2}),
           "%s: the completion routines ran in submission order", f.name);
    f.close(queue);
}

// Cancellation: XAsyncCancel reaches the provider with XAsyncOp::Cancel; a provider completing with E_ABORT in that
// op makes the call's status E_ABORT, invokes the completion routine with that status, suppresses the queued DoWork
// and still cleans the provider up once.
void scenario_cancellation(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for cancellation", f.name))
        return;
    CompletionObservation completion;
    completion.facade = &f;
    CancelProvider provider;
    provider.facade = &f;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &completion;
    block.callback = completion_callback;
    provider.block = &block;
    const bool begun = f.async_begin(&block, &provider, nullptr, "task_queue_parity_test", cancel_provider) == S_OK &&
                       provider.begins.load() == 1 &&
                       f.async_schedule(&block, 0) == S_OK;
    checkf(begun, "%s: a cancellable call is begun and its work queued", f.name);
    if (!begun) {
        f.close(queue);
        return;
    }
    f.async_cancel(&block);
    checkf(provider.cancels.load() == 1,
           "%s: XAsyncCancel delivers XAsyncOp::Cancel to the provider exactly once", f.name);
    checkf(f.async_get_status(&block, false) == E_ABORT,
           "%s: a provider completing with E_ABORT in Cancel makes the status E_ABORT", f.name);
    checkf(completion.calls.load() == 0,
           "%s: the completion routine for the canceled call is queued, not run yet", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Completion, 0) && completion.calls.load() == 1,
           "%s: dispatching the completion port invokes the canceled call's completion routine once", f.name);
    checkf(completion.status_inside.load() == E_ABORT,
           "%s: inside that completion routine the status is E_ABORT", f.name);
    // The queued WorkerCallback entry still exists; draining it must not run DoWork (the call is over) and must
    // release the last state reference so the provider cleanup happens exactly once.
    while (f.dispatch(queue, XTaskQueuePort::Work, 0)) {}
    checkf(provider.works.load() == 0,
           "%s: the canceled call's DoWork never ran", f.name);
    checkf(provider.cleanups.load() == 1,
           "%s: the canceled call's provider is cleaned up exactly once (count %d)", f.name,
           provider.cleanups.load());

    // The vendored library only delivers XAsyncOp::Cancel and leaves the E_ABORT completion to the provider
    // (XAsyncProvider.h documents that responsibility). A provider that ignores Cancel therefore leaves the call
    // E_PENDING; the two peers named in the docs complete the call with E_ABORT themselves, which is the measured
    // cross-check difference this expectation records.
    PendingProvider ignoring;
    XAsyncBlock ignored_block{};
    ignored_block.queue = queue;
    ignored_block.context = &ignoring;
    const bool ignored_begun =
        f.async_begin(&ignored_block, &ignoring, nullptr, "task_queue_parity_test", pending_provider) == S_OK &&
        f.async_schedule(&ignored_block, 0) == S_OK;
    checkf(ignored_begun, "%s: a second, cancellable call is begun", f.name);
    if (ignored_begun) {
        f.async_cancel(&ignored_block);
        checkf(ignoring.cancels.load() == 1 && f.async_get_status(&ignored_block, false) == E_PENDING,
               "%s: a provider that ignores XAsyncOp::Cancel leaves the call E_PENDING (the library only delivers "
               "the op)", f.name);
        f.async_complete(&ignored_block, E_ABORT, 0);
        while (f.dispatch(queue, XTaskQueuePort::Work, 0)) {}
        checkf(ignoring.works.load() == 0 && ignoring.cleanups.load() == 1,
               "%s: that call's queued work still never runs and its provider is cleaned up exactly once", f.name);
    }
    f.close(queue);
}

// XAsyncBlock lifetime: the same block may not be reused while a call is in flight, and may be reused once the
// completion routine has returned (XAsync.h documents both directions).
void scenario_block_reuse(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for block reuse", f.name))
        return;
    CompletionObservation completion;
    completion.facade = &f;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &completion;
    block.callback = completion_callback;
    g_work_runs = 0;
    checkf(f.async_run(&block, run_work) == S_OK && g_work_runs.load() == 0,
           "%s: the block starts the first async call", f.name);
    checkf(f.async_run(&block, run_work) == E_INVALIDARG,
           "%s: reusing the block while its call is in flight returns E_INVALIDARG", f.name);
    f.dispatch(queue, XTaskQueuePort::Work, 0);
    f.dispatch(queue, XTaskQueuePort::Completion, 0);
    checkf(g_work_runs.load() == 1 && completion.calls.load() == 1,
           "%s: the first call ran once and its completion routine ran once", f.name);
    checkf(f.async_run(&block, run_work) == S_OK,
           "%s: the same block is accepted again after the completion routine returned", f.name);
    f.dispatch(queue, XTaskQueuePort::Work, 0);
    f.dispatch(queue, XTaskQueuePort::Completion, 0);
    checkf(g_work_runs.load() == 2 && completion.calls.load() == 2,
           "%s: the reused block ran the second call and its completion routine once more", f.name);
    f.close(queue);
}

// One provider cleanup and one result retrieval per call: a payload call is cleaned up exactly once, and
// XAsyncGetResult hands the payload out once.
void scenario_result_cleanup(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for the result contract", f.name))
        return;
    PayloadProvider provider;
    provider.facade = &f;
    static const int kIdentity = 0;
    XAsyncBlock block{};
    block.queue = queue;
    block.context = &provider;
    const bool begun = f.async_begin(&block, &provider, &kIdentity, "task_queue_parity_test", payload_provider) ==
                           S_OK &&
                       provider.begins.load() == 1 && f.async_schedule(&block, 0) == S_OK;
    checkf(begun, "%s: a payload call is begun and its work queued", f.name);
    if (!begun) {
        f.close(queue);
        return;
    }
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && provider.works.load() == 1,
           "%s: the payload call's DoWork ran once", f.name);
    checkf(f.async_get_status(&block, false) == S_OK, "%s: the payload call's status is S_OK", f.name);
    size_t size = 0;
    checkf(f.async_get_result_size(&block, &size) == S_OK && size == sizeof(std::uint32_t),
           "%s: XAsyncGetResultSize reports the 4-byte payload", f.name);
    std::uint32_t value = 0;
    size_t used = 0;
    checkf(f.async_get_result(&block, &kIdentity, sizeof(value), &value, &used) == S_OK &&
               value == 0x1EA0 && used == sizeof(value),
           "%s: XAsyncGetResult returns the provider's payload and its used size", f.name);
    checkf(f.async_get_result(&block, &kIdentity, sizeof(value), &value, &used) == E_ILLEGAL_METHOD_CALL,
           "%s: a second XAsyncGetResult is refused with E_ILLEGAL_METHOD_CALL", f.name);
    checkf(provider.get_results.load() == 1 && provider.cleanups.load() == 1,
           "%s: the payload provider saw GetResult once and Cleanup exactly once (GetResult %d, Cleanup %d)", f.name,
           provider.get_results.load(), provider.cleanups.load());
    f.close(queue);
}

// Waiter (slot 20) and monitor (slot 23): a signaled wait handle queues its callback on the requested port; a
// monitor sees every submission with the right port; unregistering either stops further delivery.
void scenario_waiter_monitor(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/Manual queue for the waiter/monitor contract", f.name))
        return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!checkf(event != nullptr, "%s: the waiter's wait handle is created", f.name)) {
        f.close(queue);
        return;
    }
    MonitorObservation monitor;
    XTaskQueueRegistrationToken monitor_token{};
    checkf(f.register_monitor(queue, &monitor, monitor_callback, &monitor_token) == S_OK &&
               monitor_token.token != 0 && monitor.calls.load() == 0,
           "%s: XTaskQueueRegisterMonitor returns a token without invoking the monitor", f.name);

    ThreadObservation submitted;
    checkf(f.submit(queue, XTaskQueuePort::Work, &submitted, counter_callback) == S_OK &&
               monitor.calls.load() == 1 && monitor.work.load() == 1,
           "%s: submitting a work callback invokes the monitor synchronously with the work port", f.name);
    // Drain that directly submitted callback first so the waiter's entry is the only entry the next dispatch sees.
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && submitted.calls.load() == 1,
           "%s: the directly submitted work callback runs once", f.name);

    WaiterObservation waiter;
    XTaskQueueRegistrationToken waiter_token{};
    checkf(f.register_waiter(queue, XTaskQueuePort::Work, event, &waiter, waiter_callback, &waiter_token) == S_OK &&
               waiter_token.token != 0,
           "%s: XTaskQueueRegisterWaiter returns a token", f.name);
    checkf(SetEvent(event) != FALSE, "%s: the wait handle is signaled", f.name);
    checkf(wait_until([&] { return monitor.calls.load() == 2; }, 5000),
           "%s: the signaled waiter queues one entry, which the monitor sees on the work port", f.name);
    checkf(wait_until([&] { return monitor.work.load() == 2; }, 5000),
           "%s: the monitor reported the waiter's entry as a work-port submission", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 2000) && waiter.calls.load() == 1,
           "%s: dispatching the work port runs the waiter's callback once", f.name);
    checkf(!waiter.canceled.load() && waiter.thread.load() == GetCurrentThreadId(),
           "%s: the waiter's callback runs uncanceled on the dispatching thread", f.name);
    f.unregister_waiter(queue, waiter_token);
    if (monitor.calls.load() == 2) {
        checkf(SetEvent(event) != FALSE, "%s: the wait handle is signaled again after unregister", f.name);
        checkf(!f.dispatch(queue, XTaskQueuePort::Work, 200) && waiter.calls.load() == 1,
               "%s: an unregistered waiter delivers no callback", f.name);
        checkf(monitor.calls.load() == 2, "%s: and no monitor submission either", f.name);
    } else {
        checkf(false, "%s: the monitor/submission counts before unregister are not as expected", f.name);
    }

    ThreadObservation work_cleanup;
    checkf(f.submit(queue, XTaskQueuePort::Work, &work_cleanup, counter_callback) == S_OK &&
               f.dispatch(queue, XTaskQueuePort::Work, 0) && work_cleanup.calls.load() == 1,
           "%s: a directly submitted work callback still runs", f.name);
    f.unregister_monitor(queue, monitor_token);
    const int monitor_before = monitor.calls.load();
    ThreadObservation after_monitor;
    checkf(f.submit(queue, XTaskQueuePort::Work, &after_monitor, counter_callback) == S_OK &&
               monitor.calls.load() == monitor_before,
           "%s: an unregistered monitor is not invoked by a later submission", f.name);
    f.dispatch(queue, XTaskQueuePort::Work, 0);
    f.close(queue);
    CloseHandle(event);
}

// Termination cancels queued callbacks (the callback sees canceled=true), invokes the termination callback, and
// refuses new submissions with E_ABORT.
void scenario_termination(const Facade& f) {
    XTaskQueueHandle queue = nullptr;
    if (!checkf(f.create(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::ThreadPool, &queue) == S_OK &&
                    queue != nullptr,
                "%s: a Manual/ThreadPool queue for termination", f.name))
        return;
    ThreadObservation pending;
    TerminationObservation terminated;
    const HRESULT submit = f.submit(queue, XTaskQueuePort::Work, &pending, counter_callback);
    checkf(submit == S_OK && pending.calls.load() == 0, "%s: a work callback is queued before termination", f.name);
    const HRESULT terminate = f.terminate(queue, false, &terminated, terminated_callback);
    checkf(terminate == S_OK, "%s: XTaskQueueTerminate(wait=false) is accepted", f.name);
    checkf(f.dispatch(queue, XTaskQueuePort::Work, 0) && pending.calls.load() == 1,
           "%s: the queued work callback is dispatched once during termination", f.name);
    checkf(pending.canceled.load(),
           "%s: the queued callback is delivered with canceled=true", f.name);
    checkf(wait_until([&] { return terminated.calls.load() == 1; }, 5000),
           "%s: the termination callback is invoked after the canceled work", f.name);
    ThreadObservation refused;
    checkf(f.submit(queue, XTaskQueuePort::Work, &refused, counter_callback) == E_ABORT,
           "%s: submitting to a terminated queue returns E_ABORT", f.name);
    f.close(queue);
}

// The default process queue exists and is a ThreadPool/ThreadPool queue: an XAsyncBlock with a null queue completes
// without any dispatch call.
void scenario_process_queue(const Facade& f) {
    XTaskQueueHandle process_queue = nullptr;
    if (!checkf(f.get_process_queue(&process_queue) && process_queue != nullptr,
                "%s: XTaskQueueGetCurrentProcessTaskQueue returns the default process queue", f.name))
        return;
    XAsyncBlock block{};  // a null queue selects the process queue
    g_work_runs = 0;
    const HRESULT run = f.async_run(&block, run_work);
    checkf(run == S_OK, "%s: XAsyncRun with a null queue starts on the process queue", f.name);
    checkf(f.async_get_status(&block, true) == S_OK && g_work_runs.load() == 1,
           "%s: a null-queue call completes without a dispatch call (the default queue is thread-pool backed)", f.name);
    checkf(g_work_thread.load() != GetCurrentThreadId(),
           "%s: the null-queue work runs on a pool thread", f.name);
    f.close(process_queue);
}

// The named expectations where the shim does not forward to a vendored function. They are deliberate and recorded
// here so a future port changes this test on purpose instead of silently:
//  * slot 8 (XAsyncBeginAlloc) is private, has no measured caller and the shim refuses it with E_NOTIMPL, while the
//    vendored library does export and serve it (docs/task_queue_parity.md section 5 already retired XAsyncBeginAlloc).
//  * slots 27-30 (XThreadSetTimeSensitive / XThreadIsTimeSensitive / XThreadAssertNotTimeSensitive and the private
//    slot 28) are the shim's own per-thread code, not vendored functions; their parity contract is the shim's flag
//    semantics, which are checked here as such.
void scenario_expected_gaps(void* threading) {
    // The vendored library's private test-harness begin (vendor/xtaskqueue/Source/Task/XAsyncProviderPriv.h).
    // XAsyncBeginAlloc hands the provider the context block it allocated (data->context), not a caller pointer, so
    // this provider counts through file-scope state instead of the context argument.
    static std::atomic<int> s_private_begins{0};
    static std::atomic<int> s_private_cleanups{0};
    auto provider = [](XAsyncOp op, const XAsyncProviderData*) -> HRESULT {
        if (op == XAsyncOp::Begin) ++s_private_begins;
        if (op == XAsyncOp::Cleanup) ++s_private_cleanups;
        return S_OK;
    };
    using BeginAllocSlot = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, const void*, const char*, XAsyncProvider*,
                                                       size_t, size_t, void*);
    XAsyncBlock refused_block{};
    const HRESULT refused =
        reinterpret_cast<BeginAllocSlot>(slot_function(threading, 8))(threading, &refused_block, nullptr,
                                                                      "task_queue_parity", provider, 16, 0, nullptr);
    checkf(refused == E_NOTIMPL,
           "expected gap: the shim's IXThreadingImpl slot 8 (XAsyncBeginAlloc) refuses with E_NOTIMPL");
    XAsyncBlock served_block{};
    const HRESULT served =
        XAsyncBeginAlloc(&served_block, nullptr, "task_queue_parity", provider, 16, 0, nullptr);
    checkf(served == S_OK && s_private_begins.load() == 1,
           "expected gap: the vendored library itself serves XAsyncBeginAlloc (so slot 8 is a shim choice, not a gap "
           "in the queue)");
    XAsyncComplete(&served_block, S_OK, 0);
    checkf(s_private_cleanups.load() == 1,
           "expected gap: the vendored XAsyncBeginAlloc call still cleans the provider up exactly once");

    // The shim-owned per-thread slots: the contract is the flag, and the vendored queue has no counterpart.
    using SetSensitive = HRESULT(STDMETHODCALLTYPE*)(void*, bool);
    using IsSensitive = bool(STDMETHODCALLTYPE*)(void*);
    using AssertNotSensitive = void(STDMETHODCALLTYPE*)(void*);
    const bool set = reinterpret_cast<SetSensitive>(slot_function(threading, 27))(threading, true) == S_OK &&
                     reinterpret_cast<IsSensitive>(slot_function(threading, 30))(threading);
    reinterpret_cast<AssertNotSensitive>(slot_function(threading, 29))(threading);
    const bool assert_leaves_flag = reinterpret_cast<IsSensitive>(slot_function(threading, 30))(threading);
    const bool cleared = reinterpret_cast<SetSensitive>(slot_function(threading, 27))(threading, false) == S_OK &&
                         !reinterpret_cast<IsSensitive>(slot_function(threading, 30))(threading);
    checkf(set && assert_leaves_flag && cleared,
           "expected gap: slots 27-30 are the shim's own per-thread time-sensitive flag (no vendored counterpart); "
           "the assert slot leaves the flag as the set slot left it");
}

// Every task-queue/XAsync name the shim registers for xgameruntime.dll is the vendored function itself, not a
// wrapper: the named-export path and the vtable-slot path reach the same code. This is the one check the two faces
// above cannot make on their own, because a slot has a forwarding wrapper by construction.
void check_named_exports() {
    struct Export {
        const char* name;
        void* vendored;
    };
    const Export exports[] = {
        {"XTaskQueueCreate", reinterpret_cast<void*>(&XTaskQueueCreate)},
        {"XTaskQueueCreateComposite", reinterpret_cast<void*>(&XTaskQueueCreateComposite)},
        {"XTaskQueueGetPort", reinterpret_cast<void*>(&XTaskQueueGetPort)},
        {"XTaskQueueDuplicateHandle", reinterpret_cast<void*>(&XTaskQueueDuplicateHandle)},
        {"XTaskQueueCloseHandle", reinterpret_cast<void*>(&XTaskQueueCloseHandle)},
        {"XTaskQueueTerminate", reinterpret_cast<void*>(&XTaskQueueTerminate)},
        {"XTaskQueueDispatch", reinterpret_cast<void*>(&XTaskQueueDispatch)},
        {"XTaskQueueSubmitCallback", reinterpret_cast<void*>(&XTaskQueueSubmitCallback)},
        {"XTaskQueueSubmitDelayedCallback", reinterpret_cast<void*>(&XTaskQueueSubmitDelayedCallback)},
        {"XTaskQueueRegisterWaiter", reinterpret_cast<void*>(&XTaskQueueRegisterWaiter)},
        {"XTaskQueueUnregisterWaiter", reinterpret_cast<void*>(&XTaskQueueUnregisterWaiter)},
        {"XTaskQueueRegisterMonitor", reinterpret_cast<void*>(&XTaskQueueRegisterMonitor)},
        {"XTaskQueueUnregisterMonitor", reinterpret_cast<void*>(&XTaskQueueUnregisterMonitor)},
        {"XTaskQueueGetCurrentProcessTaskQueue", reinterpret_cast<void*>(&XTaskQueueGetCurrentProcessTaskQueue)},
        {"XTaskQueueSetCurrentProcessTaskQueue", reinterpret_cast<void*>(&XTaskQueueSetCurrentProcessTaskQueue)},
        {"XAsyncGetStatus", reinterpret_cast<void*>(&XAsyncGetStatus)},
        {"XAsyncGetResultSize", reinterpret_cast<void*>(&XAsyncGetResultSize)},
        {"XAsyncCancel", reinterpret_cast<void*>(&XAsyncCancel)},
        {"XAsyncRun", reinterpret_cast<void*>(&XAsyncRun)},
        {"XAsyncBegin", reinterpret_cast<void*>(&XAsyncBegin)},
        {"XAsyncSchedule", reinterpret_cast<void*>(&XAsyncSchedule)},
        {"XAsyncComplete", reinterpret_cast<void*>(&XAsyncComplete)},
        {"XAsyncGetResult", reinterpret_cast<void*>(&XAsyncGetResult)},
    };
    int resolved = 0;
    for (const Export& entry : exports) {
        const void* shim = rt::shims::find("xgameruntime.dll", entry.name);
        if (checkf(shim != nullptr, "named export: xgameruntime.dll serves %s", entry.name)) ++resolved;
        checkf(shim == entry.vendored,
               "named export: xgameruntime.dll's %s is the vendored function itself", entry.name);
    }
    checkf(resolved == static_cast<int>(sizeof(exports) / sizeof(exports[0])),
           "named export: every task-queue/XAsync name the shim declares resolves");
}

}  // namespace

int main() {
    std::printf("task_queue_parity_test: the IXThreadingImpl task-queue/XAsync contract through the shim and the "
                "vendored queue\n");

    check_named_exports();

    void* threading = threading_object();
    if (!checkf(threading != nullptr, "QueryApiImpl hands out IXThreadingImpl for its measured class id")) {
        std::printf("task_queue_parity_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    const Facade shim = shim_face(threading);
    const Facade vendored = vendored_face();

    for (const Facade* face : {&shim, &vendored}) {
        scenario_dispatch_modes(*face);
        scenario_manual_dispatch(*face);
        scenario_immediate(*face);
        scenario_thread_pool(*face);
        scenario_serialized_thread_pool(*face);
        scenario_delayed(*face);
        scenario_async_e_pending(*face);
        scenario_completion_port(*face);
        scenario_completion_order(*face);
        scenario_cancellation(*face);
        scenario_block_reuse(*face);
        scenario_result_cleanup(*face);
        scenario_waiter_monitor(*face);
        scenario_termination(*face);
        scenario_process_queue(*face);
    }
    scenario_expected_gaps(threading);
    release_object(threading);

    if (g_failures) {
        std::printf("task_queue_parity_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("task_queue_parity_test: OK (%d checks)\n", g_checks);
    return 0;
}
