# The streamed-block consumer: a title CPU deserializer, and the GPU-side step that never happens

**Status: measured, and the last open lead is closed.** Three 130 s harness boots name the consumer of the DirectStorage
blocks and answer the question the black frame raised: do the streamed bytes reach a GPU resource? They do not. All 4,096
decompressed 64 KiB blocks land in ordinary process-heap addresses, a title CPU deserializer (`game+0x248F680`, reached
as the virtual method `[obj+0x18]` through the job wrapper `game+0x4F65820`) reads the first batch's blocks straight
after they complete, and **no device entry point ever names any of the 4,096 destination ranges** - not as an argument,
and not inside a structure an argument points to. The earlier linear-layout A/B appeared to hand a block address to
console view-creation calls; this increment settles that call shape and shows those words were saved-register and
caller-stack carry-over, not arguments. The measured Xbox-ABI shapes are PC-shaped (3-argument
`CreateShaderResourceView`, 4-argument `CreateUnorderedAccessView` with a NULL counter resource, and a 3-argument
descriptor creation on console slot 70), each followed by a CPU cache-line writeback of its destination descriptor. The
sampled texture is black for a separate, already watched reason: its own title pages are never written.

**Load-stall increment (sections 6 and 7).** The "consumer stops after 16 blocks" premise is corrected: with the first
64 destinations guarded, the consumer reads the first **50** blocks (requests 1-50, the first streamed resource - the
pipeline library) as they complete, and the title then spends ~10 s creating pipeline states; the renderer starts its
stationary two-pass loop **after 15 s** and after that no load job is dispatched again. The DirectStorage status array is
polled correctly (26.16 million `IsComplete(0)` polls from 0.05 s to completion, then `S_OK` and the polling stops; no
`E_PENDING` is left, no signal/event is ever requested). The load window contains **no queue or fence calls at all** (the
loader threads never wait on the GPU there), and every title thread is in an ordinary idle wait (its own pool
event/semaphore) once the pipeline burst ends - no busy title site, no pending status. So the gate that keeps the title on
the loading screen is **not** DirectStorage emulation and **not** a GPU-fence wait: it is a non-DirectStorage loader gate
that opens after pipeline precompile, and that gate is the next named measurement.

Counts, RVAs, ranges, descriptor dwords and content-class labels only; no package bytes, paths, shader bytes or extracted
tables.

## 1. What was added (diagnostic only, off by default)

`port/shims/dstorage.cpp` - `PALMETTO_STREAM_CONSUMER=1` (read once; with it unset every hook returns on one cached
boolean and no guard, handler or range is created):

* each completed request's destination is reported to the device proxy's range registry, and the first bytes of the
  first blocks are classified as a content class (a label, never a byte value);
