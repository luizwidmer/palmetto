# Peer streaming and IO completion: how the peering ports finish async work, and the ranked port plan

**Status: read-only peer audit, 2026-11-26. No code was edited, no boot was run, no CMake/README/NOTICE file was
touched and no git write was made.** This page is the streaming/async/IO-completion slice of the peer programme:
[cleanroom_peers.md](cleanroom_peers.md) is the broad parity matrix, [upstream_port.md](upstream_port.md) is the
no-copy policy under which every port here would be written, and [decomp_stage.md](decomp_stage.md) sections 10-15 are
the measurement this page compares against. The five peer clones were refreshed before this audit; their source and
docs were read only, never copied. Peer claims are self-reported; where this page states a mechanism as fact it cites
the peer file and commit, and where it states a mechanism as our measurement it cites the page and RVA.

## 1. Question, snapshots and method

The question is narrow. Our boot with the class-0xa mitigation holds stage 3 for minutes; the session-3 loading request
is created, attached and stepped every frame, but the async engine's out-record state stays `0`, so the completion
branch never runs ([decomp_stage.md](decomp_stage.md) sections 13-15). The peers report the same title through its
menus and - Five Star Recomp's wiki status, VIctory and the fork, and XCL's own README each claim it - into its world.
So what do their runtimes do on the streaming/async/IO-completion side that our runtime does not, and which of those
mechanisms is worth porting, in what order?

| peer | repository | licence | snapshot read | what it is to this audit |
|---|---|---|---|---|
| Five Star Recomp | <https://github.com/ThatRecompDev/five-star-recomp> | GPL-3.0-or-later | `45071d52` (2026-11-26) | the same title and executable version as ours: Xbox Series 1.0.2.1, title `7952987A`; its wiki `Status.md` reports booting to the world, so it has crossed our wall |
| PercyScarlett | <https://github.com/rpatty-dev/PercyScarlett> | GPL-2.0-only | `e44c465` (2026-11-25) | its own runtime (`PercyPort/`) with a VIctory runtime copy under `runtime/victory`; its DirectStorage comments name the GTA VI call sites |
| VIctory | <https://github.com/Marvisak/VIctory> | GPL-3.0-or-later | `63bcca4` (2026-11-26) | the runtime PercyScarlett is built on; tracker stage 6 |
| victory-fork | <https://github.com/fxceless/victory-fork> | GPL-3.0-or-later | `52ac4ec` (2026-11-27) | the fork with the measured in-world performance work; its async submission and title-memory modules are the most developed of the three victory trees |
| XCL | <https://github.com/xcl-dev/xcl> | GPL-2.0-only | `8b957be` (2026-11-26) | a separate runtime (kernelx, D3D12.X, DirectStorage, libHttpClient port) on the same title - its shader pack count, 26,698 programs over 26,503 records, is the corpus our pipeline check also measures |

Method: read each peer's docs and the source headers/modules that own task queues, async calls, DirectStorage, file IO,
GPU completion cells, pipeline creation and movie loading; compare each mechanism with the one our tree implements and
with the chain [decomp_stage.md](decomp_stage.md) measured. No peer code was executed, no peer tree was copied, and no
boot was run for this audit. The previous parity snapshot recorded older commits (`0bfa755`, `6d12f58`, `13f91ca`,
`9c184b3`); the SHAs above are the refreshed ones and the file paths below are from those trees.

## 2. Where each peer's async, streaming and IO completion lives

