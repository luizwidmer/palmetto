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

## 10. Measured: implemented port 1 - the non-blocking pipeline hold, so no title thread waits for a pipeline build

**Status: measured; opt-in and off by default.** This section implements rank 1 of the section 6 plan. The deferred compile
([deferred_compile.md](deferred_compile.md)) already moves the driver build off the title's threads, but the submission
that first names a still-unbuilt compute pipeline waits for that build on the thread that submits - a title thread - and
the peer audit names exactly that pattern as the documented worker-stall cause (sections 3, 5.2 and 6). The port here is
our own: a bounded, switchable **submission hold**. When the switch is on and a submitted list names a deferred compute
pipeline the compile workers have not finished, the batch is held instead of waited for, in submission order, with its
lists copied and their pipeline states pinned; it is replayed by the next submit/present on the submitting thread or by
a layer-owned hold flusher as soon as every pipeline it names has finished. The switch is `PALMETTO_PIPELINE_HOLD`;
unset is the previous path exactly. No peer code was copied (sections 1-9 are the audit this port was planned from;
[upstream_port.md](upstream_port.md) is the policy; section 10.8 is the attribution).

### 10.1 The measured wait this slice removes

Two pre-change measurements were taken before any code of this section existed. The first is a 300 s light boot of the
current committed behaviour with the class-0xa mitigation and the section-12 content environment from another in-flight
slice's arm on this tree (`boot-stream3`, binary `208D7699...`, one boot under the boot lock, deferred compile on with
bound 4,096 and 4 workers): it logged **9,993 submit-side pipeline waits, the longest 450.2 ms**, with the compile queue
reporting 2,742 deferred pipelines, 356 built, **0 fallbacks** and a longest single worker build of **104.1 s**. The second
is the same environment booted by this section before its code was written on the pre-change binary
(`boot-nb-baseline`, `57EFCB2E...`, 56.7 s, 1 device loss, stopped on a title-side write
`game+0x4d9f6c4` at `0x14cb183c0` before reaching stage 3 - the stage-3 race of
[decomp_first_ui.md](decomp_first_ui.md) section 17, not this slice):

| measurement | `boot-nb-baseline` (pre-change, 56.7 s) |
|---|---|
| submit-side pipeline waits | **17, total 511.1 ms, the longest 510.7 ms** |
| of those, a driver build run on the waiting (title) thread | **1** - tid `0xd14c...` made 1 compute driver call of 510.7 ms and 0 creation calls, i.e. it claimed a queued compile task and ran the driver build itself |
| graphics pipeline driver calls on title threads | **2 calls, 2.3 ms** (one thread, at first bind) |
| compute driver calls | 325, all on the 4 `[compile worker]` threads; longest single 141.8 ms in this window |
| caller threads / creation threads | 19 (14 title threads + 4 compile workers + 1 hold-free layer thread) / 14 |
| session/stage state at the stop | init 2, loading 2, session 1, machine 2/2: stage 3 not reached |

Both measurements say the same thing: **the wait family is the compute pipeline build, not graphics**, and when a
submission waits out a build it is one driver call long (0.45-0.51 s in these windows, 0.5-63 s in the smaller boot set
of [deferred_compile.md](deferred_compile.md) section 6). The wait is in `VulkanQueue::execute_now` ->
`DeviceState::prepare_deferred_compute_pipelines`: the submitting thread first asks the compile queue to claim and build
the task it needs (`CompileQueue::wait_or_build`), or waits for the worker that already owns it. The submitting thread is
a title thread - it is the frame/present thread, the same pool the loading jobs run on - so a title thread is inside
`nvgpucomp64` for up to a minute, which is the measured stall the peer audit ranks first. The five pool-2 events are
unset in the same windows ([wait_forensics.md](wait_forensics.md) section 2: the `game+0x43C421E` wait site has 5 waits
and 0 signals, the five per-thread events have no traced set), which is the title-side state that wait leaves behind.

### 10.2 The design

Our own mechanism, in our structure and naming, composed from the parts this tree already owns: the `CompileQueue` task
map and the queue's CPU-side hold machinery (the `held_` deque of [queue_holds.h](../port/src/queue_holds.h), already
used for Wait-before-Signal).

| piece | what it does |
|---|---|
| `PALMETTO_PIPELINE_HOLD` | opt-in. On: a submission that names an unfinished deferred pipeline is held. Unset: the previous path exactly - no hold, no flusher thread, no extra work in `execute()` beyond one false branch. |
| `PALMETTO_PIPELINE_HOLD_BOUND` | the most submissions held for pipelines at once, default **16**, clamped 1..1024. |
| `PALMETTO_PIPELINE_HOLD_BUDGET_MS` | how long one held submission may wait for its pipelines before the old synchronous wait is forced, default **2000 ms**, clamped 1..600000. |

- **The decision is in `VulkanQueue::execute` (port/src/vulkan_backend.h), before anything is recorded.** The context has
  two device hooks and a wake hook, registered only when the switch is on: `collect` answers "does this batch name a
  deferred pipeline the compile queue has not finished, and which ids" (read from the same task map the compile queue
  already keeps), `finished` says whether a set of ids has finished, and `wake` starts the device's hold flusher. The
  batch is held when `collect` finds at least one unbuilt id and the hold budget has room. Nothing is built or waited
  for on the submitting thread on that path; the call returns as an ordinary held ExecuteCommandLists does.