* the pages **wholly inside** the tracked destinations get `PAGE_GUARD` on the worker thread right after the block is
  written (the block's first/last partial pages are left alone, so heap metadata and neighbours cannot fault). The first
  access after completion faults into a vectored handler that prints the accessing code as a title RVA, a
  host-runtime (`palmetto.exe`) RVA - a shim read, the XG fill being the one this looks for - or neither, the faulting
  thread id, and the title frames on the stack, then clears the guard and lets the access retry. Bounded: the first 64
  destinations are tracked one by one (so the exact stop block is measured) plus one every 512th request (8 per boot),
  96 guard slots, 128 fault lines, fixed lines, no allocation or module lookup in the handler. A block written again by
  a later request is reported and re-armed;
* periodic counters every 1,024 completed requests (requests, MiB, distinct destinations, tracked blocks, fault count);
* `PALMETTO_DS_LOAD_TRACE=<from_s>,<to_s>` (`1` means 0..15 s): the loader-side trace. Every status-array poll prints its
  index and value with the polling caller (`game+RVA`), every enqueue/submit/`Query1`/factory `GetStats` prints its
  values, and a 1 s heartbeat for the first 20 s keeps cumulative requests, completions, submits, poll counts,
  `E_PENDING` counts and the last poll after the window. Bounded (2,000 window lines, 64 late lines, 20 heartbeat lines);
  off by default, changes no call;
* the queue and fence proxies get the same window trace from the device side (`PALMETTO_LOAD_TRACE=<from_s>,<to_s>`,
  see below), so a poll that never satisfies, a second batch and a GPU wait are all visible in one boot.

`port/shims/d3d12_xs_device.cpp`:

* the streamed-range registry (8,192 ranges, lock-free min/max prefilter) and the device-side correlation: every device
  call's first six argument words are tested against the ranges, and the first call of each entry point a consumer's
  GPU step would use is printed with its caller (bounded; a hit is reported as `STREAM CONSUMER: ... names streamed
  block #N`);
* `PALMETTO_LOAD_TRACE=<from_s>,<to_s>` (`1` means 0..15 s): one line per device and queue/fence proxy call inside the
  window with elapsed seconds since the first device-proxy call, the calling thread id, the caller as `game+RVA` and the
  call's argument words; a `GetCompletedValue` also prints the value it returned and console fence slot 12 prints its
  target and completed value. `PALMETTO_LOAD_TRACE=1.5,7` is what the load-stall boot used. Bounded (20,000 lines), off
  by default, changes no call;
* `PALMETTO_XBOX_VIEW_SHAPE=1`: the Xbox-ABI call-shape probe for the three console view-creation sites. For slots 18
  (`CreateShaderResourceView`), 19 (`CreateUnorderedAccessView`) and 70 it prints, once per call site, the caller as
  `game+RVA` and all fourteen words the thunk receives with each word's origin (`arg`, `saved-rsi`, `retaddr`, `local`,
  `caller-stack`), the first 8 dwords of every readable pointer word, and it scans the first 256 bytes of every readable
  word for values inside a streamed block (the indirect case). Across the boot it counts streamed-range hits per slot and
  per word position, and scans pointed-to structures on every call for slots 18/19 and every 1024th call for slot 70;
  the histogram prints on the first call, on every new position hit and every 256 calls. Off by default, bounded, and it
  changes no argument;
* an exported range lookup (`palmetto_streamed_block_index`) for a future XG-shim correlation, unused by this increment.

Both files are also linked into GPU-free test binaries; the new code has no shim-registry dependency.

## 2. The stream, measured in the same boots

