# Wait forensics: what the parked title threads wait on, and the one read that never completes

**Status: measured.** Two debugger snapshots (all-thread stacks and the whole handle table at 60 s and 120 s) and one
bounded in-process wait tracer name every wait the 35 title-image threads are in after the title's own work stops at
~30.8 s: which object, which timeout, which of them is ever woken, and which are still outstanding at 120 s. The answer
is not a stage guess. Ten of the thirty-five wait on their own event, every one of which was last set at 27.6-27.7 s by
the title's worker-wake walker; five wait on events no traced setter ever touches; three wait on condition-variable
semaphores last notified at 27.5-27.7 s; the thread that runs the title entry point has been blocked since 27.7 s
acquiring a lock whose release event is never set. The one title thread that is not waiting on a signal is a streaming
read job stuck in a `ReadFile` retry loop that starts at **+27.719 s** and never stops: its `ReadFile` fails on every
attempt and the title's own reader retries forever, so the completion signal all the parked waits depend on is never
produced. Addresses are RVAs from the image base, counts and API names only; no game data, paths or shader bytes.

## 1. What ran

| | |
|---|---|
| Boots | three 130 s boots under `_setup/boot_lock.ps1`: `boot-waits-cdb` (unmodified binary, debugger dumps at 60 s and 120 s), `boot-waits-trace` (`PALMETTO_WAIT_TRACE=60,120`), `boot-waits-control` (same binary, tracer unset) |
| Binary | `boot-waits-cdb`: `palmetto.exe` built 11:24:05, SHA-256 `8821CF5F…`; A/B pair: built 11:40:22, SHA-256 `3B8E5DDD…` (both are the working tree of their moment; see the ReadFile note in section 7) |
| Switches | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=30`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_WAIT_SAMPLE=60,120` in all three; only the tracer boot adds `PALMETTO_WAIT_TRACE=60,120` |
| Debugger | `cdb -p <pid> -c "~*k; !handle 0 f; .detach; q"` at 60.1 s and 120.1 s, 9.1 s and 6.2 s per attach; system DLL frames are symbolised, the mapped title image appears as raw addresses (reported here as RVAs) |
| Tracer boot | 30 target APIs, 29 patched, 197 import slots in 38 host modules, 133 detail lines of the 512-line budget |
| Result | all three boots ALIVE to the 130 s timeout, 0 exceptions; the presented image stays the stationary black two-pass loop of [long_boot.md](long_boot.md) |

The wait-sampler and loader measurements of [wait_state.md](wait_state.md), [long_boot.md](long_boot.md) and
[streamed_consumer.md](streamed_consumer.md) are the starting point: the title's own work ends at ~30.8 s and the
process then sits in ordinary kernel waits. This page adds the two things those measurements could not see - the object
behind each wait, and whether it is ever signalled.

## 2. The 35 title-image threads at 60 s and 120 s

`cdb ~*k` at both times sees the same population: 91 threads at 60 s and 90 at 120 s, of which **35 have title-image
frames at both dumps and 90 of the 91 thread ids are the same in both**, with the same wait chain (the one thread that
differs is the sampler thread that had already exited; a second thread was executing). Grouped by wait API and the first
title frame above it (the wait site) at 60 s:

| wait API | wait site | threads | object, as named by the tracer in the same boot | timeout | last wake measured |
|---|---|---|---|---|---|
| `NtWaitForSingleObject` | `game+0x221C3C2` | 11 (10 at 120 s) | 11 distinct Events `0x7b8`-`0x830`; ten are per-thread wake events, `0x828` is the per-frame event | INF | `0x828` wakes 100/s; the other ten last set 27.6-27.7 s |
| `NtWaitForSingleObject` | `game+0x43C421E` | 5 | 5 distinct Events `0x1a3c`, `0x1a5c`, `0x1b98`, `0x1bc8`, `0x1bd4` | INF | never set in 130 s |
| `NtWaitForSingleObject` | `game+0x56A0D` | 7 | condition-variable Semaphores `0xd40`, `0xd44`, `0xd48`, `0xd4c`, `0xc78`, `0x1eac` | INF | `0xd44` still notified to 120 s; `0xd40`/`0xd48`/`0xd4c` last released 27.5-27.7 s |
| `NtWaitForSingleObject` | `game+0x258DBA6` | 1 | Event `0x1d94`, waited in the lock-acquire helper (section 3) | INF | never set in 130 s |
| `NtWaitForSingleObject` | `game+0x19B4EAF` | 1 | Event `0x1a88` | INF | never set in 130 s |
| `NtWaitForSingleObject` | `game+0x3A1A200` | 1 | Event `0x494` | INF | never set in 130 s |
| `NtWaitForSingleObject` | `game+0x221957F` | 1 | Event `0xd10` | INF | never set in 130 s |
| `NtWaitForSingleObject` | `game+0x22511F` | 1 | Semaphore `0x1a60`, polled with a 1 ms timeout | 1 ms | 75,447 polls at 120 s, **0 acquires** |
| `NtWaitForSingleObject` | `game+0x43ADEDA` | 1 | Event `0x1e38` | 100 ms | 9,228 waits by 120 s, all signalled, ticking loop |
| `NtWaitForSingleObject` | `game+0x55E8B2A` | 1 | audio Semaphore `0x7a8` | INF | released by the audio shim (host side, not traced), normal |
| `NtDelayExecution` | `game+0x3142B2` | 1 | `ReadFile` retry loop, `Sleep(1)` per attempt | 1 ms | **active since +27.719 s**, 59,421 attempts by 120 s |
| `NtDelayExecution` | `game+0x2215B0B` | 1 | 1 Hz tick, `Sleep(1000)` | 1000 ms | 120 calls by 120 s, alive |
| `NtDelayExecution` | `game+0x43AE2AD` | 1 | `Sleep(1)` poll loop, 15 ms cadence | 1 ms | 17,197 calls by 120 s, alive |

The render thread is not parked: at 60 s one title thread is inside our Vulkan
`ExecuteCommandLists` (`game+0x24821E3` caller) and at 120 s it is in the replay's copy loop
(`game+0x248268F`), with 10,506 presented frames and 21,012 submissions by the +120 s marker. The waits above are the
title's non-render work: its job pools, its loader and the thread that runs the title entry point. The in-process wait
sampler (`PALMETTO_WAIT_SAMPLE=60,120`, both A/B arms) independently reports the same picture at both times, with the
same handles, timeouts and object states - the entry thread on Event `0x1d94` not set, the pool workers on their own
not-set events, the audio thread on its semaphore - so the two methods agree on what each parked thread holds.

The debugger's own handle table agrees that nothing is being signalled: 1,109 Event handles at 60 s, 74 of them set and
1,035 waiting; 1,110 at 120 s, 73 set and 1,037 waiting. A system that was still passing work between threads would not
leave that proportion fixed across a minute.

## 3. The wait sites, read out of the image

Each wait site is named by the first title frame above the kernel wait, and the containing function is given by a
`.pdata` lookup on the retail image. Instruction-level facts below are from a linear disassembly of those functions;
no instruction bytes are reproduced.

* **`game+0x4DA4240`, the worker-wake walker.** It walks an array of `{pending count, Event handle}` entries (stride
  `0x40`, handle at `+0x18`), and for each entry it clears the pending word and calls `SetEvent` on the handle
  (the `SetEvent` tail-jump at `game+0x5930`); the return address of that call is the `game+0x4DA42B6` setter site in the
  tracer's signal table. 17,197 of its 17,245 calls by 120 s set `0x828`, the per-frame event; the rest are the
  startup wake-ups of the pool events, all before 27.7 s, so no entry after the first is set again after that.
* **`game+0x221C1F0` and `game+0x43C4140`, the pool bodies.** Both are thread bodies that take a TLS slot, then wait
  on a per-thread Event from the pool's array (`game+0x221C3C2` and `game+0x43C421E` are the `WaitForSingleObject`
  calls inside them, through the image's INFINITE wait tail-jump at `game+0x1784D0`).
* **`game+0x62D0`, the condition-variable notify.** It updates a 64-bit waiter word with `lock cmpxchg`, then selects
  the wait semaphore from the object's slot array (`game+0x4DAA210`) and tail-jumps into the `ReleaseSemaphore` thunk
  (`game+0x7330`); the notify's return address is the `game+0x6380` release site. The CVs the loader threads sleep on
  are released only from here.
* **`game+0x56A0D`, the CV wait.** The loop in `game+0x569F0` waits `count` times with `WaitForSingleObject(handle,
  INFINITE)`; the CV dispatch `game+0x4E7F340` picks the semaphore from the object's slot array and calls that loop, and
  `game+0x56A0D` is its wait.
* **`game+0x258DB70`, the lock acquisition the entry thread is stuck in.** It spins on a per-object word with
  `lock xadd` and, when the lock is taken, waits on the event the object keeps at `+0x70`; the wait's return address is
  `game+0x258DBA6`, so the entry thread is waiting for the *release* of a title lock, not for a stage to start.