- **Order and lifetime come from the existing hold machinery.** The batch is copied into the queue's `held_` deque
  (`CommandList` copies, as the Wait-before-Signal path already does), its pipeline states and root signatures are kept
  alive by the existing `PinFn` pin, and every later submit, Signal, Wait, marker and Present is held behind it in the
  same order. A held batch's new dependency is one field: the policy token of its pipeline ids. `ready()` is
  "no pending Wait and no unfinished pipeline"; the release loop only releases the front, so a later ready batch cannot
  overtake an unfinished earlier one.
- **Release is off the title's threads.** The device starts one layer-owned **hold flusher** thread when the switch is on
  (`[hold flusher]` in the pipeline-call report). A compile worker that finishes a build marks the pipeline id finished
  in the hold policy and wakes the flusher; the flusher pumps the queues and the front batches replay on it, in order,
  as ordinary submissions. The same pump also runs at the end of any title `execute`/`Signal`/Present, so a hold whose
  pipelines are already built is completed by whichever thread gets there first - in the common case the next
  submit/present on the submitting thread, with no wait to do.
- **Two budgets, and the declared fallback.** At the bound, the oldest held batches that wait *only* for pipelines are
  completed by the previous synchronous wait on the submitting thread, in order, until the budget has room; if the gate
  still refuses the new submission, that submission takes the old synchronous wait itself and is then held for order
  only - i.e. the previous behaviour, exactly, and only when the hold budget is exceeded. A hold past its budget (for
  example one pipeline stuck in a 100 s driver compile) is completed the same way by the old wait, forced on the
  releasing (layer) thread. A batch with a pending Wait can never be forced: that would reorder it past the work ahead
  of it, so it keeps waiting for its fence as before.
- **Torn down in order.** The flusher is joined in `~Device` before the compile workers and before the Vulkan context; a
  queue destroyed with held work drops the batch, releases its token (counted as `dropped`) and reports it, the existing
  held-work teardown. A device loss drops held batches the same way.
- **What is deliberately not changed.** The graphics path is untouched: graphics pipelines are still built at their first
  bind on the recording thread ([memory_growth.md](memory_growth.md)), measured at 2 calls / 2.3 ms in the pre-change
  window, so they are not the family this slice removes. The compile queue, its worker count, its bound, the disk/recipe
  caches and the named-refusal paths are unchanged. `port/src/vulkan_replay.h` is not modified. With the switch unset the
  measured path is the one in section 10.1: the same tasks, the same submit-side wait, one extra false branch per
  execute.
- **Measurement, not behaviour.** The slice adds counters and two report lines, never shown with the switch unset except
  one startup sentence that says the hold is off; the existing `compile wait submit` timeline line gains the waiting
  thread's id so a boot can name the thread a wait happened on, and the compile report splits its submit waits into
  "waited for a worker's build" and "built by the waiting thread" (the build-on-a-title-thread case).

### 10.3 Tests

`port/test/deferred_compile_test.cpp` (the GPU-free gate target) went from 18 to **50 checks**:

| check group | what it proves |
|---|---|
| switch parsing | every off spelling (`unset`, empty, `0`, `off`, `OFF`, `false`, `no`, surrounding blanks) and every on spelling; the bound's default, floor and ceiling; the budget's default, floor and ceiling |
| hold policy state machine | a token with two pipeline ids is ready only after both finish; a later token at the bound refuses a third (the fallback signal); the expiry boundary is exact (999 us not expired, 1000 us expired, unknown token never); release/drop bookkeeping, peak, and the report line |
| shutdown | `clear()` drops the tokens still held and counts them |
| disabled policy | the unset switch's policy is disabled and reports its defaults: what the queue never consults |
| submit-wait split | a wait the waiting thread spent in a build it claimed itself is counted apart from a wait for a worker's build, and the per-thread report names each waiting thread |