`PALMETTO_DSTORAGE_TRACE=1` plus the consumer counters, standard environment (shader corpus, Vulkan device,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`; linear texture layout is the default now, so
`PALMETTO_XG_LAYOUT` is omitted), final binary, 130.2 s boot:

| Measure | Value |
|---|---|
| Run | ALIVE 130.2 s (killed by the timeout as usual), 0 exceptions, no device-removed line, 572,218 recorded d3d12 calls |
| Requests | 4,096, all zlib file requests, every status entry `S_OK` |
| Decompressed | 256.0 MiB in 1.7 s (152.2 MiB/s; 121-157 MiB/s across the repeats) |
| Enqueue caller | `game+0x28C9A0E` for every request |
| Destination ranges | 4,096 **distinct** process-heap addresses, 64 KiB each |
| First destinations | `0x...7FB2A0`, `0x...80B2A0`, ... - 64 KiB blocks spaced 1 MiB apart; the first resource is 42 such blocks (#0-#41), then further 1 MiB-strided families (`#42-#47`, `#48`, `#49`, `#50-#63`) |
| Later sampled destinations | different allocations (`0x...162AE68`, `0x...555F4F6D4`, ...) |
| Content class (first 8 blocks) | structured binary 7/8; 16-byte repeating pattern 1/8; no known container tag, all-zero or still-compressed block |

## 3. The consumer chain (first-touch evidence)

Seventy-two destinations were guarded in the load-stall boot (the first 64 requests one by one, then one destination every
512th request; earlier boots tracked only 16 + 8, which is where the "stops after 16" reading came from). The first
**50** blocks were read, in resource order, as the queue completed the first resource's requests:

| Blocks | First access | Accessor |
|---|---|---|
| #0-#49 (all fifty) | first guarded page, +0xD60..+0x9BD8 into the block (block-base dependent); all reads, no writes | thread `65652` blocks #0-#48, thread `54600` block #49; RIPs `game+0x248FEF9` (most), `game+0x248F6EF` (#3, #10, #17, #24, #31, #38), `game+0x248FEBC` (#42-#48); same deserializer stack (`game+0x248F772/79D/7F3/849` <- `game+0x4F6584A`) |
| #50-#64 (tracked one by one) and the eight later samples (requests 512, 1024, ... 4096) | **never touched** in the 130 s window (0 faults, still guarded at the end) | - |

So the consumer reads the first streamed resource (fifty 64 KiB blocks: the pipeline library, as section 6 shows) and then
stops because the loader has nothing further to hand it in the measured window - not because it is stuck mid-batch. The
consumer is the loader job wrapper `game+0x4F65820` invoking the deserializer virtual method `game+0x248F680`
(disassembly in section 5: it walks a record pointer at `[object+0x150]`, feeds dwords to a pool helper and resolves a
run of `+0x38..+0x68` fields through the two tiny thunks that own the faulting RIPs). **No host-runtime first touch was
observed on any tracked block** - the XG fill path does not read them (section 5).

## 4. The GPU side: no device call names the ranges

The device-side correlation ran for the whole boot, from the first completed block on:

| Question | Result |
|---|---|
| Any device call argument (first six words, every slot) pointing into a streamed range | **0** - no `STREAM CONSUMER` line at all |
| Any value inside a structure an argument points to (256-byte window, first call per site) | **0** in the boot-shape1 probe; the wider probe in section 5 scans across the boot and finds only caller-stack structures, never an argument's |
| Copy-family command-list slots (15/16/17/18/19/85/146) | **never called** in the window |
| COPY command queue or COPY command list created | **no** (queue/list creation lines show command type 0, no type 3) |
| Heap/placed-resource entry points naming a block address (slots 27/28/29/48/49/53/54/69/70/76/77/127/133) | **none** |
| Console `CreatePlacedResourceX` (slot 133) | runs (first call caller `game+0x193E2DB`), names **title page** VAs, not streamed blocks |

This agrees with the replay-wide measurements (`docs/page_content.md` section 8: 0 copies, 0 dispatches, 0
`WriteToSubresource` in a 300 s boot) and sharpens them: no device call carries a streamed block address at all. The
blocks are consumed by the CPU loader and never handed to the GPU in this window.

## 5. The console view-creation sites: the block address is carry-over, not an argument

The earlier linear-layout A/B saw addresses inside a streamed block in the *higher* words of the console
`CreateShaderResourceView` (slot 18) / `CreateUnorderedAccessView` (slot 19) calls and of console slot 70. This increment
settles the call shape offline (disassembly of the wrappers, RVAs only) and measures it in the boot with
`PALMETTO_XBOX_VIEW_SHAPE=1`.

**Offline: the three wrappers and the writeback.**

* slot 18, wrapper `game+0x164EE0` (the call returns to `game+0x164F01`): `push rsi; sub rsp,0x20; rsi=r8; r8=rdx;
  rdx=rcx; rcx=<device global>; r9=rsi; call [rax+0x90]`, then `jmp 0x4ECC830` with `(rsi=r8, 0)`. It forwards
  **three** arguments after the device - (descriptor, resource, destination descriptor) - exactly the PC
  `CreateShaderResourceView` shape. Callee words: 1-3 arguments, 4 = the caller's `rsi` saved by `push` (a register save),
  5 = the wrapper's return address, 6+ = the wrapper caller's stack;
* slot 19, wrapper `game+0x16475F0` (returns to `game+0x1647616`): `push rsi; sub rsp,0x30; r9=rdx; rdx=rcx;
  rcx=<device global>; rsi=r8; [rsp+0x20]=r8; r8d=0; call [rax+0x98]`, then the same writeback on `rsi=r8`. It forwards
  **four** arguments - (descriptor, resource, **NULL** counter resource, destination descriptor) - exactly the PC
  `CreateUnorderedAccessView` shape, and the explicit NULL for the counter resource is the giveaway that this is the PC
  prototype. Callee words: 1-4 arguments (4 is stored to the stack), 5 = an unwritten local slot, 6 = the caller's `rsi`
  saved by `push`, 7 = the wrapper's return address, 8+ = the caller's stack;
* slot 70, wrapper `game+0x4ECC800` (returns to `game+0x4ECC821`): byte-identical prologue to the slot-18 wrapper, calling
  `[rax+0x230]`, then the same writeback on its third argument. It forwards **three** arguments. Observed at the site:
  arg1 = pointer to a stack struct, arg2 = pointer to a `D3D12_CONSTANT_BUFFER_VIEW_DESC`-shaped struct (first dwords
  `BufferLocation 0x100000027`, `SizeInBytes 0x1688`), arg3 = a destination descriptor in the console descriptor heap
  (`0x...510000220`). Its other (and dominant) call site is `game+0x4D9F369`, where the same 3-argument call carries a
  CBV-shaped second argument and a destination descriptor loaded from an engine object at `+0x1FA38`; it runs once in the
  per-frame constant-buffer path and accounts for the 13,568 calls;
* all three wrappers tail-call `game+0x4ECC830`, a cache-line writeback loop (`clwb` over `[pointer, pointer+global)`,
  pointer rounded to a cache line). This is the title's UMA publication step for CPU-written descriptors: write the
  descriptor on the CPU, then flush its cache lines so the GPU sees it.

**In the boot (final binary, 130.2 s).** The probe monitored 13,629 calls: 13,568 on slot 70, 39 on slot 18 and 22 on
slot 19 (one site each for 18/19; two sites for 70). Measured:

| Question | Result |
|---|---|
| Streamed-range hit in any of the 14 words, any call, any of the three slots | **0** (histogram: "no streamed-range hit in any word" at every print through slot 70's 13,568 calls; no position-hit line for slots 18/19) |
| Streamed-range hit inside an argument-pointed structure (all scans: every 18/19 call, every 1024th slot-70 call) | **0** |
| Streamed-range hit inside a **caller-stack**-pointed structure (not an argument) | **10**: slot 19, calls 12-16, caller `game+0x1647616`, words 9 and 10 (both `caller-stack`), structures at `+0x80` and `+0x38` holding pointers inside streamed blocks #44 and #45 |
| Observed argument words | slot 18: word 3 = console descriptor address `0x...510000000`; slot 19: word 4 = `0x...5100000A0`; slot 70: word 3 = `0x...510000220` - all console descriptor destinations, never a block address |

So the earlier A/B matches sat exactly in the positions the offline analysis predicts for carry-over: word 4 of the
slot-18/70 thunk (saved caller `rsi`), word 5 of the slot-19 thunk (unwritten local) and words 6+/8+ (caller stack). The
A/B happened to catch a boot in which the caller's saved register held a block pointer; this boot's 13,629 calls produced
no word hit at all, and the only block addresses near these calls are in the caller's own frame structures, which the
device call never receives.

**What that means for the upload lead:** the direct-argument and indirect-argument (structure) cases are both negative.
The streamed data is present in the title's CPU frames next to the descriptor-creation path (blocks #44/#45 in two caller
locals during five UAV creations) but it is not handed to any device call. The port does not drop a streamed address at
these sites - there is no such address to drop.

## 6. Why the consumer stops: the load sequence, the busy poll, and the gate after pipeline precompile

One 130 s boot with the loader-side instrumentation (`PALMETTO_WAIT_SAMPLE=2,3,4,5,6,8,10,15`,
`PALMETTO_DS_LOAD_TRACE=0,12`, `PALMETTO_LOAD_TRACE=1.5,7`, `PALMETTO_STREAM_CONSUMER=1`,
`PALMETTO_DSTORAGE_TRACE=1`; ALIVE 130.2 s, 0 exceptions, 566,248 recorded d3d12 calls; boot `boot-load1`) answers the
question the "consumer stops" observation raised. **The consumer does not stall after 16 blocks, and the DirectStorage
status semantics are not the gate.**

* **The consumer reads the first 50 blocks, as they complete.** With the first 64 destinations guarded one by one, the
  faults show blocks #0-#48 read on thread `65652` and block #49 on thread `54600`, in resource order (with the
  resource's own table entries #42-#48 interleaved); blocks #50-#64 (tracked) and every sampled later destination
  (requests 512, 1024, ... 4096) are **never touched** in the window. Fifty 64 KiB blocks is the first streamed resource -
  the pipeline library, as the next point shows. The earlier "first 16" was the guard's sampling window, not the
  consumer's stop.
* **The streamed resource is the pipeline library; the title then builds pipelines for ~10 s.** Immediately after the
  block reads the load-window trace shows thread `65652` creating 52 console compute pipeline states (device slot 62) and
  26 root signatures (slot 16), then seven threads (`38024`, `4372`, `56760`, `66732`, `40108`, `54600`, `52604`) create
  ~1,880 graphics pipeline records (device slot 57) plus slot-67 deserialized compute states between +5.22 s and +7.00 s
  of the trace (the window end; the whole pipeline precompile runs into the next phase). So the bytes the consumer reads
  are consumed: they become shader/pipeline records. The title has *not* stalled at the consumer.
* **The status array is polled correctly.** Heartbeats: 4,199 requests enqueued; at +1.0 s 14,457,850 `IsComplete(0)`
  calls (14,457,846 of them `E_PENDING`), 2,454 requests done; at +2.0 s **26,161,577 polls, all but five `E_PENDING`,
  then `value 0x0`** (S_OK) and 4,199 requests done. From +2 s to +20 s the poll count is flat (only +92k by +13 s from
  later trickle batches), i.e. the title sees the completion and stops polling. No `EnqueueSignal`/`EnqueueSetEvent` is
  requested, no `E_PENDING` is left, and the title never asks for a signal it does not poll - the poll loop terminates
  exactly as the real runtime would.
* **A second batch is enqueued, but slowly.** After the 4,199-request burst: request #4,200 at +10 s, #4,234 by +13 s,
  submits 5 -> 6 -> 17. So the loader is alive after the burst and asks for a little more; it is not blocked in
  DirectStorage.
* **The load window contains no queue or fence calls at all.** The load trace (device + queue + fence proxies) has only
  device slots between +1.5 s and +7.0 s: no `Signal`, no `Wait`, no `SetEventOnCompletion`, no
  `GetCompletedValue`, no fence slot-12 cell. The loader threads never wait on the GPU in that window, so a missing
  GPU-fence delivery cannot be what stops them there.
* **After the pipeline burst every title thread is in an ordinary idle wait.** The wait samples: 2 s - 41 threads, 31
  waiting (the status-poll thread is the one busy site, caught at the poll); 3, 4 and 5 s - 50 threads, 39 waiting, 6-7
  RIP sites, no busy title site; 6 s - 68 threads, 56 waiting; 8, 10 and 15 s - 68 threads, 51 waiting, no busy title
  site. The wait sites are the ntdll wait/alert/delay stubs; the title threads wait on their own pool events
  (`0x7A8`-`0x820`) and pool semaphores (`0xC80`, `0xD18`), plus the audio semaphore (`0x79C`) and the CRT/worker waits.
  Nothing samples a title-code spin after the poll ends.
* **The renderer starts after 15 s and then never changes.** The first console submit (queue slot 35) appears after the
  15 s wait sample; the replay counter then runs its stationary two-pass loop (replay submission 1 at its clock 0.0 s,
  16 at 2.6 s, 256 at 3.6 s, 1024 at 6.6 s). From then on the loader pool stays idle and DirectStorage goes quiet after
  the trickle at 13 s.

* **One new observation for the next run (not a binding).** In the frame-loop phase the device-side correlation prints a
  repeated hit: `CreateConstantBufferView` (device slot 17) calls from caller `game+0x4DA8F63` carry
  `0x24700000060` in thunk word 6, which the stream registry matches to streamed block #4074. Slot 17's PC shape reads
  only its two arguments, so word 6 is caller-stack carry-over and the CBV call does not consume the address - but it
  means the frame loop's caller keeps a pointer *into a streamed block* in its frame on every frame. Block #4074 was not
  one of the guarded destinations (guard covers requests 1-64 plus eight samples), so whether the frame loop reads late
  blocks is **unmeasured**; guarding a late sample (requests 4000-4096) is the smallest way to check it.

**So the diagnosis is (c), not (a) or (b):** DirectStorage completion is delivered and consumed correctly (a ruled
out); no GPU/fence wait exists in the load window (b ruled out there); the title's loader stops dispatching jobs after
the pipeline precompile and every worker goes idle. The gate is a non-DirectStorage loader condition that opens after
`game+0x248F680`/`game+0x4F65820` have consumed the pipeline library and the pipeline states are built. It is not named
yet; section 9 names the measurement that would name it.

**Confound noticed in this boot (report it, do not act on it).** The working tree carried other agents' in-progress
`graphics_pso` changes: slot-57 histogram #5 reports `10,000` records with `built 6337, refused before the call 3663`
(`root signatures ... null 3663`) where the committed tree measured `10,241/10,241, 0 refusals`. The boot above is
therefore a hybrid tree for the pipeline counts. The stationary loop itself is not explained by that: the clean-tree
300/600 s boots (`docs/frame_capture.md`) showed the same loop before those edits. The counts above (blocks, polls,
threads, waits, submits) are shim-side and unaffected.

## 6a. The gate-naming boot: the late block is never read, the pipeline workers stop at ~7 s, the frame loop keeps running

One more 130 s boot (`boot-gate1`: ALIVE 130.2 s, 0 exceptions, 565,799 recorded d3d12 calls) added the late-block guard,
a loader-thread-filtered device trace (`PALMETTO_LOAD_TRACE=5,25` logs device calls only from threads that polled/enqueued
DirectStorage or first-touched a streamed block, plus the first four queue/fence calls per slot), `PALMETTO_XS_CALLERS=1`
with the caller profile, and wait samples at 20/25/30/45/60/90 s.

* **The late block is not read.** The first 50 blocks are read again (#0-#48 on thread 47652, #49 on thread 13368 through
  a host-DLL memcpy path); blocks #50-#64 and every tracked destination in requests 4,060-4,096 (including request 4,074,
  the block the frame loop keeps in a caller-frame word) are **never touched** in the 130 s window. So the frame loop does
  not read the streamed data it keeps a pointer to.