| peer | task queue and async calls | streaming, DirectStorage and file IO | GPU-side completion and pipeline async | title services it answers |
|---|---|---|---|---|
| Five Star Recomp | `src/FiveStar.Loader/Shims/Gdk/TaskQueueService.cs`, `TaskPort.cs` (Manual, ThreadPool, SerializedThreadPool, Immediate), `TaskWaiter.cs`, `TaskQueueMonitor.cs`, `AsyncService.cs` (DoWork on the work port, `E_PENDING`, completion routine on the completion port, one cleanup), `AsyncProviders.cs`, `XAsyncShim.cs`, `XTaskQueueShim.cs` | `src/FiveStar.Loader/Shims/Storage/StorageQueue.cs`, `RequestExecutor.cs`, `ReadSlot.cs` (overlapped unbuffered reads in reusable 4 KiB-aligned staging), `StatusArrayMethods.cs`, `QueueItemKind.cs` (Request, Status, Signal), `PhysicalPageSink.cs`, `BcPackPages.cs`, `NativeZlibInflater.cs` | `src/FiveStar.Loader/Shims/D3D12X/BottomOfPipeWrites.cs`, `MemoryWaits.cs`, `MemoryWaitWriters.cs`, `QueueProgress.cs`, `Present/GpuFrameThrottle.cs` | GDK task queues and XAsync, console `XCRDAPI.dll` DirectStorage, D3D12.X list completion cells |
| PercyScarlett | `PercyPort/crates/percyport-hle/src/modules/xasync.rs` (XTaskQueue and XAsync: work and completion ports, four dispatch modes), `xgameruntime.rs` | `PercyPort/crates/percyport-hle/src/modules/dstorage.rs`: one worker pool for every queue, weighted round robin over queue priorities, completion stays in queue order, destination is an address or physical pages; also `files.rs`, `storage.rs`, `media.rs` | `runtime/victory/crates/victory-gpu/src/host.rs` (per-submission wait, flights and the late wait), `memory.rs` | GDK runtime, DirectStorage, file system, Media Foundation resolver (local files only) |
| VIctory | `crates/victory-hle/src/modules/xasync.rs`, `crates/victory-gpu/src/queue.rs` | `crates/victory-hle/src/modules/dstorage.rs`, `files.rs`, `storage.rs` | `crates/victory-gpu/src/queue.rs` (submitted lists run on the host and are waited for, so fence signals complete at once), `host.rs` late wait | GDK runtime, DirectStorage, file system, Media Foundation |
| victory-fork | `crates/victory-hle/src/modules/xasync.rs`, `crates/victory-loader/src/threads.rs` (title-image thread notices) | `crates/victory-hle/src/modules/dstorage.rs`, `files.rs` | `crates/victory-gpu/src/queue.rs`, `queue2.rs` (copy lanes with their own fences and holds), `memory.rs` (title memory by raw address, per-64 KiB page directory and shader faults), `profile.rs` (thread sampler) | the same runtime plus its measured in-world work |
| XCL | `crates/xcl/src/hle/xgameruntime/taskqueue.rs` (libHttpClient port: four dispatch modes, reference-counted ports, delayed callbacks, terminate drains cancelled work), `xasync.rs` | `crates/xcl/src/hle/dstorage.rs`: one worker thread per queue, file and memory sources, zlib and BCPack, status arrays, `EnqueueSignal` sets the fence from the CPU; the title's own VFS for file reads | `crates/xcl/src/hle/d3d12x/pso_async.rs` (stand-in at once, real pipeline built on a worker, compute waited for at its first bind), `list_x.rs` (bottom-of-pipe writes and waits), `frame.rs` (frame clock and pacing) | GDK runtime, DirectStorage, D3D12.X command lists and frame clock |

### 2.1 Mechanism notes, peer by peer

- **Five Star Recomp** is the only peer that is the same build as ours. Its DirectStorage is a CPU implementation with
  one worker thread per queue: `ReadSlot` runs an overlapped unbuffered read whose range is widened to whole sectors,
  `RequestExecutor` runs entries in order, `QueueItemKind` has exactly three kinds (a request, a status entry, a fence
  signal), and `PhysicalPageSink` writes a request whose option bit says the destination is a list of physical page
  numbers. Its task queue is a full in-process implementation of `XTaskQueue.h` and `XAsync.h`, including monitors,
  waiters, delayed callbacks and termination. Its GPU side implements the console bottom-of-pipe writes and the
  `Wait64BitValueX` memory wait with a queue split, a watcher thread and queue-progress reporting.
- **PercyScarlett and VIctory** share a runtime and therefore a mechanism. Their `dstorage.rs` runs every queue on one
  worker pool, in parallel, started in queue order, with weighted round robin over priority classes and in-order
  status completion; a request destination may be an address or physical pages. Their `xasync.rs` serves the task
  queue and async-call contract as the task-queue provider object. PercyScarlett's own `PercyPort` repeats the modules
  for its own loader; its `media.rs` keeps Media Foundation as the host's but replaces the source resolver so a title
  opens local files only.