`port/test/vk_queue_test.cpp` (GPU-backed, run on this machine's RTX 4090 under the boot lock) has a new section with
synthetic hold hooks: two batches copy different source patterns into the same destination range and the second source
must win (submission order); the second batch's pipeline finishes first and nothing runs and nothing overtakes; `bound=1`
forces the oldest batch through the old wait on the submitting thread (prepare hook count and `forced` counter) before
the second is held; a 1 ms budget forces an expired hold (`timeouts` counter); a queue destroyed with held work drops it
and leaves the policy empty (`dropped`); and after the hooks are removed the same lists run the default path (no hold).
It prints `pipeline hold: ordering, bound fallback, budget force and queue-teardown drop ok` and `vk_queue_test: OK`.

Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`;
the GPU-free gate subset (`deferred_compile` 50 checks, `frame_dump`, `unimplemented_policy` 98, `graphics_record` 180,
`graphics_pso` 350, `buffer_va_index`, `dstorage` 234, `xapu_chad` 87, `arena_size`, `xbox_present`, `xs_proxy_mock`)
exits 0; the docs checks pass with this section naming every switch above.

### 10.4 A/B boots

Two 300 s light boots, one binary, one at a time under `_setup/boot_lock.ps1`, the section-12 content environment from
`D:\leonida-work\fsrstable\boot_arm.ps1` plus the class-0xa mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1` and
`PALMETTO_WAIT_TRACE=60,120,240`; arm A leaves `PALMETTO_PIPELINE_HOLD` unset, arm B sets it to `1` (bound 16, budget
2000 ms). Binary SHA-256 `3ABBF220150DB0C1477FCF209412495E3443445100F3D3DF2E2C72DFD7227144` for both arms; the shared tree
also carried another slice's in-flight `xbox_present`/`frame_pacer` files, unchanged between the two arms.

The stage-3 race of [decomp_first_ui.md](decomp_first_ui.md) section 17 made the pair take retries: two hold-off boots
(`boot-nb-holdoff`, `boot-nb-holdoff2`) stopped at 58.8 s on title-side `0xC0000005` reads (`game+0x43adfb0` read
`0x82ea`, then `game+0x28cd3cc` read `0x574d20fc10`, both in the worker/wake family and with 0 device losses), and the
third hold-off boot (`boot-nb-holdoff3`) plus the single hold-on boot (`boot-nb-holdon`) are the pair reported here.
The pre-change baseline in section 10.1 (`boot-nb-baseline`) is a fifth boot of the same environment on the pre-change
binary; it stopped at 56.7 s on the same race after one device loss.

| measurement | A: hold off (previous path) | B: hold on |
|---|---|---|
| verdict / exceptions / device losses | ALIVE 300.2 s (harness timeout), 0, 0 | ALIVE 300.2 s (harness timeout), 0, 0 |
| wall / D3D12 calls / PresentX calls | 300.2 s / 1,102,874 / 13,271 (44.2/s) | 300.2 s / 833,986 / 10,110 (33.7/s) |
| submit-side pipeline waits (queue report) | **10,949 calls, total 696.6 ms, max single 524.1 ms**, all on one title thread (`tid 0xf8837ec980bc83b4`) | **1 call, 550.3 ms**, on the hold flusher: the budget-expired forced fallback. **0 on any title thread** |
| of those, built by the waiting thread | **1, 524.1 ms** - a driver build run by the submitting title thread itself | 1, 550.3 ms, run by the `[hold flusher]` layer thread after the 2000 ms budget |
| waits by thread (queue report lines) | one line, the title thread above; no other thread waited | none on a title thread; the flusher line is the forced wait above |
| pipeline submit hold counters | switch off: no hold, no flusher (startup line says off) | bound 16, budget 2000 ms; **holds 2, releases 2, forced 1, timeouts 1**, bound refusals 0, fallback waits 0, dropped 0; peak 2, longest hold **2597.7 ms**; forced at t=29.45 s with n=2 lists |
| compute driver calls / total / longest single | 360 / 484,986.2 ms / 106,368.4 ms (all on the 4 compile workers except the 1 title-thread build above) | 371 / 508,946.5 ms / 109,122.0 ms (all on the 4 compile workers except the 1 flusher build above) |
| graphics driver calls / time / thread | 4 / 7.8 ms (max 2.2 ms) / one title thread at first bind | 4 / 7.6 ms (max 2.3 ms) / one title thread at first bind |
| submissions / captures | 24,576 submissions at 259.5 s; 12 presenter captures (frames 1-2201) | 16,384 at 222.0 s; 12 presenter captures (frames 1-2201) |
| captured pixels | 2564x1421, every capture mean rgb `0.00 0.00 0.00`; centre readbacks show the layer control colour `51 102 204` / `0 0 0`, no title pixel | same: every capture mean rgb `0.00 0.00 0.00`; centre readbacks the same layer colours |
| state words: init/loading/session/machine at 298 s | 3 / 2 / 3 / current 3 next 3 (last changes 66 s, 62 s, 66 s) | 3 / 2 / 3 / current 3 next 3 (last changes 66 s, 62 s, 66 s) |
| wait trace `game+0x43C421E` at 60/120/240 s; five pool events | 5 waits / 0 signalled in all three dumps; the five events (`0x1764`, `0x1770`, `0x1778`, `0x17c0`, ...) have **0 set calls**, `Event not-set` | same: 5 waits / 0 signalled in all three dumps; **0 set calls** on the five events |
| class-0xa upload | `BLOCK ALLOC hit 1 size 0x1000 width 256 height 1`, job-block 0, overlap no; stride mitigation redirected at 3 call sites | same: `BLOCK ALLOC hit 1 size 0x1000 width 256 height 1`, job-block 0, overlap no; 3 call sites |
| movie player at 298 s | state `0x9`, buffers 0, open 1, non-null handle (decode probe not enabled in this environment) | same: state `0x9`, buffers 0, open 1, non-null handle (decode probe not enabled) |
| session-3 loading record completion | not probed in this pair (`PALMETTO_SLOT_WATCH` unset); the state words above are the measured stage evidence | not probed in this pair; same state words |

**Verdict: the wait is removed from the title's threads and the worker-pool falsifier fires.** Arm A's title thread spent
**696.6 ms inside the submit-side wait in 300 s**, one of those calls a 524.1 ms driver build it claimed and ran itself;
the other 10,948 calls are the old path's per-named-pipeline bookkeeping (they find the task finished or finishing on a
worker: 172.5 ms total, whose largest is under half a millisecond). Arm B spent **no time in that wait on any title
thread**: the two submissions that named a pipeline still under construction were held (longest 2597.7 ms), replayed by
the hold flusher once built, and the one that outlived its 2000 ms budget had the old wait forced on that layer thread,
where the 550.3 ms build shows in the report as `[hold flusher]`. The restart of the earlier stall did not happen: no
title thread built or waited for a pipeline in B. **But the five pool-2 events are still never set** in either arm (5
waits, 0 signalled at every dump) and every state word is identical at 298 s, so the peer plan's falsifier fires:
removing the compile wait from title threads does **not** wake the five-thread pool and does not complete the session-3
loading record. The wait was one title-thread symptom, not the worker-stall cause this boot needs; the next plan step is
the out-record state (sections 5.3 and 6's ranks 4-5), not more pipeline work. The presented frame stays black (all
captures mean 0.00) and the movie player stays at state `0x9` with `buffers 0` in both arms; the class-0xa mitigation
behaves identically in both, so the pair differs only in the pipeline hold.

