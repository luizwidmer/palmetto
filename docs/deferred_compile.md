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

## 9. Attribution and fix: the stop after the task pool is unblocked (follow-up)

**Status: measured, attributed, and fixed in our structure; the boot then advances to a new wall.** The null at
`game+0x6EB759` is not the deferred compile's semantics and no pipeline handle or recording-context side effect is
missing: our console device's slot-133 handler answered the title's 128 KiB placed-buffer creation with
`E_OUTOFMEMORY` because the GPU address arena's 256 MiB default was full. The factory that builds the resource
returned null, the title cached it and dereferenced it. The arena default is now the measured need (1024 MiB), and
with it the `0x6EB759` stop does not occur: the title's own **loading word advances 1 -> 2 and the session word
0 -> 1 for the first time in any measured boot** (`docs/stage2_blocker.md` sections 11-15 never saw either), before a
new title-side null at `game+0x146F23D`. The off-thread compile stays as it is: it is the change that lets the
loader's stage run at all. The next stop is recorded in section 9.7 for a separate slice.

### 9.1 What ran

| | |
|---|---|
| Tree | the shared working tree after `375a9b14c` (the deferred-compile commit), with other slices' in-flight files present; the networking slice's commit `8b61b96ba` landed between the boots, and each boot's binary hash is in the Binaries row |
| Binaries | the shared tree rebuilt between boots; the measured builds are `31F6FC99...7F249F` (poke boot), `8C7AAC75...3C4B01` (plain 130 s, cdb and arena-probe arms) and `41F31C38...4A4288` (the fix build, 300 s boot) |
| Boots | five boots under the boot lock, one at a time: `boot-poke1` (in-process register probe, 130 s), `boot-plain1` (plain reproduction, 130 s), `boot-cdb1` (one debugger session, 130 s), `boot-arena4095` (`PALMETTO_ARENA_MB=4095` and `PALMETTO_ARENA_LOG=1`, 130 s), `boot-defer300` (after the fix, 300 s) |
| Harness | `tooling/boot_report.py`, timeout 130 or 300 s, captures outside the repository under `D:\leonida-work\postdefer\` |
| Environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_STATE_TRACE=2,130` (300 in the last boot), `PALMETTO_STAGE2_READY=20`, `PALMETTO_PIPELINE_CALL_STATS=5`, and the deferred arm `PALMETTO_PIPELINE_DEFER_COMPILE=1 PALMETTO_PIPELINE_COMPILE_BOUND=4096 PALMETTO_PIPELINE_COMPILE_WORKERS=4` |
| In-process probes | `PALMETTO_POKE_LOG=6DCE7,6DD8B,6DE0B,6DE1F` (registers and read windows at four title sites, no debugger), `PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1` (the crash report carries the faulting registers and the memory around them) |

### 9.2 The null source, decompiled