* **The pipeline workers stop at ~7 s and never wake again.** The loader-tid filter shows the only worker thread that was
  registered by a streamed-block touch (13368) creating slot-57 graphics records from +5.0 s to +7.13 s and then making no
  further call. The caller profile snapshots are decisive: by the first snapshot (~15 s) the counts are device slot 57 =
  **10,241** (the whole graphics record set), slot 55 = 22,843, slot 86 = 22,393; the last snapshot (~120 s) has the same
  three counts **unchanged**, while every frame-loop site scaled with time. So loading's pipeline precompile finished
  ~15 s, and after that the loader performs no device work at all.
* **The frame loop is a self-driving thread, and its per-frame fence waits work.** The hottest frame sites and their title
  callers: queue slot 35 submit at `game+0x24821E3` / `game+0x248268F`; queue slot 16 signal at `game+0x248260E`; queue
  slot 32 `PresentX` at `game+0x24824C7`; device slot 80 `WaitFrameEventX` at `game+0x248240B` / `game+0x248244E`;
  console fence slot 12 at `game+0x4FB0AEF` (a generic condition-wait helper, function `game+0x4FB0AD0`, which freezes
  the tick when a predicate callback says stop and otherwise waits with a timer); per-frame views: slot 17 at
  `game+0x4DA8F63` / `game+0x4D9F627`, slot 70 at `game+0x4D9F369`; device slot 78 `SetFrameIntervalX` at
  `game+0x247ABDE`; slot 79 `ScheduleFrameEventX` at `game+0x247AC02` / `game+0x2478DEA`. The fence slot-12 values in
  the trace grow monotonically (target 0x2 ... 0xEE6 by ~25 s) and are satisfied, so the loop is not stuck on a GPU
  fence: it is running the loading screen at frame rate.
