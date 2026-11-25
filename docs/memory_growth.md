# Memory growth: what the 40 GB is, how much the pipeline budget bounds, and what is left

**Status: measured owner, bounded with a named remainder; the default boot's first-bind freeze found and fixed**,
2026-11-25, on the retail 1.0.2.1 title with this repository's Vulkan backend. [story_load_probe.md](story_load_probe.md)
measured one 300 s boot whose process private bytes grew from 16.7 GB to 83.6 GB (about 94% of the machine's commit
limit) while nothing was read and no platform or DirectStorage call started after 115.2 s. This page names the owner,
records the fix, and reports the A/B - including the part the fix does not recover, and the freeze the default-on lazy
build hit at its first bind (section 2.1: a re-entrant resource lock, measured with cdb and fixed). Counts, module names
and offsets only: no game data, package path or executable byte.

**Short answer:** the growth is the NVIDIA driver's host memory for the console pipeline states this title creates.
The title creates **51,786** graphics PSOs at load; this layer built a VkPipeline for every one, eagerly, and the
driver's shader compiler (`nvgpucomp64.dll`) and Vulkan driver (`nvoglv64.dll`) keep multi-GB host arenas and
per-pipeline allocations for that compilation work. The frame loop binds two. The fix builds a graphics pipeline at its
**first bind** instead of at creation and keeps a hard cap of **16 live pipelines** (LRU, rebuild-on-use, deferred
destruction), so a boot builds 2 pipelines instead of 51,786. Same-switch 120 s comparator: private 45.2 GB -> 11.5 GB
and driver-owned live allocations 55.4 GB -> 5.7 GB when the frame loop is not yet running; a clean 300 s boot with the
cap reaches a **plateau of about 40 GB that no longer rises** (storyprobe's before arm rose to 83.6 GB and was still
rising). The plateau is the driver's compiler arenas for the two compiles the loop needs; they are driver-retained and
are the named remainder, with the next falsifier being a warm persistent pipeline cache (section 6).

## 1. What was measured, and what was ruled out first

Three measurement layers, all opt-in and logging counts only:

- `PALMETTO_MEM_TRACE=<seconds>` (`port/shims/xmem.cpp`, landed with this slice): process private/working set, the
  process heaps' committed bytes (`HeapSummary`), XMemAlloc/XMemAllocDefault live bytes with a per-caller split,
  XMemVirtualAlloc calls/requested/committed, kernel32!VirtualAlloc as the title calls it, and the physical-page tables
  (live, mapped, sections, pages the first-touch handler committed).
- `PALMETTO_ALLOC_TRACE=1` (`port/src/alloc_trace.h`): rewrites every loaded module's import slots for
  `ntdll!NtAllocateVirtualMemory`/`NtFreeVirtualMemory` and `RtlAllocateHeap`/`RtlFreeHeap` to a counting thunk that
  captures a 32-frame stack for every 1 MiB-or-larger request and keeps live bytes per stack. This is what reaches host
  allocations the driver and host DLLs make, which an imported-symbol counter cannot see.
- an external VMMap-style sampler (`VirtualQueryEx`, no target memory read) every 10 s, grouping committed bytes by
  region type, protection and allocation base, plus the 1 Hz process series of the boot harness.

The title's own memory and this layer's GPU allocations are accounted for and are small:

| owner | counter | value at 110-140 s |
|---|---|---|
| title XMem heap (XMemAlloc / XMemAllocDefault) | live bytes, per caller | **94.4 MB** (3 blocks) |
| XMemVirtualAlloc | calls / requested / committed | 16 calls, 60,338 MB requested, **4,580 MB committed** |
| kernel32!VirtualAlloc as the title calls it | calls | **0** |
| physical pages | calls / pages / committed | 3 calls, 114,096 pages (7,131 MB requested), **638 MB committed** |
| process heaps (all) | `HeapSummary` | 1.3-4.4 GB committed |
| `VkDeviceMemory` for images/buffers | [streamed_image_backing.md](streamed_image_backing.md) census | ~535 MB at 130 s |