The crash stack is the one section 5.1 records; the job that runs is the stage job whose work function is the
10-byte thunk `game+0x3750CE0` (section 13's ready ids are the same stage batch). Top-down:

| # | RVA | size | what it does |
|---|---|---|---|
| 1 | `0x3750CE0` | 0x10 | calls `0x593BD40` and returns 1 (the job's work function) |
| 2 | `0x593BD40` | 0x1C | calls `0x593BD60`; when that returns false it **tail-jumps to `0x1EB9FD0`**, the loading-stage body |
| 3 | `0x1EB9FD0` | 0x851 | the stage body; calls the resource-registry init `0x5B03CB0` and returns at `0x1EBA80B` |
| 4 | `0x5B03CB0` | 0x4F4 | registry init: named registrations through `0x8E8930` (hashes `0x964deeb7`, `0x9237d37a`, `0xd00e950b`, ...) and the renderer's first buffers. Three buffers come from `0x141D47DA0` plus the view builder `0x6EB740`; then the resource factory `0x6EBAA0(0x8000,4,1,0,0x90,0,0x27,0)` is called at **`0x5B03EBC`** and its result is passed to `0x6EB740` at **`0x5B03EE0`** (the fault) |
| 5 | `0x6EBAA0` | 0x142 | resource factory: allocates the engine record (`0x52A40A0`), fills its description (`0x320090`), calls `0x6DC20` for the GPU resource; on failure frees the record and returns **null** |
| 6 | `0x6DC20` | 0x23E | allocates the title-side memory (`0x142474690`: `0x8000 * 4 = 0x20000` bytes, 16-byte aligned), builds a 0x30-byte **buffer** description (Dimension 1, Width `0x20000`, Height/Depth/Mips 1, Format 0, SampleDesc 1, Xbox flag word `0x20004` read as PC flags 0x4) and calls **console device slot 133** at `0x6DD85` through `[device+0x428]` with (va, desc, state `0xFFFFFFFF`, 0, riid, out) |
| 7 | `0x6DC20` failure | - | a negative HRESULT at the slot call takes the branch at `0x6DD96` to `0x6DE0B` and the shared null tail `0x6DE1F`, so the function returns 0 and `0x6EBAA0` returns null |
| 8 | `0x6EB740` | 0x170 | the view builder the null is passed to: `test byte [rbx+0x10],4` is its first use of the object, and `rbx` is the null, so the read is of address `0x10`. The null check above it (`0x6EB750`) jumps to an `int3` at `0x6EB888` - the title's own assert, stepped over by `PALMETTO_SKIP_INT3` - and the code then jumps back to the dereference, which is why this stop is a read of `0x10` and not the assert |

So the null object is the engine resource wrapper that `0x6EBAA0` returns, and the only way that function returns
null after its allocation succeeded is `0x6DC20`'s resource creation failing.

### 9.3 The producer, measured with the in-process register probe

`boot-poke1` (binary `31F6FC99...`, deferred arm, pokes on the title's own code; the crash report fired at trace
53.3 s at the same RVA):

| poke site (RVA) | what the registers say there | hits | the failing creation's values |
|---|---|---|---|
| `0x6DCE7` | immediately after `0x6DC20`'s engine-memory allocation (`rax` = the allocation) | 32 | allocation **non-null**; record `+8 = 0x8000`, `+0xC = 4`, `+0xE = 0x90`, `+0x10 = 0x1385` |
| `0x6DD8B` | immediately after the console slot-133 call (`rax` = HRESULT) | 3 | first two `S_OK`; the third **`0x8007000E`** (`E_OUTOFMEMORY`) with the same record |
| `0x6DE0B` | the slot-failure branch, taken only when the HRESULT is negative | 1 | `rax = 0x8007000E` |
| `0x6DE1F` | the shared failure tail | 1 | - |

The same boot's stderr has the reason:

```text
[rt] d3d12: GPU address arena full (raise PALMETTO_ARENA_MB); buffer size in KiB: 128
```

`0x8007000E` is our slot-133 handler's answer, and the line says why: a 128 KiB placed buffer whose **host import was
unavailable** (the documented stand-in path of [docs/replay_resolution.md](replay_resolution.md) section 5 needs an
arena range for its ordinary-buffer stand-in) could not reserve its range, because the 256 MiB arena was full. The
title therefore received a null resource and dereferenced it. Crash-report registers at the fault on that build:
`rcx`, `rbx`, `rdi`, `r8` and `rax` are all 0, `rdx` equals `rsi` and is the caller's descriptor local, `rbp` is
`0x1ff`, and the stack is section 5.1's.

### 9.4 Causality: the arena size, measured

`boot-plain1` reproduces the stop on the shared tree's later binary (trace 123.5 s, wall 139.0 s, 147,272 D3D12
calls) with the same arena-full line. Raising only `PALMETTO_ARENA_MB` to 4095 on the same binary
(`boot-arena4095`) removes it and measures the need:

| measurement | default arena (`boot-plain1`) | `PALMETTO_ARENA_MB=4095` (`boot-arena4095`) |
|---|---:|---:|
| verdict | STOP `game+0x6EB759` reading `0x10` at trace 123.5 s | STOP `game+0x146F23D` reading `0x30` at trace 107.2 s |
| `GPU address arena full` lines | 1 (the 128 KiB buffer) | **0** |
| arena allocations / peak end / refusals | not logged | 2,963 / **0x1F5D8C10 = 501.8 MiB** / **0** |
| loading word / session word | loading 1 / session 0 up to the stop | **loading 2 / session 1 at 106.0 s**, the first change of either in any measured boot |
| machine word (current / next) | 2 / 2 | 2 / 2, so the stage-3 request at `0x81B2EF` is still not written |

The arena peak is the measured need at the furthest point this slice reaches: **501.8 MiB** against the old
256 MiB default. That is the whole causal chain: the deferred compile lets the stage finish (section 5), the stage
creates placed-buffer stand-ins (section 9.2), the 256 MiB arena fills (this section), and the next creation is
refused.

### 9.5 The fix

- `port/src/arena_size.h`: `kArenaDefaultMiB` changes from 256 to **1024** (the measured 501.8 MiB peak plus
  margin), with the measurement in the comment. The 16..4095 MiB clamp, the device-limit caps and
  `PALMETTO_ARENA_MB` are unchanged, so the entry remains one branch of a pure function.
- `port/test/arena_size_test.cpp`: the unset and empty expectation is 1024; the synthetic-limit checks are
  unchanged, and the rebuilt test prints its line and exits 0.
- `port/shims/d3d12_vulkan_device.cpp`: the `create_arena` comment now states the measured default and why (no
  behaviour code changed); `port/test/d3d12_compute_test.cpp`: one comment counted the default as 256.
- `port/src/deferred_compile.h` and the off-thread design are **not** changed: the queue, the workers, the bound and
  the submit-side wait are exactly section 3. No synchronous fallback was reintroduced, and the eager path is
  unchanged.
- **Growth policy:** the arena is one VkBuffer whose device address is the shader heap base (section 3.3), so it
  cannot grow in place; a dynamic policy would need a second segment plus a relocation/rebase design for every live
  range - a separate architectural slice, not a size knob. The measured need (501.8 MiB) is well inside the
  documented 4095 MiB clamp, so the honest fix for this slice is the measured fixed default, and a boot that advances
  past `0x146F23D` is the measurement that would justify more.

### 9.6 The 300 s boot after the fix

One boot, `boot-defer300`, binary `41F31C387B37D8A75B18B367821FA3942BDD47B216F3E1008944DBBEF04A4288` (the fix
build), timeout 300 s, the deferred switch (4 workers, bound 4096) and the stage-2 probe. It stops at 79.0 s wall
(trace 61.6 s) on the new wall, not the old one:

| measurement | `boot-defer300` |
|---|---:|
| verdict / exceptions | STOP `game+0x146F23D` reading `0x30` at trace 61.6 s, wall 79.0 s; the `0x6EB759` stop is **gone** |
| D3D12 calls | 168,348 (trace max #183,476) |
| loading word / session word | init 2 at 18.0 s; **loading 1 to 2 and session 0 to 1 at 60.0 s** |
| machine word (current / next) | 2 / 2 at 73.9 s and 76.5 s; the stage-3 request at `0x81B2EF` is not written |
| stage-2 probe `wait` (+20.0 s) | pending `0x7b` (123), one ready bit (id 152) |
| stage-2 probe `steady` (+26.2 s) | pending `0x19` (25), **0 ready bits**, aggregate cleared: job 109's poll returned (section 5's A/B shows the same 124 to 25 step) |
| deferred submitted / built / failed / fallback / submit waits | 2,742 / 315 / 0 / 0 / 0 |
| deferred pending at the stop | 2,427 of 4,096; build time 4,384.6 ms, max single 162.1 ms, 4 workers |
| pipeline calls / driver calls | graphics 55,051 / 2; compute 4,825 / 315, all on the compile workers; driver compute time 4,378.8 ms, max single 162.0 ms |
| driver threads / creation threads / max driver calls in flight | 5 / 14 / 4 |
| submissions / presents / captures | `submission 4096 at 31.5 s`; 2,984 `PresentX` calls; one capture frame (2564x1421, mean rgb 0.00 0.00 0.00) |
| arena allocations / peak end / refusals | 2,960 / **501.8 MiB** / **0** (no arena-full line) |

The measured contract of section 5 survives (no title thread makes a compute driver call; the loader's stage
completes and the stage-2 holder's poll returns), and the boot now gets past the wall section 5.3 named: the loading
and session words move, and the machine word still stays 2 / 2 because the boot ends on the new null first.

### 9.7 The moved stop, recorded for a separate slice

- **Site:** `game+0x146F23D`, read of `0x30`, trace 61.6 s. Stack:
  `0x146F23D <- 0xF7DE8E <- 0xF7C8C1 <- 0xCC1F00 <- 0x570009B <- 0xCBA84A <- 0x56FDC26 <- 0x63E78` (job runner)
  `<- 0x4EAA05A` (item executor) `<- 0x221C29C` (worker loop) - the same task-pool path, no layer frame.
- **Null source (decompiled):** `0x146F230(param_1, ...)` first reads `[param_1+0x30]`; at the fault `param_1`
  (`rcx`) is 0 and `rbx`, `r12`, `rdi` are also 0, while `r13` is `image+0x849C100`. Its call sites are in
  `0xF7D7C0`, which walks a global table at `image+0x849C0E0` (entry stride `0x10`) and passes an entry's `+0x8`
  object pointer into the call; the entry itself is non-null, so the new null is **a registry entry whose `+0x8`
  resource object was never filled**.
- **Measured correlation at the end of the same boot:** the last three D3D12 calls before the fault are three
  `Xbox CreatePlacedResourceX (device slot 133) failed` (#183474 to #183476), directly after
  `slot 25 refused description ... dimension 3 format 118 flags 0x800002 layout 0x0 1024x1024x1 mips 1 samples 1`
  and `CreateCommittedResource refused (resource flag combination), dimension 3`. The likely producer is therefore
  the same class as this section's: a console description our PC validation refuses (a dimension-3 texture carrying
  an Xbox flag word that masks to the PC depth-stencil flag), so the title again gets a null resource it does not
  check.
- **Not arena-related:** zero arena refusals, the peak is 501.8 MiB of the new 1024 MiB default, and the null is a
  title object field, not an allocation answer.
- **Next falsifier:** one boot with the register probe on the slot-133 failure path and on `0x146F230`'s entry
  (`rcx == 0`) to prove the three refused creations are the same object as the null registry field, then pin the
  console meaning of the `0x800002` flag word on a dimension-3 resource; if the console accepts it, the fix is to
  drop the PC-invalid flag combination in the slot-133 translation instead of refusing, as the proxy already does
  for other Xbox bits. If it cannot be pinned, the measured negative stands with the exact field and RVAs.

### 9.8 Limits

- **One boot per arm.** The 256 MiB arm's stop moved from trace 53.3 s to 123.5 s across two boots under the shared
  machine's varying load; stage timings are not stable (one boot has shown a single compile taking 67 s).
- **The debugger session is a measured negative.** `boot-cdb1` attached one session at +38 s with second-chance AV
  filters; the boot stayed ALIVE for 130.3 s and no AV was caught, with only 100,540 D3D12 calls against 147,272 in
  the unattached 130 s boot, so the debugger's per-exception overhead changed the timing enough that the stop did
  not occur inside the window. The fault evidence this page relies on is the **in-process** crash report (registers
  and fault memory) and the in-process poke log, which need no debugger. One `.echo` line of the command file also
  contained a `;` and was split by the command parser, which affected only that echo.
- **The poke boot ran on an earlier build of the same tree** (`31F6FC99...`) than the plain reproduction and the fix
  arms; the stop reproduced on both builds. The shared tree rebuilt between boots (another slice's in-flight shim
  file), so the binary hash is given per boot.
- **The new stop's producer link is a correlation at the end of one boot** (three failed slot-133 creations directly
  before the fault), not a traced cause; section 9.7 names the trace that would settle it.
- **The arena peak is this slice's furthest point**, not the whole boot's: a boot that advances past `0x146F23D` may
  need more than 501.8 MiB, and the device-release report line names the size such a boot needs.
- No game data: RVAs, field offsets, hashes, counts and outcomes only; the one capture is a black frame with mean
  rgb 0.00 0.00 0.00.

### 9.9 Tests and gates (this section)

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` on the fix build (SHA-256 `41F31C38...`, the binary booted by 9.6) |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| fix test | `arena_size_test` rebuilt with the new expectation: `arena_size: request clamp, each device limit, unknown limits, allocation overhead and the floor`, exit 0 |
| test subset (`port/build`) | `frame_dump_test`, `unimplemented_policy_test` (98 checks), `xbox_present_test`, `xs_proxy_mock_test`, `graphics_record_test` (180), `graphics_pso_test` (312), `buffer_va_index_test`, `dstorage_test` (224), `xapu_chad_test` (87), `deferred_compile_test` (18) and `arena_size_test` all exit 0 |
| docs checks (`tests/test_docs_*.py`, with the session's `GIT_CONFIG_*` variables cleared) | eleven pass; `test_docs_env_vars` reports `PALMETTO_ROOT_PACK_MEASURE` read at `port/shims/d3d12_com.cpp:93`, another slice's file in flight in the shared tree - the variable is named in that slice's untracked `docs/root_packing.md`, so the check passes once the lead commits it; none of this slice's variables is missing |
| default path | with the deferred switch unset the boot is section 5's eager path; the arena default is the only plain-path change, and it is the subject of this section |

### 9.10 Attribution (this section)

Same attribution as section 8: this follow-up was implemented from this project's own measurements. No upstream code
or behaviour claim is used, and no git writes were made by this slice.