### 10.5 Limits and the next falsifier

- **What the wait counter means.** `submit waits` in the queue report counts one call per named deferred pipeline the
  old path looked up, even when the task is already finished; the honest axis is the blocked time and the per-thread
  split the section adds, which is why arm A's 10,949 calls total 696.6 ms while exactly one of them (524.1 ms) is a
  driver build on the waiting thread. Arm B's single reported wait is the budget-forced fallback, and its line says
  `[hold flusher]`, not a title thread.
- **A hold can be moved, not removed.** With the switch on, a submission whose pipeline is still being built returns
  instead of waiting, but the work behind it - and every later submit, Signal and Present - is held until the pipeline is
  built or the budget forces the old wait. The measured arm held 2 submissions (longest 2597.7 ms) and forced 1 after
  the 2000 ms budget; a boot whose submission names a pipeline stuck in a 100 s driver build would be held for the
  budget and then complete the old wait on the flusher, so the title's own fence or present wait can still be late by
  one budget. The budget bounds that window; it does not make the compile faster.
- **The compile queue's own bound is unchanged.** A creation past `PALMETTO_PIPELINE_COMPILE_BOUND` still falls back to a
  synchronous driver build at creation, on the creating thread; that is not this slice's fallback and was 0 in both arms.
- **The flusher is a layer thread, not a title one.** A forced or fallback wait can therefore still run in the layer -
  on the submitting thread only when the hold budget is exceeded (the declared fallback), on the hold flusher when a hold
  expires - and never on a title *worker* thread. The boot's `[hold flusher]` line in the pipeline-call report says where
  a forced build ran.
- **Graphics are untouched.** A graphics pipeline's first bind still builds on the recording thread (4 calls / 7.6-7.8 ms
  in the pair); a workload with expensive graphics first-bind builds would still park a title thread, and is a separate
  port.
- **Held work at shutdown is dropped.** A queue destroyed while it holds a pipeline batch drops that batch (counted
  `dropped`, reported on stderr), exactly as any other held work is dropped at queue teardown; no work is submitted for
  it. A device loss drops it the same way.
- **Present cadence is measured, not attributed.** Arm B recorded fewer `PresentX` calls and D3D12 calls than arm A over
  the same wall time (10,110 against 13,271, and 833,986 against 1,102,874), while holding only 2 batches for at most
  2.6 s; one pair on a shared host cannot say whether that is the deferred Present path, host load or the title's own
  frame loop, and the slice does not claim it either way. A present-cadence A/B is the measurement that would settle it.
- **The worker-pool falsifier fired.** The five pool-2 events were never set in either arm and the state words are
  identical at 298 s (section 10.4): the peer plan's stated falsifier - "workers still parked and the dword still 0, so
  the wait is not the wall" - is what this pair measures. The next step is the out-record state store (sections 5.3 and
  6 ranks 4-5), and the session-3 loading-record probe (`PALMETTO_SLOT_WATCH`) is the diagnostic this pair did not carry.
- **One machine, one driver, one arm per boot.** The pair is one RTX 4090 / Vulkan 1.3 boot each, the shared tree was
  rebuilt by other slices before the pair, and two hold-off attempts stopped on the known stage-3 race before the
  reported arm (section 10.4). Stage timings on this host are not stable
  ([decomp_first_ui.md](decomp_first_ui.md) section 17), so a state that does not move in one pair is not a negative for
  the title as a whole.

### 10.6 Cross-check against the peers (reference only, no code copied)