The external sampler puts all of the growth in `MEM_PRIVATE` committed host memory (mapped stays 1.73 GB, image 0.33
GB), so it is host memory, not GPU memory. The allocation trace names the maker:

| captured stack (module+offset) | live bytes | what it is |
|---|---:|---|
| `... <- nvgpucomp64+0x5971f9 <- nvgpucomp64+0x594ba0 <- ...` | **6,144 MB in 2 calls**; 4,096 MB in 1 call; 2-3 GB singles | the shader compiler's host arenas, a multi-GB reservation per compiler context |
| `... <- nvgpucomp64+0x58c981 <- ...` | 4,634 MB in 4,125 calls; 2,225 MB in ~2,000 calls | per-compiled-shader working memory |
| `... <- nvoglv64+0xe15e3f <- palmetto.exe+0x422ef1 <- +0x436414 <- +0x3c770d <- +0x253737 <- +0x1f0f7f` | 2,048 MB in single calls | `nvoglv64`'s per-VkPipeline allocation, called from `PipelineState::build` <- `create_graphics` <- `Device::CreateGraphicsPipelineState` <- `proxy_create_derived_pipeline_state` <- `proxy_slot<55>` (resolved from palmetto.pdb) |

The driver-owned live total in the pre-fix comparator boot was **55.4 GB in 18,571 blocks at 110 s**, matching that
boot's own process private series (45.2 GB at 100 s and still rising). The title creates 51,786 console PSOs (10,241
slot-57 record calls + 41,545 derived slot-55 calls); [pipeline_cache.md](pipeline_cache.md) measured that only 4,571
are distinct recipes for the driver, and the frame loop binds **two**. Two alternatives are measured and rejected: the
opt-in per-process recipe cache does not move the growth (39.4 GB at 110 s with `PALMETTO_PIPELINE_RECIPE_CACHE=1`, 40
GB without) - it is not the creation-call count but the VkPipelines the layer builds; and the allocation trace itself
perturbs the compiler enough to change the boot's shape (it slows every allocation and makes the compile burst
dominate), which is why section 3 keeps timing claims to boots without it.

## 2. The fix: a bounded live-pipeline budget, and a build at first bind

1. **`port/src/pipeline_budget.h` - the pure policy.** A hard cap on live pipeline ids with least-recently-used
   eviction and a grace: a pipeline bound within the last 8 other binds is never evicted (a title records a bind and may
   submit the list after another thread binds something else). Defaults **16 live, grace 8**; no Vulkan, no device, so
   it is tested without a GPU.
2. **Build at first bind (`port/shims/d3d12_vulkan_device.cpp`).** Every graphics PSO is created with its recipe,
   shader modules and layout but **no VkPipeline**; `graphics_pipeline_id()` - the thread recording the bind, before the
   list is submitted - builds it, so the replay's copy of the entry gets the fresh handle. A bind past the cap evicts
   the least recently bound pipeline: the state forgets the handle (a later bind rebuilds from the retained recipe),
   and the old VkPipeline is destroyed through the device's deferred queue (`DeletionQueue::Kind::pipeline`), i.e.
   after the submissions already given to the GPU complete. The opt-in recipe cache keeps its eager build when it is
   switched on, so that slice's semantics are unchanged.
3. **Counters at the existing milestone** (`port/src/vulkan_backend.h`): the bounded `[rt] replay: submission N` line
   now also prints `graphics PSOs N created; L live, B built, E evicted, R rebuilt; cap C`.

Switches: `PALMETTO_PIPELINE_LIVE_CAP` (default 16; **0 restores the pre-slice eager path exactly, for the A/B**) and
`PALMETTO_PIPELINE_LAZY_BUILD` (default on; 0 = eager builds). The defaults were briefly flipped to the safe pair (cap 0,
lazy opt-in) so other slices could boot while the default-on freeze measured below was open; section 2.1 fixes the
freeze, and the measured-correct pair is restored.