* **No thread spins on a loader condition.** At 20-90 s the process has 86-88 threads, 74-76 in waits (ntdll waits, the
  pool's `NtWaitForAlertByThreadId` workers, `NtWaitForWorkViaWorkerFactory`, the audio semaphore). The only title-code
  sites the sampler catches are transient cache-line zeroing loops (`game+0x4E8AAB3`), not a poll loop.
* **The load submit chain dead-ends at an indirect dispatch.** The DirectStorage request submission helper is
  `game+0x28C97C0` (called from `game+0x28C9680`, which is called from `game+0x28C9874`); that function has no direct
  `call` site - its address is an entry of an interface/dispatch table in `.rdata` (RVA `0x62FC780`), next to the
  console DirectStorage interface ids used by `game+0x28C950B` / `game+0x28C9579`. So the next loading stage would be
  dispatched through that table by the loading manager, and after the precompile the manager does not dispatch it.

**Diagnosis after this boot:** the gate is title-side (c); no shim primitive is named. The observable is that the loading
manager stops dispatching load jobs after the pipeline precompile while the present thread keeps the loading screen at
frame rate. What the manager itself waits for is not visible in the shim-visible APIs; the next measurement is a stack
scan at the loader-side calls (see section 9).

## 7. The answer: where the GPU-side step is missing

* **Do the streamed bytes reach a texture?** No. They reach a title CPU deserializer and, in the whole 130 s window, no
  device call names them - not directly, and not through a descriptor an argument points to. The first streamed resource
  (50 blocks) is deserialized; every sampled later block is untouched. The "streamed bytes -> sampled texture"
  hypothesis is closed: the sampled T# image is black because the streamed content never enters a GPU resource.
* **What is the missing GPU-side step?** There is no GPU-side step for the streamed 256 MiB in the measured window. The
  data ends on the CPU: no copy is recorded, no placed resource or heap is created at a block address, no view names one.
  The Xbox call shape at the three console sites is PC-shaped and carries descriptor destinations, so the A/B's
  "console-addressed view" reading is withdrawn. The GPU publication step that does exist for CPU-written memory is the
  cache-line writeback (`clwb`) after each descriptor creation - a UMA flush, not a copy, and it is not applied to the
  streamed blocks.
* **The sampled texture's own gap is separate and already watched.** `framecap`'s page-writer watch (linear layout
  default) reports the sampled T# placed texture's title pages all-zero and never written over 150 s, and this boot's
  readback agrees: T# image format 37 3840x2160 (title VA `0x...22D0000`), 128x128 window mean 0 0 0, non-black
  0/16384; target readback 0/16384; write control 51/102/204; the V# constants are plausible. So the black sampled
  image is "the page fill never happened", not "a fill landed elsewhere".
* **XG is not the carrier either.** In this boot the texture computer did: suggest 44, create 24, `GetResourceLayout`
  (slot 2) 2, `CopyIntoSubresource` (slot 8) 65 calls copying 34,044 bytes, slot 10 never called. The first copy is the
  256x128 atlas from static title CPU memory; the rest are 4-byte 1x1 defaults from stack addresses. None of them reads
  a tracked streamed block, and no host-runtime first touch was observed on any of the 32.
* **Timing/wait:** no DirectStorage or GPU-fence wait is missing (sections 6 and 9): the status poll terminates, the load
  window has no queue/fence call, and every worker is in an ordinary idle wait after the pipeline precompile. The loader
  gate after that phase is the open question, not a waitable object in the stream path.

## 8. Limits of this measurement

1. Guard sampling: in the gate-naming boot the first 64 destinations and requests 4,060-4,096 were tracked one by one:
   blocks #0-#49 read, #50-#64 and the 4,060-4,096 range **not** read; the eight sampled later destinations (requests
   512, 1024, ... 4096) are proven untouched; requests 65-511 and 4,097+ are unobserved. The block #49 read on a second
   thread shows the consumer also runs on that thread (through a host memcpy).
