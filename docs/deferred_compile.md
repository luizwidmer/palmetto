# Deferred, off-thread pipeline compilation: unstarving the title's task-pool threads

**Status: measured; opt-in and off by default.** This page records the measured starvation of the title's own
task-pool: worker threads enter the Vulkan layer's synchronous compute-pipeline creation and block inside the NVIDIA
shader compiler (`nvgpucomp64`), so the pool that should run the stage-2 holder's work items cannot. It then describes
our own deferred compile: the pipeline state is registered at creation and one of the layer's own compile workers makes
the driver call, while the submission that first names the pipeline waits for it on the submitting thread, never on a
title worker. The switch is `PALMETTO_PIPELINE_DEFER_COMPILE=1`; unset is the eager path exactly. The A/B pair, its
numbers and the honest limits (including what the switch does and does not move) are in section 5.

All numbers are from this machine (NVIDIA RTX 4090, Vulkan 1.3 driver), one boot at a time under the repository's boot
lock, harness `tooling/boot_report.py`, 130 s. The captured logs and the capture folders are under the local work
folder (`D:\leonida-work\asynccompile`); nothing of the game is in the repository.

## 1. Why this slice: the measured starvation

[docs/stage2_blocker.md](stage2_blocker.md) section 15 measured the state the stage-2 holder is in, with two cdb
all-thread dumps 40 s apart in one 130 s boot:

- the holder is **job 109**; its stage function posts **13 work items** of `0x30` bytes into the title's own lane-0 /
  class-4 task-pool ring (post RVA `0x52718B0` -> ring push `0x4DA41A0` -> wake `0x4DA4200`) and then polls their
  completion counter in `while (counter != 13) { SleepEx(1); tick; }` (poll at RVA `0x1DEE6C0`, sleep call at
  `0x1DEF278`);
- the counter is written only by those items' own executor body (RVA `0xFA8740`), so **no runtime event is missing** -
  the completion is produced by the title's own task-pool items;
- at both dumps **11 title worker threads** of a different batch are inside our shim's synchronous compute-pipeline
  creation: `proxy_slot<62>` -> `Device::CreateComputePipelineState` -> `PipelineState::create_compute` ->
  `vkCreateComputePipelines` -> `nvgpucomp64`, with the same thread ids, the same stack and the same stack pointer at
  +55.0 s and +95.1 s, so those calls had not returned for 40 s;
- **none of job 109's 13 items executes**, the ready set stays frozen (pending `0x69`, one ready bit, aggregate = job
  109's 21-bit mask), no stage-3 request is made, and the boot kills at 130 s.

In one sentence: the title's worker threads are the pool that must run the 13 items, and our synchronous pipeline
compilation has taken 11 of them out of that pool, so the pool cannot run the items. This slice moves the driver
compile off those threads.

Section 15's limits apply to the claim as well: the two snapshots are stationary samples, they did not trace the
pushes, and they did not prove the 11 compile calls would never return. The measurement below (section 2) replaces
that snapshot with per-caller numbers from the runtime itself, and the A/B boot (section 5) tests the causal step.

## 2. Measured shapes

### 2.1 The pipeline-creation call sites

The title reaches the PC device through the console device proxy. The compute paths that end in a driver pipeline
compile are:

| path | console call | layer code | driver call |
|---|---|---|---|
| direct compute PSO | device slot 62 (`CreateComputePipelineState`) | `Device::CreateComputePipelineState` -> `PipelineState::create_compute` | `vkCreateComputePipelines` |
| deserialized compute PSO | device slot 67 (`DeserializeComputePipelineStateX`) | the same `create_compute` through `LoadComputePipeline` | the same |
| pipeline-state stream | PC `ID3D12Device2::CreatePipelineState` with a CS stage | `create_compute` | the same |
| graphics PSO | device slots 55/57 -> `CreateGraphicsPipelineState` | `create_graphics` (built at its first bind since [docs/memory_growth.md](memory_growth.md)) | `vkCreateGraphicsPipelines`, on the recording thread at the bind |

One 130 s boot makes **51,786 graphics creation calls**; in the eager arm it reaches **462 compute creation calls**,
and the deferred arm (where creation no longer blocks) had already passed **4,825** compute creations when it stopped.
With the committed lazy-build default the graphics driver calls happen at the first bind, not at creation: the measured
eager boot made **2 graphics driver calls (3.1-5.0 ms)** and **403 compute driver calls**. So the driver calls that block
a caller in the compiler are the compute ones, and the compute burst is far larger than the earlier
[docs/pipeline_cache.md](pipeline_cache.md) boot measured (95) - that boot did not yet create the loader's compute
pipelines. [docs/pipeline_cache.md](pipeline_cache.md) still has the font/layout/module cost split, which this slice
leaves on the creating thread.