* **`game+0x314200`, the `ReadFile` retry wrapper** (section 5) and **`game+0x313FC0`, the buffer fill that calls it**.

## 4. What is woken, and what is not

The tracer records every `SetEvent`/`ReleaseSemaphore`/`ReleaseMutex`/`Wake*` a title thread makes, with its caller
RVA, and matches it against the objects that have been waited on. At 120 s the object table reads (object, type, wait
entries, set/release calls, last set time, last setter site):

| object | type | wait entries | set/release calls | last set/release | setter site |
|---|---|---|---|---|---|
| `0x828` | Event | 17,197 | 17,197 | 120,000 ms | `SetEvent` `game+0x4DA42B6` |
| `0x7b8`, `0x7c0`, `0x7c4`, `0x7c8`, `0x7cc`, `0x7d4`, `0x7fc`, `0x80c`, `0x810` | Event | 4-10 each | 3-9 each | 27,619-27,719 ms | `SetEvent` `game+0x4DA42B6` |
| `0x830` | Event | 1 | **0** | - | - |
| `0x1d94` (entry thread) | Event | 1 | **0** | - | - |
| `0x1a3c`, `0x1a5c`, `0x1b98`, `0x1bc8`, `0x1bd4` | Event | 1 each | **0** | - | - |
| `0x1a88`, `0x494`, `0xd10` | Event | 1 each | **0** | - | - |
| `0xd40`, `0xd48`, `0xd4c` | Semaphore | 1,070 / 16,486 / 15,545 | 796 / 16,207 / 15,364 | 27,563-27,719 ms | `ReleaseSemaphore` `game+0x6380` |
| `0xd44` | Semaphore | 11,775 | 11,353 | 119,985 ms | `ReleaseSemaphore` `game+0x6380` |
| `0x1a60` | Semaphore | 75,447 | **0** | - | - |
| `0x1e38` | Event | 9,229 | **0** | - | - |

The signal-side aggregate says the same: `SetEvent` from `game+0x4DA42B6` has 17,245 calls at 120 s (17,197 matched to
`0x828`), `ReleaseSemaphore` from `game+0x6380` has 44,908 (the two live CV paths keep cycling), and the 10,068
`WakeConditionVariable` calls from `game+0x4FB0AEF` are the frame loop's own timer helper. Everything the parked waits
need is on the same title-side machinery, and none of it names their objects after 27.7 s.

The one active poll is worth separating from the parked waits: a job worker calls `WaitForSingleObject(0x1a60, 1ms)`
75,447 times by 120 s and never acquires it, and another thread polls at `game+0x5441BEC` with a 0 ms timeout 45,443
times. Both are trying to pick up work that is never posted.

## 5. The signal that is missing: the read at +27.719 s never returns

One title thread does not wait on a signal at all: it retries a `ReadFile` forever. The tracer's first record for the
sleep site `game+0x3142B2` is at **t = 27,719 ms**, and the site then runs at ~640 attempts/s until the kill (59,421
sleeps by 120 s). The function is `game+0x314200`, a read helper:

1. it calls the import at the image's `ReadFile` slot (`0x6793970`) in a loop, with a stack `OVERLAPPED` and a
   `lpNumberOfBytesRead` slot;
2. when `ReadFile` returns 0 it reads `GetLastError` (slot `0x67939C0`);
3. **errors 6 (invalid handle), 0x26 (handle EOF) and 0x57 (invalid parameter) end the loop; every other error is
   treated as transient, `Sleep(1)` and the same read is retried** - the loop at `game+0x314259`;
4. the call chain is the load job's read path: job `game+0x2499630` -> virtual stream read `game+0x5CD3D50` ->
   range step `game+0x28CF940` -> `game+0x1330B60` -> stream read at offset `game+0xCDD930` -> fill buffer
   `game+0x313FC0` -> `game+0x314200`.

So from 27.719 s on, every attempt of one streaming read returns an error the title's own reader classifies as
retryable, and nothing above the call can proceed. The timing is the whole boot in miniature: the last `SetEvent` of
the pool events and the last release of the stalled CV semaphores are at 27.6-27.7 s, and the entry thread's lock wait
(32,312 ms elapsed at the 60 s dump) also begins at 27.7 s.

**The named missing signal is the completion of that read.** It is not an event the runtime failed to set: the title
never reaches the code that would set it, because the reader's `ReadFile` never returns success. The exact title code
that must produce the signal is the return of `game+0x314200` to `game+0x313FC0` (a successful `ReadFile`) and, above
it, the job completion at `game+0x2499630`; only then do the release sides run - the pool waker
`game+0x4DA4240`, the CV notify `game+0x62D0` and the lock release that sets the entry thread's `0x1d94`. The parked
waits listed in section 2 are all downstream of that one call.

