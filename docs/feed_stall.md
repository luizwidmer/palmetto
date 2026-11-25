# The feed stall: a CPU deadlock inside the submission, not a GPU hang

**Status: measured; cause named.** The freeze that appears when the vertex-stream driver block and the console
root-argument feed are both on (`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` + `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`) is a
**CPU wait**, not a GPU hang and not an invalid fed descriptor: the replay's submission thread deadlocks on
`resource_mutex_` (a `std::shared_mutex`) because the replay's device-memory callbacks call
`VulkanContext::heap_base()`, which re-acquires that mutex **shared** from a thread that already holds it shared for
the whole submission; a concurrent pipeline registration then queues as a **writer**, and Windows SRW locks are
writer-preferring, so the nested shared acquire blocks behind the writer while the writer waits for the outer shared
lock to drop. The feed supplies the nested acquire through the driver-block V# write, which happens on every fed
draw. Two cdb all-thread dumps 15.2 s apart show the identical stack on the submission thread and the identical
writer threads, the Vulkan validation layer (core + synchronization validation) reports **zero** messages, and the
driver reports no device-lost event. A small fix is named in section 8; this slice changed no code (measurement and
this page only).

## 1. What the stall is

With both feeds on, the title's frame loop stops after a small number of frames: submissions and presents freeze,
memory stops growing, the title's threads park, no exception is thrown, and no device-lost line is printed. The
earlier A/B ([vertex_constant_feed.md](vertex_constant_feed.md) section 5) measured this as a freeze at 154
submits / 76 presents (two of three with-feed arms) while the control and console-only arms ran the full window, and
named it as the blocking falsifier for defaulting the feeds on.

This measurement reproduces the same class of freeze on the current tree and names the exact wait:

* **The wait:** `ntdll!NtWaitForAlertByThreadId` <- `ntdll!RtlpAcquireSRWLockSharedContended` <-
  `ntdll!RtlAcquireSRWLockShared` <- `VulkanContext::heap_base()` (`port/src/vulkan_backend.h:1756-1759`, body
  `std::shared_lock lock(resource_mutex_); return replay_.heap_base;`).
* **The producer:** a concurrent `std::unique_lock lock(resource_mutex_)` in `VulkanContext::add_pipeline()`
  (`port/src/vulkan_backend.h:1763`), called by the title's own threads from `PipelineState::create_compute` ->
  `Device::CreateComputePipelineState` -> `proxy_create_compute_pipeline_from_native_code` -> console proxy
  slots 62/67.
* **Why it never resolves:** the submission thread already holds `std::shared_lock resource_lock(
  context_.resource_mutex_)` for the whole replay (`port/src/vulkan_backend.h:2849`, in `VulkanQueue::execute_now`);
  the nested shared acquisition cannot pass a queued writer, and the writer cannot pass the outer shared lock.

The feed's part is the path that reaches `heap_base()` from inside the submission: the driver-block feed calls
`vk.write_device_memory` for the bound vertex view's V# on every fed draw (`port/src/vulkan_replay.h:3166`), and
`DeviceState::write_device_memory` (`port/shims/d3d12_vulkan_device.cpp:2574-2582`) obtains the arena base through
`context->heap_base()` instead of reading the value already captured for the submission. The vertex-path and
frame-dump diagnostics reach the same accessor through `read_device_memory` (same file, 2558-2570), but only at an
armed draw. With the feeds off, the write callback never runs inside the frame loop and the read callback runs only
when an armed draw's prepare reaches it, which is why the unfed arms did not reliably take the nested acquire - and
why the freeze looks intermittent even with the feeds on: it needs the writer to be queued at that instant.

## 2. What ran

One boot per attempt under the repository's single boot slot (`_setup/boot_lock.ps1`), 200 s cap each (120 s for the
last attempt), one frozen binary per arm so other agents' rebuilds could not change it. The environment is the
shipping one plus the read-only diagnostics: `PALMETTO_VERTEX_PATH=1`, `PALMETTO_FRAME_DUMP=1`,
`PALMETTO_FRAME_DUMP_SAMPLE=1`, `PALMETTO_READBACK_FULL=1`, `PALMETTO_PIPELINE_TIMELINE=5`,
`PALMETTO_VK_VALIDATION=1`.