### 2.2 The new per-caller counters

`PALMETTO_PIPELINE_CALL_STATS=<seconds>` (measurement only, off by default) prints two cumulative report lines every
`<seconds>` (default 5) plus one line per caller thread:

- **creation scope** around `create_graphics` / `create_compute`: how many `Create*PipelineState` calls each thread
  made and the layer call's wall time;
- **driver scope** around `vkCreateGraphicsPipelines` / `vkCreateComputePipelines`: how many driver pipeline calls each
  thread made, their total and maximum wall time (the compiler time), and a duration histogram
  (`<1 ms`, `<10 ms`, `<100 ms`, `<1 s`, `<10 s`, `>=10 s`);
- the largest number of creation calls and of driver calls in flight at once - the starvation measurement;
- a compile-worker thread names itself `[compile worker]`, so the after arm's driver calls are distinguishable from the
  title's callers.

The counters are the runtime's own; the thread ids are `std::thread::id` hashes, not game addresses.

### 2.3 Baseline numbers (A arm, switch off)

The full table is in section 5. In short, the baseline boot shows exactly the shape section 15 saw as a snapshot:
**12 title threads are inside pipeline creation and 12 are inside the driver call at once; 13 distinct threads spent
time in a driver pipeline call; 403 compute driver calls took 728.0 s of blocked thread wall time**, with **8 calls over 10 s** (the
longest 92.4 s) - while the compute work is spread over a small number of calls, not a burst of cheap ones. The title
pool is therefore 13 threads of its own work down. The stage-2 probe freezes in the same state at +27.9 s and +70.3 s:
pending `0x69` (105), one ready bit (id 152), the 21-bit aggregate equal to job 109's claim, and job 109's `f0` still 4
(inside its work function).

## 3. What we implemented

### 3.1 Design

`port/src/deferred_compile.h` (new, GPU-free) contains two pieces:

1. **`CompileQueue`** - a bounded pool of compile workers. `submit` returns a task or `nullptr`; `nullptr` means "the
   caller takes the synchronous path" (the switch is off, the pool was never started, or the queue is at its bound, so
   nothing is ever dropped). A task moves `queued -> building -> done/failed`; `wait` blocks only the caller that waits;
   `cancel` lets a queued task be skipped; `stop` joins the workers after the queued tasks have run (a queued task owns a
   pipeline state's reference, so dropping it would leak that reference).
2. **`PipelineCallStats`** - the per-thread counters of section 2.2, with RAII `Scope`s so the default path pays one
   null-pointer branch per creation.

`PipelineState::create_compute` is split at the driver call:

- the recipe, program, shader module, pipeline layout, subgroup decision, code base (the host copy of the embedded
  constant tables) and the replay-visible `ComputePipeline` entry are all built **on the creating thread**, as before -
  everything that validates the request and can refuse it stays synchronous, so `CreateComputePipelineState` still
  returns the same refusals at the same point;
- with the switch on, the state is registered in the replay map with a null `VkPipeline`, and a task is queued. The task
  holds a COM reference to the pipeline state (and through it a device reference), so the module and layout it reads
  cannot be freed under it;
- the one worker calls `vkCreateComputePipelines` with the device's existing `VkPipelineCache`, then sets the handle on
  the state and publishes it into the replay map through `VulkanContext::set_pipeline_handle` (the same publication the
  lazy graphics path uses, under the resource lock);
- a driver failure marks the task failed and logs once; the submission that needs that pipeline is then refused by the
  replay (`unresolved_resource`), the same honest outcome as any unresolved pipeline.

**The wait is at submit, not at record.** `VulkanContext` gained a `set_prepare_pipelines` hook, called from
`VulkanQueue::execute_now` *before* the replay takes the resource lock and prepares the lists. The device's hook scans
the submitted lists for the pipeline ids a dispatch actually names (`Dispatch`, and an `ExecuteIndirect` whose kind is a
dispatch) and waits only for ids that are deferred. So:

- a title worker recording a command list never waits;
- a submission that names no deferred pipeline never waits;
- a submission that does name one waits on the thread that executes the batch (the render/submit thread), which is
  exactly where a D3D12 runtime is allowed to wait.

This is the small hunk coordinated with the card-copies slice, which owns `port/src/vulkan_backend.h`: the accessor,
its member, and one call after `context_.tick_page_watch()`. `port/src/vulkan_replay.h` is not modified.

### 3.2 Switches

| switch | effect |
|---|---|
| `PALMETTO_PIPELINE_DEFER_COMPILE=1` | opt-in. Compute pipeline states are created without the driver compile and a compile worker builds them; the submission that names one waits for it. Unset: the eager path exactly (every compute PSO compiles inside `create_compute`). |
| `PALMETTO_PIPELINE_COMPILE_WORKERS=<1..8>` | the worker count, default **1**. |
| `PALMETTO_PIPELINE_COMPILE_BOUND=<n>` | the most outstanding builds before a creation falls back to the synchronous path, default **256**, at least 1. Measured use in the boot: the 95 compute creations queue and drain; see section 5. |
| `PALMETTO_PIPELINE_CALL_STATS=<seconds>` | the per-caller report of section 2.2, default 5 when set, off when unset or `0`. Measurement only. |

The existing switches keep their meaning: `PALMETTO_PIPELINE_CACHE` (persistent `VkPipelineCache` file) is still the
cache the worker compiles into; `PALMETTO_PIPELINE_RECIPE_CACHE` still adopts an identical earlier description at
creation (that path stays eager); `PALMETTO_PIPELINE_LIVE_CAP` / `PALMETTO_PIPELINE_LAZY_BUILD` still govern the
graphics path.

### 3.3 Lifetime and deletion

- The task's COM reference keeps a pipeline state alive until its build has run; a title `Release` before that only
  drops the title's reference.
- The handle is written under a per-state mutex and read under it by the destructor; the destructor destroys the
  `VkPipeline`, layout and module exactly as the eager path does (no new deferred-deletion kind: the same D3D12 rule
  applies, and a state released while the GPU could still use it is a title error both paths share).
- The device's destructor stops the report thread and the compile workers before the Vulkan device goes. A queued task
  holds a device reference, so the queue cannot be non-empty when the device can be destroyed.
- The compile queue's map entry (pipeline id -> task) is erased when the pipeline state dies.

### 3.4 What is deliberately not deferred

- **Graphics pipelines.** They are already built lazily at the first bind under the live-pipeline cap
  ([docs/memory_growth.md](memory_growth.md)); the measured blocked family is the compute one, and re-plumbing graphics
  would duplicate that slice's lifetime and eviction rules.
- **The recipe cache's insertion.** A deferred build does not insert a recipe-cache entry (the shared entry also owns
  the module and layout, which this path keeps on the state). Adoption of an existing entry still happens eagerly at
  creation. With both switches on, the first description of each recipe compiles; there is no measured need for more in
  the A/B, and section 6 names it.
