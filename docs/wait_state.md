# What the title is waiting on: thread sampling and the DirectStorage path

**Status: measured.** A 150-200 s boot that renders black frames was sampled thread by thread at fixed times, and the
DirectStorage queue calls were traced. The result: **nothing is blocked on a waitable object the shims own, and no
queue signal or event is missing**. The title's threads are in ordinary pool/condition/frame waits while its loader
streams through DirectStorage and its renderer presents clears+draws; the GPU consume path (copies/dispatches) is never
invoked. Counts, RVAs, handle values and object types only; no game data, paths or shader bytes.

## 1. How the wait was measured

`PALMETTO_WAIT_SAMPLE=<seconds,...>` (`runtime/src/main.cpp`, in the runtime itself so the title's own threads are
sampled; `1` means 5,10,20,40,60). At each sample time a sampler thread:

* snapshots every other thread of the process (Toolhelp32), `SuspendThread`/`GetThreadContext`, and records RIP, RSP,
  the four integer-argument registers and the thread's Win32 start address;
* names a wait by RIP: the addresses of `NtWaitForSingleObject`, `NtWaitForMultipleObjects`, `NtDelayExecution`,
  `NtWaitForAlertByThreadId`, `NtWaitForWorkViaWorkerFactory`, `NtRemoveIoCompletion`, `NtSignalAndWaitForSingleObject`,
  the kernelbase `WaitFor*`/`Sleep*` wrappers and `SwitchToThread` are resolved at start, and a RIP inside a stub
  (within 0x40 bytes) is that call - on this build the sampled stub RIPs sit exactly `+0x14` into each ntdll stub;
* for a handle wait, names the object type through `NtQueryObject(ObjectTypeInformation)` ("Event", "Semaphore",
  "Thread", "Mutant"); for `NtWaitForMultipleObjects` it reads the object array and names the objects too;
* scans the first two stack pages for return addresses inside the mapped image or a loaded module (shallow walk; every
  page is `VirtualQuery`d first, so a bad stack end is a skipped page, not a fault) and prints them as module+RVA.

`PALMETTO_DSTORAGE_TRACE=1` (`runtime/shims/dstorage.cpp`) logs the first few of each queue call with the **title-side
caller's module+RVA**, every entry the worker runs with its HRESULT, and periodic progress counters (requests, MiB,
MiB/s). Both are off by default and change no behaviour.

## 2. What the samples show

Thread counts and RIP-site histogram, one 200 s boot (`F:\palmetto-work\boot-wait2`; the 150 s `boot-dscaller` and
`boot-dsprogress` agree):

| Sample | Threads | Dominant RIP sites (counts) |
|---|---:|---|
| 10 s | 61 | ntdll `NtWaitForSingleObject` 26, `NtWaitForAlertByThreadId` 16, `NtWaitForMultipleObjects` 11, `NtWaitForWorkViaWorkerFactory` 8, `NtDelayExecution` 2, busy title site 1, our runtime 1 |
| 30 s | 67 | `NtWaitForSingleObject` 24-26, `NtWaitForAlertByThreadId` 18, `NtWaitForMultipleObjects` 11, worker factory 8, 2x nvgpucomp64 (shader compiler running) |
| 60 s | 82 | `NtWaitForSingleObject` 36, `NtWaitForAlertByThreadId` 23, `NtWaitForMultipleObjects` 10, worker factory 8 |
| 120 s | 79 | `NtWaitForSingleObject` 35, `NtWaitForAlertByThreadId` 23, `NtWaitForMultipleObjects` 10, worker factory 5 |
| 180 s | 79 | `NtWaitForSingleObject` 36, `NtWaitForAlertByThreadId` 23, `NtWaitForMultipleObjects` 10, worker factory 5 |

Translated to **named waits**, with the object types and the title-side frames the stack scan found:

| Wait | Object | Title-side site (RVA) | Threads | What it is |
|---|---|---|---|---|
| `NtWaitForSingleObject` | **Event** handles `0x7A8, 0x7AC, ... 0x7C4` (one per thread) | `game+0x1F678F5` in a loop, started at `game+0x2215EA0` | 8-11 | the title's own worker pool: each worker waits on its own event, woken by work |
| `NtWaitForMultipleObjects` | 2 objects: **Event** `0x2B4` + **Thread** `0x2C8` | host CRT frames (`ucrtbase+0x3D6AE <- ntdll+0x17FE00 ...`), start `palmetto.exe+0x1856B` | 10-11 | CRT thread start/join wait of threads we create for the title; ordinary |
| `NtWaitForAlertByThreadId` | - | - | 16-23 | Windows thread-pool / condition-variable waits (wait-on-address), idle workers |
| `NtWaitForWorkViaWorkerFactory` | - | - | 5-8 | Windows thread pool workers idle, waiting for work |
| `NtWaitForSingleObject` | **Semaphore** `0x798` | `game+0x55E8B2A` (start `game+0x55E8B00`), frames `MMDevAPI.DLL+0x436D0` | 1 | the audio device's render thread waiting for the next buffer |
| `NtDelayExecution` | - | `game+0x2215EA0` workers; our runtime thread `palmetto.exe+0x2049CB` | 2-3 | timed sleeps in pool loops / runtime pacing |
| `win32u` waits | window objects | - | 2 | the host message loop |