2. `PAGE_GUARD` covers only pages wholly inside a block; a consumer touching only a block's first or last partial page
   would not fault. The observed consumer walk crosses the first guarded page, so this did not hide it here.
3. The load-stall boot is 130 s; the title's pipeline precompile itself runs past the load trace's 7 s window end, so the
   exact end of the pipeline burst is bounded by the first submit (after 15 s), not measured. `framecap`'s 900-1200 s
   boot tests whether the loading screen ever changes later.
4. The load-window queue/fence statement is about the window `+1.5` s to `+7.0` s of the device-proxy clock. Later queue
   signals exist (the frame loop uses them) and are not part of this conclusion.
5. The structure scan covers 256 bytes from each readable word pointer - enough for the measured descriptor structs, not
   a proof for arbitrary pointer graphs. The slot-70 structure scan runs every 1024th call (14 samples); slots 18/19 on
   every call (61 calls total).
6. Content classes are first-256-bytes statistics and container-tag matches, not decoding; no byte value is printed.
7. The address values change from boot to boot (ASLR/runtime allocation); the RVAs, counts, shapes and the word-origin
   analysis are the stable facts.
8. Console slot 70's *semantics* are not decoded: it is measured as a 3-argument console descriptor creation whose second
   argument is a CBV-shaped struct, refused by name, 13,568 calls per boot. Nothing in this increment depends on its
   meaning; it is named as a device-side follow-up.