- **victory-fork** carries the same base plus the measured in-world work. `queue2.rs` adds copy lanes with their own
  fences and a documented wait/hold contract, `memory.rs` maps title memory for shaders by raw 40-bit address with a
  per-page directory and shader fault collection, and `profile.rs` samples the layer's own threads (submit, record,
  pipeline and late-wait workers) which is how its timing claims are checked.
- **XCL** has the widest streaming surface. Its DirectStorage is the console interface layout read off a title's own
  call sites (its comment records Fortnite for those slots), with one worker thread per queue and a CPU fence signal. Its
  task queue is a port of Microsoft's libHttpClient (MIT) implementation with the same four dispatch modes. Its pipeline
  creation is the mechanism this audit ranks first: a pipeline whose DXIL is not cached gets a stand-in at once and the
  real pipeline is built on a worker, and a compute pipeline is waited for at its first bind because a skipped dispatch
  would corrupt results.

## 3. The mechanisms, side by side with ours

| mechanism | Five Star Recomp | PercyScarlett and VIctory | victory-fork | XCL | our tree |
|---|---|---|---|---|---|
| task queue | in-process queue objects with work and completion ports, monitors, waiters, delayed callbacks, termination | same contract in `xasync.rs`, four dispatch modes | same | libHttpClient port, reference-counted ports | `vendor/xtaskqueue` (libHttpClient) through `QueryApiImpl`; [task_queue_parity.md](task_queue_parity.md) measured only queue setup, 26 class calls, and zero submit, dispatch, wait or XAsync calls |
| async call completion | provider DoWork, `E_PENDING` completes, completion routine on the completion port, one cleanup | `XAsyncBegin`, `XAsyncSchedule`, `XAsyncComplete`, `XAsyncGetStatus(wait)` | same | `XAsyncBegin` and `XAsyncComplete` with a cached-only provider attempt | same contract from the vendored implementation; no async call measured in the load window |
| DirectStorage queue | one worker thread per queue, status and fence entries, physical-page sink | one worker pool, priority round robin, in-order status, physical pages | same | one worker thread per queue, status array, CPU fence signal | one worker thread per queue, in-order completion, staging/pinning/page-list behind `PALMETTO_DS_*`; the page-list path measured 0 requests ([page_resolver.md](page_resolver.md)) |
| file read completion | overlapped unbuffered read into reusable staging, then inflate or decode, then the status entry | direct file reads per request | direct file reads per request | reads through the title VFS | host reads with a first-touch repair of `ERROR_NOACCESS` and one retry (`file_system.cpp`, RT-READFILE-NOACCESS) |
| GPU completion cells | bottom-of-pipe writes; `Wait64BitValueX` with a list split, a watcher thread and a 5 s bound; queue progress kept per queue | the base runtime rarely reads cells; the fork's copy lanes carry fences and holds | copy lanes wait on and are waited for by the direct queue, each wait's partner ahead of it | bottom-of-pipe writes and waits with a 2 s bound and list splits | write model at record time; wait evaluated at record time; the cross-list hold is not carried ([bottom_of_pipe.md](bottom_of_pipe.md) limit 1) |
| pipeline creation | stand-in until ready; a first use waits only when the programs are ready | pipeline workers and a disk cache | same, plus finishing a pipeline queue entry whose build failed | stand-in at once, real pipeline on a worker, compute waited for at its first bind | `port/src/deferred_compile.h` queue plus a synchronous first-use wait on the submitting thread; measured stalls 0.5-63 s ([decomp_jobs.md](decomp_jobs.md) section 7) |
| movie route | its own call wrapper around the title's `Bink2Scarlett.dll` | Media Foundation resolver, local files only | same | wrap the Bink exports to fast-forward non-looping movies while still loading the title's DLL | the title's movie library is mapped and opens twice, but its entry never runs and the per-frame decode `0x2EF0E0` never executes ([movie_path.md](movie_path.md)) |

Two negatives belong here because they decide the port plan. First, our task-queue surface is not the stalled job's
dependency: the measured calls are queue setup only, and the stall's wait objects are title-owned events and
semaphores ([task_queue_parity.md](task_queue_parity.md), [wait_forensics.md](wait_forensics.md)). Second, our
DirectStorage completion is not the stalled loader's dependency either: the status arrays report complete, the poll
loop terminates where the console's would, and the loader object's own armed flag is what stays false
([streamed_consumer.md](streamed_consumer.md) sections 6b-6d). A peer's richer storage implementation is therefore not,
by itself, the reason their equivalent completes.