Which error the read gets is the one thing the tracer does not print (it is not a wait). The port's own reader note in
`port/shims/file_system.cpp` (RT-READFILE-NOACCESS) records the same retry rule in the title and documents the
expected failure: the title's memory is handed out as reserve-only views, a kernel-mode copy into an uncommitted page
returns `ERROR_NOACCESS` (998), and the title treats that as transient, so the read is a silent stall. The measured
binary bound `ReadFile` to the host (the binding report counts 93 shim imports for the image and names no `ReadFile`;
the current working tree's file shim registers it and counts 94), so no page-touch repair was on the path in these
boots - consistent with 998, but the error value itself is the next measurement (section 9), not a claim of this page.

## 6. Tracer switches and bounds

`PALMETTO_WAIT_TRACE=<seconds,...>` (`port/src/main.cpp`, `port/src/wait_trace.h`) patches the import slots that name
the wait and set APIs in the mapped title image and in every loaded host module, so a call reaches a wrapper that calls
the original through the pointer it replaced. It is off unless the variable is set; the control boot of section 7 has
zero tracer lines.

| | |
|---|---|
| APIs covered | `WaitForSingleObject(Ex)`, `WaitForMultipleObjects(Ex)`, `WaitOnAddress`, `SleepConditionVariable(SRW/CS)`, `Sleep(Ex)`, `NtWaitForSingleObject`, `NtWaitForMultipleObjects`, `NtWaitForAlertByThreadId`, `NtDelayExecution`, `RtlSleepConditionVariableSRW`, and the set side `SetEvent`, `NtSetEvent`, `PulseEvent`, `NtPulseEvent`, `ReleaseSemaphore`, `NtReleaseSemaphore`, `ReleaseMutex`, `NtReleaseMutant`, `WakeByAddress(Single/All)`, `RtlWakeAddress(Single/All)`, `Wake(All)ConditionVariable`, `RtlWake(All)ConditionVariable` (30 names) |
| Recorded per call | thread id, API, nearest title-image frame (the site) and immediate caller, object (handle, or the wait-on-address key), object type and current state at entry, timeout (ms, `INF`, or `abs`), the return time and outcome (signalled / timed out / other), and the set-side callers with the object they touched |
| Bounds | 96 aggregate (API, site) rows; 192 tracked wait objects; 128 signal caller sites; 512 detail lines in total and 4 per aggregate row (the rest is counted); 128 outstanding-wait slots; up to 8 dump times; 6 module rescans after start (1/3/10/30/60/120 s); no allocation on the wait path except the fixed tables |
| Dumps | one block at each requested second: aggregate wait sites, per-object set correlation and state, signal sites, outstanding waits with object and elapsed time, unmatched set counts |
| Default path | nothing runs with the variable unset; the wrappers are dead code. The A/B of section 7 is the measurement |

## 7. A/B: the tracer's own cost, and no behaviour change

| measurement | tracer on | control (same binary, switch unset) |
|---|---|---|
| verdict | ALIVE 130.3 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| startup trace events | 19,076 | 19,170 |
| `[d3d12-trace]` calls | 481,913 | 508,309 |
| pipeline population at +120 s | slot-57 10,241 converted / 0 refused, slot-55 41,545, graphics 51,786, compute 95/95 | identical |
| loader object counters | begin/end 1/1, 16/16, 1/1, 1/1; spinner depth 0 | begin/end 1/1, 17/17, 1/1, 1/1; spinner depth 0 |
| submissions / presents at +120 s | 21,012 / 10,506 | 22,562 / 11,281 |
| console-ABI proxy calls at +120 s | 264,060 | 275,688 |
| process threads at +120 s | 85 | 85 |
| wait sample at 120 s | 85 threads, 75 in a wait/busy title site, 10 distinct RIP sites | same |

Every state counter is identical - pipeline population, loader end state, thread count, the presented black target - and
the rate counters are 5.2-6.9% lower with the tracer on, which is the tracer's own work on the wait path (874,120 traced
titled waits by 60 s), not a behaviour change. The loader begin/end difference of one call on one object is the
inter-boot variance [platform_gate_ab.md](platform_gate_ab.md) section 4 measures between any two boots of this tree.

## 8. Limits

- **The debugger pauses the process.** Each attach suspends all threads for 9.1 s and 6.2 s, and the counters inside
  those windows are lower; the two snapshots are the measurement, not part of the steady state.
