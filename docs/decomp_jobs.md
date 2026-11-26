# The job system, the thread topology and the timing contract our shims must honour

**Status: static decompilation.** This page reads the retail image's own job/threading code out of the reused Ghidra project
(read-only), maps the structures, state words and wait/signal pairs, and states the ordering the title assumes. Every row
says whether it is **measured** (read from the image at the named RVA, or taken from a named prior boot on this tree) or
**inferred** (a reading of the code's shape that no runtime probe has yet confirmed). No boot was run for this slice, no
code or repository page outside this one changed, and no git writes were made. RVAs, field offsets, counts and API names
only; no game data, strings or hashes.

The three questions the page answers: which object a title wait is on and what signals it; which thread owns which object
and therefore which ordering the title's own job system needs; and where an answer of ours (an `ExecuteCommandLists`
return, a fence value, a deferred-compile wait) can move that ordering, with a switch or probe that bounds each timing
hypothesis for the stage-3 lifetime/overwrite family.

## 1. Evidence and method

| | |
|---|---|
| Image | the retail title image this campaign decompiles (kept outside the repository), analysed from the existing read-only project `D:\leonida-work\classa\ghidra-proj.rep` |
| Tooling | this project's `DecompileRanges`/`DisassembleRange` scripts under `D:\leonida-work\rea-tooling\ghidra_scripts`, run through `classa\decomp.ps1` (whole-function decompiles into `classa\decomp-jobs1..3` outside the repository), plus a small RVA-only helper (`D:\leonida-work\decomp\jobs_scan.py`: PE `.pdata` function map, vtable qword dump, direct `E8`/`E9` caller scan, RIP-relative xref scan with capstone verification, disassembly) |
| Work notes | `D:\leonida-work\decomp\jobs-threads.md` (outside the repository) |
| Prior evidence | [vertex_buffer_va.md](vertex_buffer_va.md) sections 13-19 (the renderer executor chain, the 0x1000-byte table copy, the zero-byte block, the class 0xa chain), [deferred_compile.md](deferred_compile.md) section 9 (the arena frontier), [wait_forensics.md](wait_forensics.md) (the 35 title threads, every wait object, what is never signalled), [3d_textures.md](3d_textures.md) sections 10.6-11 (the measured hang-watchdog abort and the three thread chains that explain it), [task_queue_parity.md](task_queue_parity.md) and [wait_state.md](wait_state.md) (the runtime task-queue surface is idle; the stalls are title memory) |

The vocabulary is fixed here: a **creator** is the per-recording-context object that creates and submits jobs
(vtable `game+0x658DB60`); a **job** is the 0xa0-byte command unit (vtable `game+0x658DA80`); the **recording context**
is the object a creator holds at `creator+0x78` and every job points back to at `job+0x28`; the **frame tick** is
`game+0x120790`; the **hang watchdog** is the thread body `game+0x2215A60`.

## 2. The job object and its two vtables

The same 0xa0-byte job object and the same vtable `game+0x658DA80` are used by three create paths, so the layout and the
wait contract below are one core, not a renderer-only structure.

| create path | call | constructor | job flavour | measured |
|---|---|---|---|---|
| creator slot 0x10 | `game+0x184EB0` (reached from the job create/find path `game+0x184770`, itself reached from the opcode dispatch at `game+0x1837A2`; also called directly by the loading batch `game+0x5B13290` three times with class keys 9, 2, 2) | `game+0x185570` | renderer command job; its item array and item count come from the creator's `+0x78` object | vtable slots and both callers read; ctor decompiled |
| direct wrapper | `game+0x1767B70` | `game+0x1767C00` | single inline item, `job+0x48 = 4` (the executor's timestamp/query branch) | decompiled |
| direct wrapper | `game+0x40370F0` | `game+0x4037810` | single inline item plus a query object, `job+0x48 = 4` | decompiled |

All three allocate the job with the title's own allocator (`_DAT_14abcc310` vtable `game+0x663BDE0`, slot 0x70, request
0xa0), install the job vtable in the constructor, and pass the object to the one submit/wait `game+0x1856D0`. The
renderer creator object itself is installed by the renderer init that also seeds the format cache
(`game+0x1DC6C33` installs vtable `game+0x658DB60`; `game+0x1B373F0`/`game+0x1B37440` seed the class table).

The job's fields, as read from the constructors, the executor and the destructor:

| offset | width | meaning | measured at |
|---:|---:|---|---|
| 0x00 | 8 | vtable `game+0x658DA80` | all three constructors |
| 0x10 | 16 | intrusive-list links (`+0x10` forward, `+0x18` back) used while queued | `game+0x1856D0` link/unlink, `game+0x13A7D50` unlink |
| 0x20 | 8 | refcounted member released through its own vtable slot 0x10 | `game+0x13A7D50` |
| 0x28 | 8 | recording context (the object at `creator+0x78`); the gate and condition variable live there | `game+0x185570`, `game+0x1856D0`, `game+0x45842F0` |
| 0x30 | 8 | command object the job executes; its vtable slot 0x40 is the 0x1000-byte table copy | `game+0x1335A20`, [vertex_buffer_va.md](vertex_buffer_va.md) section 14 |
| 0x38 | 8 | two 32-bit dimensions copied from the graph node | `game+0x185570` |
| 0x40 | 4 | **state word**: 0 created/pending, 1 aborted, 2 executed, 3 pre-set (the executor must not overwrite 3) | ctor stores 0; `game+0x1863B2` stores 1; `game+0x18642E` stores 2; `game+0x1861A2`/`game+0x1863A8` compare 3 |
| 0x44 | 1 | creation-parameter byte (the loading batch's probes read it as 1) | `game+0x185570` |
| 0x45 | 1 | item count (byte) | `game+0x185570`; `game+0x186170` loop bound |
| 0x46 | 2 | flags; bit 2 selects the executor's abort path | `game+0x186170` |
| 0x47 | 1 | flags; bit 2 makes the executor allocate an extra per-item object (`game+0xCC2840`) | `game+0x186170` |
| 0x48 | 1 | flags; bit 2 selects the executor's timestamp/query branch | `game+0x186170`, both single-item ctors store 4 |
| 0x50 | 8 | work function called by the generic pool runner (0 for the renderer command job) | `game+0x63CD0` |
| 0x58 | 8 | format object; its slot 8 returns the format class the executor uses | `game+0x6FECC0`, `game+0x6FECE0` |
| 0x60 | 8 | item-array pointer; single-item flavours point it at the inline item at `+0x68` | constructors |
| 0x68 | 0x30 | inline item 0 (an item is 0x30 bytes; the executor writes its `+0x28` descriptor entry) | `game+0x186170`, `game+0x455A5E0` |
| 0x74 | 0x0C | two words initialised to `0x0fffffff` (`+0x74`) and `0x80000000` (`+0x78`); the destructor tears down the member at `+0x74` through `game+0x4FEFC30` | `game+0x185570`, `game+0x13A7D50` |

The vtables, slot by slot, as read from the two `.rdata` arrays (`game+0x658DA80` for jobs, `game+0x658DB60` for
creators). Only slots the code named in this page actually calls are described; the rest are listed as measured RVAs.

| vtable | slot | entry | what it is (inferred where marked) |
|---|---:|---|---|
| job `0x658DA80` | 0x00 | `game+0x13A7D20` | deleting destructor: calls `game+0x13A7D50` then returns the block to allocator slot 0x60 |
| job | 0x08 | `game+0x186170` | **the executor** (no direct call site: reached only through this slot) |
| job | 0x10 | `game+0x1858E0` | "has items" predicate: `return job+0x60 != 0` |
| job | 0x28 | `game+0x6FECC0` | class getter: `*(job+0x58)` then its slot 8 (inferred: the format class) |
| job | 0x40 | `game+0x184220` | takes the lock at `recording_context+0x18`, clears `job+0x30`, releases it (inferred: detach the command object) |
| job | 0x48/0x50/0x58 | `game+0x4584360` / `game+0x4584410` / `game+0x45844A0` | small frame-slot helpers (the executor's per-item fill path) |
| job | 0x60 | `game+0x13A7E30` | per-item cleanup used by the destructor when the state was 2 |
| job | 0x90 | `game+0x1335A20` | **the per-command executor** (runs the command object's slot 0x40) |
| job | 0x98/0xA0/0xA8/0xB0 | `game+0x40372A0` / `game+0x4584B90` / `game+0x13362A0` / `game+0x40372D0` | measured entries, not called on the paths below |
| creator `0x658DB60` | 0x10 | `game+0x184EB0` | create a job and submit/wait it (the three-call family above) |
| creator | 0x20 | `game+0x56C0500` | measured entry |
| creator | 0x48 | `game+0x1858F0` | **inline predicate**: true when `creator+0x48 == 0` or `GetCurrentThreadId() == creator+0x48` |
| creator | 0x50 | `game+0x4332F0` | **flush**: takes the gate, calls creator slots 0x98/0xA0, drains the creator's list at `+0x80` (`game+0x4334D0`), releases the gate |
| creator | 0x60 | `game+0x4332C0` | takes the gate, calls slot 0x50 then the object the creator holds at `+0x58` (inferred: flush, then submit) |
| creator | 0x98/0xA0 | `game+0x433340` / `game+0x433590` | begin/end hooks the flush and the inline submit call around execution |
| creator | 0xA8 | `game+0x185010` | format-object lookup by class key (the record-8 lookup of [vertex_buffer_va.md](vertex_buffer_va.md) section 16) |
| creator | 0xC0/0xC8 | `game+0x1336320` / `game+0x1336250` | measured entries |

## 3. The exact submit/wait/signal contract

`game+0x1856D0(creator, job, flags)` is the only submit/wait; every create path ends in it. Its steps, decompiled:

1. `job` slot 0x10 (`game+0x1858E0`) must return true; otherwise the job is released through `game+0x503E5C0` and null is
   returned - a job with no items never runs.
2. The recording-context **gate** is acquired. `creator+0x78` points at the recording context; the gate object is at
   `*(recording_context+0x58)` and is a waitable event plus an owner thread id and a waiter count:
   acquire `game+0x4333D0` -> `game+0x4333F0` (`WaitForSingleObject(handle, INFINITE)`, store the current thread id at
   `+0x8`, `lock inc` the count at `+0x10`); release `game+0x4333E0` -> `game+0x433450` (only the owner decrements, at
   zero it clears the owner, sets the event and wakes the context's condition variable through `game+0x45D91F0` /
   `game+0x45D8F80`; the event identity inside those two helpers is inferred from the call pattern, the wait/release
   pair is measured).
3. The **inline decision** is creator slot 0x48 = `game+0x1858F0`: true when `creator+0x48` is zero or equals
   `GetCurrentThreadId()`. So `creator+0x48` is the owning thread id of the recording context (inferred), and a
   submission from any other thread takes the queued path.
4. **Inline path** (the measured renderer path): call creator slot 0x98, slot 0xA0, then `job` slot 0x08 = the executor
   `game+0x186170`. If the executor leaves `job+0x40 == 1` the job is released and null returned; if it leaves 2 the gate
   is released and the job is returned to the caller. The frame chain of [vertex_buffer_va.md](vertex_buffer_va.md)
   section 14.2 is exactly this path: flush `game+0x184EB0` -> submit/wait `game+0x1856D0` -> executor `game+0x186170` ->
   per-command executor `game+0x1335A20` -> command slot 0x40 copy, all on the submitting (frame) thread, so the renderer
   command job is executed synchronously by the thread that built it, not handed to a pool.
5. **Queued path** (inferred as the console cross-thread route): link the job's `+0x10`/`+0x18` links into the creator's
   pending list (sentinel `creator+0xB0`/`+0xC0`), release the gate, call slot 8 of the object whose pointer the creator
   stores in its field at `+0x50` (the field, not the vtable slot) with `creator+0x30` (the enqueue/wake - the object's
   identity is not decompiled, so its role is inferred), then
   re-acquire the gate and sleep on the recording context's **condition variable** `game+0x45D8F70` ->
   `game+0x45D92D0` while `job+0x40 == 0`. The CV object is `*(recording_context+0x60)` (event at `+0`, waiter count at
   `+0x8`); `game+0x45D92D0` increments the waiter count, releases the gate, waits on the CV event, and re-acquires the
   gate per waiter. The woken condition is the same `job+0x40 != 0` predicate.
6. When the wait returns, `job+0x40 == 1` releases the job and returns null; otherwise the job is returned to the caller.

The executor `game+0x186170` itself, decompiled: if `job+0x48 & 4` it takes a timestamp/query branch
(`QueryPerformanceCounter`-family helpers and the globals `0x14AB42E90`/`0x14AB42E94`); otherwise it reads the class
through job slot 0x28, pre-allocates per-item objects when `job+0x45` is non-zero (calls `game+0x501F4A0`,
`game+0x5392460`, and `game+0xCC2840` when `job+0x47 & 4`), and when `job+0x46 & 2` is clear runs the per-command
executor `game+0x1335A20` and ends with `job+0x40 = 2`; when the flag is set it calls job slot 0x60 with 1 and ends with
`job+0x40 = 1`. A completion notification `game+0x45842F0` is called just before the `= 2` store and does nothing unless
`job+0x40` already reads 2 (a guard read from the code; a runtime probe has not been placed on it).

The graph side that produces the jobs: `game+0x1836D0` dispatches a command opcode, `game+0x183A20`/`game+0x184250`
walk a command graph's repeated lists, `game+0x183B70` is the job cache that returns an existing job for an identical
descriptor, and the flush's list walk `game+0x4334D0` runs each entry: an untagged entry is executed through its vtable
slot 8 (the job executor), and a tagged entry (low bit set) is resolved through slot 0xA8 (`game+0x185010`) and then run
through its slot 0x90 (`game+0x1335A20`).

| # | wait/ordering event | word or object | wait side | signal side | measured |
|---:|---|---|---|---|---|
| 1 | a submitted job is finished | `job+0x40` state dword | queued submitter's CV loop in `game+0x1856D0` | the executor stores 2 at `game+0x18642E` | both decompiled |
| 2 | the recording context is entered | gate at `*(creator+0x78)+0x58`: event + owner id + count | `game+0x4333D0`/`0x4333F0` | `game+0x4333E0`/`0x433450` | call pair measured; helper identity inferred from `WaitForSingleObject(handle,-1)`, `GetCurrentThreadId`, `SetEvent` |
| 3 | the recording context is idle | CV at `*(creator+0x78)+0x60` | `game+0x45D8F70`/`0x45D92D0` | the gate release path (`0x45D91F0`, `0x45D8F80`) | call pair measured; CV internals inferred |
| 4 | inline or queued | `creator+0x48` owning thread id | `game+0x1858F0` | set when the recording context is created (inferred; the setter site was not read) | predicate decompiled |
| 5 | a queued job is woken | object at `creator+0x50`, slot 8, argument `creator+0x30` | - | `game+0x1856D0`'s queued branch | call measured; object identity inferred |
| 6 | the queued work runs | creator list at `+0x80`: array + count at `+0x8` | - | flush `game+0x4332F0` walks `game+0x4334D0` on the owner thread | measured |
| 7 | a job is retired | job slot 0 | - | deleting destructor `game+0x13A7D20` -> `game+0x13A7D50` unlinks, releases items and the recording-context reference, frees 0xa0 | measured |
| 8 | the frame is flushed | creator slot 0x50 = `game+0x4332F0` | - | called by the frame path; gate held for the whole drain | measured |

The job system's own wait objects are an event gate and a condition variable; the semaphores in this picture belong to
the loader's separate semaphore-based condition variables (`game+0x62D0` notify tail-jumps into the `ReleaseSemaphore`
thunk `game+0x7330`; the wait is `game+0x56A0D`) and to the audio shim, both measured parked in
[wait_forensics.md](wait_forensics.md) sections 2-3 (semaphores `0xd40`/`0xd48`/`0xd4c` last released at 27.5-27.7 s). No
renderer job waits on a semaphore.

Two consequences the shims must honour. First, the renderer command job is **synchronous on the submitting thread**, so
anything inside it (including an `ExecuteCommandLists` the title calls while building) is on the frame thread; a shim that
blocks there blocks the title's frame work, not a pool worker. Second, the job is **returned to the caller**, and the
caller destroys it through slot 0 after it has used or queued it - so the job block's lifetime is caller-owned and the
title's allocator may hand the same 0xa0 to the next pass.

## 4. The two task pools

The renderer job core is not the only asynchronous machinery. Two title-owned thread pools exist, both event-driven and
both outside the runtime's task-queue surface ([task_queue_parity.md](task_queue_parity.md) measured that surface silent
after +25.5 s).

| | pool 1 (the general task pool) | pool 2 (the work-item pool) |
|---|---|---|
| creation | `game+0x1AD0D60` allocates the thread-descriptor array (0x40 stride) and the lane rings (0x6008 stride), then creates each thread through `game+0x1AD0FD0` (`CreateThread`, entry `game+0x2215EA0`, body `game+0x221C1F0`, 0x57800 stack) | `game+0x43C38A0` creates **5** threads (the literal 5 is stored at `0x149C3D660`) through the same `game+0x1AD0FD0`, body `game+0x43C4140` |
| thread descriptor | `+0x00` handle, `+0x08` pool, `+0x10` pending count, `+0x18` per-thread event, `+0x20` index | per-thread entry at `0x149C3D670 + index*0x40`: event at `+0x8`, second handle at `+0x10` |
| wait site | `game+0x221C3C2` on `descriptor+0x18` through the image's INFINITE wait thunk `game+0x1784D0` | `game+0x43C421E` on the entry's event |
| work intake | lane rings: pop `game+0x4D9D9F0` over 0x6008-byte rings, push/wake `game+0x4DA41A0`, `game+0x4DA4200`, `game+0x4DA4240` | a 128-entry work ring at `0x149C3BA30` with the counters at `0x149C3BE30`; the item executor is `game+0x43C4370` |
| item record | 0x30 bytes: function at `+0`, refcount u16 at `+8`, dependency count at `+0xA`, source lane at `+0xB`, target ring at `+0xC`, cost at `+0xD`, dependency pointers from `+0x10` | work items are the pool's own records (not decoded here) |
| wake walker | `game+0x4DA4240` walks the pool object at `0x1471BFD00 + lane*0x40` (`array +0`, lane rings `+8`, count `+0x14`, lanes `+0x18`, outstanding `+0x1C`, fallback pool `+0x28`): sets a zero pending word to 1 and calls `SetEvent` on the descriptor's event; `game+0x4DA4200` repeats on the fallback pool | the per-thread event is set by the fill side of the same pool (the setter site was not traced) |
| item-to-job | `game+0x4EAA040`, the item's executor, calls the generic runner `game+0x63CD0(item-0x68, item+0xC, item+0xB)`: the queue-job layout has its work function at `+0x50`, an execution state dword at `+0xF0` (4 while the work function runs, 2 queued/entered, 3 done) and a completion notifier `game+0x560B0` | - |
| measured thread count | 10 per-thread wake events plus the per-frame event `0x828` in the wait tracer ([wait_forensics.md](wait_forensics.md) section 2) | 5 waiters on 5 events that are never set ([wait_forensics.md](wait_forensics.md) section 2) |

The pools explain the parked-thread picture the wait tracer measured: after ~27.7 s the ten pool-1 wake events are last
set at 27.6-27.7 s, the five pool-2 events are never set, and the workers sit in the two waits above. Neither pool is
the runtime's task queue; both are title memory.

## 5. The frame tick, the heartbeat and the hang watchdog

This is the timing contract the title enforces against itself, and the one an emulation delay can most easily break.

| item | measured value | evidence |
|---|---|---|
| frame tick body | `game+0x120790`: calls `game+0x22159F0` at `+0x9` (a `QueryPerformanceCounter` stored as a 64-bit value), computes the frame delta into `+0x1C`, updates renderer frame words, then calls `game+0xC9E330` which pets the heartbeat again at `game+0xC9E368` | decompiled |
| frame tick identity | installed as slot `+0x38` of the tick-dispatch qword tables at `game+0x6195020` and `game+0x65F43A0`; prior measured call chain `game+0x11FD60` -> `game+0x120790` in the loading task's thread | data scan; [3d_textures.md](3d_textures.md) section 11.3 |
| heartbeat word | `game+0x71B6338`, 8-byte `QueryPerformanceCounter` value, written by `game+0x22159F0` (call sites `game+0x120799` and `game+0xC9E368`) and by the watchdog itself | xref scan over `.text`: 9 reference sites, all inside those functions |
| heartbeat cadence | advances once per frame, measured ~24 times a second | [3d_textures.md](3d_textures.md) section 11.2 |
| watchdog thread | start `game+0x2215A20` -> generic thread wrapper `game+0x2215EA0` -> body `game+0x2215A60`; one `Sleep(1000)` per pass (`game+0x92ECA0`), `QueryPerformanceCounter` before and after | decompiled; [3d_textures.md](3d_textures.md) section 10.6 |
| hang budget | `0x1E` = 30 passes without the heartbeat advancing, then abort: crash report `game+0x144E4BA80`, callback at `0x71B6348`, deliberate null write and `int3` (abort site `game+0x2215C16`/`game+0x2215C35`) | decompiled; prior measured abort RVA and signature |
| pause hook | `game+0x22159C0(state)`: state `0x24` sets byte `0x71B6330` (the watchdog pets the heartbeat itself and does not count down); state `0x25` clears it and sets `0x71B6331` (one grace pass) | decompiled |
| shutdown | byte `0x71B6340` ends the loop; `0x71B6348` is the abort callback pointer | decompiled |
| worker progress table | 64 entries: per-entry live count at `0x146844A24 + index*0x48`, 8-byte timestamps from `0x146844A28 + index*0x48`; the watchdog aborts when a live entry's timestamp is older than its threshold and its condition test passes; entries registered at `game+0xB5B6B0`/`game+0xB5B6F0` | decompiled + xref scan |

So the title's own console timing assumption is: **one heartbeat per frame, a 30 s hang budget at 1 Hz passes, and an
explicit pause state for operations the title itself knows are long.** A shim whose synchronous work keeps the
frame/tick path from running for 30 s does not slow the title down; it makes the title abort. That is not hypothetical:
[3d_textures.md](3d_textures.md) sections 10.6-11 measured exactly this abort when our deferred-compile submit wait
blocked the loading task thread whose chain pets the heartbeat, and it names the fix (build the named pipeline on the
submitting thread) that moved the boot past stage 3 entry. The port's `port/src/deferred_compile.h` submit wait and
`wait_or_build` exist for this reason, and its comment names this watchdog.

## 6. Thread topology and object ownership

Measured thread population is the wait tracer's: 35 title-image threads of 91 at 60 s and 120 s
([wait_forensics.md](wait_forensics.md) section 2); the roles this slice can name are:

| thread | what it runs | objects it owns / waits on | evidence |
|---|---|---|---|
| frame thread | the title frame path: `game+0x41F380` -> `game+0x503CDE0` -> `game+0x180F00` -> `game+0x503D4C0` -> command build/dispatch/walk -> flush -> submit/wait -> executor -> per-command executor | transient jobs and command objects (returned and destroyed by it); the outermost frame, not a wait | [vertex_buffer_va.md](vertex_buffer_va.md) section 14.2; [3d_textures.md](3d_textures.md) section 11.3 |
| loading task thread | the loading update `game+0x4FD1AD0`; its chain reaches the registered frame tick `game+0x120790`, which pets the heartbeat | a pool-1 work item; the loading completion wait | [3d_textures.md](3d_textures.md) section 11.3 |
| 10 pool-1 workers | items popped from the lane rings, then jobs through `game+0x63CD0` | their own descriptor and per-thread event | [wait_forensics.md](wait_forensics.md) section 2; this page section 4 |
| 5 pool-2 workers | work items from the 128-entry ring | their own entry and per-thread event | same |
| watchdog thread | `game+0x2215A60` | none of the title's job objects; it only reads the heartbeat and the progress slots | this page section 5 |
| title entry thread | blocked since 27.7 s acquiring a title lock whose release event is never set (`game+0x258DB70`, wait at `game+0x258DBA6`) | that lock | [wait_forensics.md](wait_forensics.md) sections 3-5 |
| audio thread | waits on the audio shim's semaphore | host-side | [wait_forensics.md](wait_forensics.md) section 2 |

Ownership rules read from the code: the creator and its recording context are created at renderer init
(`game+0x1DC6C33`) and are shared by every job submitted through that context; the creator records its owning thread at
`creator+0x48` and only that thread executes a job inline (section 3 step 3); a job holds a reference on the recording
context and on the command object, and after submit/wait the caller owns the job until it calls slot 0; the title's
allocator (vtable `game+0x663BDE0`) is title-private, so the same 0xa0 block can be reused for a later job. Our shim
threads never run title code (the compile workers, the fence dispatcher and the present path only call the driver and
host APIs), so our answers can move *when* a title thread runs its own code, not *which* thread runs it.

## 7. Where our answers can move the title's ordering

Each row is a shim answer that the title's own code does not have to wait for today, the console assumption it replaces,
and the effect on the job/thread ordering above. File references are repository paths.

| answer | where | what it does now | console assumption it replaces | what it can move |
|---|---|---|---|---|
| `ID3D12CommandQueue::ExecuteCommandLists` | `port/shims/d3d12_com.cpp` (the method), `port/src/vulkan_backend.h` | validates the lists, replays them on the calling thread, submits asynchronously and returns | console submission is asynchronous; the call returns before the GPU work finishes | it returns while the frame thread continues the same job pass; a long replay is time the title spends inside our call *before* the rest of its frame |
| a held batch | `port/src/queue_holds.h`, `port/src/vulkan_backend.h` | a batch whose queue waits on a fence value with no submitted signal is **held on the CPU** and replayed later by the thread that submits the signal | a console queue would accept the wait in order | the replay of that batch moves to another thread and the lists stay pinned; ordering inside the title's own frame does not change, but the CPU cost lands on the signalling thread |
| queue backpressure | `port/src/vulkan_backend.h` (`kMaxQueuedCommandBuffers = 64`, `wait_for_room`) | at 64 live command buffers the submitting thread waits for the oldest submitted one | the console does not make the CPU wait for its own frames this way | the frame thread can stall inside our submission, which is exactly the path the watchdog budgets |
| submission pacing knobs | `port/src/vulkan_backend.h` (`PALMETTO_PACE_IDLE`, `PALMETTO_PACE_DELAY_US`, `PALMETTO_PACE_CONTENT`) | opt-in: drain the queue after each accepted batch, sleep after a submission, or drain at a content boundary | console submission is asynchronous; these are emulation-only experiments | they deliberately serialise the producer thread against the GPU; measured in [vertex_buffer_va.md](vertex_buffer_va.md) sections 12-13 to move the stage-3 stop within the family |
| fence visibility | `port/shims/d3d12_com.cpp` (`Fence::GetCompletedValue`, `Fence::Signal`, `Fence::SetEventOnCompletion`, `CommandQueue::Wait`) | `GetCompletedValue` reads the GPU timeline; a CPU `Signal` above a signal a queue still owes is **held** and applied by the dispatcher after the GPU reaches that queue value; a queue `Wait` on an unsubmitted value holds the batch | D3D12 lets a CPU signal become visible immediately and a queue wait is a GPU-side wait | a title thread that reuses an object off a CPU fence signal can observe the value later than on console |
| deferred pipeline compile | `port/src/deferred_compile.h` (queue, workers, bound; submit `wait` and `wait_or_build`), `port/shims/d3d12_xs_device.cpp`/`port/src/graphics_pso.cpp` callers | the submission that first names a deferred pipeline waits for the build; `wait_or_build` claims the task and bounds the stall to one driver build | the console builds pipelines ahead of the submission | the submitting title thread is blocked inside our code between two title-side operations; measured stalls of 0.5-63 s on this tree, and the 63 s arm moved stage 3 from ~57 s to 84 s ([vertex_buffer_va.md](vertex_buffer_va.md) section 14.6, [3d_textures.md](3d_textures.md) section 11) |
| deferred resource destruction | `port/src/retire_tickets.h` | frees only behind the contiguous completed ticket prefix | the console frees when the fence says so | a title allocation can stay live longer than the console would keep it, which changes which block the title's allocator can hand out next |

The two rules that matter for the stage-3 family: (a) the title's frame work and the 0x1000-byte copy are on the frame
thread, and our `ExecuteCommandLists`/compile waits are the only places that thread enters our code - so a shim stall
changes *when* the next frame pass allocates, not the order inside a pass; (b) the pool threads run title jobs
concurrently with the frame thread, and our per-submission blocks (a PSO build, a held batch, a 64-deep backpressure
wait) change which pool work has finished when the frame pass allocates its blocks.

## 8. Ranked timing hypotheses for the stage-3 lifetime/overwrite family

The family's producer is measured and title-side: the constant-table helper asks the title allocator for **0 bytes**
(class 0xa, size computed from a row-stride table with no case for 0xa), then the command object's slot 0x40 copies
**0x1000 bytes** into that block; the live 0xa0 job sits 0xE30-0xEF0 bytes after the block in two catches and 0xA0
before it in a third, so the copy overwrites the live job or the allocator's own nodes
([vertex_buffer_va.md](vertex_buffer_va.md) sections 14-16). No layer frame is between the executor entry and the store,
and no layer answer participates in the class, the size or the copy length. The hypotheses below are therefore about what
selects the **victim** of the overrun and whether a shim's timing can select a different one - not about whether the
overrun exists.

| rank | hypothesis | shim file | bounded falsifier | prior evidence for/against |
|---:|---|---|---|---|
| **H1** | Our deferred-compile stall shifts the frame phase at which the flush allocates the 0xa0 job relative to the title's other allocations, so the zero-byte block lands on a different side of / distance from the live job (the two measured layouts `job-0xEF0` and `job+0xA0` are both possible). | `port/src/deferred_compile.h` (submit wait, `wait_or_build`, bound/workers), the PSO submission callers | one A/B pair with `PALMETTO_PIPELINE_DEFER_COMPILE=1` and the existing `PALMETTO_ALLOC_WATCH` probe on the flush's job allocation (`game+0x184F0C`) and the helper's block allocation (`game+0x455964C`), reading the `(block - job)` delta and the stop site; the eager (`PALMETTO_PIPELINE_DEFER_COMPILE` unset) arm is the control. If both arms show only the measured delta set and the same stop family, H1 is refuted. | prewarm/8-worker A/B kept the stop at `game+0x13360A3` and only collapsed the submit waits ([vertex_buffer_va.md](vertex_buffer_va.md) section 13.5), so the cheap version of this knob is already negative; the 63 s build still moved stage 3 in time (section 14.6) |
| **H2** | The window between the flush allocation and the copy is where another thread's 0xa0 block would have to be allocated to be the victim; a shim block inside the submitting thread's pass (a held batch released elsewhere, a bindless upload wait, a compile wait) widens or moves that window. | `port/shims/d3d12_com.cpp` (`ExecuteCommandLists`), `port/src/queue_holds.h`, `port/src/vulkan_backend.h` (pacing, room wait) | one arm `PALMETTO_PACE_IDLE=1` (drain after each accepted batch) and one `PALMETTO_PACE_DELAY_US=<n>`, each with the alloc-watch probe and the stop-site census; a moved stop within the family under a paced producer is the positive, an unchanged stop family is the negative | `PALMETTO_PACE_CONTENT=1` moved the stop to `game+0x1335DCB` without removing the family ([vertex_buffer_va.md](vertex_buffer_va.md) section 13.6), so pacing can move the victim but is not the cause |
| **H3** | A title thread that reuses or frees the command object off a fence value sees the value later in our layer when the CPU signal is held behind a queue signal; the job/command block then stays live when the copy lands, so the copy hits a live object instead of a freed one. | `port/shims/d3d12_com.cpp` (`Fence::Signal`, `GetCompletedValue`, `SetEventOnCompletion`, `CommandQueue::Wait`), `port/src/vulkan_backend.h` (`VulkanFence`, fence dispatcher) | one boot with the existing `PALMETTO_TIMELINE` submission/fence log plus the stage-3 writer watch; count the `cpu fence signal` lines that were held versus applied and the fence values the title reads around the copy. If no CPU signal is held at the pass, H3 is absent in that window. | not measured; the held-signal path is ours by construction (`d3d12_com.cpp` comment at `Fence::Signal`) |
| **H4** | A stage-3 job could take the queued path instead of the inline path if the submitting thread differed from the creator's recorded owner thread; a queued job stays live until the owner's flush drains it, which changes the victim window. | none of ours touches `creator+0x48`; relevant because a shim cannot change the calling thread | a probe on the two returns of `game+0x1858F0` plus the `creator+0x48` value at stage-3 entry; all-inline is the expected negative | the measured renderer chain is inline ([vertex_buffer_va.md](vertex_buffer_va.md) section 14.2); our threads never run title code |
| **H5** | Some stops counted in the stage-3 family are the title's hang watchdog (null write + `int3`, abort at `game+0x2215C16`) rather than the overwrite, which would mis-attribute a shim-induced stall. | any synchronous shim answer that can block the frame/tick thread (`ExecuteCommandLists` replay, backpressure, compile wait) | the stop signature: the watchdog abort is a deliberate fault after the 30-pass countdown and a write/execute at address 0, while every measured family stop is a read of `-1`, `0x18`, `0x30` or `0x3F800000`; one boot with a probe at `game+0x2215C16` and on the 30-count confirms or refutes a watchdog stop | measured watchdog aborts have their own signature and RVA ([3d_textures.md](3d_textures.md) sections 10.6-11); the family's stops in sections 13-16 are reads in title code |
| **H6** | The title heap/arena placement our `XMemVirtualAlloc` answers create decides where the zero-byte block can be and therefore the victim. | `port/shims/xmem.cpp`, `port/src/arena_size.h` | the arena base/alignment/front-space arms already run | measured negative: every placement arm stopped in the same family within 0.9-4.1 s ([vertex_buffer_va.md](vertex_buffer_va.md) section 19) |

Reading the ranks as a decision order: H1 and H2 are cheap and mostly runnable with switches that already exist; H3
needs one instrumented boot; H4 and H5 are cheap negatives that protect the attribution; H6 is already answered. If H1
and H2 both come back negative (same delta set, same stop family, only timing moved), the honest statement is the one
sections 15-16 reached from the allocation and class ends: the overrun's victim is decided by the title's own allocation
order at the pass, and no shim timing change can move it.

## 9. Limits

- **Static only.** No boot was run for this page; every runtime number is a citation of a prior boot on this tree, and
  every structure claim is a decompile of the retail image. The image is one build.
- **Inference is marked.** The gate and CV primitive identities are inferred from the instruction pattern
  (`WaitForSingleObject(handle, INFINITE)`, `GetCurrentThreadId`, `SetEvent`, a per-waiter release/re-acquire loop);
  the queued-path dispatcher object at `creator+0x50` and the `creator+0x48` setter were not decompiled. The
  renderer-init claim for `game+0x1DC6C33` reads the vtable install site, not the whole init.
- **Only the slots the page names were followed.** The remaining job/creator slots are listed by RVA but not interpreted.
- **The pool-1 thread count is not a literal in the code**; the 10-worker figure comes from the wait tracer's event count.
  The pool-2 count is a literal 5 in `game+0x43C38A0`.
- **One positive watchdog/compile interaction is cited, not re-measured.** The claim that our deferred-compile wait
  aborted the title is [3d_textures.md](3d_textures.md) sections 10.6-11's measured result; this page only reads the code
  that makes that contract.
- **No game data.** RVAs, field offsets, classes, counts and outcomes only; the page names no string, asset or hash.

## 10. Attribution

This page is the project's own static work: the job/creator/executor/submit/wait decompiles, the two vtable reads, the
`.pdata`-based caller and RIP-relative cross-reference scans, the data-section pointer scan for the frame-tick install
sites and the heartbeat's writers, and the gate/CV/disassembly readings were all produced from the user's own image with
the existing Ghidra project and scripts (read-only) plus a small local RVA helper. No peer code was used, no code was
edited, no boot was run, no git writes were made, and no game data is added. Prior measurements are cited to their pages
and their RVAs, not restated as this slice's own.