The mechanism is the one section 6 ranks first, and the peer files that pointed at it are: **Five Star Recomp**
`45071d52` (`src/FiveStar.Loader/Shims/D3D12X/` pipeline creation, its `debugging.md` on the loading-screen frame, its
wiki `Status.md`; the same title build as ours) and **XCL** `8b957be` (`crates/xcl/src/hle/d3d12x/pso_async.rs`: the
stand-in returned at once, the real pipeline built on a worker, compute waited for only at its first bind) - both read in
sections 1-9. What we took from them: the shape (never block the title's thread on the layer's compiler; a submission
that needs a pipeline waits where the layer can afford it, or not at all). What is ours and differs: an in-process
compile queue and worker pool (no stand-in pipeline object, no separate compile process), a **submission hold with two
budgets and a forced fallback** on an existing held-batch machinery, the hold flusher as our own layer thread, unchanged
graphics, and our `PALMETTO_*` switches, counters and tests. No peer code, comment, identifier or table was copied.

### 10.7 Attribution and uncommitted files

This section was implemented from this project's own measurements: the pre-change boots in section 10.1, the
`CompileQueue`/`held_` primitives it composes, and the peer audit of sections 1-9 read as reference only under
[upstream_port.md](upstream_port.md). The behaviour was cross-checked against the public GPL-3.0-or-later project
Five Star Recomp (<https://github.com/ThatRecompDev/five-star-recomp>, commit `45071d52`) and the public GPL-2.0-only
project XCL (<https://github.com/xcl-dev/xcl>, commit `8b957be`), files as cited in section 10.6 and section 9; no code
from either was copied, and their trees remain the copyright of their contributors under their own licences. The
`NOTICE.md` row for this slice is the lead's to add at commit time (the two projects, commits and licences above).
Uncommitted files of this slice: `port/src/deferred_compile.h`, `port/src/vulkan_backend.h`,
`port/shims/d3d12_vulkan_device.cpp`, `port/test/deferred_compile_test.cpp`, `port/test/vk_queue_test.cpp` and this
section of `docs/peer_port_streaming.md`. No git write was made by this slice; boots and captures are under
`D:\leonida-work\fsrstable\` outside the repository, and no game data is added: RVAs, thread-id hashes, counts, times,
state words and image means only.

## 11. Measured: implemented ports 4-5 - the cross-list memory wait and the GPU-ordered bottom-of-pipe write

**Status: measured; opt-in and off by default.** This section implements ranks 4 and 5 of the section 6 plan: the console
64-bit memory wait (`Wait64BitValueX`, console list slot 74) as a cross-list hold with a list split and a watcher, and
the bottom-of-pipe writes (slots 71/72) as GPU-ordered immediate writes at the recorded position instead of record-time
CPU writes. Both extend the model of [bottom_of_pipe.md](bottom_of_pipe.md), which writes at record time and evaluates
slot 74 at record time; with the two new switches unset that model's measured path is unchanged. No peer code was copied:
sections 1-9 are the audit this port was planned from, [upstream_port.md](upstream_port.md) is the policy and section
11.6 is the attribution.

### 11.1 The measured shape this port answers

One 130 s measurement boot (`boot-memwait-measure2`, content environment and class-0xa mitigation, `PALMETTO_XS_CONSOLE_LIST_EXTRAS=1`
and the trace switches, binary `F52AE7B6...`): **ALIVE 130.2 s, 0 exceptions, 314,804 D3D12 calls**, every capture mean
rgb `0.00 0.00 0.00`, the class-0xa upload `BLOCK ALLOC hit 1 size 0x1000 width 256 height 1, job-block 0, overlap no`.

| measurement | value |
|---|---|
| slot 70 calls / forwarded | 9,375 / 9,375 |
| slot 71 calls / writes / refused | 7,775 / 7,775 / 0 |
| slot 72 calls / writes / timestamps / refused | 12,492 / 12,492 / 12,492 / 0 |
| slot 74 calls / met / unmet / unreadable / refused | 3,126 / **3,126** / **0** / 0 / 0 |

The result is the negative the section 6 plan predicted: **every measured wait is met at record time**, so no wait needs
the cross-list consumer at this content population, and the hold stays unexercised rather than proven unnecessary. The
waited cell is the slot-71 ring's base cell (`base + 0x00`), which neither slot-71 write (`+0x08`/`+0x0C` of a 72-byte
record) nor slot 72 writes: the writer is the title's own GPU work ([bottom_of_pipe.md](bottom_of_pipe.md) 2.5), whose
value is already there from the previous frame by the time the CPU records the wait. Whether a cell written by a *later*
list is ever waited on is answered by the new counters in section 11.2: the measurement arm reports
`record waits met by a write 0 (cross-list 0 same-list 0)` - no unmet record-time wait met a later write either.

### 11.2 The design

Our own mechanism, in our structure and naming: a pure policy header, the shim's list/queue code, and the layer's own
Wait-before-Signal hold as the release path. Nothing in the replay or the queue code changed.

| switch | what it does |
|---|---|
| `PALMETTO_XS_MEMORY_WAIT_HOLD=1` | split a list at an unmet wait and hold the segment after it (off: the record-time evaluation, unchanged) |
| `PALMETTO_XS_MEMORY_WAIT_BOUND_MS` | the bound, default **5000** (the peers' bound), clamped 1..600000 |
| `PALMETTO_XS_MEMORY_WAIT_MAX` | concurrent held segments, default **16**, clamped 1..256 |
| `PALMETTO_XS_BOTTOM_OF_PIPE_GPU=1` | write slots 71/72 as GPU-ordered immediate writes at the recorded position (off: the record-time CPU write, unchanged) |
| `PALMETTO_XS_MEMORY_WAIT_TRACE=1` | measurement only: the first 64 refusals/hold events and a counters line on the model's 16,384-event milestone; no call changes |

- **The pure policy** is the new include-only header under `port/src`: the switch spellings and clamps, the measured comparison
  (GREATER_EQUAL, D3D12 value 7, whole 64 bits; every other comparison refused by name), the hold records (cell,
  deadline, submission order, states pending/satisfied/timed-out/cancelled), their bounded release and the segment plan a
  batch of split lists becomes. It has no Vulkan, Windows, clock or thread dependency; the shim passes its clock.
- **The split** (shim, slot 74, unmet wait, hold switch on): close the part recorded so far, create the next segment on
  the shim's own command allocator (so the title's allocator stays free for it), replay the state the list had set into
  it, and go on recording there. `closed_parts`/`between_waits` on the list proxy keep the part order; a Close closes the
  last part, a Reset or a re-record without a submission drops the unused parts (counted as a named drop).
- **The state replay** is the part that makes a split faithful: a desktop list after the wait does not inherit the head's
  state. The shim keeps the measured by-value setters (pipeline state, root signature, root descriptor tables, 32-bit
  constants with their values copied, root CBV/SRV/UAV views, viewports, scissors, blend factor, stencil reference,
  topology, vertex/index buffers, depth bounds) and replays them into the new segment in order. A call whose state cannot
  be re-set safely - render targets (CPU descriptor handles), descriptor heaps, stream-out targets, a bundle's own state,
  predication on, a query left open - marks the list unsplittable, and a bounded state log (48 calls) does the same when
  it overflows: the wait then keeps the record-time evaluation, counted as `split refused` and named on stderr.
- **The submission** (shim, queue submit): the batch becomes a plan of parts and waits (`BatchPlan`). The shim
  pre-creates one fence per boundary, registers one hold each, then submits the parts up to the first wait as one
  `ExecuteCommandLists`, posts the queue's own PC `Wait` on that fence, and submits the segment after it - which the
  layer's existing Wait-before-Signal hold (`queue_holds.h`) holds, in order, with everything the title submits after it.
- **The watcher is the release**: one layer-owned shim thread polls each queue's oldest hold's cell in submission order
  (a later boundary never overtakes an earlier one), and releases a hold when the comparison holds or the bound passes by
  **CPU-signalling its fence** (`ID3D12Fence::Signal(1)`), which `VulkanFence::signal` records in `QueueHolds` and pumps -
  the same release the peers' watcher thread gives, through machinery this tree already owned. A hold is cancelled (not
  signalled) when its queue is destroyed or the process tears down; a queue destroyed with a held batch reports the drop.
- **Bounded fallbacks, all counted and named**: a bundle (no submission of its own), a list without an allocator, a list
  whose state cannot be replayed, an open query or predication at the wait, the state-log bound, the split bound (4
  segments a list), a fence or a hold that cannot be created, or a plan past 64 parts: the batch is submitted as one
  unsplit call with every part in order (the measured record-time model), never left unreleased.
- **The GPU-ordered write** (shim, slots 71/72): the measured value is recorded as a desktop `WriteBufferImmediate` in
  `MARKER_OUT` mode at the title VA, at the position the title's call was recorded - so the card writes it after the
  list's earlier work, and a wait can see a value the CPU could not have written. Slot 72's 64 bits go as two dwords
  (back to back; the measured call site only ever asks for a timestamp, whose value remains the CPU 100 MHz clock). The
  write resolves through the replay's title-page import path: a command VA no mapping resolves is imported over the
  title's own committed pages at first use. The measurement found that import's rule: a 4-byte write at a non-page
  offset cannot be bound, so the form is only used at a **page-aligned** destination; the measured slot-71/72 cells are
  not page-aligned, so the form is refused by name there and the record-time CPU write is the bounded fallback (see
  11.5).

### 11.3 Tests

`port/test/xs_proxy_mock_test.cpp` gained two checks (GPU-free, the existing target; a new target would need a CMake
change this slice does not own):

- **the pure policy** (`check_memory_wait_policy`): every off and on spelling of the two switches, the bound and
  hold-count clamps and defaults; the measured comparison and the refused comparisons; the hold records met by a value,
  met by a later list's write (cross-list counted apart from same-list), the exact bound boundary, submission order (a
  later hold never overtakes an earlier one), the concurrent bound and a queue's cancellation; the segment plan of a list
  split twice plus an unsplit list (segments, part order, wait positions, segment membership) and its part bound.
- **the proxy mechanism** (`check_memory_wait_split`): with the hold off an unmet wait is only counted (no split, no
  second list, one `ExecuteCommandLists`, no `Wait`); with it on the head closes, a real list is created with the state
  replayed (`OMSetStencilRef` arrives in the new segment), later recording goes there, a second unmet wait splits again,
  and the submit becomes three `ExecuteCommandLists` and two queue `Wait`s with two holds pending; setting the cell makes
  the watcher CPU-signal each fence with 1 in order; the bound releases a hold whose cell never holds and counts it; the
  GPU write records one `WriteBufferImmediate` at a page-aligned VA (value and `MARKER_OUT` checked) and refuses a
  non-page-aligned one with the record-time CPU write as the fallback. The target prints
  `xs_proxy_mock_test: PASS` with every earlier check unchanged.

Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints
`TESTS_BUILT`; the GPU-free gate subset (`unimplemented_policy`, `buffer_va_index`, `deferred_compile`,
`xs_proxy_mock`, `graphics_record`, `graphics_pso`, `frame_dump`, `xbox_present`, `dstorage`, `xapu_chad`) exits 0.

### 11.4 A/B boots

Two 130 s light boots, one binary, one at a time under `_setup/boot_lock.ps1`, the content environment from
`D:\leonida-work\fsrstable\boot_arm.ps1` plus the class-0xa mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1` and the model switch
`PALMETTO_XS_CONSOLE_LIST_EXTRAS=1`; arm A leaves the two new switches unset, arm B sets
`PALMETTO_XS_MEMORY_WAIT_HOLD=1`, `PALMETTO_XS_BOTTOM_OF_PIPE_GPU=1` and the bound 5000 ms. The pair is binary
`7E59D9DB...`, the one whose on arm recorded the GPU-form writes; it is the pair that measured the refusal of section
11.5. After it, the GPU form was restricted to page-aligned destinations (the fix the pair showed was needed, pinned by
the proxy test). The stage-3 race of [decomp_first_ui.md](decomp_first_ui.md) section 17 costs retries: the final binary
was re-verified by one clean switch-off boot (binary `4D97F918...`, ALIVE 130.2 s, 0 exceptions, 333,007 calls, same state
words and capture means) while its switch-on retries stopped before 130 s on the same title-side race (`game+0x86f9df` at
60.5 s, `game+0x559468f` at 55.2 s, `game+0x86f9fdf` family, 0 device losses), which is why the pair below is the earlier
binary's: no regression was measured, and the fallback that came out of it is the one the tests pin.

| measurement | A: switches off | B: switches on |
|---|---|---|
| binary SHA-256 | `7E59D9DB...` | `7E59D9DB...` |
| verdict / exceptions / device losses | ALIVE 130.2 s (harness timeout), 0, 0 | ALIVE 130.3 s (harness timeout), 0, 0 |
| D3D12 calls | 369,797 | 322,144 |
| slot 71 calls / writes / refused | 7,474 / 7,474 / 0 | 7,912 / 7,912 / 0 |
| slot 72 calls / writes / timestamps / refused | 12,642 / 12,642 / 12,642 / 0 | 12,423 / 12,423 / 12,423 / 0 |
| slot 74 calls / met / unmet | 3,164 / 3,164 / **0** | 3,109 / 3,109 / **0** |
| memory-wait counters line | switches 0/0; split 0 refused 0; holds 0; record waits met 0; gpu writes 0 | switches 1/1, hold 1, gpu-writes 1; slot74 unmet 0; split 0 refused 0; holds created 0; record waits met 0; **gpu writes 14,162 refused 9,217, addresses 4** |
| refused submissions (`ExecuteCommandLists failed`) | 0 | **3,545** (the GPU form's 4-byte title-page imports; see 11.5) |
| state words at 128 s (init / loading / session / machine) | 3 / 2 / 3 / current 3 next 3 (init 64 s, loading 60 s, session 64 s) | 3 / 2 / 3 / current 3 next 3 (init 68 s, loading 64 s, session 68 s) |
| movie player at 128 s | state `0x9`, buffers 0, open 1, probe 0 | same |
| class-0xa upload | `BLOCK ALLOC hit 1 size 0x1000 width 256 height 1`, job-block 0, overlap no | same |
| captures / pixels | 13 captures, every mean rgb `0.00 0.00 0.00` | 13 captures, every mean rgb `0.00 0.00 0.00` |

**Verdict: the hold is implemented and tested, but it is unexercised at this content population and both switches stay
off by default; the GPU-write form is measured as refused by the replay's import rule, and the final code falls back to
the record-time write.** The model's wait is met in 3,164 of 3,164 arm-A and 3,109 of 3,109 arm-B calls, so no split and
no hold was ever created in either arm: the counters that would move if the port worked (split, holds, segments) are 0
and the measurement axis that would prove a cross-list dependency (a record-time wait met by a later write) is 0 too.
That is the plan's stated falsifier, measured. The GPU-write form did record writes in arm B (14,162, of which 9,217 fell
back to the CPU write at their shape), and what it recorded at non-page offsets made the replay's 4-byte title-page
import refuse **3,545 whole submissions** ("unknown resource; nothing was submitted") - the arm is ALIVE and the state
words are arm A's, but a dropped submission is a regression no switch-on arm may keep. The final form therefore only
records a page-aligned destination: at the measured cells it is refused by name and the record-time CPU write lands the
same values as arm A, and the proxy test pins both halves (a page-aligned write recorded; a non-page-aligned one refused
with the CPU fallback). Both arms' state words, movie state, class-0xa upload and capture means are identical: no
movement, and the regression the first on arm showed is closed by the fallback rather than by the switch.

### 11.5 Limits and the next falsifier

- **The wait is unobservable at this population.** Every measured slot-74 cell already holds when the CPU records the
  wait, so the hold's consumer never runs. **Next falsifier:** a boot or content window in which a slot-74 cell is short
  of its target at record time (a cell a later list's GPU work writes): the `split`/`holds created` counters and the
  hold's release would go non-zero there. If they stay zero again, the port stays readiness, not value.
- **The split carries the list state only within bounds.** The measured frame lists set far more than the 48-call state
  log and touch render targets/descriptor heaps, so in the on arm almost every list is marked unsplittable; a wait in
  such a list keeps the record-time evaluation. The query-record list the measured slot-74 call site is in is short
  enough; a wait in a frame list is not. **Next falsifier:** a wait in a list whose state is not replayable would show
  `split refused` with the state name; lifting it needs the layer, not the shim, to own the segment state.
- **The GPU-ordered write cannot reach the measured cells.** The replay imports a 4-byte command VA as its own small
  span, and the title cells sit at non-page offsets, so the form is refused by shape in the final code and the
  record-time CPU write is used. The measured arm B recorded the form before that rule and shows why: 14,162 immediate
  writes, 9,217 refused at the destination shape, and 3,545 submissions refused by the import of the rest. The value
  therefore still lands at record time, not at GPU order, and the port's GPU half is readiness measured as refused, not
  a carried ordering. **Next falsifier:** a replay import that covers a whole page for a 4-byte write (or a registered
  marker buffer the write can name) would let the form carry; until then the record-time model is the measured one.
- **A 64-bit write is two dwords.** The desktop form has no 8-byte immediate write; the measured call site only asks for
  a timestamp, and a reader of both halves could see them one at a time where the console writes 8 bytes.
- **The timestamp value is still the CPU's clock.** Flags 1/2 ask the console for the GPU clock; the write's ordering is
  GPU-ordered now but its value stays the CPU 100 MHz conversion (the same limit as [bottom_of_pipe.md](bottom_of_pipe.md)
  limit 3).
- **The release path is the layer's Wait-before-Signal hold, in order.** A hold whose fence is never signalled (device
  loss, queue teardown) is cancelled and the held batch is dropped and reported by the layer, as any held work is; the
  bound is what prevents a missing writer from holding a queue. One machine, one driver, one arm per boot; the stage-3
  race costs retries (11.4), so a state that does not move in one pair is not a negative for the title as a whole.

### 11.6 Cross-check against the peers, attribution and uncommitted files

The behaviour was cross-checked against the public projects the audit names; no code, comment text, identifier or table
was copied, and this implementation is this repository's own structure, naming and tests. **Five Star Recomp**
(`45071d52`: `src/FiveStar.Loader/Shims/D3D12X/MemoryWaits.cs`, `ListSplits.cs`, `ListReplayState.cs`,
`BottomOfPipeWrites.cs`): the same shape - `Wait64BitValueX` with a 5 s bound, a list split at the wait with the desktop
state re-set on the new segment, a watcher thread that signals the queue's fence, and `WriteBufferImmediate` in
`MARKER_OUT` mode at a translated address with the CPU clock for timestamps - reached independently from our own
measurement; what is ours and differs is the release (the layer's existing `QueueHolds` CPU fence signal instead of a
per-queue fence class), the segment plan and pure policy header with their tests, the bounded state-log refusal, the
shim-created segment allocator, the per-submission planning in the proxy, and our switch/counter shape. **XCL**
(`8b957be`: `crates/xcl/src/hle/d3d12x/list_x.rs`, `cpu_writes.rs`) carries the same split and a 2 s bound and sets the
fence from the CPU; our bound is the 5 s one and our release is the same CPU signal through the layer. **Victory-fork**
(`52ac4ec`: `crates/victory-gpu/src/queue2.rs`) carries the copy-lane fences and holds the audit names; our port uses
the queue's own Wait hold rather than copy lanes. The upstream projects remain the copyright of their contributors under
their own licences; the `NOTICE.md` row for this slice is the lead's to add at commit time: Five Star Recomp
(GPL-3.0-or-later, commit `45071d52`), XCL (GPL-2.0-only, `8b957be`), victory-fork (GPL-3.0-or-later, `52ac4ec`).
Uncommitted files of this slice: the new include-only header under `port/src`, `port/shims/d3d12_xs_device.cpp`,
`port/shims/d3d12_xs_device.h`, `port/test/xs_proxy_mock_test.cpp` and this section of `docs/peer_port_streaming.md`. No
git write was made by this slice; boots and captures are under `D:\leonida-work\fsrstable\` outside the repository, and
no game data is added: RVAs, counts, addresses-as-shapes, state words and image means only.