- **The tracer's thread counter is a record counter.** The dump line says "title threads followed" but the value
  (874,120) is titled *wait records*; the thread table is the outstanding section, and the debugger dumps are the
  thread-set check. This page reads the field as records.
- **The tracer cannot read `GetLastError`.** It shows that a `ReadFile` fails every attempt and is retried; which error
  code it is (998, 997 or another) is the next measurement, and section 5 states the reason 998 is the expected one
  without claiming it as measured.
- **The tracer patches import slots, not ntdll.** A wait made by ntdll internally (the thread pool's
  `NtWaitForWorkViaWorkerFactory`, an SRW lock inside ntdll) is not traced; those threads are the `-` rows of the
  debugger histogram and they are not title threads. A call resolved through `GetProcAddress` instead of an import is
  likewise untraced.
- **Host-created signals are invisible on the set side.** The tracer does not patch `palmetto.exe` itself, so an event
  our shims set (the audio semaphore, the frame event our presenter sets) reads as "0 traced sets"; this is why the
  audio thread's wait is marked host-released and not called a stall. The objects the parked title threads wait on are
  title-created and title-woken, so that gap does not hide their signal.
- **One machine, one overlay, three boots.** The thread ids and handle values differ between boots (allocator order);
  the wait sites, object roles and the 27.7 s boundary are identical in the debugger boot and the tracer boot.
- **The measured binary's file binding.** Both A/B arms were built at 11:40 and bind the title's `ReadFile` to the host
  (binding report: 93 shim imports, no `ReadFile`); a later working-tree build registers the file shim's `ReadFile`
  (94 shim imports). A background section 5 read on the older binary and a page-touch repair on the newer one are
  therefore not the same code path, and the retry loop may change between them - which is exactly what section 9
  measures.

## 9. Next falsifiable measurement

1. **Name the read error and the file.** One 130 s boot of the same tracer with a bounded `ReadFile` wrapper (the same
   import-slot mechanism, first N calls per caller, logging only the error code, the handle and the destination
   range) prints what `game+0x314200` gets on its failing attempts. Prediction if the 998 account is right: error 998
   on a destination inside the title's reserve-only range, with `game+0x314200` as the caller; a 997 would mean an
   asynchronous handle, which changes the fix. Two existing switches already narrow the file without code: one boot
   with `PALMETTO_FILE_TRACE=1` logs every open and the first `ReadFile` caller (the file slice's diagnostic).
2. **Repeat the tracer boot on the current tree**, whose file shim registers `ReadFile` and repairs one `ERROR_NOACCESS`
   by touching the destination pages. Prediction if the read is the gate: the retry loop at `game+0x3142B2` no longer
   starts at ~27.7 s, the pool events `0x7b8`-`0x830` and the CV semaphores `0xd40`/`0xd48`/`0xd4c` keep being set past
   30 s, and the entry thread leaves `game+0x258DBA6`. A negative - the loop stays - refutes the read as the gate and
   puts the lock wait `game+0x258DB70` at the head of the queue instead.
3. **Only if both above are negative.** The lock the entry thread wants (`game+0x258DB70`, event at `[obj+0x70]`) is
   held by a thread whose release side is not on the measured paths; a hardware watchpoint on that object's lock word
   (the `PALMETTO_LOADER_WATCH` mechanism) names its owner in one boot.

## 10. Evidence and reproduction

The boots' own files are outside the repository in `D:\leonida-work\`: `boot-waits-cdb\` (stderr 24.9 MB, stdout with
the binding report, `boot.jsonl`, `cdb\dump-60.log` / `dump-120.log` with `dumps.csv`), `boot-waits-trace\` and
`boot-waits-control\` (stderr, stdout, `boot.jsonl`), with each arm's binary SHA-256 in its `meta.txt`. The analysis
scripts are `waitforensics/analyze_cdb.py` (thread/frame and handle-table parser), `threads_table.py` (grouping),
`build_tables.py`, `title_sites.py` and `title_functions.py` (static reference scan, `.pdata` lookup and
disassembly of the named functions), and `boot_cdb.ps1` / `boot_wait_arm.ps1` (the boots).

Reproduce: acquire `_setup/boot_lock.ps1 -Acquire -Who <name>`; run one boot through `tooling/boot_report.py` with the
environment of section 1; at 60 s and 120 s run
`cdb -p <pid> -c "~*k; !handle 0 f; .detach; q" -logo <file>`; for the tracer add `PALMETTO_WAIT_TRACE=60,120` and
read the `[rt] wait trace` blocks out of stderr; then release the lock. No shim semantics were changed for this page.
