# Console task-queue and threading parity against the job-system stall: measured

**Status: measured negative. The runtime's task-queue/threading surface is not the stalled job's dependency.** One
130 s boot with a new per-slot trace of `XThreadingImpl` records the complete task-queue/threading call surface the
retail executable makes: **26 class requests / slot calls (the bounded `QueryApiImpl` trace prints 9 lines of them), all
of them queue setup** - `XTaskQueueCreate` with modes
`ThreadPool`/`ThreadPool` twice from the title and `Immediate`/`Immediate` once from `libHttpClient.GDK.dll`, the
default process queue, three times duplicate -> Work port -> composite -> close, and ten
`XThreadAssertNotTimeSensitive` - plus **zero** calls to every submit, dispatch, wait, monitor, terminate and `XAsync`
slot. No callback or waiter exists that could be the missing wake and no dispatch is outstanding. The stall measured in
the same boot is the one [stage2_blocker.md](stage2_blocker.md) section 15 names: job 109's runner is inside a title-side
`SleepEx(1)` poll on a completion counter of 13 task-pool items **it posted itself**, while the pool's workers are inside
our synchronous compute-PSO creation. A task-queue parity divergence cannot keep a job from returning here, so this
slice makes **no behaviour change**; it records the call table, the comparison, the tests and the negative.

## 1. Why this surface, and what was measured