## 4. Our measured chain, end to end

| step | measured shape in our boots | page |
|---|---|---|
| request attach | `subscriber-start 0x2355DD0` 1 hit, record reset `0x22310D0`, command id 0; `stream-submit 0x1DBC0F0` 1 hit; `stream-pending-register 0x402F0F0` 1 hit stores the request at `stream+0x120`; request vtable `game+0x658DE18`, key `0x9837FB2E`, owner `+0x30` the stream, flag `+0x3c` | [decomp_stage.md](decomp_stage.md) 13.3, 14.3, 15.3 |
| command start and registration | `stream-command-start 0x402F120` 1 hit; `register-open 0x402D010` 4 hits; a start event on the stream's queue `+0x128` | 15.3 |
| open gate | `stream-open-gate 0x45E640` 55 hits; the first condition opens through the registry engine pointer at `table + (id-1)*0xc0 + 0x10`, written by the title setter `0x5AEAC50` (writer `game+0x5AEAC5F`); the second condition (entry pointer non-null and `entry+0xac == 2`) is never the opener, and no title store writes that state block in the window | 15.3, 15.4 |
| cache walk | the loading update walks the request cache `0x528CD20` every frame; 11 buckets, one entry, the request in cache at every sample | 14.4 |
| stream pump | `stream-pump 0x45E570` 9,588 hits, `stream-step 0x402E260` 9,592 hits, `async-engine-step 0x402E340` 16 hits all inside the 0.7 s attach window and zero after | 14.3, 14.5 |
| out-record state | `completion-dispatch 0x402E8A0` 3 hits, each with an out record of vtable `game+0x658DF20` whose `+0xac` reads 0; each is re-registered and closed instead of completed | 15.3, 15.4 |
| completion branch | state-2 branch `0x402E050` 0 hits; cache completion `0x4043390` 0; request consume `0x4043300` 0; subscriber completion `0x2355D40` 0; record writer `0x5862370` fires only for two other loading records, never for `0x78AC9C8` | 13.3, 14.3, 15.3 |
| record and session | completion dword `image+0x78ACB20` (slot 4, `+0x18`) stays 0 in every sample; session word stays 3; machine 3/3; movie player state `0x9` with `buffers 0`; every capture and readback black | 10.3, 12.3, 15.4 |

The case split is settled: the request is created, attached, cached and stepped every frame, and the engine does run -
but the records it produces carry state 0, the dispatch re-registers them, and whatever should store state 2 into the
out record's `+0xac` is title-side code that did not run in the measured windows. Sections 13.5 and 14.6 measured that
no file, DirectStorage, D3D12 fence, copy or queue submission is issued at or after the start, so the awaited producer
is not one of our runtime answers on that path.

## 5. The comparison, and the most plausible reason their equivalent completes

### 5.1 The same-build anchor

Five Star Recomp is not an analogy for the loading screen; it is the same code. Its `debugging.md` names the same
stage-3 entry `0x140812620` and the same two flags our [decomp_stage.md](decomp_stage.md) sections 3 and 5 name: the
loading-screen flag `0x147453ED4` (our `0x7453ED4`) and byte `+0x1B3` of the session object at `[0x14AAE3B70]` (our
`0xAAE3B70`). Its `GameHangWatch.cs` reads exactly those globals, and its account of the fresh-load path is the one
our measurements see from below: the entry does not go straight to state 4, it stops the loading screen first and waits
without a limit for the loading-screen render thread to signal the end of a frame, and that thread "renders through
`0x1443c38a0` and waits for each of the five `Subrender:%02u` workers (`0x1443c4140`)". Our own decompilation names the
same five-thread pool: `game+0x43C38A0` creates five threads whose body is `game+0x43C4140`, and our wait tracer
measured their per-thread events as never set in the whole 130 s window ([decomp_jobs.md](decomp_jobs.md) section 4,
[wait_forensics.md](wait_forensics.md) section 2).

### 5.2 What they do that we do not