- **Shader-module and pipeline-layout creation** stay on the creating thread: they are cheap (measured 8.3% and 11.9%
  of the boot's pipeline time in docs/pipeline_cache.md, and none of it in `nvgpucomp64`) and keeping them there keeps
  the refusal points synchronous.

## 4. Tests

`port/test/deferred_compile_test.cpp` (new target `deferred_compile_test`, added to `_setup/build_tests.cmd`) is
GPU-free and tests the queue's state machine and the counters directly, with synthetic build closures:

| check group | what it proves |
|---|---|
| submit before start / after stop | the caller gets `nullptr` (the synchronous fallback) and the rejected counter moves |
| state before build | a submitted task is not finished while its build waits on a gate; `wait` returns only after the gate opens |
| failed build | a build that reports failure is `failed`, `wait` returns false, the failed counter moves |
| bound and fallback | at the bound the third submit is refused (not dropped), and a later submit is accepted once drained |
| two workers | two compiles run at the same time when two workers are started |
| cancellation | a queued, cancelled task never runs its closure and its waiter gets false |
| shutdown | `stop()` joins the workers and refuses later work |
| counters | per-thread creation/driver counts, total/max driver time, the histogram bucket, the concurrency maxima and the compile-worker marker appear in the report |
| report extension | a device-owned line (the queue's counters) appends to the report |

Run: `port\build\deferred_compile_test.exe` -> `deferred compile: ok (18 check(s))`.

The GPU-backed arms:

- `pipeline_recipe_cache_test` with `PALMETTO_PIPELINE_DEFER_COMPILE=1 PALMETTO_VK_VALIDATION=1`:
  `pipeline recipe cache stress: ok (0 validation error(s))` - deferred compute creation, release (including states
  released while a build may still be queued) and the shared-recipe path under `VK_LAYER_KHRONOS_validation`;
- the same test with the switch unset (the eager control) passes the same way;
- the boot A/B of section 5 is the runtime path and exercises the submit-side wait (the GPU-free tests cannot).

## 5. A/B measurement

Two 130 s boots, one at a time under the boot lock, **the same binary**, standard environment plus
`PALMETTO_PIPELINE_CALL_STATS=5`, `PALMETTO_STAGE2_READY=20`, `PALMETTO_STATE_TRACE=2,130` and the presenter capture
switch (every 6,000 presented frames). Arm A leaves the deferred switch unset (the default/eager path); arm B sets
`PALMETTO_PIPELINE_DEFER_COMPILE=1` and, for this pair, `PALMETTO_PIPELINE_COMPILE_BOUND=4096`
`PALMETTO_PIPELINE_COMPILE_WORKERS=4`.

### 5.1 The two arms

| measurement | A: eager (default) | B: deferred, bound 4096, 4 workers |
|---|---:|---:|
| verdict / exceptions | ALIVE 130.3 s, 0 | **STOP (0xC0000005) at 62.8 s** (process exits 72.9 s) |
| D3D12 calls traced | 249,148 | 175,979 |
| graphics creation calls / driver calls | 51,786 / 2 (5.0 ms) | 54,905 / 2 (3.7 ms) |
| compute creation calls / driver calls | 462 / **403** | 4,825 / **318** |
| creation threads / driver threads | 14 / 13 | 14 / 5 |
| driver time graphics / compute | 5.0 ms / **728,030.2 ms** | 3.7 ms / **4,782.9 ms** |
| longest single driver call (compute) | **92,416.4 ms** | **169.6 ms** |
| concurrent creation calls max / driver calls max | 12 / **12** | 11 / **4** |
| compute driver histogram `<1 ms`/`<10 ms`/`<100 ms`/`<1 s`/`<10 s`/`>=10 s` | 40/195/108/42/10/**8** | 24/178/108/8/0/**0** |
| deferred: submitted / built / failed / fallback / submit waits | 0 / 0 / 0 / 0 / 0 | 1,623 / 318 / 0 / 0 / 0 |
| pending queue at the last report | 0 of 256 | 1,305 of 4,096 (peak 1,385) |
| pipeline compile workers | 0 | 4 |
| stage-2 pending / ready bits / aggregate = holder 109 | wait `0x7c` -> steady `0x69` -> steady2 `0x69`; 1 bit (id 152); aggregate = 21-bit mask | wait `0x7c` -> steady `0x19`; 1 -> **0 bits**; aggregate nz 3 -> **0** |
| job 109 `f0` (4 = inside its work function) | **4** at both steady samples; holder id 109, aggregate equals its claim | not sampled at steady (the probe was inside its slow table dump when the process died; arm C below samples it) |
| stage word (machine current / next) | 2 / 2 at 128 s | 2 / 2 at 70.7 s |
| stage-3 request at `0x81B2EF` | not requested | not requested (the boot stops before) |
| submissions / PresentX calls / captured frames | 8,192 at 81.7 s / 5,211 in 130.3 s / 1 frame, mean rgb 0.00 0.00 0.00 | 4,096 at 29.3 s / 3,218 in 72.9 s / 1 frame, mean rgb 0.00 0.00 0.00 |

**Arm A reproduces the starvation exactly as a per-caller measurement.** 13 of the boot's 14 pipeline-caller threads
spent time inside a driver pipeline call and 12 were inside the compiler at once, 403 calls consumed 728.0 s of blocked thread wall time,
8 of them took over 10 s (longest 92.4 s), and the title's stage-2 job graph never moved: the probe found the same
frozen holder job 109 (`f0` = 4, inside its work function) at +27.9 s and +70.3 s.

**Arm B removes that starvation.** All 318 completed compute driver calls ran on the 4 compile-worker threads
(`[compile worker]` lines in the report) and **no title thread made a compute driver call**; the longest single call
was 169.6 ms, the driver total 4.8 s instead of 728 s, and at most 4 compiles were in flight. With the queue bound high
enough to take the whole burst, no creation fell back to the synchronous path. The loader moved: pending fell from
`0x7c` to `0x19`, the ready bit and the 21-bit aggregate cleared, the 4,096-submission mark came at 29.3 s against 81.7 s in the eager arm, and the
frame loop called PresentX 3,218 times in the 72.9 s the boot lasted (44/s against 40/s over the eager arm's 130.3 s).

**And then it stopped somewhere new.** At trace time 62.8 s a title task-pool worker faulted while running a work item:
`game_scarlett_master.exe+0x6EB759`, a read of address `0x10`, stack
`0x6EB759 <- 0x5B03EE5 <- 0x1EBA80B <- 0x3750CE9 <- 0x63E78 (job runner) <- 0x4EAA05A (item executor) <- 0x221C29C
(worker loop)`. No frame in that stack is the layer's: the dereference is title code reading a pointer that is null in
its own data. The same address and the same null read appeared in an independent earlier arm with the same switch
(different binary, 4 workers) at trace time 54.2 s, so the stop is deterministic once the loader gets that far, not a
scheduling accident. The eager arm cannot reach it because the loader does not move.

### 5.2 The default bound, and why the bound matters

An earlier matched pair on one binary tested the switch with its default bound (`PALMETTO_PIPELINE_COMPILE_BOUND=256`,
one worker) instead:

| measurement | eager | deferred, bound 256, 1 worker |
|---|---:|---:|
| verdict | ALIVE 130.4 s | ALIVE 130.4 s |
| compute driver calls / time | 403 / 740,495.6 ms | 395 / 993,711.9 ms |
| deferred submitted / built / fallback | 0 / 0 / 0 | 497 / 241 / **166** |
| threads in a driver call (creation threads / all threads seen) | 14 / 14 | 14 / 15 (13 title threads with fallback compiles + the worker) |
| stage-2 pending / ready bits / aggregate | `0x69` frozen, 1 bit, 21-bit mask | +22.2 s `0x7d`/1 bit -> +28.3 s **`0x1d`/0 bits/empty** |
| job 109 `f0` | 4 (inside its work function) | **0** (its poll had returned) |

That arm shows the mechanism from the other side: when the queue bound saturates, the late creations fall back to the
synchronous path on title threads, and those fallbacks contend with each other in the compiler (993 s of blocked
thread time against the eager arm's 740 s) - the pool is only 241 compiles better off. It still advances the loader
(job 109's poll returns, the aggregate clears, pending drops 105 -> 29 by +28.3 s) and it does not hit the new stop
within 130 s. Raising the bound (arm B) is what removes the fallbacks and the title-thread compiles entirely.

### 5.3 Verdict

- **Measured and confirmed:** the starvation is real, it is our synchronous compute-pipeline creation, and it is
  per-thread quantified: up to 12 title threads inside the compiler at once, 13 caller threads, 403 calls and 728 s of
  blocked thread time per 130 s boot.
- **Measured and fixed:** with the deferred switch on and a bound that takes the burst, no title thread enters the
  compiler, the engine's task pool is no longer stolen, and the stage-2 holder's poll returns - job 109's 13 items ran
  (its stage function only leaves the `while (counter != 13)` poll when all 13 completions have arrived), the aggregate
  claim is released and 76-99 pending jobs complete in the first half minute.
- **Not fixed, honestly:** stage 3 is still not requested within the window. With the whole burst deferred, the boot
  stops at a deterministic title-side null dereference on a task-pool worker (`game+0x6EB759`, read `0x10`) at ~63 s;
  with the default bound it stays alive to 130 s but the frozen stage-2 state is only partly relieved (some title
  threads still compile) and stage 3 is still not requested. This is a moved wall, not a stage advance: the next
  falsifier is that null dereference (its item's data is title-side and is not attributed by this slice), and a 300 s
  boot should follow only after it stops the boot. A moved stall or a negative was an accepted result and that is what
  the second half is.

## 6. Limits

- **A moved stall is possible.** If the title submits a dispatch that names a just-created deferred pipeline before
  the worker has finished, the submitting thread waits for the compiler instead of a worker thread. This is the design
  (the wait is where D3D12 allows one), but it means the slice can move a stall rather than remove it. In the measured
  arm the submit-side waits stayed 0 (`submit waits 0`), so no submission named a deferred pipeline there; the compiler
  wall moved to a title-side null dereference instead (section 5).
- **The bound decides everything under a slow driver.** The eager arm's single driver call reached 92.4 s, so with a
  finite bound the queue can saturate and the late creations fall back to compiling on the title's threads again - the
  measured default-bound arm (section 5.2) is that case: 166 fallbacks, 993 s of blocked title-thread time and the same
  166 fallbacks contend in the compiler. A deployment must set `PALMETTO_PIPELINE_COMPILE_BOUND` high enough for the
  measured compute burst; the default 256 is a safety bound, not a tuned value.
- **Some driver compiles are very long.** The driver compiles a few compute programs for 90+ s under contention; a
  single worker can be stuck in one while the rest of its queue waits (the measured arm still had 1,305 queued when it
  stopped). Workers are a switch (`PALMETTO_PIPELINE_COMPILE_WORKERS`, 1..8); the pair used 4 and no submitted dispatch
  waited on the queue in the measured window, but a workload that dispatches a heavy program early would wait.
- **A driver failure surfaces later.** A `vkCreateComputePipelines` failure is asynchronous now: creation succeeds and
  the submission that needs the pipeline is refused by the replay. The eager path returns E_FAIL at creation. Both are
  honest, but they differ in where a broken driver shows up.
- **Held batches.** A batch held by the queue's deferral machinery (a Wait for a fence no Signal has submitted) is
  replayed on the thread that submits the missing Signal, and the deferred wait runs on that thread. That is not the
  frame's submitting thread in the measured boot, but it is a possibility the design accepts.
- **The recipe cache is not populated by deferred builds** (section 3.4). With both switches on, identical compute
  descriptions adopt only entries the eager path created.
- **The new stop is not attributed.** The title-side null dereference at `game+0x6EB759` (read `0x10`) is reached only
  when the loader advances, appears at the same address in two independent deferred arms and has no layer frame in its
  stack. Whether it is a latent title bug, another runtime gap now exposed, or a consequence of this layer returning
  from creation before the driver build finished is **not** established here; that attribution (cdb on the worker's
  item, its data, and the job graph around it) is the next slice. A 300 s boot only makes sense after it stops the boot.
- **The completion counter itself is on the runner's stack.** The probe can read the job objects and the queue's masks but
  not the runner's 13-item counter; job 109 leaving its work function (0 4 -> 0), its 21-bit claim leaving the
  aggregate and 76-99 pending jobs completing are the readable evidence that the 13 items ran.
- **Bounded samples.** The stage-2 numbers are the probe's samples in each boot (+21.8/+27.9/+70.3 s in the eager arm,
  +23.5/+29.6 s in the deferred arm; the deferred probe was still in its slow table dump when the process died, so its
  job 109 `f0` was not sampled - the 5.2 arm supplies that sample). A later change inside one boot is not claimed.
- **One machine, one driver.** All numbers are one RTX 4090 / Vulkan 1.3 boot pair; the 92 s compile is this driver and
  these programs.

## 7. Cross-check against the reference (peer evidence, no code copied)

The public prior-art projects all have the property this slice adds: pipeline compilation does not block the title's
own threads. The evidence, recorded as reference only in [docs/remaining_mechanisms.md](remaining_mechanisms.md)
section 3.8 and [docs/cleanroom_peers.md](cleanroom_peers.md):

- **five-star-recomp** compiles translated shaders in a separate compile process (5-20x faster in its measurement, with
  a disk cache);
- **XCL** parallelises DXC compilation;
- **VIctory** translates ahead of time with a disk cache and worker threads;
- the public GPL-3.0 fork **ubiquitarianism/leonida** carries pipeline compile workers (`3de22a1904` / #1547, and the
  compile/checkpoint headers in `273b3bdf9` and `ed94fbef9`), landed there after its story load worked.

Our implementation is our own: a bounded in-process compile queue in our structure (`port/src/deferred_compile.h`), our
own counters, and a submit-side wait fitted to our replay and lock rules (`docs/memory_growth.md`'s recorded
resource-lock deadlock is why the wait is outside the shared lock). Nothing is copied from the reference tree. The
differences from the reference are deliberate and named above: we keep the driver call in-process (no separate compile
process), one worker by default, and the recipe cache is unchanged.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against public
projects recorded as reference in [docs/remaining_mechanisms.md](remaining_mechanisms.md) section 3.8 and
[docs/cleanroom_peers.md](cleanroom_peers.md): five-star-recomp's separate-process shader compilation and disk cache,
XCL's parallel DXC, VIctory's ahead-of-time worker/disk-cache translation, and the GPL-3.0 project
ubiquitarianism/leonida (https://github.com/ubiquitarianism/leonida), commits `3de22a1904`, `273b3bdf9` and
`ed94fbef9` (pipeline compile workers and compile checkpoints). No code from any of them was copied; the implementation,
its tests and this page are this project's own. The upstream projects remain the copyright of their contributors and
their licenses continue to apply to their own trees.