| arm | binary | feed switches | frame dump / readback armed | outcome |
|---|---|---|---|---|
| Feed | current-tree build, SHA-256 `6EF088C0…` (HEAD `66b33d78` plus the tree's uncommitted work at 15:25) | both on | draws 100/101 (the boot's first composed frame) | **froze**: 106 submits / 52 presents from +75 s flat to the +105 s kill; two cdb dumps |
| Feed, safe pipeline defaults | same binary, `PALMETTO_PIPELINE_LIVE_CAP=0`, `PALMETTO_PIPELINE_LAZY_BUILD=0` | both on | draws 100/101 | three attempts, all terminated before the frame loop at the same pre-device point (section 2.1); no feed-on result obtainable from the opt-out path |
| Control | previous slice's frozen binary, SHA-256 `E5396B47…`, recorded in `D:\leonida-work\vfeed\boot-control` | both off | draws 4000/4001 (a later repetition of the same stationary frame) | ran its window; gives the unfed state at the same draw pair |

The Feed and Control arms ran the same translated frame modules at the armed draws (`fspx/lib_vs_00821` +
`fspx/lib_ps_00820` for the constant pass, `fspx/lib_vs_00823` + `fspx/lib_ps_00822` for the compose pass), the same
input layout (stride 24, 170 records, 4080 B), and the same one root parameter (a descriptor table at `s0`), so the
draw states are directly comparable. The freeze is a race: it happened when a title thread was inside a pipeline
registration at the moment a fed draw's prepare took the nested lock. The earlier arms that survived
([vertex_constant_feed.md](vertex_constant_feed.md) section 5, "both feeds, early readback") are the same race won.

### 2.1 The safe-default arm did not produce a clean boot

The lead flipped the lazy live-pipeline cap's defaults to the no-cap/no-lazy path and asked for the feed arm on that
path. Three attempts with the same frozen binary and `PALMETTO_PIPELINE_LIVE_CAP=0`,
`PALMETTO_PIPELINE_LAZY_BUILD=0` ended before the frame loop:

* attempt 1 (`boot-feed-safe`): the process ended at +64.5 s, mid-line inside a slot-57 record conversion (stderr
  ends mid-record, 1.96 MB printed; no AppCrash report for that binary in the Application log, so an external
  termination cannot be excluded);
* attempt 2 (`boot-feed-safe2`): the process ended at +33.6 s after the pipeline timeline stopped at +25 s (the
  +30 s line is missing, so the process was already stuck or gone by then); the Application log has a
  `RADAR_PRE_LEAK_64` event for the binary at that time and no AppCrash report;
* attempt 3 (`boot-feed-safe3`, 120 s cap): the process ended at +33.0 s with stderr at exactly the same 101,757
  bytes as attempt 2 and the same last line (the +25.0 s timeline), i.e. the same deterministic stop point; no
  frame-loop submission ever starts (`submits 0`, `presents 0`).

The two identical attempts show that the opt-out path itself stops deterministically at about +26 s in this tree -
before the device has converted a single graphics PSO - so it is not a usable measurement vehicle for the feed
question (that is a separate finding for the pipeline-budget/lazy owner). No arm reached the armed draws, so this
arm neither reproduces nor refutes a feed-only freeze on the safe-default path; the clean answer needs the fixed
binary built from the tree after the `heap_base()` fix and the default flip, run with the feeds on. That
verification belongs to the lazyfix slice (the fix, its regression test and the independent reproduction are
recorded in [memory_growth.md](memory_growth.md) sections 2.1 and 7).


## 3. The frozen boot