Transient busy title code appears in single samples (`game+0x4F657F1` at 10 s, the NVIDIA shader compiler threads at
30 s); by 60 s every thread is in one of the idle waits above. **No thread is in a wait the shims created and failed to
signal; nothing sampled is in a spin, and no single object holds the process.** The largest wait classes are the title's
worker-pool events (8-11), the CRT thread start/join Event+Thread pair (10-11), the thread-pool/condition waits
(16-23), the Windows pool workers (5-8), one audio semaphore, and the frame/render waits - all normal idle waits.

## 3. The DirectStorage hypothesis, tested: no signal or event is missing

`PALMETTO_DSTORAGE_TRACE=1` over 150 s (`F:\palmetto-work\boot-dscaller`) shows the whole queue traffic:

* **Requests:** `file -> memory`, `zlib=1`, `bcpack=0`, `swizzle=0`, ~17.8-24 KiB source and 64 KiB destination each.
  The 64 KiB destinations are ordinary process-heap addresses (e.g. `0x20ED6BFB2A0`-style, not a title-page VA).
  The title-side caller is **`game+0x28C9A0E`** for every request.
* **Batches:** seven `Submit` calls with 43, 7, 2, 475, 3,677 and 2 entries (plus a later 2); the status entry and the
  submit are enqueued from **`game+0xE8943`** and **`game+0xE8950`** (adjacent small helpers).
* **Completion:** 4,096 requests / 256 MiB decompressed in 2.0 s in that boot (131-144 MiB/s; 55-86 MiB/s in the slower
  runs), every entry ran with `hr=0x00000000`, and the status array completed `S_OK` each time. **`EnqueueSignal`
  count: 0. `EnqueueSetEvent` count: 0.** The title never asks the queue for a fence signal or an event set, so there is
  no signal the shim could fail to deliver; it polls its `IDStorageStatusArrayX` instead.
* **After the batch:** the queue is idle and receives only occasional 2-3 entry submits later in the boot (seen at
  ~85 s and later), i.e. the loader is alive and still prefetching, not stuck.
* No queue error record was produced and no request failed.

So the "loading thread waits on a DirectStorage fence/event that is never signalled" hypothesis is **disproven for
this build**: no fence or event is enqueued at all, every request completes, every status completes, and the queue
worker is idle-waiting (a condition-variable waiter in the samples) rather than blocked.

## 4. What the title is waiting on, in one sentence

Between small streaming jobs the title idles in **its own pool events and condition variables** (plus the Windows
thread pool and one audio semaphore) and its render thread produces one clear+draw frame after another - the process is
not blocked; it is in a **pre-content loading loop whose GPU consume path is never invoked** (no
`CopyBufferRegion`/`CopyTextureRegion`/`CopyResource`/`Dispatch` anywhere in 150 or 300 s), so the streamed data never
reaches a GPU texture and the frame stays black.

## 5. Smallest next measurement (no shim fix is indicated)

There is no wait to fix in the shims: no signal is missing and no object is stuck. The next smallest measurements, in
order:

1. **Name what consumes the streamed memory.** The anchors are the title RVAs `game+0x28C9A0E` (request enqueue) and
   `game+0xE8943`/`0xE8950` (status+submit) and the destination heap range. A protected-destination or write-watch
   sample (or a caller-RVA log on the D3D12 copy/view calls, in the device shim's owner's scope) says which title
   function reads those 64 KiB blocks and what it does next - that is the function whose GPU-side step never happens.
2. **Instrument the status polling.** Log `IDStorageStatusArrayX::GetHResult`/`IsComplete` caller RVAs and values, to
   confirm the loader sees its batch complete (it does today) and what it calls next in the same turn.
3. **Re-check the black is not the title's intended loading state.** The sampled waits are consistent with a normal
   load loop; one boot longer than 300 s (the boot already runs 40,000+ draws) with the window captured at intervals
   would separate "still loading" from "loaded but nothing is rendered".

## 6. Instrumentation added (all env-gated, no behaviour change)

`runtime/src/main.cpp`: `PALMETTO_WAIT_SAMPLE` sampler as described in section 1 (thread snapshot, RIP naming, object
types, shallow stack, per-sample site histogram and wait detail). `runtime/shims/dstorage.cpp`: `PALMETTO_DSTORAGE_TRACE`
per-call trace with caller RVAs, per-entry completion HRESULTs, and periodic request/MiB/MiB/s progress counters; no
queue semantics changed.

Measured boots: `F:\palmetto-work\boot-waits`, `boot-dstrace`, `boot-dsprogress`, `boot-wait2` (200 s with samples at
10/30/60/120/180 s), `boot-dscaller`. All ALIVE to their timeout, 0 exceptions, 0 device-lost, 0 failed `Close`,
0 refused submits.