| our chain step | our measured state | the peer equivalent | the difference that matters |
|---|---|---|---|
| the five worker jobs that the loading screen and the loading record wait on | the five pool-2 events are never set; the loading record's five completion dwords stay 0 | five-star's loading-screen frame waits for its five Subrender workers; its two measured causes of that frame taking over 30 s were workers parked in the loader (pipeline stand-in waits) and workers killed by pool-allocator corruption, and its fixes make the frame complete | their worker jobs run to completion; ours never do |
| the async engine step | 16 hits only around the attach, then zero | not documented per function by any peer; the peers instead report the loading screen and the world reached by the title's own state machine | they report the state machine past the record this poll waits on, so the record's completion never had to come from our layer |
| the producer of the out-record state | a title-side store that never runs; no shim, D3D12 event or IO on the path | their GPU completion is console-faithful: bottom-of-pipe writes at GPU order, memory waits with a list split and a watcher, queue progress kept | a state store that depends on GPU ordering would be missing in our tree, which writes and tests the cell at record time |
| pipeline creation on a title thread | the submitting thread can wait for a build, measured stalls 0.5-63 s on this tree | XCL gives a stand-in at once and builds on a worker, waiting for compute only at its first bind; five-star's fix makes a first use wait only when the programs are ready | their title worker never blocks inside the layer's compiler; ours can |
| IO completion | every request delivered, every status array complete, no pending entry; the one unexplained stall is the title's own `ReadFile` retry loop | peers complete each request on the worker and hand the status the first failure since the previous entry, so a retryable error cannot reach the title | a retryable read error is the one IO answer that can park a title thread silently; our repair exists but was measured on a binary whose read path was the host's |
| task queue | queue setup only, nothing dispatched | four dispatch modes, monitors, waiters, delayed callbacks | not the difference: the title's pools are title memory in both trees |

### 5.3 Verdict

**Most plausible reason their equivalent completes: in their boots the title's own worker jobs finish, and in ours the
worker-side chain is parked.** The peers do not complete the session-3 record for us; their runtimes keep the title's
loading workers running and returning, so the loading-screen frame and the record it feeds are completed by title code
before the poll matters. Five Star Recomp, on the same build, documents both fixes that made that possible on its tree:
a title worker is never left waiting inside the layer for a pipeline build, and the title's transient pool is never
corrupted by the layer's shaders or its own allocator. Our tree still has the first pattern by construction (the
first-use wait in `port/src/deferred_compile.h` runs on the submitting thread, measured stalls 0.5-63 s) and still has
the second family open ([decomp_jobs.md](decomp_jobs.md) section 8), while its five worker events are measured never
set. The out-record state 0 is then the symptom one level down: with the worker jobs not completing, no title store
ever writes the state 2 the dispatch tests.

Two alternatives are named because they would take the plan in a different direction after the first two ports:

- **A GPU-ordered completion cell.** If the out-record state is written after a console memory wait whose producer is
  another list, our record-time model cannot observe it. This is the second-ranked family below, and it becomes the
  leading one if the decisive diagnostic of [decomp_stage.md](decomp_stage.md) section 15.6 (treat the out record as
  state 2 at `0x402E8A0`) completes the request and then the state store turns out to sit behind a wait.
- **The media/movie gate after session 4.** The movie player sits at state `0x9` with `buffers 0`, and the peers do
  serve the title's own Bink DLL. But our boot never reaches session 4, so this is the gate after the wall, not the
  wall; port 7 below is deliberately last of the behavioural ports.

What the evidence does **not** support: that our task queue is the wall (measured silent), that our DirectStorage
completion is the wall (measured faithful), or that a peer's richer storage implementation is what turns their boot.
Those three negatives are the most useful part of this audit, because they keep the port plan off the paths two
earlier slices already closed.

Confidence: high that the wall is title-side worker/wake ordering rather than a missing runtime call (sections 10.5,
12.3, 13.5, 14.6 all measured no runtime answer on the path); medium that the peer-identifiable difference is the
synchronous pipeline wait rather than pool corruption or a GPU-ordered cell; low for any specific RVA in the peer
trees, which publish no loading-record function.

## 6. Ranked port plan

Ranked by (a) how directly the mechanism stands between the stalled worker chain and completion, (b) how cheap the
falsifier is, and (c) how much of the port our tree already carries. Effort is S (about a day), M (two to five days),
L (a week or more), each including its page, tests and one A/B boot as [upstream_port.md](upstream_port.md) requires.
"Boot" says whether measuring the port needs a boot at all.