Measured facts from `D:\leonida-work\feedstall\boot-feed\`:

* **Counters:** the pipeline timeline is flat from +75 s to +100 s: `submits 106`, `presents 52`,
  `console calls 107438`, `threads 99`-`100`. The counters before the freeze show a normal loading phase and a
  frame loop that had just begun (`slot57 calls 10241` converted 10241 refused 0; `graphics created 51786`;
  `compute calls 140 created 121`).
* **Memory and CPU:** working set 31,640.9 MB and private 37,768.3 MB from +79 s to the kill, exactly flat; the
  process's total CPU advances 187.5 s -> 187.9 s over the 21 s between the two dumps (the title does no work and
  does not spin).
* **Threads:** 99-100 threads, 94-99 with a title image frame over the freeze, exactly the parked-thread signature
  the earlier measurement recorded.
* **Last API calls:** after the frame's two lists are closed, the title resets the next list and records
  `SetDescriptorHeaps` and `ID3D12GraphicsCommandList7::Barrier` (the trace's last three records are `Reset ok`,
  `SetDescriptorHeaps called`, `Barrier called`); the `ExecuteCommandLists` for the closed two-draw list is then
  entered and never returns (it prints only on exit), and the replay thread's last printed state is the armed
  draw's user data (section 7).
* **No failure line:** no `ExecuteCommandLists failed`, no `vkQueueSubmit failed`, no `device lost`, no exception
  and no crash report for the boot.

## 4. Validation and driver messages

* **Validation:** `VkLayer_khronos_validation.dll` is mapped into the process (cdb `ModLoad` list), the device was
  created with `VK_LAYER_KHRONOS_validation` and the synchronization-validation feature (the boot's device lines
  show the device name and feature set, not a fallback), and over the whole 4.67 MB stderr - loading phase, the
  frame loop and the frozen minutes - there is **no** line containing `Validation Error`, `Validation Warning`,
  `VUID`, `vkCreateDevice` or `vkAllocateMemory` failure. There is no descriptor/environment message at all.
* **Driver:** the Windows System log contains no display-driver event (no TDR/device-lost entry) after the boot
  started; `gpu_events.txt` records "(no matching System events)". The cdb module list names the ICD
  (`nvoglv64.dll`, on this machine an NVIDIA GeForce RTX 4090) and the validation layer; the driver's own worker
  threads are parked in their normal waits.
* **Conclusion:** nothing on the GPU side reports a fault. The freeze is host-side: a wait with no wakeup.

## 5. cdb at the freeze

Two attaches with `cdb -p <pid> -c "~*k; .detach; q"` - at 84.4 s (freeze detected at +75 s) and at 99.6 s - 6 s
and 8 s per attach, both returning exit 0. The frozen binary's PDB was symbolised with llvm-symbolizer. Both dumps
have 99 threads and the **same** thread ids, states and wait sites (module offsets are the loaded image's, first
dump).

**5.1 The submission (replay) thread.** This is the title's render thread, inside its `ExecuteCommandLists`:

```text
ntdll!NtWaitForAlertByThreadId+0x14
ntdll!RtlpAcquireSRWLockSharedContended+0x11b
ntdll!RtlAcquireSRWLockShared+0x5e
palmetto_stall+0x477f2c   VulkanContext::heap_base()                     port/src/vulkan_backend.h:1758
palmetto_stall+0x3f43fd   <Device lambda_10> (read_device_memory callback) shims/d3d12_vulkan_device.cpp:2561
palmetto_stall+0x499255   frame_dump::print_fetch_slots_table            port/src/vulkan_replay.h:1669
palmetto_stall+0x36e0e7   detail::prepare_draw                           port/src/vulkan_replay.h:3481
palmetto_stall+0x36926d   detail::prepare                                port/src/vulkan_replay.h:4536
palmetto_stall+0x4699e1   VulkanQueue::execute_now                       port/src/vulkan_backend.h:2994
palmetto_stall+0x468b73   VulkanQueue::execute                           port/src/vulkan_backend.h:2524
palmetto_stall+0x4f7e1a   CommandQueue::ExecuteCommandLists              shims/d3d12_com.cpp:1845
palmetto_stall+0x18bcd1   d3d12_xs::child_slot<0,35>
title image +0x247fe6d  -> +0x221c29c -> +0x2215eca (title frames)
```

The thread is in the armed draw's prepare (the vertex-path diagnostic's fetch-slot read of the driver block), which
runs under `execute_now`'s `std::shared_lock resource_lock(context_.resource_mutex_)` (vulkan_backend.h:2849). The
first two dwords of that prepare, the fed user data, are printed before the deadlock (section 7), so the fed state is
not in doubt.

**5.2 The writer threads.** 12 threads, all with the same stack shape, are waiting to acquire the same mutex
exclusively (the count and tids are identical in both dumps):

```text
ntdll!NtWaitForAlertByThreadId+0x14
ntdll!RtlpAcquireSRWLockExclusiveContended+0x14b
ntdll!RtlAcquireSRWLockExclusive+0x4d
palmetto_stall+0x4227f8   std::unique_lock<std::shared_mutex>::{ctor}   MSVC <mutex>:145
palmetto_stall+0x440ce2   VulkanContext::add_pipeline                    port/src/vulkan_backend.h:1763
palmetto_stall+0x3d3f7c   PipelineState::create_compute                  shims/d3d12_vulkan_device.cpp:6700
palmetto_stall+0x25b86d   Device::CreateComputePipelineState             shims/d3d12_vulkan_device.cpp:9058
palmetto_stall+0x1fc4cf   d3d12_xs::proxy_create_compute_pipeline_from_native_code  d3d12_xs_device.cpp:5996
palmetto_stall+0x1fdcc5   d3d12_xs::proxy_slot<62>/<67>                  d3d12_xs_device.cpp:6450/6460
title image +0x247fe2a -> +0x247fd11 -> +0x221c29c -> +0x2215eca
```

**5.3 The rest.** The launcher thread waits for the game thread to exit (`rt::process::run_entry`), the title entry
thread is in its own long wait on an event (the parked-entry signature of [wait_forensics.md](wait_forensics.md)),
the Vulkan fence dispatcher and the pipeline-timeline thread are in their normal loops, and the driver's worker
threads are parked. No thread is inside `vkQueueSubmit`, `vkQueueWaitIdle` or a GPU-side fence wait, and no thread
is spinning.

**5.4 The invariant.** Both dumps, 15.2 s apart, have the same 99 threads and the same two wait sites: the submission
thread in the nested shared acquire and the 12 writers in the exclusive acquire. That is a deadlock, not a slow
frame.

**5.5 Independent reproduction.** The same deadlock was reproduced from the other side by the memory-growth slice
(task-84): on the pre-fix build, with the same diagnostics, cdb all-thread dumps at +55 s and +85 s in
`D:\leonida-work\memgrowth\debug\hangdbg\` show the replay thread in
`execute_now -> prepare -> prepare_draw -> write_device_memory -> heap_base()` (the feed's write callback, exactly
the path section 6 names) and title threads queued exclusive in `add_pipeline`, `add_graphics_pipeline` and
`set_pipeline_handle`; the boot was frozen at 48 submits / 23 presents from +50 s with 2 live / 2 built pipelines.
That is the same reader-writer cycle with the graphics-pipeline registration variants as writers, and it is recorded
in [memory_growth.md](memory_growth.md) sections 2.1 and 7.

## 6. Cause

`resource_mutex_` is a `std::shared_mutex` read-locked (shared) by `VulkanQueue::execute_now` for the whole replay
and written (exclusive) by every pipeline registration. The replay's device-memory callbacks need the GPU address
arena's base, and obtain it through `VulkanContext::heap_base()`, which takes **another shared lock on the same
mutex**. On MSVC's SRW-lock-backed `std::shared_mutex`, a shared acquisition from a thread that already holds shared
is allowed only while no writer is queued (SRW locks are writer-preferring to prevent writer starvation). When a
pipeline registration queues as a writer:

1. the writer waits for the reader(s), including the replay thread's outer shared lock;
2. the replay thread waits for the writer, at its nested shared acquire;
3. neither can progress: the submission thread is the one that would release the outer lock.

The feed selects the path: the driver-block feed writes the vertex-stream V# through `write_device_memory` on every
fed draw, from inside the submission, so the nested acquire is taken while the frame loop runs. The console-only
arm never writes device memory from the submission, and the control arm never does either, so neither can form the
cycle. Earlier the same code path was reached only by diagnostics (`read_device_memory`), which is why the freeze
looked intermittent: it needs the writer to be queued at the moment of the nested acquire, and that is a race
against the title's own pipeline creation.

This also reconciles the other freezes seen in the same slicing window: the current tree's default (lazy live-pipeline
cap) makes pipeline registration happen from the frame loop as well, so it supplies exclusive waiters at the exact
moment the replay holds the shared lock - the ~105-submission freeze. The lazy path is a *supplier of the writer*,
not the hang: the hang is the nested acquire above, and it also explains the earlier 154/76 feed freeze (feed write
callback) and the PS-control arm's freeze at the same submission (the readback-armed prepare reaches
`read_device_memory` for the matrix/fetch diagnostics before it reaches the substituted-pipeline build).

## 7. Fed state versus control at the same draw

Both arms' armed draws are the frame's constant pass (`fspx/lib_vs_00821` + `fspx/lib_ps_00820`, 3840x2160, one
root parameter, a descriptor table at `s0`), with the same bound view (`vb0` 4080 B, stride 24, 170 records). The
layout and provenance dumps are identical; exactly the user-data dwords and the descriptor the vertex/pixel stages
read differ:

| state | Control (feeds off) | Feed (both on) |
|---|---|---|
| recorded user data `s0`-`s1` | `6e20ef00 0000019e` = title VA `0x19e6e20ef00`, the descriptor table (PC packing) | `787fef00 00000190` = title VA `0x190787fef00`, the descriptor table (PC packing) |
| recorded `s8`-`s9` | `0 0` (console slot never written) | `0bfaef00 0` = arena-relative VA of the same table (console feed) |
| emitted `s0`-`s1` (what the shader receives) | `0c06ef00 0` = the table's arena VA after the title-VA rebase | `0d4e2800 0` = the synthesised driver block's arena VA |
| emitted `s8`-`s9` | `0 0` | `0bfaef00 0` = the same table through the console slot |
| fetch the vertex stage performs | the table's first V#: address `0x76ff20`, stride 0, records 96, format 20 (the matrix-A descriptor, not a vertex stream) | the driver block's V#: the boot's first fed frames log `wrote V# address 0x6ef30 stride 24 records 170 format 22`; at the armed draw the same write path encodes the bound view's own arena offset (`vb0` resolves to `0x6aef30`) with the same stride/records/format |
| bound view's resolved buffer offset | `0x76ef30` | `0x6aef30` (this frame's view) |
| fetch the constant pass's pixel stage performs | none (0 image samples; the PS reads the table's first dwords) | the same table through `s8` (0 T# keys, 8 V# keys; the table is the title's own) |
| driver block / feed messages | `driver block: none (the recorded s0/s1 pair is emitted; the console-layout s8 dword is unset)` | `vertex stream descriptor: … wrote V# address 0x6ef30 stride 24 records 170 format 22 (dwords 0006ef30 00180000 000000aa 00016fac)`; `graphics console root: table title VA … -> arena-relative … at s8` |

Validity of the fed values, checked against the pipeline:

* both emitted addresses are arena offsets produced by the layer (`arena_allocate(64, 256)` for the driver block, the
  title-VA rebase for the table), so `heap_base + address` lands inside the arena;
* the driver-block V# the feed writes takes its address from the layer's bounds-checked `buffer_at(view.va, size)`
  resolution and its record count from `size / stride`; the logged decode for the frame's first fed draws is stride
  24 / records 170 / format 22, exactly the shape the bound 4080-byte view implies, so the vertex fetch names the
  draw's own stream; the same path runs for the armed draw with that draw's offset (`0x6aef30`);
* the console `s8` dword is the arena VA of the same descriptor table the title wrote (the frame dump reads 64 slots
  at the title VA and finds the same V# words), so the pixel stage reads the title's table, not a heap mismatch;
* no validation message, no invalid or out-of-range descriptor, no unbound resource: the fed state is consistent
  with the pipeline. The only broken thing at the freeze is the host-side lock.

## 8. Required fix

Make `VulkanContext::heap_base()` lock-free. The value is written once, by `set_heap_base()` (vulkan_backend.h:1381),
from the device's init (`shims/d3d12_vulkan_device.cpp:8296`), before any submission exists; the accessor's shared
lock buys nothing and is what re-enters the mutex. Two minimal shapes:

1. `std::uint64_t heap_base() const { return heap_base_.load(std::memory_order_relaxed); }` with
   `std::atomic<std::uint64_t> heap_base_` set by `set_heap_base()` (or, equivalently, return
   `replay_.heap_base` with a comment that it is set before the first submission); or
2. pass the arena base into the device-memory callbacks when they are registered, so the replay's callbacks never
   call back into `VulkanContext` for it.

Then audit the other `resource_mutex_` accessors reachable from inside `execute_now`/`prepare`
(`has_resource`, `resolve_acceleration_structure`, `query_heap_type`, and any future callback) for the same
re-entry; the invariant is that nothing called from a replay callback may take `resource_mutex_`.

Regression proof: a test that runs a submission whose prepare calls `read_device_memory`/`write_device_memory` while
another thread registers a pipeline, under `PALMETTO_VK_VALIDATION=1` (core + synchronization validation), and
requires the submission to complete with no validation message; plus a boot with both feeds on, which must reach the
armed readback instead of freezing.

The fix belongs to the slice the lead routed it to (the lazy/live-cap owner, task-97), which now owns implementing
it, the lock-order regression test and the verification boots; it is recorded in
[memory_growth.md](memory_growth.md) sections 2.1 and 7 together with the independent reproduction. This slice
changed no runtime code - the tree is untouched by it except this page; the two safe-default feed-on arms described
in section 2.1 were run to answer the lead's feed-on question but did not produce a usable boot.

## 9. Limits

* The freeze is a race: this measurement caught it on the current tree, but an arm can win the race and run; a
  surviving boot is not evidence that the deadlock is gone.
* The armed diagnostics themselves make the read callback reachable at the armed draw, so this boot's particular
  nested acquire is the vertex-path diagnostic's; the feed's write callback reaches the same accessor on every fed
  draw even with the diagnostics off. The fix removes both, and the diagnostics stay read-only.
* The state delta is measured at the frame's constant pass; the compose pass's state (the same table through `s8`,
  matrix A slot empty, 6 image samples) is recorded in the same logs but its draw was not reached before the freeze.
* The frozen binary is the current tree plus other slices' uncommitted work at 15:25 (the boot metadata records the
  exact files and hashes); the feed code itself is the previous vertex commit. The control arm is the previous
  slice's frozen binary (recorded in its own evidence): its state is the same code path the current tree produces
  with the feeds off, and its boot ran with the same diagnostics armed. A current-tree control boot would not add a
  different state, but with the diagnostics armed it can take the same deadlock race as the feed arm; the control's
  unfed state is what section 7 compares.
* The GPU is this machine's Vulkan device (an NVIDIA GeForce RTX 4090, ICD `nvoglv64.dll`); the mechanism is a host
  mutex and is driver-independent.

## 10. Evidence

External, not in the repository:

* `D:\leonida-work\feedstall\boot-feed\` - the frozen boot: `boot\stderr.log` (4.67 MB, all diagnostics and the
  deadlock's last state), `boot\samples.csv` (the flat memory/CPU/threads series), `boot\boot.jsonl` (d3d12 trace),
  `boot\gpu_events.txt` (no driver event), `boot\arm_meta.txt` (binary and file hashes, git status), `cdb\dumps.csv`,
  `cdb\dump-84,4-freeze.log`, `cdb\dump-99,6-confirm.log` (the two all-thread dumps), `cdb\symbolized-84.txt` and
  `cdb\symbolized-99.txt` (both dumps with our frames symbolised).
* `D:\leonida-work\vfeed\boot-control\` - the control arm's boot and state at the same frame content.
* `D:\leonida-work\feedstall\boot-feed-safe\`, `boot-feed-safe2\`, `boot-feed-safe3\` - the safe-default feed-on
  attempts of section 2.1 (`boot\stderr.log`, `boot\samples.csv`, `boot\gpu_events.txt`; attempts 2 and 3 stop at
  the same 101,757-byte stderr).
* `D:\leonida-work\memgrowth\debug\hangdbg\` - the memory-growth slice's independent cdb dumps at +55 s and +85 s of
  the same deadlock (section 5.5).
* `D:\leonida-work\feedstall\boot_stall.ps1` and `grab_boot.ps1` - the harness (boot slot per boot, freeze detection
  from the pipeline timeline, two cdb attaches at the freeze, an optional timed cdb dump); `analyze_stderr.py`,
  `symbolize_all.py` - the state extractor and the symboliser.