The trade-off is named: a driver error from `vkCreateGraphicsPipelines` now surfaces at the first bind rather than at
`CreateGraphicsPipelineState`. The layer's own validation refusals still answer at creation.

## 2.1 The default-on freeze: a re-entrant resource lock, its measurement and its fix

**Status: measured, root-caused, fixed, defaults restored.** With the switches at their measured defaults (cap 16,
build at first bind), the raster slice's measurement arm - the standard boot plus the frame-dump/full-target-readback
diagnostics armed at draw 100 - **froze** during the frame loop. The pipeline timeline stopped at **106 submits / 52
presents** (last census, at submission 16: 51,786 PSOs created, **2 live, 2 built**, cap 16) and stayed byte-identical
in every 5 s dump from +50 s to +140 s wall clock, with 99 threads, no validation message and no device loss. The same
tree with `PALMETTO_PIPELINE_LIVE_CAP=0 PALMETTO_PIPELINE_LAZY_BUILD=0` ran the window. The first reading was "the lazy
build's first bind hangs"; the measured stacks say the lazy build only supplied a lock writer.

**The frozen stacks.** Two cdb all-thread `~*k` dumps 15 s apart (84.4 s and 99.6 s of the feed-stall slice's comparable
arm: same switch pair, `PALMETTO_VK_VALIDATION=1`, same frame diagnostics; dump in
`D:\leonida-work\feedstall\boot-feed\cdb\`, summary in its `cdb\summary.txt`) show the same state both times:

- one render thread is inside `VulkanQueue::execute()` with `resource_mutex_` held **shared**: the stack is
  `VulkanContext::heap_base()` (a `std::shared_lock` constructor at the top) <- the device's `read_device_memory()`
  callback <- the armed readback/frame-dump print inside `detail::prepare_draw` <- `prepare` <- `VulkanQueue::execute_now`
  (the shared lock is taken once, for the whole replay, in `vulkan_backend.h`);
- 12 title worker threads are queued **exclusive** on the same mutex in `VulkanContext::add_pipeline()` (the title's own
  compute-PSO creation), each waiting on `RtlAcquireSRWLockExclusive`;
- nothing else is waiting on our locks, and no thread can release the render thread's outer shared lock.

**Root cause.** `execute_now()` holds `resource_mutex_` shared for the whole replay so its lookups see one stable
resource set (and two queues may prepare at once). A shared lock is not re-entrant: taking it again from the same thread
is a second reader, and Windows SRW locks are writer-preferring, so that second acquire blocks as soon as an **exclusive**
acquire is queued - while the exclusive writer cannot run until the first, outer shared lock is released. The replay's
device-memory callbacks read `heap_base()` from inside that window; it was a `shared_lock` read then, and the diagnostics
that call it (frame dump, readback, vertex path) were its only route, so the latent re-entrancy was invisible. The lazy
build turned the frame loop itself into an exclusive writer of the same mutex on the recording thread
(`set_pipeline_handle()` after a lazy build, `add_pipeline()` for the title's compute PSOs), which made the circular wait
certain at the first frame that binds a lazily built pipeline. That is why the cap-off tree does not freeze: the eager
path finishes every pipeline creation before the loop, so no writer is queued while a replay holds the lock.

**The fix.** `VulkanContext::heap_base()` is now a lock-free atomic read. `set_heap_base()` still writes under
`resource_mutex_` (and into the replay copy execute() takes), but the device sets the value once, when the arena is
created, before any queue can execute - one writer before every reader - so the read needs no lock. The audit of every
other accessor a replay callback can reach found no second one: `read_title_memory()` and the page pulls read the
console-VA map without a lock, `sampled_table()` takes only `descriptor_mutex_`, the bindless prepare takes
`descriptor_mutex_` then `submit_mutex_` (after the resource lock in the fixed order), and `has_resource()`,
`query_heap_type()` and the public `resolve_acceleration_structure()` are COM/title-thread calls that never run from
inside `execute()` (the replay's own acceleration-structure lookup is lock-free under that lock). Each accessor now says
so next to the lock, and the budget block in `d3d12_vulkan_device.cpp` states the lock order the lazy build must respect.

**Regression test.** `port/test/pipeline_recipe_cache_test.cpp` opens with a GPU-free section (fake Vulkan procedures,
no device): the real `VulkanContext` is set up, a reader thread holds the resource lock shared (execute()'s position) and
calls `heap_base()`, while a writer thread keeps queueing `add_resource()` exclusively (the bind path's position). With
the old `shared_lock` read the reader blocked; the test now fails on its 10 s bound instead of hanging, and with the fix
the read returns the set value. The validation-layer budget arm below it (24 PSOs, a bind past the cap, an evicted
pipeline rebuilt on its rebind) is unchanged and still reports 0 validation errors.

**Restored default and boot evidence.** With the fix in place the defaults are the measured-correct pair again: cap
**16**, lazy build **on** (`PALMETTO_PIPELINE_LIVE_CAP=0` and `PALMETTO_PIPELINE_LAZY_BUILD=0` remain the A/B arms). One
explicit arm (cap 16 + lazy + validation + the same frame diagnostics) and one arm of the exact measurement configuration
that froze, plus three consecutive default boots, are recorded in section 7; details:

- **The explicit arm** (`D:\leonida-work\lazyfix\boot-explicit`, 150 s, `PALMETTO_PIPELINE_LIVE_CAP=16
  PALMETTO_PIPELINE_LAZY_BUILD=1 PALMETTO_VK_VALIDATION=1` plus the frame-dump/readback/vertex-path diagnostics and the
  console/driver feeds; binary SHA-256 `9586D2F3...61AD5E6FF`, built 16:02:15) **ran the window**. The timeline shows
  the pre-fix frozen state at +65/+70/+75 s - submits 106, presents 52, console calls 108,611, 101 threads, the same
  numbers as the pre-fix freeze - and then resumes: +80 s 168/83, +100 s 964/481, +145 s **3,810 submits / 1,904
  presents**. 0 validation errors (no `Validation Error`/`VUID-` line) and no device lost. cdb `~*k` dumps at 40.1 s and
  70.1 s (`boot-explicit\cdb`): no thread waits on `RtlAcquireSRWLock*`; at the parked point the render thread is
  executing inside `execute_now` while 12 `nvgpucomp64` compiler threads work - it is the title's compute-PSO burst
  (compute calls 95 -> 384, 323 built), so the old state resolves into a driver compile stall instead of the deadlock.
  Census: 51,786 PSOs created; at submissions 16, 256 and 1,024 **2 live, 2 built, 0 evicted, 0 rebuilt, cap 16**.
- **The restored-default arm with the same diagnostics** (`boot-default-diag`, 150 s, no switch overrides at all):
  the same shape - a 15-20 s plateau at 106/52 (console calls 107,982, 99 threads) from +35 s to +50 s - resuming at
  +55 s (478/238) and reaching **7,967 submits / 3,983 presents** at +145 s; 0 validation/device-lost lines; cdb dumps
  at 40.1 s and 70.2 s with no lock wait. Private bytes 6.2/6.5 GB at 21/28 s (load) and 32.1 GB at 148.9 s - the same
  load-rise-then-plateau shape as the section 3 after arm (28.7 GB at 60 s, the 35-44 GB band at 90-270 s).
- **Three consecutive default boots** (standard environment only plus `PALMETTO_PIPELINE_TIMELINE=5` for the counters,
  150 s each; `boot-defaultA/B/C`; 0 exceptions in each boot's JSONL trace, no `device lost`/validation line; captures on
  as the arm had them). Every one ran the window with no flat run:

  | run | +60 s submits/presents | +90 s | +120 s | +145 s | private at 60 / peak / 148 s |
  |---|---:|---:|---:|---:|---:|
  | A | 2,409 / 1,204 | 4,949 / 2,474 | 8,237 / 4,118 | 10,332 / 5,165 | 30.6 / 36.8 / 33.0 GB |
  | B | 2,404 / 1,202 | 4,834 / 2,416 | 8,111 / 4,055 | 10,488 / 5,243 | 30.4 / 36.5 / 32.3 GB |
  | C | 2,404 / 1,202 | 4,693 / 2,346 | 7,745 / 3,872 | 10,145 / 5,072 | 30.4 / 36.7 / 32.1 GB |

  The census printed at every milestone (`1, 16, 256, 1,024, 4,096, 8,192` submissions): **51,786 graphics PSOs created;
  2 live, 2 built, 0 evicted, 0 rebuilt; cap 16** after the load's creation burst. The private curve is the documented
  one: ~6.3 GB during early load, ~30 GB by 60 s, a 35-37 GB plateau at 90-120 s and ~32 GB at 150 s - the same
  rise-then-plateau shape as the section 3 after arm (28.7 GB at 60 s, the 35-44 GB band at 90-270 s), slightly below
  that arm at matched times because it is 150 s rather than 300 s and has no validation layer.
  The three runs were on the final binary (`port/build/palmetto.exe`, SHA-256 `60E6FB...4D652C`, 16:27:45) except run A,
  which used the same source one linkage earlier (`099C34...EDC50C`, 16:14:48); the only later source change was another
  slice's opt-in `main.cpp` probe, inactive with its variable unset.

**Cross-check, feed side.** docs/feed_stall.md measured the same deadlock from the other arm: the driver-block feed calls `write_device_memory` on every fed draw from inside the shared lock, so the nested acquire is in the frame loop even with the readback diagnostics off, and its frozen arm stopped at **106 submits / 52 presents** with 12 compute-PSO writers queued in `add_pipeline`. Its page cites this slice's independent reproduction (pre-fix build, 48 submits / 23 presents, dumps under `D:\leonida-work\memgrowth\debug\hangdbg`); together the two measurements name both writer families - the title's compute-PSO registration and the lazy build's graphics handle publication.


## 3. A/B measurement

All boots: one per arm, same machine, same title, under the boot lock. The frames/boot binaries are the tree's
`port/build/palmetto.exe` at the build named in each row.

**Same-switch 120 s comparator (both arms: standard environment + `PALMETTO_MEM_TRACE=10` + `PALMETTO_ALLOC_TRACE=1`,
no capture).**

| measurement | before (`PALMETTO_PIPELINE_LIVE_CAP=0`, eager) | after (default, build at first bind) |
|---|---:|---:|
| process private at 20 / 40 / 60 / 80 / 100 s | 6.7 / 21.8 / 36.5 / 41.3 / **45.2 GB** | 6.1 / 11.5 / 11.5 / 11.5 / **11.5 GB** (flat) |
| process working set at 100 s | 37.5 GB | 6.8 GB |
| driver-owned live allocations at 110 s | **55.4 GB** in 18,571 blocks | **5.7 GB** in 34 blocks |
| VkPipelines built in the boot | **51,786** (eager) | **1** (the frame loop's bind) |
| XMem heap / XMemVirtualAlloc committed / physical pages | 94.4 MB / 4,580 MB / 638 MB | identical |
| frame loop | stalls: 70 submits, 34 presents at 120 s | stalls in the same way: 26 submits, 12 presents |

The comparator is honest about what it is: both boots stall before the stationary loop (the title's own post-load
stage, unchanged by this slice), so it measures the *load-time* pipeline component with the loop out of the picture: 1
built pipeline instead of 51,786, and 11.5 GB instead of 45.2 GB and rising.

**300 s arms with the frame loop running.** The before arm is [story_load_probe.md](story_load_probe.md)'s published
300 s boot (same title, machine and switch family; tree before this slice): private 16.7 GB at 30 s -> **83.6 GB at
300 s, still rising**. The after arm is this slice's 300 s boot with capture on and the cap arm (defaults then: cap 16,
the first 8 PSOs still eager): 9.2 GB at 30 s, 28.7 at 60, 39.4 at 90 and 120, then 35.6 / 34.8 / 42.3 / 41.3 / **44.1
GB at 270 s** with the loop running (11,002 submits and 5,500 presents at 150 s, `PresentX` milestones to the kill).
That curve **stops rising** after ~120 s; it oscillates in a 35-44 GB band. The measured eviction counters are 10
pipelines built, 0 evicted, cap 16.

So the fix bounds the curve (rising -> plateau) but the plateau is not small: about 40 GB, of which the two or three
compiled pipelines' driver arenas are the remaining owner (a fully-lazy 150 s boot with 2 built pipelines and the loop
running still reaches 36.5 GB private, while the same build with the loop stalled stays at 11.5 GB).

**Frame behaviour.** The fix changes no submitted command for a pipeline that is built. Captures in the arms that reach
the loop are the same uniformly black presented frame earlier pages measured, and the stationary two-pass loop is
unchanged; a boot that spends its window compiling 51,786 pipelines can stall before the loop (the before arm above
does, the published before arm does not), which is the memory effect itself, not a rendering change.

## 4. What the cap recovers, and what it cannot

- **Recovered: the load-time pipeline component.** 51,786 eager VkPipelines -> 2 built; same-switch 120 s comparator
  45.2 GB -> 11.5 GB private, 55.4 GB -> 5.7 GB driver-owned, and the 300 s curve goes from rising to plateau.
- **Not recovered: the driver's compiler arenas for the pipelines that are still compiled.** They are multi-GB host
  reservations per compiler context (`nvgpucomp64`), allocated while compiling, and the driver does not give them back
  when the pipeline is destroyed. The A/B's plateau (~40 GB) is dominated by them; the cap counts VkPipelines, not the
  driver's arenas.
- **A warm persistent pipeline cache is the next lever, not measured here.** [pipeline_cache.md](pipeline_cache.md)
  measured a boot whose driver-reported pipeline cache hits are total (`0` misses) and whose per-pipeline cost drops
  138.4 us -> 26.0 us: a boot whose pipeline creations hit the cache should not allocate the compile arenas at all.
  The falsifier is one cold + one warm 300 s pair with `PALMETTO_PIPELINE_CACHE=<file>` on this build: if the warm
  arm's plateau drops toward the 11.5 GB load baseline, the remainder is the compiler and the persistent cache is the
  fix; if it does not, our rebuild path is not reaching the cache and that is the next slice.
- **The per-frame question is measured in the clean arm.** In the 300 s cap arm memory stopped rising after ~120 s
  while submissions continued to 300 s; the allocation-trace boots (perturbed) instead keep showing driver allocation
  growth, so a per-submission component cannot be excluded from them. The honest reading is that the clean arm bounds
  the observable growth, and the diagnostic arm cannot separate "driver per-submission work" from its own perturbation.
- **One boot per arm, one machine, timing-sensitive.** The title's boot progress varies run to run (the compile burst
  and the allocation trace change where the loop starts and whether it stalls), and the before 300 s arm is the earlier
  page's boot, not a rerun. The owner attribution does not depend on that variance: the stacks, the live-byte counters
  and the creation path are the same in every diagnostic boot.

## 5. Tests

- `port/test/graphics_pso_test.cpp` (GPU-free, in the gate's test list): a new `test_pipeline_budget` section, **23
  checks**: a cap of four keeps four live of ten builds and evicts 1..6 in LRU order; a re-touch makes a pipeline the
  newest; a duplicate touch counts once; a grace above the growth keeps every just-bound pipeline live and a growth
  past it resumes eviction inside grace+1; `remove()` forgets without an eviction and keeps the LRU order; a cap of 0
  leaves the budget off. The binary passes **263 checks**.
- `port/test/pipeline_recipe_cache_test.cpp` (Vulkan-backed; validation-layer arm): the pure policy is checked at
  start, and a new section creates 24 PSOs, binds every one on a direct command list and binds 8 a second time. With
  `PALMETTO_PIPELINE_RECIPE_CACHE=0 PALMETTO_PIPELINE_LIVE_CAP=8 PALMETTO_PIPELINE_LAZY_BUILD=1 PALMETTO_VK_VALIDATION=1`
  this builds deferred pipelines, evicts past the cap and rebuilds on the rebind; the run reports
  `pipeline recipe cache stress: ok (0 validation error(s))`. The shared-entry arm also passes with 0 validation
  errors. The test only sets `PALMETTO_PIPELINE_RECIPE_CACHE` when the caller did not, so one binary covers both arms.
- The same binary opens with the **replay accessor lock-order regression** (GPU-free, no device, fake Vulkan
  procedures): a reader thread holds the real `VulkanContext`'s resource lock shared - execute()'s position - and calls
  `heap_base()`, while a writer thread keeps queueing `add_resource()` exclusively - the bind path's position. With the
  old `shared_lock` read that call blocked behind the writer; it must return the set value within the test's 10 s bound.
  This is the section 2.1 freeze as a test.
- `port/test/xmem_test.cpp` passes (258 checks) with the trace instrumentation in `port/shims/xmem.cpp` (default path
  unchanged; the header exists only when the switch is on).
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the slice's
  tests pass, and the documentation checks (`tests/test_docs_*.py`) pass except two failures that are other slices'
  (an environment variable added to `port/src/main.cpp` by the stage-2 probe, and `interval option` in
  `frame_first_light.md`, both untracked-by-this-slice and named in the report).

## 6. Limits and the next falsifier

- **The plateau is driver memory we do not own.** The compiler's arenas are reserved by `nvgpucomp64` while compiling
  and are not released when the VkPipeline is destroyed; the cap bounds our VkPipelines, not the driver's arenas. A
  warm persistent pipeline cache (section 4) is the measurement that would remove them.
- **Driver creation errors are deferred for every graphics PSO**, not just those past a threshold: a PSO the driver
  would refuse at creation answers `graphics_pipeline_id()` = 0 at its first bind. This title's PSOs are accepted; the
  trade-off is stated here rather than hidden.
- **The eviction path is exercised by the tests, not by the title's loop.** The loop binds two pipelines, so the cap is
  not reached in the measured boots (0 evictions). The lifetime is proven by `pipeline_recipe_cache_test`'s budget arm
  under `VK_LAYER_KHRONOS_validation` (0 validation errors), and the deferred destruction path by the same run.
- **The opt-out arms are not a clean verification vehicle on the pre-fix build.** Two attempts at the pre-slice path (`PALMETTO_PIPELINE_LIVE_CAP=0 PALMETTO_PIPELINE_LAZY_BUILD=0`) on the frozen build terminated before the frame loop: one at +64.5 s in the middle of a slot-57 record, one at +33.6 s with a `RADAR_PRE_LEAK_64` event and no crash report. The before/after numbers therefore use the published 300 s before series plus the fixed build for every verification boot, never a fresh opt-out arm on the frozen binary.
- **The replay's accessor lock order is documented and tested, not enforced at every call site.** The fixed accessor
  is `heap_base()`; every other replay-side callback was audited and comments at each accessor state whether it may run
  from inside `execute()`. A future callback that takes `resource_mutex_` would reintroduce the section 2.1 deadlock, so
  the next accessor added for the replay must be checked against the same rule (the regression test covers the one value
  the replay reads today).
- **The next falsifier:** cold + warm 300 s pair with `PALMETTO_PIPELINE_CACHE=<file>` on this build: the warm arm's
  plateau should fall toward the 11.5 GB load baseline. If it does not, the rebuild path does not reach the driver's
  cache and that is the next slice.

## 7. Reproduction and evidence

- Boots (outside the repository): `D:\leonida-work\memgrowth\boot-memF` (120 s, before, allocation trace),
  `boot-memH` (120 s, after, allocation trace), `boot-abB2` (300 s, cap arm, capture), `boot-frame` (150 s, fully
  lazy, allocation trace), each with `boot\stderr.log`, `boot\boot.jsonl`, `series.csv` (10 s VMMap-style samples),
  `groups.csv`, `samples.csv`; captures under `capture`. Scripts: `boot_mem2.ps1`, `memmap_sample.py`, `ab_report.py`,
  `alloc_series.py` (all under `D:\leonida-work\memgrowth`). The 300 s before series is
  [story_load_probe.md](story_load_probe.md)'s.
- The freeze (section 2.1): the raster slice's frozen default arm is `D:\leonida-work\raster\boot-measured` (timeline
  stopped at 106 submits / 52 presents, 2 live / 2 built); the cdb all-thread dumps 15 s apart are
  `D:\leonida-work\feedstall\boot-feed\cdb\dump-84,4-freeze.log` and `dump-99,6-confirm.log`, with the per-thread
  summary in `cdb\summary.txt`. Both are the same tree and switch pair as the measurement below. An independent
  reproduction on the pre-fix build with its own all-thread dumps is `D:\leonida-work\memgrowth\debug\hangdbg\`
  (`cdb-55.log`, `cdb-85.log`): the same replay thread in the nested `heap_base()` acquire with title threads queued
  exclusive in `add_pipeline`, frozen at 48 submits / 23 presents from about +50 s; the rebuilt tree's lock-order test
  passes.
- The fixed default: `D:\leonida-work\lazyfix\boot-explicit` (150 s, cap 16 + lazy + `PALMETTO_VK_VALIDATION=1` + the
  same frame-dump/readback diagnostics; cdb `~*k` dumps in `cdb\`), `D:\leonida-work\lazyfix\boot-default-diag` (150 s,
  the exact measurement arm that froze, restored defaults) and `D:\leonida-work\lazyfix\boot-defaultA/B/C` (150 s each,
  standard environment only, restored defaults, census lines and timeline in `boot\stderr.log`/`samples.csv` with
  `PALMETTO_PIPELINE_TIMELINE=5`, measurement-only; `boot-default1`/`boot-default2` are the same arm from a launcher
  whose teardown hung on its console pipe and are not used as evidence). Harness:
  `D:\leonida-work\lazyfix\boot_lazy.ps1`. No game byte is read beyond what the boot itself reads.
- Reproduce: acquire `_setup/boot_lock.ps1 -Acquire -Who <name>`, run `tooling/boot_report.py` (or the script above)
  with the standard environment plus `PALMETTO_PIPELINE_TIMELINE=30` and, for owner attribution only,
  `PALMETTO_MEM_TRACE=10 PALMETTO_ALLOC_TRACE=1`, then release the lock. No game byte is read.
- This slice makes no git write: its uncommitted files are `port/src/pipeline_budget.h` (new),
  `port/shims/d3d12_vulkan_device.cpp`, `port/src/vulkan_backend.h`, `port/test/graphics_pso_test.cpp`,
  `port/test/pipeline_recipe_cache_test.cpp`, `docs/memory_growth.md` and the switch rows in `docs/dev-tools.md`.
  `port/shims/xmem.cpp` and `port/src/alloc_trace.h` were committed by the lead with the page-resolver slice;
  `port/src/alloc_trace.h`'s stack table was then enlarged for the last attribution boot (a diagnostic-only change,
  still uncommitted), and the freeze fix touched the two files above that other uncommitted slices also extend.

## 8. Attribution

This slice was implemented from this project's own measurements: the counters, the external map sampler, the allocation
tracing, the A/B boots and the driver-side stacks are all ours. Cross-check against the public GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida): its pipeline-cache work is the prior art our own
[pipeline_cache.md](pipeline_cache.md) cross-checked, and its image-pool notes are cited by
[streamed_image_backing.md](streamed_image_backing.md); no code or comment text from that project was copied. The
upstream project remains the copyright of its contributors under GPL-3.0.