| rank | port | mechanism to port | our target files | effort | test or falsifier | boot |
|---|---|---|---|---|---|---|
| 1 | Never wait for a pipeline build on a title thread | stand-in returned at once; the real pipeline built on the layer's workers; compute waited for only at its first bind, with a budget; a graphics draw skipped or deferred rather than the caller blocked (Five Star Recomp and XCL both do this) | `port/src/deferred_compile.h`, `port/src/graphics_pso.cpp`, `port/shims/d3d12_xs_device.cpp`, `port/src/vulkan_backend.h`, `port/test/deferred_compile_test.cpp` | M | unit: a first-use caller gets a decided answer with no driver build; boot A/B with the wait off: the five pool-2 events are set during the load phase, the record's slot-4 dword becomes non-zero and `0x402E050` runs. Falsifier: workers still parked and the dword still 0, so the wait is not the wall | yes |
| 2 | Make a load read either succeed or fail terminally | the worker hands the title the first terminal result of a read; a destination page nobody touched is committed before the copy, and a retryable host error is converted once, never repeated to the title's retry loop | `port/shims/file_system.cpp`, `port/shims/first_touch.h`, `port/src/main.cpp` (read trace), `port/test/file_system_test.cpp` | S | unit: an uncommitted destination is committed and the first `ReadFile` returns true; boot with the read trace: the title's `Sleep(1)` retry site `game+0x3142B2` does not start at the same second as before. Falsifier: the loop still starts, so the error is not 998 (a 997 would mean an overlapped handle, a different fix) | yes (one trace arm) |
| 3 | Console-faithful completion delivery on the IO worker, including the physical-page destination | status, fence-signal and request entries run in queue order on the worker; a request whose option bit names a physical page array writes the page frames the title maps, so every mapping sees the bytes | `port/shims/dstorage.cpp`, `port/shims/dstorage_advanced.h`, `port/shims/xmem.cpp`, `port/test/dstorage_test.cpp` | M | unit: a page-list request writes its page array and a mapping of it reads the bytes; boot with `PALMETTO_DS_PAGES=1` and the load trace: the destination-pages counter and the consumer's reads go non-zero. Falsifier: still zero at this population, so the port stays off by default | yes (one arm) |
| 4 | Cross-list memory wait: hold the queue, release it at the GPU-ordered cell | a list split at the wait, a queue held until the cell reaches its comparison, and a watcher thread that signals the queue's fence, with a bounded timeout (Five Star Recomp, XCL and the fork all carry a form of it) | `port/src/vulkan_backend.h`, `port/src/queue_holds.h`, `port/shims/d3d12_xs_device.cpp`, `port/test/xs_proxy_mock_test.cpp`, `port/test/d3d12_queue_wait_test.cpp` | M | unit: a wait whose producer is a different list holds that batch and releases it when the cell reaches the value; an unmet wait releases at the bound and is counted. Boot: an unmet wait appears, the held batch is released by the signalling thread, and no watchdog abort follows. Falsifier: every wait is met at record time (as measured so far), so the mechanism is unobservable | yes |
| 5 | Bottom-of-pipe write at GPU order instead of record time | the measured value is written by the card after the list's earlier work (a desktop immediate-write marker at the resolved title address), not by the CPU when the recording call is made | `port/shims/d3d12_xs_device.cpp`, `port/src/vulkan_replay.h`, `port/src/buffer_va_index.h`, `port/test/replay_write_immediate_predication_test.cpp` | M | replay test: the write is recorded as a GPU immediate write at the resolved address; boot A/B: a cell the title reads shows the GPU-ordered value and not the record-time one. Falsifier: both models read identically at this population | yes (A/B) |
| 6 | Task-queue and async parity test against the vendored implementation | close the contract gap only if one exists: `E_PENDING` completion, callback on the completion port, cleanup once, dispatch modes, monitor and waiter semantics (Five Star Recomp, XCL and both victory trees carry their own implementations) | `port/shims/xgameruntime.cpp`, `port/test/xgameruntime_test.cpp`, `vendor/xtaskqueue` | S | GPU-free unit parity for the measured setup calls and the pending/cleanup contract; one trace boot with `PALMETTO_USER_TRACE` that keeps the surface silent. Falsifier: a submit, dispatch or wait slot appears in the load window and diverges from the contract | yes (one trace arm) |
| 7 | Serve the title's own movie stream route | the title's Bink DLL and its file handles are served so the loading screen's movie stream can start and the per-frame decode runs, as every peer runtime does in some form | `port/shims/library_loader.cpp`, `port/shims/file_system.cpp`, `port/src/main.cpp`, `port/test/bink_abi_test.cpp`, `port/test/bink_play.cpp` | S/M | library test already covers the export surface; boot after the redirect arm of [decomp_stage.md](decomp_stage.md) 15.6: the movie start sites and `0x2EF0E0` appear once session 4 is reached. Falsifier: session 4 reached and `0x2EF0E0` still 0, so the movie is not the next gate | yes |
| 8 | Overlap submission with the card instead of waiting per submission | send submissions as flights, settle them at the points where the title can observe them, and give copies their own lanes with fences and holds (victory's late wait, the fork's copy lanes, XCL's frame clock) | `port/src/vulkan_backend.h`, `port/src/queue_holds.h`, `port/src/retire_tickets.h`, `port/shims/d3d12_com.cpp`, `port/test/d3d12_queue_wait_test.cpp`, `port/test/vk_fence_dispatcher_sync_test.cpp` | L | A/B boot counting title threads inside our submission and the pool wake cadence. Falsifier: no title thread is inside our submission during the stall (the frame thread is in its 33 ms poll), so this is a performance fix and not the completion wall | yes (A/B) |

### 6.1 How the ranks were chosen

Ports 1 and 2 are the two cheapest experiments that can move a *title-side* worker out of a state this tree creates,
and each has a same-boot observable that is already instrumented (the pool events, the record's slot-4 dword, the read
retry site). Port 3 is the storage half the peers carry furthest, but our own earlier measurements make it the least
likely producer, so it is ranked above the GPU-cell ports only because it is a bounded, off-by-default addition with a
unit test. Ports 4 and 5 are the GPU-ordering family: they only matter if the out-record state store turns out to sit
behind a console memory wait, and both stay behind their current switches until a boot shows an unmet cell. Ports 6
and 7 are cheap and close questions this tree has already opened; port 7 cannot be measured before session 4 and is
therefore last of the behavioural ports, not first. Port 8 is a performance port, recorded so it is not mistaken for
the wall.

## 7. Sequence, gates and what would change the order

1. The decisive diagnostic is already named by the measurement, not by a peer: the one-byte redirect at `0x402E8A0`
   in [decomp_stage.md](decomp_stage.md) section 15.6 needs the lead's routing because it is a title-side patch. If it
   completes the request, the missing producer is the out-record state store and ports 4 and 5 move to the front. If
   it does not, ports 1 and 2 are the next boots, and the state watch is re-targeted on the out record's `+0xa8` and
   `+0xac`.
2. Ports 1 and 2 are independent and can run as one A/B pair each; both are opt-in switches and both keep the default
   path byte-identical.
3. Port 3 and port 6 are unit-first; their boots measure counters, not the stage word.
4. Ports 4 and 5 only start after a boot shows an unmet cell or a value only the GPU could have produced.
5. Port 7 waits for session 4; port 8 is scheduled last and only if the stage is moving.

## 8. Limits

- **No boot and no code.** This audit read five peer trees and our own pages; it changed nothing and measured nothing
  new. Every "our state" value is another page's measurement, cited by section.
- **Peer claims are self-reported.** Only Five Star Recomp documents our loading-screen path by RVA; the two victory
  trees and PercyScarlett are a runtime family, not independent implementations, and XCL is a separate codebase on the
  same title. Their docs describe their own boots, their own overlays and their own titles' flow, so a mechanism's
  presence there is evidence of what they found necessary, not of what our boot needs.
- **No peer publishes our loading-record functions.** Five Star Recomp names the stage-3 entry and the loading-screen
  flags, which is why it anchors section 5; none of the five publishes a per-function account of `0x41E8C0`, the
  loading record or the async engine, so the cross-check stops at mechanism level.
- **The five-subrender identification is an inference.** Our decompilation names a five-thread pool
  (`game+0x43C38A0` / `game+0x43C4140`) and the loading record has five slots; five-star names five `Subrender` workers
  on the same loading-screen path. That the record's five slots are those five workers is plausible and unmeasured; port
  1's falsifier is designed to test it without relying on the identification.
- **The commit SHAs are a snapshot.** The peer trees move; this page pins the five SHAs in section 1 and every cited
  file is from that snapshot.

## 9. No-copy and attribution

**No peer code, comment text, identifier or table was copied into this page or into any proposed implementation.** The
peer trees are evidence: their commits, measurements and docs tell us which mechanism the title needs and what a call
shape means; the implementation in this repository is our own, in our structure, naming (`PALMETTO_*`, `rt::`) and test
style, written from our own measurement as [upstream_port.md](upstream_port.md) requires. Where a peer's reading and our
measurement disagree, our measurement wins and the disagreement is recorded here (the queue-setup-only task surface and
the faithful DirectStorage status semantics are the two cases in this slice). Each port above names the peer file that
pointed at the mechanism and the port's own page would repeat that attribution in its last section; the `NOTICE.md` row
is the lead's to add at commit time, not this page's.

What our own implementation would look like is fixed by our layout, not by theirs: a bounded, switchable mechanism in
the repository file named in section 6, off by default, with its own GPU-free unit test in `port/test/`, our
`startup_trace` events and measurement switches, comments that state the measured facts and limits, and the default
path byte-identical when the switch is unset. The state machines our tree already owns stay the primitives it composes
with - `CompileQueue` in `port/src/deferred_compile.h`, the hold and release machinery in `port/src/queue_holds.h` and
`port/src/vulkan_backend.h`, and the storage queue in `port/shims/dstorage.cpp` - so a port adds a caller, a policy or
a test to them rather than importing a peer's module.

Reference material read for this audit, by snapshot (all licences as in section 1):

- Five Star Recomp, commit `45071d52`: `src/FiveStar.Loader/Shims/Gdk/` task queue and async service files,
  `src/FiveStar.Loader/Shims/Storage/` the CPU DirectStorage queue, `src/FiveStar.Loader/Shims/D3D12X/` the bottom-of-pipe
  and memory-wait files, `src/FiveStar.Loader/Diagnostics/GameHangWatch.cs`, its `directstorage.md`, `debugging.md`,
  `d3d12x-shim.md`, `loader.md`, its wiki `Console-Runtime.md` and wiki `Status.md`.
- PercyScarlett, commit `e44c465`: `PercyPort/crates/percyport-hle/src/modules/dstorage.rs`, `xasync.rs`, `files.rs`,
  `media.rs`, `storage.rs`, and the same modules under `runtime/victory`, plus `runtime/victory/crates/victory-gpu/src/host.rs`.
- VIctory, commit `63bcca4`: `crates/victory-hle/src/modules/dstorage.rs`, `xasync.rs`, `files.rs`, `media.rs`,
  `crates/victory-gpu/src/queue.rs`, its `architecture.md`.
- victory-fork, commit `52ac4ec`: `crates/victory-hle/src/modules/dstorage.rs`, `xasync.rs`,
  `crates/victory-gpu/src/queue.rs`, `queue2.rs`, `memory.rs`, `profile.rs`, its `fork-ray-tracing.md`.
- XCL, commit `8b957be`: `crates/xcl/src/hle/dstorage.rs`, `crates/xcl/src/hle/xgameruntime/taskqueue.rs`, `xasync.rs`,
  `crates/xcl/src/hle/d3d12x/pso_async.rs`, `list_x.rs`, `crates/xcl/src/hle/bink.rs`, its `HANDOFF.md` and design spec.
- Our pages this audit compares against: [decomp_stage.md](decomp_stage.md) sections 10-15, [decomp_jobs.md](decomp_jobs.md),
  [streamed_consumer.md](streamed_consumer.md), [wait_forensics.md](wait_forensics.md), [bottom_of_pipe.md](bottom_of_pipe.md),
  [task_queue_parity.md](task_queue_parity.md), [page_resolver.md](page_resolver.md), [movie_path.md](movie_path.md),
  [cleanroom_peers.md](cleanroom_peers.md) and the external parity evidence table `D:\leonida-work\peer_parity.md`.

The upstream projects remain the copyright of their contributors under the licences named in section 1; this repository
grants no rights over their trees and claims none of their work as its own.