The console runtime keeps a title's asynchronous work moving: [XCL](https://github.com/xodus-gaming/xcl)'s feature table
lists "kernelx - threads on the PC's processors" and "task queues, async ... through the PC's XGameRuntime",
[VIctory](https://github.com/Marvisak/VIctory) serves XTaskQueue itself, and [five-star](https://github.com/ThatRecompDev/five-star-recomp)
states its offline GDK gives "real queues and async calls with the console's ordering" while the game boots through its
first-run screens. This port serves `XThreadingImpl` from `vendor/xtaskqueue` (Microsoft's MIT-licensed libHttpClient
implementation, commit `c5007fd9cbdc785518ea6dccc5751b125d451cc1`) through `QueryApiImpl`. The open question for this
slice was whether the job-system stall - a running stage job whose runner never returns, pending counter frozen - is a
task-queue dispatch or wake the console provides and this port does not.

The title does not import `xgameruntime.dll`: its 161 `xgameruntime.lib` thunks call a shared helper
(`game+0x55DE08C`) that asks `QueryApiImpl` for the class **on every call**, then calls one vtable slot and releases the
object ([runtime_imports.md](runtime_imports.md)). The existing `PALMETTO_USER_TRACE` line therefore names the class but
not which of the 28 methods ran. `port/shims/xgameruntime.cpp` now writes the served slots its own wrappers with one
extra thing: under `PALMETTO_USER_TRACE` each `XThreadingImpl` call prints its **slot, method name, calling thread id,
caller RVA, up to five scalar arguments, and its answer on return**. The trace is bounded per slot like the other served
slots (the first eight calls, then the tenth, hundredth, ...), so a polled slot cannot flood the log; a call that blocks
would show an entry line with no return line. Nothing is read, written or returned differently with the switch unset;
the wrapper only tests the cached `platform_trace_enabled()`. The job 109 state is read by the existing opt-in
`PALMETTO_STAGE2_READY` probe, and the same boot's thread state by the existing `PALMETTO_WAIT_SAMPLE` and
`PALMETTO_THREAD_STACKS` samplers. No behaviour switch is added.

## 2. The measured call table

Boot: `D:\leonida` working tree at `31a1e37ab` with other slices' uncommitted files present (see section 7), binary
`port\build\palmetto.exe` SHA-256 `683FF9531E52FCE3E3D14DF17A01B506E867280FC46875D3CD375008B8C76400` (mtime 18:48:28),
`tooling/boot_report.py --timeout 130`, capture outside the repository. Environment: the standard set
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_STATE_TRACE=2,130`) plus `PALMETTO_USER_TRACE=1`, `PALMETTO_WAIT_SAMPLE=25,40,50,70,100`,
`PALMETTO_THREAD_STACKS=40,50,70,100` and `PALMETTO_STAGE2_READY=20`. Slot numbers are
[tooling/gdk_runtime_classes.json](../tooling/gdk_runtime_classes.json)'s layout for `IXThreadingImpl`; the caller RVA
is where the slot was called from (the title's thunk, or `libHttpClient.GDK.dll` for one call).

| slot | method | calls | measured arguments | answer | thread(s) | when (s) |
|---:|---|---:|---|---|---|---|
| 12 | `XTaskQueueCreate` | 3 | `(1, 1, out)` twice - `ThreadPool`/`ThreadPool` from the title (`game+0x55DD7F4`); `(3, 3, out)` once - `Immediate`/`Immediate` from `libHttpClient.GDK.dll+0x6BE10` | `S_OK` + handle, each | 72952; 25172 (twice) | +1.11; +25.51 |
| 25 | `XTaskQueueGetCurrentProcessTaskQueue` | 1 | `(out)` (`game+0x55DDC3A`) | `true` + handle | 25172 | +25.51 |
| 15 | `XTaskQueueDuplicateHandle` | 3 | `(queue, out)` (`game+0x55DD98B`) | `S_OK` | 25172 | +25.51 |
| 14 | `XTaskQueueGetPort` | 3 | `(queue, 0 = Work, out)` (`game+0x55DD90C`) | `S_OK` | 25172 | +25.51 |
| 13 | `XTaskQueueCreateComposite` | 3 | `(port, same port, out)` (`game+0x55DD883`); the same value `0x1D4A4014330` for both ports in all three | `S_OK` | 25172 | +25.51 |
| 17 | `XTaskQueueCloseHandle` | 3 | `(duplicate)` (`game+0x55DDA7F`) | void | 25172 | +25.51 |
| 29 | `XThreadAssertNotTimeSensitive` | 10 | none (`game+0x55DE02F`) | void; the calling thread's flag stays clear | 72952, 52920, 10492, 74500, 25172 | +1.11 - +2.84, +22.88, +25.17, +25.51 |

**Every other slot of the class was never called:** `XAsyncGetStatus`, `XAsyncGetResultSize`, `XAsyncCancel`,
`XAsyncRun`, `XAsyncBegin`, `XAsyncSchedule`, `XAsyncComplete`, `XAsyncGetResult` (slots 3-11), the private
`XAsyncBeginAlloc`/`XTaskQueueIsEmpty` (slots 8 and 28), `XTaskQueueDispatch` (16), `XTaskQueueSubmitCallback` (18),
`XTaskQueueSubmitDelayedCallback` (19), `XTaskQueueRegisterWaiter`/`UnregisterWaiter` (20/21), `XTaskQueueTerminate` (22),
`XTaskQueueRegisterMonitor`/`UnregisterMonitor` (23/24), `XTaskQueueSetCurrentProcessTaskQueue` (26),
`XThreadSetTimeSensitive` (27) and `XThreadIsTimeSensitive` (30). The class count is 26 slot calls over the whole
130.4 s, and **the last one is at +25.51 s**, 4.7 s before the pending counter's steady sample: for the remaining 105 s
of the boot the runtime's entire task-queue/threading surface receives **no call at all**.

The `QueryApiImpl` trace's own bounded form corroborates the count: the class is requested 26 times but only 9 lines are
printed (`#1`-`#8`, `#10`), which is exactly the trace's 1, 2, ..., 8, 10, 100, ... schedule. The other classes asked
for in the same boot are the known platform set ([runtime_imports.md](runtime_imports.md)); one of them is discussed in
section 3.

The vendor functions the slots reach are the ones this shim has always used: slots 12/13/14/15/17/25 forward to
`XTaskQueueCreate`, `XTaskQueueCreateComposite`, `XTaskQueueGetPort`, `XTaskQueueDuplicateHandle`,
`XTaskQueueCloseHandle` and `XTaskQueueGetCurrentProcessTaskQueue` in `vendor/xtaskqueue`; slot 29 is this file's
per-thread flag setter. Every answer is the documented one for that call: a queue handle, the default process queue
which the GDK documents as existing by default with the thread pool for both ports, `S_OK` from duplicate, get-port and
composite, and a void assert. **No answer in the table is a failure and none is outstanding.** The composite calls are
worth naming because the trace shows them: the title passes **the same port value for the work and the completion
argument** (`a0 == a1 == 0x1D4A4014330` in all three), which the GDK permits (a composite duplicates the handles of the
queues that own the ports) and the vendored implementation accepts.

## 3. The stall in the same boot, and the one runtime call still live during it

The state word and the ready probe moved as in [stage2_blocker.md](stage2_blocker.md) sections 14-15: `init 1` at 4.0 s,
`init 2` and `loading 1` at 22.0 s, machine `current 2 / next 2` to the kill at 130.4 s, and no stage-3 request. The
`PALMETTO_STAGE2_READY` samples in **this** boot:

| sample | tracker `+0xB0` pending | ready bitmask | holder |
|---|---|---|---|
| `wait` +24.104 s | `0x7B` (123) of 249 | 1 id (`152`) | graph still executing |
| `steady` +30.182 s | `0x69` (105) | 1 id (`152`) | id `109`, hash `0xA599B5CB`, item executor `image+0x4EAA040`, lane 0 class 0, `e0 0 f0 4` |
| `steady2` +74.235 s | `0x69` (105) | 1 id (`152`) | id `109`, unchanged |

`f0 4` is the generic runner's "inside the work function" state, and the pending counter is frozen from +30.2 s to at
least +74.2 s. Section 15 of [stage2_blocker.md](stage2_blocker.md) - the sibling slice's cdb all-thread dumps on the
same tree - names the wait precisely: job 109's runner is in `SleepEx(1)` inside `image+0x1DEE6C0`, polling a
completion counter that the executor bodies of **13 task-pool items the same stage function posted into the title's
lane-0/class-4 ring** increment; at +55 s and +95 s worker threads of another title batch are inside our synchronous
compute-PSO creation (`rt::d3d12_xs::proxy_slot<62>` -> `Device::CreateComputePipelineState` -> `nvgpucomp64`) and the
13 items run in no thread. This boot's debugger-free samplers see the same shape: at 40/50/70/100 s, 3-5 threads have
their instruction pointer inside `nvgpucomp64` (busy in the compiler, not in a wait API) out of 91-93 threads, 67-70 of
which are in a wait/busy title site. **The wait job 109 is in is title memory (the posted items' counter), not a
runtime-provided event, queue or fence**, and section 15 rules out a runtime answer for it.

What the platform trace adds, and what it does *not*: after +25.5 s the only platform class the title keeps calling is
`XNetworkingImpl` - one request at +1.11 s, then `#100` at +26.31 s, `#1000` at +33.77 s and `#10000` at +108.43 s, i.e.
a title thread polling a networking answer ~120 times a second for the rest of the boot (the shim's answers are the
offline "no connectivity" ones, [runtime_imports.md](runtime_imports.md)). Another title thread makes 6
`XUserImpl` requests at +114.97 s. That is the title's own online/session plumbing being polled on a thread that is not
the frozen job; it is **not** job 109's wait (section 15 names that as the task-pool counter poll) and it is **not** a
task-queue call - the whole `XThreadingImpl` surface is idle after +25.51 s. It is recorded here as the frontier fact it
is: a platform poll still running during the freeze, whose owner thread and networking slot are not identified by this
slice (section 6, next falsifier).

## 4. The parity comparison: GDK documentation, the vendored implementation, the peers

**GDK documentation** (Microsoft, [XTaskQueueDispatchMode](https://learn.microsoft.com/gaming/gdk/docs/reference/system/xtaskqueue/enums/xtaskqueuedispatchmode),
[XTaskQueueSetCurrentProcessTaskQueue](https://learn.microsoft.com/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueuesetcurrentprocesstaskqueue),
[Using the process task queue](https://devdocs.xbox.com/build/core-features/common/async/async-task-queue-design-howto/using-process-task-queue),
[XThread](https://learn.microsoft.com/gaming/gdk/docs/reference/system/xthread/xthread_members)): a queue has a work
and a completion port; `Manual` callbacks are dispatched only by `XTaskQueueDispatch`, `ThreadPool` ones are queued and
dispatched automatically on the system thread queue, `SerializedThreadPool` ones one at a time on that queue, and
`Immediate` ones on the submitting thread; a default process task queue using the thread pool for both ports exists
unless replaced, and every API that takes a queue accepts `nullptr` for it; `XThreadSetTimeSensitive` marks a thread
time-sensitive and `XThreadIsTimeSensitive` reads the mark.

**The vendored implementation** is Microsoft's own libHttpClient task-queue code at the commit above, which is the same
code family the GDK's runtime implements; its dispatch-mode model, default process queue (lazily created
`ThreadPool`/`ThreadPool`, as the public header comment documents) and `Immediate` drain match those pages, and this
slice's tests replay and check the measured shapes (section 5). The one implementation difference worth naming is the
pool itself: the vendor uses the Windows system thread pool (as XCL does, because XCL delegates task queues to the PC
GDK runtime), while VIctory's and five-star's own implementations run callbacks on a pool they own and grow on demand
("a new one starts whenever more tasks wait than workers idle, so a callback that blocks on another never starves it" -
VIctory, `crates/victory-hle/src/modules/xasync.rs`, commit `13f91ca`; five-star's `TaskPort` has the same four-mode
model and a lazily created `ThreadPool`/`ThreadPool` process queue, commit `0bfa755`). That difference affects callbacks
blocking inside other callbacks. **This title never submits one callback, so none of it is exercised by this boot and
none of it can be the stalled job's dependency.**

**Divergence found in the surface the title uses: none.** All 26 measured calls are answered with the documented
success values, the modes the title and libHttpClient ask for are accepted, the process queue the GDK promises is
present, and the thread-sensitivity call is a no-op that leaves the flag clear (it is an assert in the GDK's debug
builds; the flag semantics of slots 27/30 are already covered by `xgameruntime_test`). A dispatch/wake cannot be
missing because **the title queued no work at all**: no submit, no dispatch, no waiter, no monitor, no terminate and no
`XAsync` call exist in the boot, and the surface is silent from +25.51 s to the kill.

## 5. What was implemented

- **The measurement, in `port/shims/xgameruntime.cpp`**: the `XThreadingImpl` vtable now enters through two small
  forwarding templates - `SlotMethod` for the vendor functions (the vtable entry supplies `this`) and `SelfMethod` for
  the file's own per-thread functions (which take `this` themselves) - that call `threading_trace_enter`/`_exit` when
  `PALMETTO_USER_TRACE` is set and otherwise forward exactly as before. A `threading_slot_name` table names the slots.
  No returned value, argument or side effect changes with the switch unset, and the switch is the existing diagnostic
  one (`PALMETTO_USER_TRACE`), off by default.
- **No behaviour change, and a measured reason for that.**
  [upstream_port.md](upstream_port.md) requires a slice to follow its measurement: there is no task-queue divergence to
  correct here, so a "fix" in this surface would be unmeasured guesswork. The callback-wrapping variant of the trace
  was deliberately removed: the title submits nothing, so a trampoline would be dead code with changed callback
  lifetimes. The negative is the slice's result, and the next falsifier is named in section 6.
- **Tests** in `port/test/xgameruntime_test.cpp` replay the measured shapes through the same vtable the title uses:
  slot 29 leaves the time-sensitive flag clear; slot 12 creates two distinct `ThreadPool`/`ThreadPool` queues (the
  title's measured modes `1/1`); slot 12 creates the `Immediate`/`Immediate` queue (`3/3`) and a callback submitted to
  it runs **on the submitting thread before the submit returns** (the GDK's documented `Immediate` semantics);
  slot 25 returns the default process queue; and the measured duplicate -> `GetPort`(Work) -> `CreateComposite` with
  the same port for both ports -> close sequence is driven once and its answers checked. These also guard the
  `SlotMethod`/`SelfMethod` forwarding: a signature or calling-convention mistake would fail the existing slots' tests.

## 6. Limits and the next falsifier

- **One boot, one machine.** The call table is one 130 s boot of one binary; the trace is bounded (first eight calls
  per slot, then powers of ten), so a poll beyond those counts is summarised rather than listed. For this title nothing
  was polled: the surface's 26 calls all fit the bound except slot 29's ninth call, which the bound skips.
- **Arguments only.** The trace prints the arguments the title passes and the answer each call **returns**, not values
  written through output pointers (queue/port handles behind the out pointers are not in the table, which is why the
  composite's port argument is reported as the same value in all three calls without claiming which port it is).
  `XTaskQueueRegistrationToken` is a struct and is logged as 0 (slots 21/24, never called here).
- **No callbacks to observe.** The task's "on which threads do callbacks run" has no answer in this boot because no
  callback is ever submitted; that is the measurement, not an omission.
- **The job-109 wait is the sibling slice's measurement.** The stack, the `SleepEx(1)` poll, the 13 posted items and the
  compute-PSO workers are [stage2_blocker.md](stage2_blocker.md) section 15's cdb dumps on the same tree; this boot
  only corroborates the busy-in-`nvgpucomp64` worker state with the debugger-free samplers. No claim in section 3 beyond
  this boot's own probes and samplers depends on the sibling measurement.
- **The XNetworking poll's owner and slot are not identified** (section 3). That is the named next falsifier for a
  *platform/networking* slice, not a task-queue one: capture the thunk return address of a `QueryApiImpl` request (a
  two-frame backtrace in the existing trace) and match it to the image's thunk table
  ([tooling/gdk_runtime_calls.py](../tooling/gdk_runtime_calls.py) already reads the image's per-slot thunk RVAs), then
  test whether the loading stage advances when the console-correct offline answer for that slot is supplied.
- No frame capture: this is a measurement boot, not a fix A/B pair, so no non-black frame is claimed.
- No game data: this page carries RVAs, slot numbers, thread ids, handles, hashes of our own build, counts and timings
  only - no package path, executable byte, shader, table or save content.

## 7. Tests and gates

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` (binary of the boot above) |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| slice test | `xgameruntime_test`: **OK (174 checks)**, including the measured-shape checks above |
| task-queue and wait suite (`port/build`, `ctest -C RelWithDebInfo -R "^(xgameruntime\|api_global_refs\|api_ref_state\|context_\|wait_registration_\|...)$"`) | 43/43 passed: `api_global_refs`, `api_ref_state`, `context_pinning`, `context_rundown`, `context_reentrant`, `context_concurrent_detach`, `context_attach_detach_stress`, the 19 `wait_registration_*` cases, `xgameruntime`, `xgameruntime_user`, `xgameruntime_gamesave`, `xgameruntime_store`, `xgameruntime_offline`, `xgameruntime_offline_hdr`, the two gamertag cases, `xbox_present`, `frame_dump`, `unimplemented_policy`, `xs_proxy_mock` |
| docs checks (`tests/test_docs_*.py`, run with the session's `GIT_CONFIG_*` variables cleared) | see the slice report; this page is untracked until the lead commits it, so the tracked-pages checks do not read it yet |
| default path | `PALMETTO_USER_TRACE` unset: no trace line, no counter change, no different answer; the wrapper is a forwarding call plus the cached environment read |

## 8. Attribution

This slice was implemented from this project's own measurements: the call table, the boot state and the samplers are
this tree's; the job-109 wait primitive is the sibling slice's measurement of the same tree (credited there). No code
was copied from a peer port. The behaviour was cross-checked, at the mechanism level only, against the public GPL-3.0
projects [Marvisak/VIctory](https://github.com/Marvisak/VIctory) (`crates/victory-hle/src/modules/xasync.rs`, commit
`13f91ca`), [xodus-gaming/XCL](https://github.com/xodus-gaming/xcl) (`README.md` feature table and
`docs/specs/2026-11-20-xcl-design.md`, commit `6d12f58`) and
[ThatRecompDev/five-star-recomp](https://github.com/ThatRecompDev/five-star-recomp)
(`src/FiveStar.Loader/Shims/Gdk/TaskQueueService.cs`, `TaskPort.cs`, `docs/wiki/Console-Runtime.md`, commit `0bfa755`);
the dispatch-mode model and the process queue's default were compared against Microsoft's public GDK documentation
(links in section 4). The vendored implementation is Microsoft's MIT-licensed libHttpClient at
`c5007fd9cbdc785518ea6dccc5751b125d451cc1` (`vendor/xtaskqueue/README.md`). The upstream projects remain the copyright
of their contributors and their licenses continue to apply to their own trees. The lead adds the `NOTICE.md` entry;
this slice makes no git writes.

## 9. Measured: the GPU-free task-queue/XAsync parity test (rank 6)

**Status: measured; test-only. No shim change, no behaviour change, no boot run by this slice.** This section
implements rank 6 of the plan in [peer_port_streaming.md](peer_port_streaming.md) section 6: "task-queue and async
parity test against the vendored implementation - close the contract gap only if one exists". The measurement in
section 2 says the title's whole task-queue surface is queue setup only and silent after +25.51 s, so the open
question is not a missing dispatch; it is whether the shim's `IXThreadingImpl` forwarding still matches the contract
the vendored queue implements, so that a later port (a callback trampoline, a dispatch-mode translation, a handle
wrapper) cannot silently diverge. The answer measured here is **no divergence**: every task-queue and XAsync slot the
title can reach (section 2's vtable) forwards the vendored contract unchanged, and the shim's named exports are the
vendored function addresses themselves. The new GPU-free test pins that contract on both paths; it is registered as
`task_queue_parity` (328 checks) and changes no runtime file.

### 9.1 The contract surface

Read from the vendored public headers (`vendor/xtaskqueue/Include/XTaskQueue.h`, `XAsync.h`, `XAsyncProvider.h`), the
implementation (`Source/Task/TaskQueue.cpp`, `AsyncLib.cpp`), the measured slot table in section 2 and the shim's slot
forwarding (`port/shims/xgameruntime.cpp`). "Shim path" is the vtable slot the title's generic thunk helper calls; the
test drives every point through that slot and through the vendored export, and requires the same answer.

| # | contract point | contract implemented by the vendored queue | test scenario |
|---:|---|---|---|
| 1 | queues and ports | `XTaskQueueCreate` takes a work and a completion dispatch mode; a queue owns both ports; `XTaskQueueGetPort` returns a port handle, `XTaskQueueDuplicateHandle` a second queue handle of the same queue | `scenario_dispatch_modes` |
| 2 | four dispatch modes | `Manual`, `ThreadPool`, `SerializedThreadPool` and `Immediate` are all accepted for either port; the measured title shape is two distinct `ThreadPool`/`ThreadPool` queues and one `Immediate`/`Immediate` | `scenario_dispatch_modes` |
| 3 | composite | `XTaskQueueCreateComposite` duplicates the handles of the queues owning the given ports and accepts the same port value for both arguments, the measured title call | `scenario_dispatch_modes` |
| 4 | Manual dispatch | a Manual-port callback runs only inside `XTaskQueueDispatch` for that port, once, on the dispatching thread; the empty port reports false | `scenario_manual_dispatch` |
| 5 | ThreadPool dispatch | the callback runs on the system thread pool without a dispatch call, not on the submitting thread; a ThreadPool completion port delivers the same way | `scenario_thread_pool`, `scenario_completion_port` |
| 6 | SerializedThreadPool | as ThreadPool but one callback at a time (eight sleeping callbacks never overlap) | `scenario_serialized_thread_pool` |
| 7 | Immediate | the callback runs on the submitting thread before the submit returns, leaving nothing queued | `scenario_immediate` |
| 8 | delayed callback | `XTaskQueueSubmitDelayedCallback` queues after `delayMs`; the callback runs in the dispatch call that reaches the deadline, not before | `scenario_delayed` |
| 9 | process queue | the default process queue exists and is thread-pool backed; an `XAsyncBlock` with a null queue completes with no dispatch call | `scenario_process_queue` |
| 10 | E_PENDING | a provider's `DoWork` returning `E_PENDING` leaves the status `E_PENDING`; `XAsyncComplete(E_PENDING, ...)` is ignored; a second `XAsyncSchedule` while work is queued returns `E_UNEXPECTED` | `scenario_async_e_pending` |
| 11 | completion routine and port | `XAsyncComplete` sets the terminal status first and submits the block's routine to the completion port; on a Manual completion port the routine runs only in `XTaskQueueDispatch` of that port, on the dispatching thread, with the status already terminal (as `XAsync.h` documents) | `scenario_completion_port` |
| 12 | completion ordering | completion routines reach a Manual completion port in submission order; three Immediate-work calls deliver in order | `scenario_completion_order` |
| 13 | cancellation | `XAsyncCancel` delivers `XAsyncOp::Cancel` once; a provider that completes with `E_ABORT` in that op makes the status `E_ABORT`, invokes the routine with that status, suppresses the queued `DoWork` and cleans up once | `scenario_cancellation` |
| 14 | cancel a provider ignores | the vendored library only delivers `XAsyncOp::Cancel` and leaves the `E_ABORT` completion to the provider (`XAsyncProvider.h` names that responsibility), so a provider that ignores the op leaves the call `E_PENDING`; five-star and VIctory complete it themselves, the recorded cross-check difference (section 9.4) | `scenario_cancellation` |
| 15 | quit and cancel pending items | `XTaskQueueTerminate` cancels queued callbacks (they run with `canceled=true`), invokes the termination callback after them, and refuses later submissions with `E_ABORT` | `scenario_termination` |
| 16 | `XAsyncBlock` lifetime | a block is refused (`E_INVALIDARG`) while its call is in flight and accepted again after the completion routine has returned (`XAsync.h` documents both) | `scenario_block_reuse` |
| 17 | cleanup and result once | the provider sees `XAsyncOp::Cleanup` exactly once per call, `GetResult` once, and a second `XAsyncGetResult` is `E_ILLEGAL_METHOD_CALL` | `scenario_result_cleanup`, `scenario_async_e_pending` |
| 18 | waiter | `XTaskQueueRegisterWaiter` on a signaled handle queues its callback on the requested port, uncanceled, on the dispatching thread; `XTaskQueueUnregisterWaiter` stops further delivery | `scenario_waiter_monitor` |
| 19 | monitor | `XTaskQueueRegisterMonitor` runs synchronously on every submission with the right port (direct submits and waiter entries); `XTaskQueueUnregisterMonitor` stops it | `scenario_waiter_monitor` |
| 20 | named exports | the 23 task-queue/XAsync names the shim serves for `xgameruntime.dll` are the vendored function addresses themselves, not wrappers | `check_named_exports` |

### 9.2 The test, and the two named expectations

`port/test/task_queue_parity_test.cpp` (target and `add_test(NAME task_queue_parity)` in `port/CMakeLists.txt`,
executed by `ctest`, so `tests/test_ctest_registration.py` covers it automatically) builds two facades over the same
contract: `shim_face` asks `QueryApiImpl` for `IXThreadingImpl` and calls vtable slots 3-25 exactly as the title's
thunks do, and `vendored_face` calls the `vendor/xtaskqueue` exports the test is linked against. One set of scenario
functions runs once per facade; every assertion names its face and has to hold for both, so a slot that stops
forwarding the contract fails against the vendored face's answer. `check_named_exports()` covers the third path, the
shim's registry entries: each of the 23 names must resolve to the vendored function address. The test is standard C++
plus Win32 handles and the vendored headers, no GPU and no game data.

Two expectations are deliberately outside forwarding, and the test pins them as such rather than pretending parity:

- **Slot 8 (`XAsyncBeginAlloc`).** Private, no public page, no measured caller; the shim refuses it with `E_NOTIMPL`
  (section 5 retired it), while the vendored library does export and serve it. The test checks both sides: the slot
  refuses, the vendored call begins a provider and still cleans it up once. A future port flips this one expectation.
- **Slots 27-30 (thread time sensitivity).** `XThreadSetTimeSensitive`, `XThreadIsTimeSensitive`,
  `XThreadAssertNotTimeSensitive` and the private slot 28 are the shim's own per-thread code, not vendored functions;
  the test pins the shim's flag semantics (set/read/clear, and the assert slot leaves the flag as it was) and says so.
  Slot 28 (`XTaskQueueIsEmpty`, private) has no measured caller and is not exercised.

No shim divergence was measured, so no shim file was changed: [upstream_port.md](upstream_port.md) requires a slice to
follow its measurement, and a wrapper change here would be unmeasured guesswork. The test therefore does not "fix"
slot 8 either; it records the gap instead.

### 9.3 Test evidence

| gate | result |
|---|---|
| build | `task_queue_parity_test` linked by Ninja `RelWithDebInfo` (MSVC 14.50.35717); the new file compiles at `/W4 /permissive- /utf-8` with no warning from it |
| build gates | `_setup\build_tests.cmd` prints `TESTS_BUILT` and `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` on this tree |
| direct run | `task_queue_parity_test: OK (328 checks)`, 0 failures, 1.6-1.9 s; eight consecutive runs identical |
| `ctest` registration | `ctest -C RelWithDebInfo -R "^task_queue_parity$"`: Test #100 `task_queue_parity` Passed, 1.63 s |
| GPU-free task-queue subset | 35/35 passed (2.5 s): `task_queue_parity`, `xgameruntime`, `xgameruntime_user`, `xgameruntime_user_gamertag`, `xgameruntime_user_gamertag_long`, `xgameruntime_gamesave`, `xgameruntime_store`, `xgameruntime_offline`, `xgameruntime_offline_hdr`, `api_global_refs`, `api_ref_state`, the five `context_*` cases and the 19 `wait_registration_*` cases |
| registration fixture | `python -I tests/test_ctest_registration.py`: 4 tests OK; the fixture derives its expectations from `add_executable`/`add_test` pairs, so adding the target needed no edit to it |
| docs checks | the twelve `tests/test_docs_*.py` run with the session's `GIT_CONFIG_*` variables cleared: this page passes every check that reads it (links, repo paths, encoding, tables, repeated lines, list items). `test_docs_env_vars.py` is red on this uncommitted tree because sibling slices' in-flight shims read new `PALMETTO_*` variables before their pages are written (the tree named `PALMETTO_DS_PAGES_ARM` in `port/shims/dstorage.cpp` while this was measured; earlier a `PALMETTO_BINK_*` pair in `port/src/main.cpp`); this slice adds no variable and none of its three files is in that report |
| A/B boot | none: the parity test is host-side only. Section 2 already records the rank-6 trace arm (one 130 s boot with `PALMETTO_USER_TRACE` and zero submit/dispatch/wait/XAsync slots), so a second queue-probe boot was not repeated; the falsifier below is what would need one |
| default path | unchanged: no runtime file is edited, and the test links its own executable |

### 9.4 Limits and the next falsifier

- **The contract is the vendored implementation's, not the console's beyond it.** The test pins `vendor/xtaskqueue`'s
  behaviour, which is the same libHttpClient family the GDK documents, but it cannot prove a console-only ordering
  the documentation does not state. Points where the GDK docs and the code agree (dispatch modes, `E_PENDING`, the
  completion port, one cleanup) are the ones asserted; the cancel-completion responsibility is the one place the
  peers differ (below).
- **Host thread pools.** ThreadPool and SerializedThreadPool callbacks run on the Windows system thread pool, as the
  vendored README says; "is a pool thread and not the submitter" is the assertion, not any console worker identity.
  The peers five-star and VIctory own and grow their pools, a difference the audit already names; the test does not
  compare pool identity across implementations.
- **Timing bounds.** The delayed-callback check waits up to 3 s for a 400 ms delay and only asserts "not before the
  delay"; the serialized check uses 1 ms sleeps. Both are generous for a host test but are not scheduling proofs.
- **Not covered.** `XTaskQueueTerminate(wait=true)` on an all-Manual queue (its termination callback needs dispatches
  from another thread, by design), delayed-callback order between two live timers, waiter retirement races (covered in
  depth by the 19 `wait_registration_*` cases directly on the vendored API), and slot 28. The test deliberately avoids
  booting the title, so no title call site is exercised; section 2's measured call table stands for that.
- **The cross-check difference recorded.** For a provider that ignores `XAsyncOp::Cancel`, Five Star Recomp's
  `AsyncService.cs` and VIctory's `xasync.rs` complete the call with `E_ABORT` themselves (five-star's comment says a
  provider that handles Cancel completes it, "otherwise ... complete here"; VIctory's `XAsyncCancel` calls the provider
  and then completes with `E_ABORT`), while the vendored libHttpClient leaves that completion to the provider. Our
  shim inherits the vendored behaviour, so the call stays `E_PENDING`; the test pins that, and it is the one peer
  difference this slice records rather than ports. No measured title call exercises it (section 2: zero XAsync calls).
- **Next falsifier, unchanged from rank 6.** A boot with `PALMETTO_USER_TRACE` that shows a submit, dispatch, waiter,
  monitor or XAsync slot in the load window would make this test the comparison contract for the measured shape; while
  the surface stays silent, the negative in section 2 stands and this test is its regression gate.

### 9.5 Attribution

The slice was implemented from this tree's own measurements (section 2's slot table, the vendored headers and the
shim's forwarding) plus the mechanism table in [peer_port_streaming.md](peer_port_streaming.md) sections 2.1, 3 and 9.
No peer code, comment text or table was copied; the test is our own and the peer trees were consulted at mechanism
level only, at the snapshots the audit pins: Five Star Recomp (`45071d52`; refresh `ce2b333b` read for this slice)
`src/FiveStar.Loader/Shims/Gdk/TaskPort.cs`, `TaskQueueService.cs`, `AsyncService.cs`, `TaskWaiter.cs`,
`TaskQueueMonitor.cs`; VIctory (`63bcca4`) `crates/victory-hle/src/modules/xasync.rs`; PercyScarlett (`e44c465`)
`PercyPort/crates/percyport-hle/src/modules/xasync.rs` and the same module under `runtime/victory`; victory-fork
(`9a9091d`) `crates/victory-hle/src/modules/xasync.rs`; XCL (`623d02e`)
`crates/xcl/src/hle/xgameruntime/taskqueue.rs` and `xasync.rs`. The dispatch-mode model and the process queue's
default were checked against Microsoft's public GDK documentation (section 4's links). The vendored implementation is
Microsoft's MIT-licensed libHttpClient, credited in `vendor/xtaskqueue/README.md`; that credit is unchanged. No
`NOTICE.md` row is proposed: this slice adds no ported mechanism and no third-party code, only a test over code the
repository already vendors (the peer-campaign row for [peer_port_streaming.md](peer_port_streaming.md) already exists).
Uncommitted files this slice leaves for the lead: the new `port/test/task_queue_parity_test.cpp`, `port/CMakeLists.txt`,
and this appended page section. No shim file, no registration fixture, no git write.