9. The pipeline-refusal counts in section 6 come from a working tree that carried other agents' in-progress
   `graphics_pso` changes; the loader-gate conclusion does not use them.

## 9. Gates, files and the next falsifier

Gates: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` (both on the binary the
load-stall and gate-naming boots ran). All switches are off by default and alter no answer. Files touched:
`port/shims/dstorage.cpp`, `port/shims/d3d12_xs_device.cpp`, this page. Evidence boots on disk (outside the
repository): `boot-shape1`, `boot-shape2`, `boot-shape3` (the call-shape increment), `boot-load1` (the load-stall
increment) and `boot-gate1` (the gate-naming boot), each with `stderr.log` and `summary.txt`.

Next, smallest honest first - the stream itself and DirectStorage emulation are closed, so the loader gate is the target:

1. **Stack-scan the loader-side calls (needs one 130 s boot; the smallest unblock).** The gate-naming boot showed the
   direct-call chain of the DirectStorage submit helper dead-ending at an indirect dispatch (`game+0x28C97C0` is an entry
   of the `.rdata` table at RVA `0x62FC780`), so the loading manager must be reached through an indirect frame. Two cheap
   additions close that: (a) in `GetHResult`, record `_ReturnAddress()` from `IsComplete` itself (today the recorded
   caller is the shim's own inlined `GetHResult`, `palmetto.exe+0xDD619`), which yields the *title polling loop* RVA; and
   (b) scan the calling thread's stack for title RVAs on the first few status polls and request enqueues, which yields the
   indirect frames above the polling loop. Then disassemble that loop's exit condition and state word, and watch the word
   (title base is known) across a boot to name the transition that never happens. Nothing about the runtime needs to
   change for this; it is instrumentation only.
2. **Test the input gate if a virtual pad appears.** The observed loop is a loading screen with a blue element
   (`docs/frame_capture.md` section 7); a "press a button" gate cannot be driven here because the GameInput shim reads
   SDL3 gamepads and no pad is attached, so this is untestable on this host as-is. If a virtual gamepad is available,
   one boot with a button press after the frame loop starts would separate "waiting for input" from "waiting for data".
3. **Console slot 70.** 13,568 calls per boot, 3 arguments, second argument a CBV-shaped struct, third a console
   descriptor destination, then a cache-line writeback; refused `E_NOTIMPL` by name. Decode it (its two stack structs and
   the engine object at `+0x1FA38`), because it is the largest unmodelled console device entry point in the boot and the
   only remaining place a console-addressed descriptor/view could be dropped. This does not reopen the stream lead: no
   block address appears in its words or pointed-to structures.
4. **The caller-stack structures at the five slot-19 calls.** They hold block #44/#45 pointers while the title creates
   UAVs; identify the struct fields and owning object (log the two `caller-stack` words' structures and the caller's
   object) to finish the CPU-side picture of what the deserialized data becomes.
5. The sampled-texture content frontier stays with `framecap`/`xgslots`: the T# pages are never written (page-writer
   watch), while the XG fill writes only the 32 KB atlas and 1x1 defaults. The next step there is to find which title
   path was supposed to write the sampled pages, not the streamed blocks.
