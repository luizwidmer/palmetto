# The stage-2 blocker: the boot sequence's completion wait on the tracker event

**Status: measured, decompiled, and bounded; the wait is causal, the producer is not yet named.** The stage-2 entry
handler `FUN_14081B270` runs the boot sequence `FUN_1421B8960` and blocks inside it: the sequence creates a 0x638-byte
tracker object, arms its pending-operation dword, and waits **INFINITE** on the auto-reset event the tracker created.
The wait site is `FUN_14258DB70+0x36` (RVA `0x258DBA6`), reached from `FUN_1421B8BD0+0x14` (call at `0x21B8BDF`) inside
`FUN_1421B8960` at `0x21B8A70` - after the loading init and before the trailing stage-3 request at `0x81B2EF`. The
entry thread (the state-machine pump loop) is the thread that waits; the object is alive and the event unsignalled at
45 s and at 100 s in the same boot. No title code, no DirectStorage `EnqueueSetEvent` and no D3D12
`SetEventOnCompletion` signals that event in a measured boot, and a one-shot signalling experiment (the state-2 wake
probe below) shows the wait is exactly what holds the machine - but also that a bare signal is not the fix: the boot
continues and faults at `0x147B961` without ever requesting stage 3. The next fact needed is the tracker's pending work
and its producer.

## 1. What ran

| | |
|---|---|
| Tree | `D:\leonida` at working tree after `66b33d782` (the `dstorage` page-resolver commit) with other slices' uncommitted files present; `port/src/main.cpp` carries the two opt-in probes this page describes (state trace, stage-2 wake probe) |
| Boots | four 130 s boots under `_setup\boot_lock.ps1`, one at a time: `boot-probe2` (wait/thread-stack samplers + two cdb dumps), `boot-probe3` (DirectStorage trace + two cdb dumps), `boot-probe4` (one-signal wake experiment) and `boot-probe5` (control of the same binary, no wake). A fifth script, `boot-probe6` (timing-robust wake + in-process stack samples), is built but did not get the boot lock inside this page's window; the follow-up slice's four boots (`boot-probe7`/`9`/`10`) are in section 11.1 |
| Harness | `tooling/boot_report.py --timeout 130`, captures outside the repository in `D:\leonida-work\stage2b\` |
| Debugger | `cdb -p <pid> -c "~*k; dq <tracker global>; dq <object fields>; !handle 0 f Event; .detach; q"` at 45 s and 100 s (2-9 s per attach) |
| Samplers | `PALMETTO_WAIT_SAMPLE=30,60,90,120`, `PALMETTO_THREAD_STACKS=30,60,90,120`, `PALMETTO_STATE_TRACE=2,130`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_DSTORAGE_TRACE=1` (one boot), and the new opt-in `PALMETTO_STAGE2_WAKE` (wake boots only) |
| Standard environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=30` |

The measured binary mtime/SHA-256 per boot is in section 6. The default boot of the current tree is **ALIVE 130 s and
stays at stage 2** (section 6, control); the wake experiment is the only boot that changes that.

## 2. Measured shapes

### 2.1 The blocking call

`cdb ~*k` at 45 s and at 100 s in `boot-probe2` finds the same title thread at the same wait (thread id 51560 of that
boot; title RVAs are the raw addresses minus the image base `0x140000000`):

| frame | RVA | what it is |
|---|---|---|
| call site | `0x258DBA6` | `WaitForSingleObjectEx(handle, INFINITE)` inside `FUN_14258DB70` (function `0x258DB70-0x258DC75`, offset `+0x36`) |
| caller | `0x21B8BE4` | return into `FUN_1421B8BD0` (function `0x21B8BD0-0x21B8C7D`), whose first call is at `0x21B8BDF` |
| caller | `0x21B8A75` | return into the boot sequence `FUN_1421B8960` (function `0x21B8960-0x21B8B00`) after the call at `0x21B8A70` |
| caller | `0x81B2E8` | return into the stage-2 entry handler `FUN_14081B270` after the call at `0x81B2E3` |
| caller | `0x53EC05E` | transition executor `FUN_1453EBFE0` (runs the entered stage handler) |
| caller | `0x53EBF3A` | state-machine pump loop `FUN_1453EBEA0` |
| caller | `0x661F0` | `FUN_1400661E0`: runs the pump and returns its result |
| caller | `0x1D109C9` | `FUN_141D109B0`: the title's per-frame main loop body |
| caller | `0x5A91C0D` | `FUN_145A91BD0`: the title's static-initialisation entry |
| caller | `0x56A1E5A` | `FUN_1456A1D54`: the title entry point, started by our `rt::process::game_thread` |

So the thread that never returns to request stage 3 is the title entry thread, and it is parked inside the boot
sequence's post-loading block, exactly where [state_machine.md](state_machine.md) left the open question.

### 2.2 The waited object and the tracker

The boot sequence's post-loop group is, in order (call sites in `FUN_1421B8960`):

```text
0x21B8A58  call FUN_145BF0B10                 creates the tracker object
0x21B8A64  call FUN_1421B8B20(global)         tracker body (arms the pending dword)
0x21B8A70  call FUN_1421B8BD0(global)         waits on the tracker event, then drains the ring
0x21B8A75  call FUN_1421B8C80()               destroys the tracker object
           ... remaining init calls ...
0x81B2EF   (stage-2 handler) requests stage 3   <- never reached in every measured boot
```

`FUN_145BF0B10` (RVA `0x5BF0B10`) is 0x2e bytes: `operator new(0x638)`, constructor `FUN_141DECCB0`, store to the
image `.data` qword at **RVA `0x9844898`**, tail-call `FUN_141DECC30`. The decompiled constructor shows the object's
layout (RVAs only, field offsets):

| field | value at construction | after `FUN_1421B8B20` | measured at 45 s / 100 s |
|---|---|---|---|
| `+0x4A0` | address of `+0x588` (ring base pointer) | - | - |
| `+0x588` | `new[16 x 0xA8]` ring array | - | not yet dumped |
| `+0x590` | `0x100010` | - | - |
| `+0x598`, `+0x59A`, `+0x59C`, `+0x59E` | 0, 0, 0, `0x10` (ring indices and capacity) | - | - |
| `+0x5F0` | 0 (a callback pointer slot) | - | not in the 45 s dump window |
| `+0x600` | 0 | - | `0x140004380` (a stub) |
| `+0x608` | **0** (pending-operation dword) | **1** (incremented under a lock in `FUN_1421B8B20`) | **`0x80000001`** - the value `FUN_14258DB70` makes by adding `0x80000000` to 1 before its wait |
| `+0x610` | **`CreateEventA(NULL, FALSE, FALSE, NULL)`** (auto-reset event) | unchanged | **`0x1D34`** (event handle), `CloseHandle`d in the destructor |
| `+0x618` | 0 | - | `0x23EB93AD480` (the object handed to the post-wait emitters as `uVar5`) |
| `+0x620` | 0 | - | the tracker object itself (`0x23EB92C9700`), so the post-wait virtual calls are the tracker's own vtable slots `+0x20`/`+0x28` |
| `+0x630` | 0 | - | `0x23EB93B6750` (an allocation the destructor frees) |

The constructor's `CreateEventA` call is the image's own IAT slot (`0x6793AB0`), reached through the wrapper at RVA
`0x19ECD00` (`xor` the four argument registers with themselves, then `jmp [rip+...]`), called from `FUN_141DECCB0` at
`0x1DECE75`. The wait helper `FUN_14258DB70` is generic over the tracker's sub-object at `+0x5A0`:

```c
// RVA 0x258DB70, disassembly at 0x258DB85..0x258DBA6 (decompile in the boot's own terms)
if (*(int *)(p + 0x68) != 0) {                 // p = tracker + 0x5A0, so p+0x68 = tracker+0x608
  LOCK(); old = *pi; *pi = *pi + 0x80000000; UNLOCK();   // locked add; eax receives the old value
  if (old != 0) WaitForSingleObject(*(HANDLE *)(p + 0x70), INFINITE);  // p+0x70 = tracker+0x610
  *(int *)(p + 0x68) = 0;
}
// then: interface vtable calls, the callback at p+0x50, and FUN_14006AED0(...), i.e. the completion emitters
```

The condition simplifies: the block is entered only when the dword is non-zero, and the value the `lock xadd` leaves in
`eax` is the old value - non-zero by that same test - so **the wait is unconditional once the pending dword is armed**.
That is why the machine cannot leave stage 2 without the event.

### 2.3 The event, measured from the handle table

`!handle 0 f Event` in the 45 s and 100 s dumps says of the waited handle (`0x1D34` in `boot-probe2`):

| property | 45 s | 100 s |
|---|---|---|
| type / attributes / name | Event, 0, none | same |
| granted access | `0x1F0003` (Delete, ReadControl, WriteDac, WriteOwner, Synch, QueryState, ModifyState) | same |
| HandleCount | 2 | 2 |
| PointerCount | 65535 | 65531 |
| object specific | Auto Reset, **Event is Waiting** (unsignalled) | same |

The wait sampler names the same object in the same boot at every sample:
`tid 51560 ... wait NtWaitForSingleObject timeout INF handle 0x1d34 (Event) state Event not-set`,
`start host:palmetto.exe+0x2AD140` (`rt::process::game_thread`). The 30/60/90/120 s samples all show it.

The **HandleCount of 2** looked, to this first slice, like a second reference held by some in-process component: the
title does not import `DuplicateHandle` or any `NtDuplicateObject`/`NtSetEvent` path, and the 45 s and 100 s counts
are identical. **That reading is retired by the follow-up measurement (section 11.3):** one cdb session lists 1,084
Event handles in this process and `!handle` reports `HandleCount 2` for essentially all of them, and `!htrace`
attributes this event's creation to the title entry thread with no duplicate. The count is this process's shape for
events, not a tracker-specific holder; no duplicate producer exists.

## 3. The decompiled region

All functions below were decompiled with the local Ghidra workflow (`DecompileRanges.java` over a copy of the title
image project, exact `.pdata` ranges; project copied outside the repository under `D:\leonida-work\stage2b\ghidra-proj`).

- **`FUN_1421B8960`** (boot sequence) calls, after its boot-media wait loop: `FUN_145BF0B10` (create tracker),
  `FUN_1421B8B20(tracker)` (body; increments `tracker+0x608`), `FUN_1421B8BD0(tracker)` (wait and drain),
  `FUN_1421B8C80()` (destroy), then `FUN_142225870(0x147127458,2)`, `FUN_145C3DC10`, `FUN_141D24C80`,
  `FUN_14282F3C0`, `FUN_14282F810`, `FUN_1426B1E60`, the loading-state test `FUN_144F862D0` and, when the loading
  state is 0, a load start `FUN_141D67570(1)`. Then the stage-2 handler requests stage 3 at `0x81B2EF`.
- **`FUN_1421B8BD0`** (0xAD bytes in `.pdata`) is the block: its first instruction calls
  `FUN_14258DB70(param_1 + 0x5A0)`, then it runs `FUN_145098940`, a scope marker, and drains the 16-entry ring by
  calling each entry's vtable `+0x80` (the entry destructors), with atomic index advance. After the drain it tests a
  flag and calls `FUN_1454F5080` / `FUN_1454F50F0(0x1471BFEC0)` / `FUN_1410D6410`, i.e. a QPC-gated second path.
- **`FUN_14258DB70`** (0x105 bytes) is the generic completion wait described in section 2.2. Its post-wait emitters
  are the tracker's own vtable slots `+0x20` (`FUN_1421B9230`) and `+0x28` (`0x4DA2850`), the callback at
  `tracker+0x5F0`, and `FUN_14006AED0(tracker, tracker+0x618, 0, 7, 4)`; `FUN_1421B9230` reads a flags dword at
  `tracker+0x618+0xD8` (`0x200000`/`0x400` branches), so the object at `+0x618` is what the completion is about.
- **`FUN_1421B8C80`** is a wrapper that loads the global and calls the destructor `FUN_1421B9170`: free `+0x630`,
  `CloseHandle(tracker+0x610)`, the callback at `+0x600`, free the ring (`FUN_141F652B0`), `FUN_1421C22B0`, delete
  0x638 bytes. The tracker therefore lives only between the create call and this destroy call in the same function.
- **`FUN_145BF0B10`** (0x2E bytes) and **`FUN_141DECCB0`** (0x22B bytes) are the factory and constructor described in
  section 2.2; the only write of the tracker global in the whole image is
  `mov qword ptr [rip+...], rsi` at **`0x5BF0B2A`**.

Static scans that bound the producer question:

| scan | result |
|---|---|
| direct `E8/E9` callers of the in-image `SetEvent` wrapper `0x5930` | 103 sites; **none** loads `[reg+0x610]` into a register in a 400-byte window before the call (and none loads `[reg+0x608]`) |
| functions that touch field `0x610` **and** call the `SetEvent` wrapper | 2 unrelated functions (one writes a table slot, one uses a stack slot), neither in the loading/boot region |
| `PALMETTO_DSTORAGE_TRACE=1` boot | 8 bounded `enqueue request` lines, **zero `enqueue set-event`** lines; the shim's `EnqueueSetEvent` (the only runtime path that duplicates a title event) was never called |
| `[d3d12-trace]` in the same boot | 221,815 lines covering every D3D12 call; **zero `SetEventOnCompletion` / `SetEventOnMultipleFenceCompletion`** calls |
| image import list | one `GameInput.dll!#1` (`GameInputCreate`), `CreateEventA`/`CreateEventW`/`SetEvent`/`WaitForSingleObject`, no `DuplicateHandle`, no ntdll event/handle imports |

So the event is created by the title, never signalled by title code, and not signalled by the two runtime paths that
could take a title event handle in this boot. The HandleCount 2 was read here as a second holder; the follow-up
measurement retires that reading (section 11.3: the count is this process's shape for essentially every event, and
`!htrace` shows the title entry thread as the only creator).

## 4. The wake experiment: the wait is causal, a bare signal is not the fix

`PALMETTO_STAGE2_WAKE=<seconds>` is an opt-in probe in `port/src/main.cpp` (off by default; it starts no thread unless
set). It waits for the named second, polls the tracker global every 250 ms until `+0x608` is armed and `+0x610` is a
handle (so it is independent of the boot's 22-44 s stage-2 timing), logs the fields, calls `SetEvent` on that handle
once, then samples every thread's title-side stack at +0.15/+0.5/+1/+2/+4 s and logs the tracker fields and the init
word.

| | boot-probe4 (wake at 40 s) | boot-probe5 (control, same binary) |
|---|---|---|
| binary | built 15:52:41, SHA-256 `1238B630...84BA0` | same |
| tracker at wake | `0x26293F40850`; `+0x608 = 0x80000001`; `+0x610 = 0x1D94` | (no wake) |
| event before the signal | `WaitForSingleObject(handle, 0)` returns `0x102` (timeout) - unsignalled | - |
| SetEvent | ok | - |
| tracker after +2 s / +7 s | `+0x608 = 0` (the title's wait returned and cleared it), event unchanged | - |
| stage word / requested stage | 2 / 2 at every sample up to 51.7 s | 2 / 2 to 130 s |
| verdict | **STOP**: exit `0xC0000005` at 55.1 s; first exception logged at **41.027 s**, read of address 0 at **`0x147B961`** | **ALIVE 130.4 s**, 0 exceptions |
| boots' other counters | 79,375 D3D12 calls by the stop, stage 2 at 24.0 s | 211,418 calls, stage 2 at 44.0 s |

The first exception's stack (`boot.jsonl`) is a component teardown walk, not the boot sequence:
`0x147B961` / `0x147B947` / `0x147B8DF` / `0x27EFB03` / `0x1D822B3` / `0x27EFAC1` / `0x1D822B3` / `0x27EFAC1` /
`0x1D822B3` / `0x5B65D6B` / `0x1DE2C51` / `0x5BE9AB9` / `0x1B15BA0` / `0x5840241` / `0x583FF0C` / `0x59965BE`. The
outermost listed frame `0x59965B0` is a tail-jump target from **`0x21B8F00`**, inside the same 0x21B8xxx class cluster
as the boot sequence; the walker at `0x147B8B0` iterates a component list and calls a virtual `is-done`/destructor.
So waking the wait lets the title continue, the tracker's `+0x608` is cleared, and the title then faults while
consuming/destroying state that the real producer would have filled - the stage-3 request is still never written.

The wake experiment therefore proves two separate things:

1. **The wait is the block.** Nothing else in the stage-2 handler moves the machine while the event is unsignalled;
   clearing the wait lets the handler continue (the armed dword goes to 0 by the next sample).
2. **Signalling the event is not the fix.** The boot sequence's continuation needs the pending work's *results*, not
   only its completion; a bare `SetEvent` leads to a null-dereference fault before `0x81B2EF`.

The control boot (same binary, wake switch unset) is ALIVE for 130 s and stays at stage 2, so the fault is caused by
the wake, not by the tree state at build time. One caveat is recorded in section 7: the two wake-boot numbers come from
a binary that also carries other slices' uncommitted files, and the 41 s exception is a first-chance record whose
fatal outcome is the 55.1 s exit.

## 5. What this says about the missing producer

The title's own code cannot be the setter of `tracker+0x610` in a way the static scans see: the handle is created in
the tracker's constructor, read only by the wait helper and the destructor, and no function that calls the image's
`SetEvent` wrapper loads that field. The two measured runtime paths that can take a title event handle either were not
called (`EnqueueSetEvent`, `SetEventOnCompletion`) or are not in the boot (GameInput shim is not compiled; the host
GameInput runtime has its own dispatcher handles). The first slice read the event's `HandleCount 2` as a second holder;
the follow-up measurement retires that reading (section 11.3: the count is this process's shape for essentially every
event, no duplicate exists).

The tracker's shape says what the producer must deliver: it is created and destroyed inside one function, its body
`FUN_1421B8B20` arms **one** pending operation (`+0x608: 0 -> 1`) and hands `+0x618` an object whose `+0xD8` flags the
post-wait emitter `FUN_1421B9230` reads, and `FUN_1421B8BD0` drains the 16-entry ring **after** the wait. The producer
is therefore the asynchronous owner of that pending operation, and it must (a) fill/complete the object at `+0x618`
and (b) signal the event. Both effects were absent in every measured boot; the wake experiment supplied only (b).

Two producers fit the measured shape and were the named falsifiers for the next slice - the follow-up measurement
decided between them:

1. **A title worker/job thread** (a streaming/loader job batch): **confirmed, and named exactly** by section 11.5 - the
   tracker is the loader's registered job graph (70 jobs in its hash table at `+0x628/+0x630`), the jobs are queued but
   never started (`+0xF0 = 2` queued, `+0xE0 = 0` not started, unchanged 3 s later), and the single completion callback
   `image+0x21C3B60` decrements the pending dword and signals the event (section 11.4). The ring dump below was run by
   the follow-up: it is empty (no waiter record) before the wait (section 11.2). The first signal needed is the
   job-completion that drops `0x80000001` *after the jobs have filled the result object at `+0x618`*.
2. **An in-process host component** holding the second handle: **retired** by section 11.3 - no duplicate exists, and
   `!htrace` attributes the event's creation to the title entry thread. The DirectStorage and D3D12 fence paths measured
   as not called remain not called.

## 6. Boot numbers

| boot | binary (mtime, SHA-256) | verdict | stage word | tracker / event | notes |
|---|---|---|---|---|---|
| `boot-probe2` | built 15:16:20 (the diagnostic build this tree booted before the 15:45 relink) | ALIVE 130.4 s, 0 exceptions, 241,094 D3D12 calls | 2 at 22.0 s, requested 2 to the kill | global `0x23EB92C9700`; `+0x608 = 0x80000001`; `+0x610 = 0x1D34` at 45 s and 100 s | wait sampler + cdb dumps; the block's stack in section 2.1 |
| `boot-probe3` | 15:45:44, SHA-256 `54D7433599C9A8710F7A0CD8AF8E1D88109B48FBFB021E6A7AD6C943008DD73D` | ALIVE 130.5 s, 0 exceptions, 218,577 D3D12 calls | 2 at 30.0 s, requested 2 | same wait at 45 s | `PALMETTO_DSTORAGE_TRACE=1`: 8 `enqueue request`, **0 `enqueue set-event`**; same blocked stack |
| `boot-probe4` | 15:52:41, SHA-256 `1238B630CE7078B5B7C3D8FDC7A495C6EB7D2C021776C15B96A99D4D93884BA0` (with the wake probe) | STOP 55.1 s, exit `0xC0000005`; first exception 41.027 s at `0x147B961` | 2 / 2 up to 51.7 s | wake at 40.0 s; `+0x608 = 0x80000001 -> 0`; event 0x1D94 | section 4 |
| `boot-probe5` | same `1238B630...` | ALIVE 130.4 s, 0 exceptions, 211,418 D3D12 calls | 2 at 44.0 s, requested 2 | - | control of the wake binary: the fault is wake-caused |
| `boot-probe6` | 16:02:15, SHA-256 `9586D2F316A166D1B9DEC4928338B184DF143282817DD060C0903EC61AD5E6FF` | not run: the boot lock was held by other slices' long boots past this page's window | - | - | the timing-robust wake probe (polls until the tracker is armed, then samples every thread's title-side stack at +0.15/+0.5/+1/+2/+4 s) is built and left in the tree for the next slice |

The stage-2 transition time varies with the machine's boot-to-boot behaviour and the other slices' load (22.0 s,
24.0 s, 30.0 s and 44.0 s across these boots) - the wait and its object are identical in all of them.

## 7. Limits

- **One machine, one overlay, one title build.** The trees booted here are the working tree with other slices'
  uncommitted files present; the binary's SHA is given per boot. The boot timing is not stable (section 6), and the
  samplers and debugger attaches perturb it: the D3D12 counts range 211k-241k across these boots.
- **The tracker's ring was not dumped in this slice.** The 40 s cdb read of `boot-probe5` found the global still zero
  because that boot reached stage 2 at 44 s; the 16-entry ring at `+0x588` and the object at `+0x618` are therefore
  known here only by their construction and by the emitters' use, not by their contents. **The follow-up dumped both
  (section 11.2): the ring is empty at the wait and `+0x618` is the job-group context object.**
- **The wake experiment changes the boot.** It supplies only the signal; the 41 s fault is the title's reaction to
  incomplete state and is not evidence about the title's healthy path beyond what section 4 states. The fatal exit
  code is `0xC0000005`, but the crash reporter records only the first exception, so the 41 s first-chance and the
  55.1 s exit are not proven to be the same site.
- **The debugger attaches pause the process** (2-9 s), so counters sampled around them are lower than the steady
  state; the two dumps are a thread/handle snapshot, not part of the steady state.
- **`HandleCount 2` was read as an attribution in this slice and is not one.** Section 11.3 measured the same count for
  essentially every event in the process and found no duplicate, so the second-holder reading is retired; the remaining
  limits of the follow-up are in section 11.8.
- No game data: this page contains RVAs, field offsets, handle values, counters, timings and Win32/HRESULT codes only -
  no package path, executable bytes, extracted table or shader data.

## 8. The next falsifiable measurement

**The follow-up slice ran items 1 and 2 below and answered them (sections 11.2 and 11.3); the next measurement is now
11.6.** The list is kept as the first slice's falsifier record.

1. **Dump the tracker before the wait** (one 130 s boot, control, `cdb` at 60 s with stage 2 already reached):
   `dq <global>`, `dq <tracker>+0x400 L 60`, `dq poi(<tracker>+0x588) L 64`, `dq poi(<tracker>+0x618) L 64`. The ring
   entries' vtable pointers and the `+0x618` object's `+0xD8` flags name the subsystem whose completion the wait is.
   Prediction if the producer is the loader/job batch: the ring holds queued entries with a job/stream vtable whose
   completion path runs on a parked title thread.
2. **Name the handle's creator**: one boot with `!htrace -enable` before the tracker is created and `!htrace -diff`
   after the wait, or `!handle` plus the thread that created it. Prediction if the second handle is a host runtime
   duplicate: the create stack is in the shim/host module that received the handle; a title-side `CreateEventA` with
   `HandleCount 1` would refute the duplicate account.
3. **Only then** the fix: route the measured producer. The follow-up reads the producer as **title-side job dispatch**
   (section 11.5: the tracker is the loader's job graph, its 70 jobs are queued and never started), so our runtime
   cannot honestly complete it from its side; the result object the post-wait emitters consume must be filled by the
   jobs, and a bare `SetEvent` was measured to fault (section 4). If a later measurement finds a runtime call in that
   dispatch chain, the fix is in that shim (completion *and* result, not just the event).

`port/src/main.cpp` carries the opt-in diagnostics used across both slices - `PALMETTO_STATE_TRACE` (from the
state-machine slice), `PALMETTO_STAGE2_WAKE` (this slice, the one-signal experiment) and `PALMETTO_STAGE2_TRACKER`
(the follow-up slice's job-list sampler); the default path starts none of them.

## 9. Tests and gates

This is a measurement slice: this slice's runtime change is the opt-in `PALMETTO_STAGE2_WAKE` diagnostic in
`port/src/main.cpp` (unset, it starts no thread and the default path is unchanged), plus this page. The follow-up
slice adds the opt-in `PALMETTO_STAGE2_TRACKER` diagnostic in the same file (its gates are in section 11.7).

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| slice test | none added: the change is a diagnostic switch on the default path, and the wake experiment it enables cannot run without the game; the matching existing tests are exercised by the test gate below |
| docs checks (`tests/test_docs_*.py`) | pass except `test_docs_env_vars.py`, whose scan reads only **git-tracked** Markdown: `PALMETTO_STAGE2_WAKE` (this page, untracked here) and `PALMETTO_RASTER_CULL_MEASURED` (another untracked page) are reported until the lead commits the untracked docs. The page does name the variable. |
| test subset (`port/build`) | `frame_dump_test`, `unimplemented_policy_test` (98 checks), `xbox_present_test` and `xs_proxy_mock_test` pass; `d3d12_fence_timing_test` reproducibly exits `0xC0000005` after the console queue's `Wait` slot, in a tree whose `port/src/vulkan_backend.h`/`graphics_pso.cpp` carry another slice's uncommitted changes - this test does not link `main.cpp`, so the fault is in that in-flight state, not in this slice |

## 10. Attribution

This slice was implemented from this project's own measurements: the wait site, the tracker object, the decompiled
boot-sequence region, the runtime paths ruled out, the wake experiment and the control boot. No upstream code or
upstream behaviour claim is used on this page, so there is no reference file or commit to credit; the policy of
[upstream_port.md](upstream_port.md) is followed in the form of "measure first, implement our own, state the limit".
No git writes were made by this slice.

## 11. Probe results (follow-up): the tracker is the loader job graph; the completion callback and the measured negative

*This section is the follow-up slice's measurement. It keeps the page above intact and appends the facts the first
probes left open: the tracker's ring and object contents, the waited event's creator, the tracker's internal job
structure, and the exact function that releases the wait. A negative is the result: the producer is the title's own
job-graph dispatch, it is never started in our boot, and no runtime call is outstanding that we could answer.*

### 11.1 What ran

| | |
|---|---|
| Tree | `D:\leonida` working tree at the same state as section 1, with this slice's only source change: the opt-in `PALMETTO_STAGE2_TRACKER` probe in `port/src/main.cpp` (off by default; the default path starts no thread) |
| Binaries | probe7 cdb boot: built 16:14:48; probe9/probe10 boots: built 16:27:45 (the same tree plus the tracker probe; `PALMETTO_OK`) |
| Boots | four 130 s boots under the shared boot lock, one at a time: `boot-probe7` (single cdb session, `!htrace -enable` at ~12 s, execution breakpoint on the wait call site, tracker dump + `!htrace -diff` + `!handle 0 f Event` + all-thread stacks), `boot-probe9` (in-process tracker probe: job lists, job fields, handle census, second sample +3 s), `boot-probe10` (single cdb session at the same breakpoint: tracker header, both job lists, the registered-job hash table and several job objects) and `boot-probe9`'s control state trace |
| Harness | `tooling/boot_report.py --timeout 130`, captures outside the repository in `D:\leonida-work\stage2c\` |
| Measure points | execution breakpoint at RVA `0x21B8BDF` (the call into the wait helper `FUN_14258DB70`), i.e. after the factory, the batch body and the job dispatch and before the first wait; in-process probe polling from 4 s and sampling again 3 s later |

The boot's own state word moved exactly as before: `init 2` at 16.5 s / 18.0 s / 24.8 s across the three measured
boots, `loading 1` two seconds later, machine `current 2 / next 2` to the kill, `ALIVE 130.3 s`, 0 exceptions.

| boot | verdict | d3d12 calls | stage | tracker moment |
|---|---|---|---|---|
| `boot-probe7` (cdb) | ALIVE 130.3 s | 240,721 | 2 at 24.8 s | breakpoint hit at ~37 s; all dumps below taken there |
| `boot-probe9` (in-process) | ALIVE 130.3 s | 280,121 | 2 at 18.0 s | tracker armed and waited at 19.5 s; second sample at 22.5 s |
| `boot-probe10` (cdb) | ALIVE 130.3 s | 280,732 | 2 at 16.5 s | breakpoint hit ~30 s; job objects dumped there |

### 11.2 The tracker at the wait (cdb, probe7 and probe10)

The tracked object is the same class in both boots (vtable RVA `0x6186300`). At the breakpoint the layout is:

| field | value | meaning (proved by the decompiles in 11.4/11.5) |
|---|---|---|
| `+0x000` | vtable `image+0x6186300` | the batch/job-graph class |
| `+0x03C` / `+0x040` | dword, array pointer | count/array of job **definitions**: 249 at the wait (one entry per registered job chain) |
| `+0x048` / `+0x050` | **1** / event handle | second pending/event pair, armed by the body and consumed by the drain (`FUN_145098940`), not by the waited event |
| `+0x088` / `+0x090` | array pointer / `0x0073` + a flag word | dispatch list of 115 definition entries (the same first object as the `+0x040` list; the two lists are two views) |
| `+0x0B0` | `0xAC407EB2` (and `0xF8` beside it) | the graph identifier the factory passes to the base constructor; the body's `+0xB0 != 0` branch tests it |
| `+0x0F8` | pointer back to the tracker | self reference |
| `+0x588` | pointer to a 16-entry ring; indices/capacity `0/0/0/16` | ring of 16 waiter records of `0xA8` bytes; **all zero at the wait** - no waiter record is ever queued here before the wait |
| `+0x5A0` | the tracked sub-object (pending `+0x68`, event `+0x70`) | see 11.4 |
| `+0x5F0` / `+0x600` | `image+0x21B8F00` / `image+0x4380` (stub) | callback slots |
| `+0x608` / `+0x610` | **1** (pre-wait; `0x80000001` inside the wait) / event handle | the pending dword and event from section 2.2 |
| `+0x618` | context object pointer | the group tracker handed to the completion emitters (11.3) |
| `+0x620` | pointer back to the tracker | used as the emitter target |
| `+0x628` / `+0x62C` / `+0x630` | `0x80` / **`0x46` (70)** / array pointer | **registered-job hash table**: capacity 128, 70 entries of `{id dword, job pointer qword}` (decompile in 11.4) |

The context object at `+0x618` (cdb header dump): field 0 points at the tracker's `+0x5A0` group; `+0x50` is
`image+0x21C3B60`, `+0x58` is `image+0x1167B0` (a one-word copy thunk), `+0x68` is `image+0x4EAA040`, `+0x80` is the
tracker itself; flags at `+0xD8` were `0x00200000` (in-process boot) and `0xFF000100` (cdb boot), i.e. the emitter's
`0x200000`/`0x400` tests depend on which flags the context carries. The ring is empty because the only waiter (the
entry thread) has not yet inserted its record when the wait begins.

One transition is worth stating explicitly, since section 2.2 records only the constructed value: the callback slot
`+0x5F0` is **0 at construction** (section 2.2) and **`image+0x21B8F00` at the wait** in probe7/probe10, so it is
written between construction and the wait. That matters because `FUN_14258DB70` invokes exactly that slot after its
wait returns (`(**(code **)(p + 0x50))(...)` with `p = tracker+0x5A0`, i.e. `tracker+0x5F0`), and `image+0x21B8F00` is
the function the wake experiment's later teardown walk entered (`section 4`, the tail-jump target from `0x21B8F00`).
So the tracker carries its own post-wait continuation callback, set by the pre-wait step, and the wait's release is
followed by that continuation rather than by a shim call.

### 11.3 The waited event's creator, measured from one session

`boot-probe7` enabled `!htrace` at ~12 s and listed the event handle the tracker created. The log attributes the
event's creation to the **title entry thread** - thread `0x8b20` of that boot, whose `~*k` stack is the boot sequence
(`0x21B8BDF` <- `0x21B8A75` <- `0x81B2E8` <- the pump, the same chain as section 2.1). There is no second creation or
duplicate of that handle in the trace.

The same dump listed **1084 Event handles in the process** and `!handle 0 f Event` reports `HandleCount 2` for
essentially every one of them - auto-reset and manual-reset, set and waiting, including handles the title closed
later. So `HandleCount 2` is the shape this process has for **its event objects in general**, not a second holder
specific to the tracker's event. The earlier "a second holder exists" reading of section 2.3/3 is therefore not
supported as a tracker-specific fact; the context-emitter account in 11.4 explains the same object without a duplicate.
This also retires the two producer candidates of section 5.2 (a host component holding a duplicate): no duplicate was
measured.

### 11.4 The completion path: exactly one callback releases the wait

The sub-object at `tracker+0x5A0` is the job group the wait is about: `+0x68` pending dword (tracker+0x608), `+0x70`
event (tracker+0x610), `+0x78` the context object (tracker+0x618), `+0x80` the tracker (tracker+0x620). The context's
`+0x50` slot is the only function that releases it (targeted Ghidra decompile, RVA `0x21C3B60`):

```c
// image+0x21C3B60, the callback stored at ctx+0x50
int job_group_complete(void **owner) {
  group = *owner;                                  // tracker+0x5A0
  LOCK(); old = *(int *)(group + 0x68);            // tracker+0x608
          *(int *)(group + 0x68) = old - 1; UNLOCK();
  if (old == 0x80000001) signal_event(*(HANDLE *)(group + 0x70));   // tracker+0x610
  return 0;
}
```

The wait helper `FUN_14258DB70` turns the armed `1` into `0x80000001` with a locked add before waiting (section 2.2),
so this callback is the **single** step the wait needs: one invocation after the graph's work has been done. The wake
experiment's bare `SetEvent` provided the signal without that invocation's work, which is why the boot then faulted
while consuming a result object the jobs never filled (section 4).

The producer is therefore the title's **job-graph execution**: the batch's registered loader jobs must run to
completion; the last completion path calls this callback once per finished job chain, and the one that drops the
pending dword from `0x80000001` signals the event.

### 11.5 What the graph is, and the measured missing dependency

The factory `FUN_145BF0B10` creates the tracker and then calls `FUN_145B41260`, which runs a long list of small
registration functions (`FUN_145B416E0`, `FUN_145B41730`, ... `FUN_145B47A50`: one per loader stage). Each builds a
job descriptor on its stack - a work function pointer (e.g. `image+0x377CCA0`), two default completion slots
(`image+0x4380`), and a 32-bit job id - registers it with `FUN_141C86590(desc, id, 0)`, and `FUN_141B8E670` inserts the
resulting job object into the tracker's hash table at `+0x628/+0x630`, keyed by the job's id at `+0xDC`. The stored job
objects (cdb `boot-probe10`, hash-table entries and both lists) have work function pointers at `+0x50`, the executor
`image+0x4EAA040` at `+0x68` (it calls the generic job runner `FUN_140063CD0(job, job+0xC, job+0xB)`), the owning
tracker at `+0x80`, and the job id at `+0xDC`. So the tracker is the **loader's registered job graph**: 70 jobs in the
table, 249/115 definition entries in the two lists.

At the breakpoint (after the body's dispatch, before the wait) the 70 registered job objects are **queued but not
started**: their state byte `+0xE0` is 0 and their scheduling counter `+0xF0` is 2 (the value `FUN_14509B890` writes
when it queues a job), and the two job-definition lists still hold entries with `+0xF0 = 0`. The in-process boot
samples the same structures 3 s apart while the wait holds: nothing changes - not one job moves to its running state,
no job completes, and the pending dword stays `0x80000001`. The frame thread keeps presenting and the D3D12 trace keeps
running (280k calls), so this is not a frozen process: the graph simply never gets picked up by the title's job
workers.

**The exact missing dependency is title-side job dispatch, not a shim answer.** Every platform path that could hold a
job is ruled out by measurement: all 4,199 DirectStorage requests completed `S_OK` (section 5), zero
`EnqueueSetEvent` and zero D3D12 `SetEventOnCompletion`/`SetEventOnMultipleFenceCompletion` calls occur in the trace,
no runtime thread waits on the tracker event (the waiter is the title entry thread), and no duplicate of the event
exists (11.3). The jobs sit in the title's own queue; the next dispatch step that should hand them to a worker is the
same loader-side indirect dispatch whose chain docs/streamed_consumer.md section 6b/6c measures dead-ending after the
pipeline precompile (the loader object is never armed again; no further read batch is posted). Our runtime cannot
honestly complete this graph from its side: the result object the post-wait emitters consume must be filled by the
jobs, and a bare completion signal was measured to fault (section 4). That is the measured negative, and it moves the
frontier from "which shim must signal the event" to "which title-side dispatch starts the graph".

### 11.6 The next falsifiable measurement

One boot with `PALMETTO_WATCH_GLOBAL` on the tracker's pending dword `0x9844898+0x608` (or the in-process probe
sampling every 100 ms instead of at +3 s) would date the last write of the pending dword. The decisive next probe is
on the job side, not the shim side: sample the registered jobs' `+0xF0`/`+0xE0` in the first 200 ms around the body's
dispatch and read the scheduler's global work queue (`_DAT_14764A8E8`) depth at the same moment, to separate "the batch
queued its jobs but the scheduler never submits them" from "the scheduler submits them and the workers never pick them
up". Either answer names the title-side owner of the missing step; neither is a call our runtime currently answers.

### 11.7 Tests and gates (this section)

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` (binary built 16:27:45 with the opt-in tracker probe) |
| `_setup\build_tests.cmd` | `TESTS_BUILT` (no test target changed; the probe is a diagnostic switch on the default path) |
| default path | `PALMETTO_STAGE2_TRACKER` unset: no probe thread is started and no extra read is performed; the three control boots in 11.1 are the same default boot behaviour (stage 2, ALIVE 130 s) |
| docs checks | `tests/test_docs_env_vars.py` sees the new `PALMETTO_STAGE2_TRACKER` name because this page is untracked until the lead commits it, as in section 9 |

### 11.8 Attribution and limits (this section)

Same attribution and policy as section 10; no upstream code or behaviour claim is used here either. Limits: the job
dispatch step is bounded by the two measured boots (one cdb breakpoint, one in-process pair of samples); the hash
table's first 48/70 buckets were read, not all 70 job objects; the second sample is 3 s after the first, so a job that
started and finished inside those 3 s would be missed - the definitions' unchanged state makes that unlikely but it is
not excluded. No game data: RVAs, field offsets, counts, handles and outcomes only; no job names, ids or package
contents.

## 12. Dispatch split (follow-up): the batch is submitted and executed by title workers; it stops with a ready set no pump drains

**Status: measured; the three-way split is answered and the batch's dispatch is not the missing step.** A cdb trace of
the stage-2 batch dispatch shows the body queueing **115** jobs, the scheduler handing them to **five different title
worker threads**, and the job runners (`image+0x4EAA040` -> the generic runner `image+0x63CD0`) executing on those
threads. The graph then stops with the tracker's pending counter at **115 of 249** and **four job ids marked ready in the
scheduler's ready bitmask that no pump ever dispatches**; the entry thread is in the section-2.1 wait throughout. So
(a) "queued but never submitted to the scheduler" and (b) "submitted but no worker picks it up" are both **refuted by
measurement**; what is missing is a scheduler wake for the ready set, and the only dispatch path that would post the work
item covering those classes is the title's own loader dispatch (the one [streamed_consumer.md](streamed_consumer.md)
sections 6a/6b measured stopping after the pipeline precompile). No runtime call is outstanding that could answer it, so
this slice is a **measured negative**: no shim change, no fix, stage 3 is still never requested.

### 12.1 What ran

| | |
|---|---|
| Tree | `D:\leonida` working tree with other slices' uncommitted files present; this slice's source change is the opt-in `PALMETTO_STAGE2_DISPATCH` probe in `port/src/main.cpp` (off by default; no thread starts unless set) plus this page |
| Binaries | probe build from this tree; `port\build\palmetto.exe` SHA-256 `B17FF83F8E94577A049CA84BB8137B95915CBF4C9F9CC8EC2E1DA006399A7A9B`, built 17:03:50 (rebuild by the shared tree after the probe was added; other slices' `vulkan_replay.h`/docs changes are in the tree) |
| Boots | four 130 s boots under the shared boot lock, one at a time: `boot-d1` (the in-process dispatch probe, no debugger), `boot-c3` (one cdb session tracing the dispatch chain, then a wait-site dump), `boot-c4` (two cdb sessions in one boot: A at the wait, B at ~82 s) and two earlier cdb attempts (`c1`, `c2`) that are **not used** because the runtime's first-chance exceptions stopped the debugger before the breakpoint (see 12.7) |
| Harness | `tooling/boot_report.py --timeout 130`, captures outside the repository in `D:\leonida-work\stage2q\` |
| Environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_STATE_TRACE=2,130`; `boot-d1` also `PALMETTO_STAGE2_DISPATCH=2` and `PALMETTO_WAIT_SAMPLE=25,40,70`, the cdb boots also `PALMETTO_WAIT_SAMPLE=30,90` (c4) |
| Measure points | in-process: the tracker global `image+0x9844898` polled every 1 ms from 2 s; cdb trace: entry of the body `0x21B8B20`, the dispatch `0x509B3E0`, the per-job queue test `0x509B890`, the scheduler pump `0x560B0`, the run item `0x4E53470`, the ready dispatch `0x4E53EF0`, the job executor `0x63CD0`, the executor thunk `0x4EAA040`, the completion walk `0x4E53A10`, the ready mark `0x3F470`, the worker dequeue `0x256FE80`, and the wait call site `0x21B8BDF` |

The state word moved as before in every measured boot: `init 2` at 18.5-23.4 s, `loading 1` two seconds later, machine
`current 2 / next 2` to the kill. No boot requested stage 3.

### 12.2 The dispatch chain, decompiled

The batch body `FUN_1421B8B20` is 0xAB bytes and does one thing: it locks in a pending operation, then tail-calls
`FUN_14509B3E0(tracker)`. That function is the dispatch:

- it walks the dispatch list at `tracker+0x88` (u16 count at `tracker+0x90`) through **`FUN_14509B890(tracker, job, run_state)`**;
  that function tests `job+0xE0` (state), the class masks `job+0xE4 & tracker+0x5E` and `(job+0xE1 & tracker+0x70) ==
  job+0xE2`, and the dependency callback at `job+0xE8`; a runnable job gets **`job+0xF0 = 2`** and is appended to the run
  state (`run_state+0x87C` count, `run_state+0x880` array);
- it then calls **`FUN_1400560B0(run_state, 0, 7, 4)`**, the scheduler pump. The pump claims the queue lock at
  `queue+0x308` (queue = **`tracker+0xB8`**), runs the current work item through `FUN_144E53470`, enqueues a command
  packet into the queue's 64-slot ring (head dword `queue+0`, tail dword `queue+4`, sequence dwords `queue+8`,
  payload qwords `queue+0x108`) and drains the ring itself before returning;
- the ready set lives in the same queue object: capacity `+0x1860`, count `+0x1864`, id array `+0x1868`, bitmask
  **`+0x1870`** (64 qwords), min/max/count at `+0x1A90/+0x1A92/+0x1A94`; it is drained by the ready dispatcher
  `FUN_144E53EF0`, reached from the run item `FUN_144E53470` and so only from the pump.

The batch object also keeps the pending-completion counter at **`tracker+0xB0`** (dword; 249 at the body entry = the
249 factory definition entries) and the graph id `0xAC407EB2` at `tracker+0xB4` - section 11.2's `+0xB0` reading is
corrected to the pair. Job execution is on the title's own task pool: worker threads entered at `game+0x2215EA0`, whose
loop `FUN_14221C1F0` dequeues a work item (`FUN_144D9D9F0`), runs its function, and parks on the worker's event at
`worker+0x18` when idle; the pool waker `FUN_144DA4240` signals those events through the image's SetEvent wrapper
`0x5930`. A job's executor at `job+0x68` is `FUN_4EAA040`, which calls the generic job runner **`FUN_140063CD0`**: it
tests the same class masks, calls the job's work function at `job+0x50`, then pumps the job's tracker batch with
`FUN_1400560B0`, so a job's completion re-enters the same scheduler.

### 12.3 The split, `boot-c3` (cdb trace)

The trace lines are printed in execution order; `tid` is the current thread, `b0` is `tracker+0xB0`:

| trace | value | meaning |
|---|---|---|
| `[BODY]` | `tid 76544 tracker 0x196dfcc92f0 armed 0 b0 f9 queue 0x196dfcc9940` | body entry, 249 pending, not armed |
| `[DISP]` | same tid, `armed 1`, `b0 f9` | the dispatch runs on the title entry thread |
| `[QUEUE] x115` | every entry `e0 0 f0 1 e8 0`, ids `0`..`247` | **all 115 dispatch-list jobs are runnable** (no dependency callback, state idle) |
| `[PUMP]` | `rdy 115 l4 0 l45c 0 l66c 0 a90 0x196dfcc9940` | the pump receives the 115 runnable jobs |
| `[RUN]` / `[READY]` | `rdy 115` / `bits 115` | the ready set is filled with 115 ids |
| `[THUNK]` / `[EXEC]` | tids **51516, 31728, 31272, 14264, 42576**; executors `0x4EAA040` -> `0x63CD0`; job `...87c40 e0 0 f0 2`, `...cb5a0 e0 0 f0 2` | **five different title worker threads execute job runners**; the jobs are no longer queued, they are running |
| `[WAIT]` | `tid 76544 tracker ... armed 1 b0 f9 queue ...` | the entry thread reaches the section-2.1 wait while the workers are still starting the graph |

At the wait breakpoint several workers were inside the job runner (stacks `0x4EAA044 <- 0x4221C29C <- 0x42215ECA` and
`0x63CD0 <- 0x4EAA05A <- ...`), i.e. the graph was actively executing on title threads when the entry thread parked.

**Reading.** (a) is refuted: the batch is queued and submitted to the scheduler, and the scheduler ring is drained by
the pump (the section 11.5 reading of `+0xF0 = 2` as "sitting in the title's own queue" is not the state at the wait).
(b) is refuted: worker threads pick the work up. Nothing in the dispatch chain is a GPU submit, so (c) does not apply.

### 12.4 Where it stops, `boot-c4` (two cdb sessions) and `boot-d1` (in-process)

| measure | `boot-c4` A: at the wait | `boot-c4` B: ~82 s | `boot-d1`: first sample / +2.1 s |
|---|---|---|---|
| pending `tracker+0xB0` | **0xF5 (245)** of 249 | **0x73 (115)** and frozen | 0x7C (124) / 0x73 (115) |
| queue ring head/tail (empty when equal) | 0/0 | 5/5 | 26/26 / 26/26 |
| ready-set count `+0x1864` | 0 | 0 | 0 |
| **ready bitmask `+0x1870` ids** | **152** | **83, 91, 92, 152** | (not sampled) |
| entry thread | wait call site `0x21B8BDF` | still in the section-2.1 wait | still in the wait at 130 s |

The graph completes a burst of ~130 nodes in the seconds around the wait (249 -> 115) and then freezes: at 82 s it is
still 115, with four job ids marked ready in the scheduler's ready bitmask and no work item draining them. `boot-d1`
shows the same terminal value (0x73) two seconds after the wait and the ring and ready set empty, so it is not a queue
backlog: the ready set's four ids are the only work left, and nothing pumps them.

### 12.5 The named missing step

The ready set is drained only by `FUN_144E53EF0`, which runs only inside the pump `FUN_1400560B0`, which is entered from
a job completion (`FUN_14006AED0`), from a job runner (`FUN_140063CD0`) or from a posted task-pool work item
(`FUN_14256FE80` and the other pump call sites). With the batch's own work items finished and no further completion
queued, no caller re-enters the pump for the four ready ids. The work-item post that would carry them is the **title's
loader dispatch**, the same path [streamed_consumer.md](streamed_consumer.md) section 6a/6b measured: the loader object
is disarmed at ~19.8 s and no further read job is posted, and the stage-2 job graph's remaining nodes are the loader
stages that depend on it. This is title-side scheduling - the title's own scheduler never posts the wake for its own
ready set in this boot - not a call our runtime drops, and not a GPU completion (no GPU call is in the dispatch chain;
the 4,199 DirectStorage requests of section 11.5 all completed `S_OK`).

Consequences: the fix cannot be a runtime signal. Programme the measured facts: the ready set exists and is drained by a
pump the title itself schedules; the stage-3 request at `0x81B2EF` is never written and the stage word stays 2/2, so the
frontier remains "which title-side dispatch posts the loader work item", not "which shim answers".
Requested follow-up: **stage 3 is not requested by this slice**; the next measurement would have to resolve the ready
ids (`83`, `91`, `92`, `152`) to their job definitions and name the work-item class lane that owns them, which needs a
decompile of the run-state class lanes (not done here; no game data is needed for it, only RVAs and ids).

### 12.6 Boot numbers

| boot | verdict | d3d12 calls | stage | dispatch facts |
|---|---|---|---|---|
| `boot-d1` | ALIVE 130.4 s, 0 exceptions, killed by the 130 s timeout | 254,083 | 2 at 22.0 s, 2/2 to the kill | tracker created at 23.420 s with the wait already armed at the first sample; ring 26/26; b0 0x7C -> 0x73 in 2.1 s |
| `boot-c3` | STOP 53.2 s, exit `0x80000003`, at `game+0x4EAA044` (a breakpoint byte left by the single cdb session, see 12.7) | 55,865 | 2 (dispatch at ~24 s) | the dispatch trace of 12.3 |
| `boot-c4` | ALIVE 130.3 s, 0 exceptions, killed by the 130 s timeout | 253,642 | 2 at 18.5 s, 2/2 at 120 s | A at the wait: b0 245, ring 0/0, ready id 152; B at 82 s: b0 115, ring 5/5, ready ids 83/91/92/152 |

No frame capture was made in these boots (they are measurement boots, not a fix A/B pair), so no non-black frame is
claimed; the presentation/frame path was already the subject of the earlier pages.

### 12.7 Tests and gates (this section)

This section's change to the runtime is the opt-in `PALMETTO_STAGE2_DISPATCH` probe in `port/src/main.cpp` (plus the
census now walking the job table's full 128-slot capacity instead of its first 70 slots, which is what under-counted the
jobs in 12.4's d1 column): off by default, no thread starts unless the variable names a second.

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| slice test | none added: the change is a diagnostic switch on the default path; the defect it measures is title-side and the slice makes no behaviour change to test |
| test subset (`port/build`) | `frame_dump_test`, `unimplemented_policy_test`, `xbox_present_test`, `xs_proxy_mock_test`, `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test` exit 0; `d3d12_fence_timing_test` exits `0xC0000005`, the same pre-existing failure section 9 records in a tree that carries another slice's in-flight `vulkan_replay.h`/`d3d12_vulkan_device.cpp` changes |
| docs checks (`tests/test_docs_*.py`, run with the session's `GIT_CONFIG_*` variables cleared) | `test_docs_links`, `_list_items`, `_table_rows`, `_table_ids`, `_repo_paths`, `_encoding`, `_design_rows`, `_repeated_lines`, `_pr_template` and `_python_deps` pass; `test_docs_cli_flags` fails on `docs/frame_first_light.md:318` and `test_docs_env_vars` on `PALMETTO_COLOUR_PATH` (`port/src/vulkan_replay.h`), both from other slices' untracked changes, neither from this page |
| default path | `PALMETTO_STAGE2_DISPATCH` unset: no probe thread and no extra read; `boot-c4`'s two sessions and `boot-c3` are default-path boots apart from the named diagnostics |

### 12.8 Cdb practice note and limits

- The title image faults with first-chance exceptions on the normal path (the runtime's page/AV handling and its int3
  emulation), so a cdb session must run `sxd av` and `sxd *` before continuing; without them the debugger stops on the
  first one and the session is useless (`boot-c1`/`boot-c2`, not used as evidence). The `c3` session's STOP at
  `game+0x4EAA044` after the session detached is a left-over breakpoint byte, not the boot's own behaviour: `c4`'s two
  sessions detached the same way and the boot then ran to 130.3 s ALIVE with 0 exceptions.
- The in-process probe's 1 ms poll cannot see inside the dispatch (it is sub-millisecond): `boot-d1`'s first sample is
  already inside the wait, so the dispatch split relies on the cdb trace and the in-process probe only supplies the
  steady state and the post-wait countdown.
- The ready-set ids are bitmask bits, not job names; the 115 pending units were not decomposed by class. The loader
  dispatch attribution is the earlier [streamed_consumer.md](streamed_consumer.md) measurement, not a new one; this
  section's new fact is that the stage-2 batch is *not* the part that stops being submitted.
- Two cdb sessions in one boot perturb timing; the 82 s dump is a snapshot, not a steady-state trace. No game data: this
  section carries RVAs, field offsets, handles, counts, thread ids and outcomes only - no package path, executable bytes
  or shader/table data.

### 12.9 Attribution (this section)

Same attribution and policy as section 10: this slice was implemented from this project's own measurements, no upstream
code or behaviour claim is used, and no git writes were made by this slice.

## 13. Ready-set resolution (follow-up): the five ready ids, the one unreleased claim, and the unposted work item

**Status: measured; the ready ids are resolved and the stall is a title-side lost work item, not a missing pump wake.**
Section 12 left one question open: which title-side dispatch posts the work item that would drain the ready set, and
what the ready ids `83`, `91`, `92` and `152` are. A new opt-in probe (`PALMETTO_STAGE2_READY`) resolves the ready
bitmask through the scheduler's own id-to-job table: all five ids it observes (`83`, `88`, `91`, `92`, `152`) are
**lane 0, class 0** scheduler jobs of the same stage batch. The queue's class-lane accounting (`queue+0x11F0`, the
aggregate of seven bit-sliced counter planes) has the bit of every one of them set, and that aggregate is
**byte-for-byte the claim mask of one job, id `97` (hash `0x7325b9`)**. Job 97's claim was taken by the dispatcher and
never released: its work item exists (`job+0x68`, executor `game+0x4EAA040`) but its job state is still `2` (queued)
and no task-pool ring holds it. The dispatcher selects `~aggregate & ready`, so with those bits set **no pump entry can
dispatch the ready ids at all** - a wake would not help. The release path is `FUN_144E53AD0`, called only by the run item
`FUN_144E53470` for the job the run state carries, so the lost item is the missing dispatch. No runtime answer is
missing; this section is a **measured negative**: no shim change, no fix, stage 3 is still never requested.

### 13.1 What ran

| | |
|---|---|
| Tree | `D:\leonida` working tree with other slices' uncommitted files present; this slice's source change is the opt-in `PALMETTO_STAGE2_READY` probe in `port/src/main.cpp` (off by default; no thread starts unless set) plus this page |
| Binaries | four probe builds of `port\build\palmetto.exe`, one per boot as the probe grew (`boot-r4` ran SHA-256 `C54DD5F9475D267EC5FE2388CB64600F70D184EFF6D8B07F2E5578CF530A7EEA`); the gate build of this tree is SHA-256 `043EACBB83C0BC19BEA6AFDCAD7F2200D61FE9877F1199153BA8228473287B95` |
| Boots | four 130 s boots under the shared boot lock, one at a time: `boot-r1` (ready-set/aggregate dump at the wait and 6/18 s later), `boot-r2` (adds a per-job claim-mask XOR census), `boot-r3` (adds the holder's work-item header and the task-pool rings) and `boot-r4` (adds the job context words; the final numbers) |
| Harness | `tooling/boot_report.py --timeout 130`, captures outside the repository in `D:\leonida-work\readyids\boot-rN\` |
| Environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_STATE_TRACE=2,130` and `PALMETTO_STAGE2_READY=1` |
| Measure points | tracker global `image+0x9844898`; queue `tracker+0xB8`; ready bitmask `+0x1870`; ready id array `+0x1868`/count `+0x1864`; id->job table `+0x1AB0`; per-id claim masks `+0x1AA0 + id*0x220`; counter planes `+0x310`, `+0x530`, `+0x750`, `+0x970`, `+0xB90`, `+0xDB0`, `+0xFD0`; aggregate `+0x11F0`; published filter `+0x1410`; pending aggregate `+0x1630`; default class `+0x1B38`; job fields `+0x50`, `+0x68`, `+0x73`, `+0x74`, `+0x9x`, `+0xC8`, `+0xCA`..`+0xCE`, `+0xD8`, `+0xE0`..`+0xF8`; tracker job table `+0x628`/`+0x62C`/`+0x630`; task-pool streams `image+0x71BFD00 + lane*0x40` and their per-class rings |

The state word moved as before in every measured boot: `init 2`, `loading 1`, machine `current 2 / next 2` to the kill;
`boot-r4` changed at 24.0 s. No boot requested stage 3.

### 13.2 The ready ids, resolved

`boot-r4`, second sample (+63.562 s), with the section-12 accounting fields:

| field | value | meaning |
|---|---|---|
| ready id array `+0x1864` | 0 | the staging array is empty; the ids live only in the bitmask, as in section 12 |
| ready bit count `+0x1A94` | 5 | `83`, `88`, `91`, `92`, `152` |
| aggregate `+0x11F0` | 21 ids | `81,82,83,88,91,92,96,97,100,101,133,142,143,144,151,152,154,162,172,180,239` |
| published filter `+0x1410` | 0 | the pending aggregate was already folded in and `+0x1850 = 1` |
| pending aggregate `+0x1630` | 0 | no ready mark is outstanding |
| planes 1..6 | 0 | every nonzero counter is exactly 1 (plane 0 is the odd/even plane) |
| ready min/max `+0x1A90/+0x1A92` | 0 / 247 | the id range, as section 12 |

Each id through the id->job table (`queue+0x1AB0 + id*8`), after the job's own fields:

| id | hash `job+0xDC` | lane `+0x73` | class `+0x74`/`+0xC8` | class masks `+0xE1`/`+0xE2`/`+0xE4` | dep callback `+0xE8` | state `+0xE0` / scheduler `+0xF0` | work function `+0x50` | body |
|---|---|---|---|---|---|---|---|---|
| `83` | `0x86c60c95` | 0 | 0 / 0 | 1 / 0 / 0 | 0 | 0 / 0 | `game+0x95EB60` | generic stage invoker `(*(code*)*job)(job)`; the stage function is the job's first word `game+0x5A67E40` |
| `88` | `0xd5e9ea68` | 0 | 0 / 0 | 1 / 0 / 0 | 0 | 0 / 0 | `game+0x95EB60` | same generic stage invoker; stage function `game+0x5AAFC30` |
| `91` | `0x260f14aa` | 0 | 0 / 0 | 1 / 0 / 0 | 0 | 0 / 0 | `game+0x5C1A960` | `FUN_141E09DC0(2)`: mode-2 stage init, four record registrations and a callback table |
| `92` | `0x5fdc36d5` | 0 | 0 / 0 | 1 / 0 / 0 | 0 | 0 / 4 | `game+0x3750CE0` | `FUN_14593BD40`: render-state/registration table init (`FUN_1408E8930` hash slots) |
| `152` | `0x3c819f1` | 0 | 0 / 0 | 1 / 0 / 0 | 0 | 0 / 2 | `game+0x5BDD700` | `FUN_141DDA290`: walk of a global 0xC0-byte record table, sets `0x1477BCA88 = 3` |

**The class lane is lane 0, class 0 for every ready id.** The lane byte `job+0x73` is the worker-pool lane (the pool
streams are at `image+0x71BFD00 + lane*0x40`; the pump's out-list posting filters lane 0 and lane 1) and the class byte
`job+0x74`/`+0xC8` is the per-lane class ring. `boot-r1` and `boot-r4` both show five ids in the frozen state; section
12's `boot-c4` snapshot at 82 s showed four (`83`, `91`, `92`, `152`) - `88` is in the same lane/class and both runs
have five at the later samples, so the set is stable and the count difference is sample timing, not a different stall.

### 13.3 The claim accounting: why no pump dispatches them

The dispatcher `FUN_144E53EF0` picks `~mask & ready-bits`, where `mask` is `queue+0x1410` when the two published flags
are both 1 and `queue+0x11F0` otherwise (`+0x1850 = 0x1` at the samples, so the aggregate is the mask). Every ready id's
bit is set in that mask, so the scan finds nothing and returns - it does not skip them and dispatch others, it dispatches
**nothing**. The accounting is bit-sliced: seven 7-bit counter planes over job ids, where

- `FUN_144E53EF0` **adds** the dispatched job's per-id claim mask (`queue+0x1AA0 + id*0x220`) to the planes at the moment
  it dispatches that job (the same code that sets `job+0x74` and appends `job+0x68` to the out list);
- `FUN_144E53AD0(queue, id)` **subtracts** one job's mask from the planes, and it is called only from the run item
  `FUN_144E53470` for the job the run state carries at `+0xA88` - i.e. the release of a dispatch happens at the next run
  item, not at the job's completion.

The measured mask equals exactly one registered job's mask: **id `97`, hash `0x7325b9`** (21 bits, XOR against
`queue+0x11F0` = 0; every other job's mask is either empty - most jobs - or differs by at least one bit). Job 97 is lane 0,
class 0, state `+0xE0 = 0`, scheduler state `+0xF0 = 2`, work function `game+0x263CDF0` -> `FUN_145A58E10(2)`
(a lazy-singleton mode-2 subsystem init, vtable `image+0x62041E0`). Its work item at `job+0x68` has the job executor
`game+0x4EAA040` as its first word, zero children, lane 0, class 0 and weight 16. The claim's id set contains every ready
id, so those five cannot be selected while job 97's claim stands.

Between the `wait` sample (+25.252 s, aggregate with id `94` and not `101`) and the `steady` sample (+31.323 s, aggregate
with id `101` and not `94`) the aggregate changed: the previous claim was released and job 97's claim was taken in that
window, i.e. the stage chain ran on after the wait and ended with job 97 dispatched. From then to +63.562 s nothing moved.

### 13.4 The lost work item

Job 97's item was built and claimed but never executed:

| check | measured |
|---|---|
| job state `+0xF0` | `2` (set by `FUN_14509B890` when the job is queued into the run state; the executor `game+0x4EAA040` sets `4` before running the body) |
| item header | `job+0x68`: first word `game+0x4EAA040`, `+0x8 = 0`, children `+0xA = 0`, lane `+0xB = 0`, class `+0xC = 0`, weight `+0xD = 16` |
| lane-0 stream (`image+0x71BFD00`, ring pointer at `+8`) | class-0 ring head 426, `slot[head&0x7FF] == head` -> **empty**; class-4 ring empty |
| lane-1 stream (`image+0x71BFD40`) | class-0 ring empty |
| task-pool post | `FUN_144DA40E0` -> `FUN_144DA41A0` per item into the lane/class ring; `FUN_144DA4200` sets the wake count |

So the item is in no ring and its job never ran. Since the pump's out-list posting block (`FUN_144DA40E0` call site in
`FUN_1400560B0`) is the only path that could have put it there, the **missing dispatch is that post: the pump build of job
97's work item into the lane-0/class-0 task stream**. Its follow-on is the release `FUN_144E53AD0(queue, 97)` at the next
run item, which would clear the aggregate bits and let `FUN_144E53EF0` dispatch `83`, `88`, `91`, `92` and `152`.

### 13.5 The named missing post, and its owner

- **Post that should carry the ready ids:** the out-list posting block of the pump `FUN_1400560B0` -> `FUN_144DA40E0`
  -> the task-pool stream `image+0x71BFD00 + (item+0xB)*0x40`, class ring `(item+0xC)*0x6008`. For the ready ids that is
  **lane 0, class 0**; the one item that is missing is job 97's (`job+0x68`, executor `game+0x4EAA040`).
- **Release that would follow:** `FUN_144E53AD0(queue, 97)` in the next run item `FUN_144E53470`; then
  `FUN_144E53EF0` dispatches the five ready ids.
- **Owner:** the title's own job scheduler / loader-batch dispatch - lane 0 is the general worker-pool lane, and the
  ready jobs are the same stage batch `streamed_consumer.md` sections 6a/6b measured stopping after the pipeline
  precompile. No title subsystem call crosses the runtime boundary for either step.

Comparison with the streamed-consumer dead chain: the shape is the same title-side stop, but the mechanism is now named:
the stage chain's **last dispatched job's claim is held by the scheduler** because its work item never reached a worker;
it is not a DirectStorage answer, not a GPU completion and not a missing event.

### 13.6 Runtime dependency verdict

Measured: **no runtime answer is missing.** The dispatch, claim, release and post code is title code; the only OS calls
on the path are the pool's worker wake (`image+0x5930` SetEvent wrapper) and the wait helpers, and no wake is
outstanding (the rings are empty and the section-12 cdb dump already showed the workers parked with no shim call
pending). There is no GPU call in the path, no DirectStorage request pending (section 11.5: all completed `S_OK`) and no
device/queue call the runtime drops or never completes. A runtime change cannot create the missing item or the missing
release: the item, the claim mask and the counter planes are title memory with no runtime counterpart, and a bare wake
was already measured to fault (section 4). The section-11.6 falsifier list is therefore answered for this path: the
scheduler's own claim accounting is the blocker, and the next measurement is a debugger breakpoint on `FUN_144DA40E0`
to see whether job 97's item ever enters the post call.

### 13.7 Boot numbers

| boot | verdict | d3d12 calls | stage | ready set | aggregate / filter / pending | extra |
|---|---|---|---|---|---|---|
| `boot-r1` | ALIVE 130.4 s, 0 exceptions, killed by the 130 s timeout | 237,951 | 2 at 26.0 s, 2/2 to the kill | `83, 88, 91, 92, 152` | 21 ids / 0 / 0 | first ready-set probe: id->job resolution works; holder not analysed yet |
| `boot-r2` | ALIVE 130.4 s, 0 exceptions | 260,809 | 2 at 18.0 s, 2/2 | `83, 88, 91, 92, 152` | 21 ids / 0 / 0 | claim-mask XOR census: only id 97 matches the aggregate (all other jobs empty or off by >= 1 bit) |
| `boot-r3` | ALIVE 130.4 s, 0 exceptions | 270,331 | 2 at 22.0 s, 2/2 | `83, 88, 91, 92, 152` | 21 ids / 0 / 0 | holder item `game+0x4EAA040`, 0 children, state 2; lane-0 class-0 ring empty |
| `boot-r4` | ALIVE 130.4 s, 0 exceptions | 263,704 | 2 at 24.0 s, 2/2 | `83, 88, 91, 92, 152` | 21 ids / 0 / 0 | job context words (stage functions of `83`/`88`); stage-3 request at `0x81B2EF` still never reached |

No frame capture was made in these boots (measurement boots, not a fix A/B pair), so no non-black frame is claimed;
loader/streaming activity after the stall is the same quiet state `streamed_consumer.md` sections 6a/6b and
`stage2_blocker.md` section 12 recorded.

### 13.8 Tests and gates (this section)

This section's runtime change is the opt-in `PALMETTO_STAGE2_READY` probe in `port/src/main.cpp`: off by default, no
thread starts and no title read happens unless the variable names a second.

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| slice test | none added: the change is a diagnostic switch on the default path; the defect it measures is title-side and the slice makes no behaviour change to test (section 12's precedent) |
| test subset (`port/build`) | `frame_dump_test`, `unimplemented_policy_test`, `xbox_present_test`, `xs_proxy_mock_test`, `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test` exit 0; `d3d12_fence_timing_test` exits `0xC0000005`, the same pre-existing failure section 9 records in a tree that carries another slice's in-flight `vulkan_replay.h`/`d3d12_vulkan_device.cpp` changes |
| docs checks (`tests/test_docs_*.py`, run with the session's `GIT_CONFIG_*` variables cleared) | `test_docs_links`, `_list_items`, `_table_rows`, `_table_ids`, `_repo_paths`, `_encoding`, `_design_rows`, `_repeated_lines`, `_pr_template`, `_python_deps` and `_env_vars` pass; `test_docs_cli_flags` fails on four other slices' untracked rows (`docs/frame_first_light.md:318`, `docs/memory_growth.md:265`, `docs/raster_state.md:212`, `docs/slot70_publication.md:234`), none from this page |
| default path | `PALMETTO_STAGE2_READY` unset: no probe thread and no extra read |

### 13.9 Limits (this section)

- The ids are job ids in the tracked stage graph; the hashes are the graph's registration keys and the work functions
  are RVAs. The four ready jobs' subsystem *names* are not recovered (the image carries no symbols for them); they are
  identified by RVA and by their mode-2 stage-init / registration-fan-out shape, and they sit in the same batch as the
  loader stages. Job 97 is named by hash and RVA only.
- The aggregate is read, not traced: the equality of job 97's mask with `+0x11F0` is exact at the sampled instants, and
  planes 1..6 are zero, so the counters are 1 where set. The claim could have been taken by a dispatch that is not the
  one that built the item in the same pump, so the post loss is inferred from state 2 + empty rings + held claim, not
  from a trace of the post call; a cdb breakpoint on `FUN_144DA40E0` is the next falsifier.
- The per-job mask census covers ids `0..255`; the aggregate's top sampled bit is `239`, so this is the whole ready
  graph as seen through the id->job table. The tracker job table is a 128-slot hash table (70 used slots in the
  samples), not the id space.
- No frame capture: measurement boots, not a fix A/B pair; no non-black frame is claimed. No game data: this section
  carries RVAs, hashes, field offsets, ids, counts and outcomes only.

### 13.10 Attribution (this section)

Same attribution and policy as section 10: this slice was implemented from this project's own measurements, no upstream
code or behaviour claim is used, and no git writes were made by this slice.

## 14. The out-list post (follow-up): job 97's item is posted on this tree, and the frozen claim moves to a running stage job

**Status: measured; section 13's missing post does not reproduce on the current tree.** A cdb software-breakpoint
session on the post and pump RVAs is a measured negative (no breakpoint reported in two 130 s boots), so the question
was answered with the runtime's own in-process poke probe instead: job 97 is dispatched once, appended to the pump's
out-list, and its item is pushed into the lane-0/class-0 task ring **by the post `FUN_144DA40E0` itself** (return
address `0x144DA4161`). Job 97 then runs and completes, and its 21-bit claim is released. The frozen state later in the
same boot holds a **different** claim, job id `109`: it took the dispatcher's selected branch, its item was never
posted, its generic runner set `job+0xF0 = 4` before calling the work function and the job is still inside that work
function at +46 s and +85 s. No runtime answer is missing in either path; the next measurement is job 109's stage
function and its runner stack, not a wake.

### 14.1 What ran

| | |
|---|---|
| Tree | `D:\leonida` at `77e44e0ab` with other slices' in-flight files present (`port/shims/gameinput.cpp`, `port/shims/xgameruntime.cpp`, `port/shims/xgameruntime_gamesave.cpp`, `port/shims/xgameruntime_user.cpp`, `port/src/vulkan_backend.h`, `port/src/vulkan_replay.h`, `port/CMakeLists.txt`, four test files, untracked shim/upscale/persistent-store headers and three new docs pages); this follow-up adds no source change (its cdb command file, poke driver and parsers live outside the repository in `D:\leonida-work\job97\`) |
| Binary | `port\build\palmetto.exe` SHA-256 `731934FD492D31E14C59C5A443D14DDCA7918C6EC287CFAF0287290AF07EA52E`, mtime 18:03:04 (the shared tree's build after the last commit) |
| Boots | five 130 s boots under the shared boot lock, one at a time: `boot-c1` (cdb attach at 3.5 s, then the session's own quit ended the process at 4.5 s - not used as evidence), `boot-c2` (cdb attach at 3.5 s), `boot-c3` (cdb attach at 11 s), `boot-p1` (in-process poke probe; the measurement) and `boot-p2` (control: same binary and environment, no poke probe) |
| Harness | `tooling/boot_report.py --timeout 130`, captures outside the repository in `D:\leonida-work\job97\` (`boot-c1` .. `boot-p2`) |
| Environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_STATE_TRACE=2,130`; the probe boots add `PALMETTO_STAGE2_READY=20`, `boot-p1` adds `PALMETTO_POKE_LOG=4E53FE3,4E54020,4E54001,4DA41A0` |
| cdb session | `cdb -p <palmetto pid> -cf probe-job97.cdb -logo cdb-cN.log`, `sxd av` and `sxd *` before continuing, 16 software breakpoints (table 14.2), run to the harness's 130 s kill |
| Poke sites | the runtime's `PALMETTO_POKE_LOG` writes one `int3` per named RVA, logs `rax/rbx/rcx/rdx/r8/r9/rdi/rsi/r12`, the eight stack words and the read window at six registers, restores the byte and re-arms (the `skip_int3` path in `port/shims/startup_trace.h`); no debugger is attached in that boot |
| Probe samples | ready-set probe dumps at +39.957 s, +46.001 s and +85.582 s in `boot-p1`: queue/ready/aggregate/planes, per-job fields, the registered-job table and the task-pool streams |

### 14.2 The cdb breakpoint session: a measured negative about the debugger

The session defined software breakpoints at the post and the pump's out-list block plus their neighbours. Every one of
them failed to report in both attached boots (`boot-c2`: 130.2 s ALIVE, 0 exceptions, 13,680 D3D12 calls; `boot-c3`:
130.1 s ALIVE, 0 exceptions, 15,824 calls), while the same boots' state trace shows the title reaching and holding
stage 2 (14.6).

| RVA | what it would show |
|---|---|
| `0x21B8BDF` | section 2.1's wait call site (the one stop-style breakpoint that worked in section 12.4) |
| `0x560B0` | pump entry: run state, queue, runnable lists, selection pointer and thresholds |
| `0x4E53470` | run item: release-job field `+0xA88`, packet lists, out-list count |
| `0x4E53EF0` | ready dispatcher: ready bits, aggregate, selection arguments |
| `0x4E53FE3` | per-job dispatch candidate: id `rax`, job `rbx`, branch inputs |
| `0x4E54020` / `0x4E54001` | out-list append / selected-instead-of-appended branch |
| `0x4DA40E0` / `0x4DA41A0` | task-pool post entry / class-ring push |
| `0x4E53AD0` / `0x55AD980` | claim release / release-job setter |
| `0x63CD0` / `0x4EAA040` | generic job runner / executor thunk |
| `0x4EACC70` | per-frame pump thunk (frame-count sampler) |

- Address evidence: `dc 140000000+0x4E53FE3` at the session start prints the unmodified dispatcher bytes (`4D 85 C0 74`,
  `TEST R8,R8` / `JZ +0x74`), and cdb logged no insertion failure (the only insertion errors in `boot-c1` were the
  duplicated-base addresses of that aborted attempt). cdb inserts software breakpoints at the first continue, so the
  front-of-session byte read alone does not prove insertion; the **absence of any hit over 130 s** is the measurement.
- The one cdb pattern that has reported on this title is section 12.4's stop-style session (`boot-c4a`: a single
  breakpoint at `0x21B8BDF` whose command does not continue); the plausible reason this session's breakpoints were
  consumed before dbgeng is the runtime's `PALMETTO_SKIP_INT3` handler, which treats an `int3` in title code as a
  console trap and steps over it (`skip_int3` in `port/shims/startup_trace.h`). That is a named hypothesis and the next
  cdb falsifier (a single stop-style breakpoint in a fresh boot), not a proven mechanism.
- Limit: this is a negative about the debugger configuration, not about title execution - the boot's own trace and
  probe output show the title ran and reached stage 2 in the same boots. `0x4DA40E0`'s hit question was therefore
  answered in-process (14.3).

### 14.3 The in-process poke probe: job 97's item enters the lane-0/class-0 ring

`boot-p1` poked four title RVAs and the runtime logged 121,897 hits, all four sites live:

| site RVA | hits | registers read | result for job 97 |
|---|---|---|---|
| `0x4E53FE3` | 218 | `rax` = dispatched job id, `rbx` = job, `rdx` = out-list, `r8` = selection slot | id `97` dispatched once: `rax 0x61` (97), job `0x2912fb42460` (hash `0x7325b9`, lane 0, class 0, work `image+0x263CDF0` - the job section 13 named) |
| `0x4E54020` | 189 | `rbx` = job, `rdx` = out-list | job 97 **appended once** to the pump's out-list (out-list capacity `0x100` read at `rdx`), in the state-trace interval at 40.0 s |
| `0x4E54001` | 29 | `rbx` = job, `r8` = selection slot | job 97 **not selected**; the selected ids were 69, 76, 77, 90, 91, 95, 96, 98, 100, 103, 104, 109, 111, 112, 145, 147, 149, 161, 164, 167, 187, 197, 201, 203, 213, 214, 215, 221, 227 |
| `0x4DA41A0` | 121,461 | `rcx` = class ring, `rdx` = item, stack word 0 = return address | job 97's item `job+0x68 = 0x2912fb424c8` **pushed once**, into ring `0x29127c66c80` - the lane-0 stream `image+0x71BFD00` ring (the ready probe's "pool lane 0" ring) - from return address `0x144DA4161`, i.e. inside the post `FUN_144DA40E0` after its push call at `0x4DA415C` |

The pump's lane filter did not drop it: the item's lane byte `+0xB` is 0 and class `+0xC` is 0, so the lane-0 batch
built by the out-list block (`0x1400566E2..0x140056713`) contains it. **This is the exact post section 13 named as
missing, and it happened.** Later in the same boot job 97's probe `cand` line reads `e0 0 f0 0` (idle, scheduler state
reset), its 21-bit mask is absent from the aggregate (`xorAggr over 1 under 1`), and the aggregate at +46 s belongs to
another job: the item ran and the claim was released.

### 14.4 The frozen state: the claim is job 109, inside its work function

The ready probe's three samples in `boot-p1`:

| sample | `tracker+0xB0` pending | ready bitmask `+0x1870` | aggregate holder | selected job state |
|---|---|---|---|---|
| wait (+39.957 s) | `0x84` (132) | 1 id (`152`), flags `+0x1850 = 0x1` | 23 bits, not yet one job's mask | graph still executing |
| steady (+46.001 s) | `0x68` (104) | 0 ids | **id `109`** (mask exactly the aggregate) | job 97 `f0 0` (idle); job 109 `f0 4` |
| steady2 (+85.582 s) | `0x68` (104) | 0 ids | id `109` (same mask) | frozen |

Job 109's holder line: hash `0xa599b5cb`, item exec `image+0x4EAA040`, children 0, lane 0, class 0, weight 16,
`e0 0 f0 4 f4 3 f5 0 f7 0 f8 0`; its work function is `image+0x95EB60`, the generic stage invoker of section 13's
table. Its claim mask is the aggregate, 21 bits: ids 81, 83, 88, 91, 92, 94, 96, 97, 100, 101, 133, 142, 143, 144,
151, 152, 154, 162, 172, 180, 239.

The poke trace names how the claim was taken: job 109 is in the dispatch site's 218 hits (`rax 0x6D`, job
`0x2912fb70a70`) and in the **selected** site's hits (not the append site), and no push of its item appears. The
dispatcher's append-or-select decision is `append` unless the selection slot is non-null and empty, the job's class
byte `+0x74` is at least `param_4` and its lane byte `+0x73` equals `param_5`; the pump's caller supplies those
thresholds (`FUN_1400560B0(param_1, param_2, param_3, param_4)`), and a selected job is returned in the caller's slot
to be run inline. Job 109 lane 0, class 0 matched such a caller: it is inside the generic runner
`FUN_140063CD0`, which writes `job+0xF0 = 4` immediately before calling the work function and replaces it with 3 when
that returns. `f0 4` at +46 s and at +85 s therefore means **job 109's work function has not returned**; the claim is a
legitimately held claim, not a lost one, and the stall is the stage function it calls (`*(job)`) not finishing.

### 14.5 Runtime-dependency verdict (this section)

Unchanged for the post chain: job 97's post, ring push and release are title code and all happened; no OS call on that
path can be the cause. The new frozen claim is also title-side: the queue, the ready set, the claim masks and the
counters have no runtime counterpart, and the job that holds it is inside its own work function. Nothing in either
measurement asks the runtime for a missing answer; no fix is proposed, and stage 3 is still never requested (state word
`init 2`, `loading 1`, machine `current 2 / next 2` to the kill in every boot).

### 14.6 Boot numbers

| boot | debugger | verdict | D3D12 calls | stage transitions | stall state |
|---|---|---|---|---|---|
| `boot-c1` | cdb attach 3.5 s | killed at 4.5 s by the session's own `q` (aborted attempt) | 1,182 | none | not used |
| `boot-c2` | cdb attach 3.5 s | ALIVE 130.2 s, 0 exceptions, 130 s kill; 0 breakpoint hits | 13,680 | init 2 at 35.6 s, loading 1 at 53.8 s, machine 2/2 to the kill | not sampled (no in-process probe; cdb did not report) |
| `boot-c3` | cdb attach 11 s | ALIVE 130.1 s, 0 exceptions, 130 s kill; 0 breakpoint hits | 15,824 | init 2 and loading 1 at 42.5 s, machine 2/2 to the kill | not sampled |
| `boot-p1` | none (poke probe) | ALIVE 130.3 s, 0 exceptions, 130 s kill | 237,280 | init 1 at 4.0 s; init 2 and loading 1 at 30.0 s; machine 2/2 to the kill; no stage-3 request | pending `0x84` -> `0x68` (104) frozen from +46 s; ready ids 0; aggregate = job 109's 21-bit mask |
| `boot-p2` | none (control, no poke) | [control result pending at the time of writing] | | | |

No frame capture was made in these boots (measurement boots, not a fix A/B pair), so no non-black frame is claimed.

### 14.7 Tests and gates (this section)

This section's runtime change is none: both diagnostics it uses already exist on the default path and are opt-in
(`PALMETTO_POKE_LOG`, `PALMETTO_STAGE2_READY`); with them unset the boot is unchanged.

| gate | result |
|---|---|
| `_setup\build_gta6rt.cmd` | `PALMETTO_OK` (`ninja: no work to do.` - the gate build of this tree is the measured binary) |
| `_setup\build_tests.cmd` | `TESTS_BUILT` |
| slice test | none added: no behaviour change and no source change; the measurement uses the existing in-process diagnostics (section 12's precedent) |
| test subset (`port/build`) | `frame_dump_test`, `unimplemented_policy_test`, `xbox_present_test`, `xs_proxy_mock_test`, `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test` and `xapu_chad_test` all exit 0 |
| docs checks (`tests/test_docs_*.py`, run with the session's `GIT_CONFIG_*` variables cleared) | ten pass (`links`, `list_items`, `table_rows`, `table_ids`, `repo_paths`, `encoding`, `design_rows`, `repeated_lines`, `pr_template`, `python_deps`); `test_docs_cli_flags` fails on `--interval` rows in `docs/frame_first_light.md:318`, `docs/memory_growth.md:265`, `docs/raster_state.md:212` and `docs/slot70_publication.md:234`, and `test_docs_env_vars` on `PALMETTO_PERSIST_PROFILE` (`port/shims/xgameruntime.cpp:667`), all from other slices' in-flight changes, none from this page |
| default path | `PALMETTO_POKE_LOG` and `PALMETTO_STAGE2_READY` unset: no int3 poke, no probe thread and no extra read |

### 14.8 Limits (this section)

- The poke probe logs four fixed sites; the job ids it names come from the dispatcher's own `rax`, and the appended /
  selected / pushed attribution is by job pointer. The stack word 0 of a hit is the caller's return address, which is
  how the post caller was identified; no other frame of that caller is read.
- The frozen claim is job 109; its stage function (`*(job)`) and the thread that runs it are **not** named here. The
  next measurement is one 130 s boot with cdb all-thread dumps (dumps work even though breakpoints did not report) or a
  poke on `image+0x95EB60` to recover the stage function and the runner stack at the freeze.
- The tree is a moving target: the measured binary was built by the shared tree after the last commit and carries the
  other slices' in-flight shim changes, so the graph's progress differs from section 13 (104 of 249 pending and a
  different frozen holder, versus 115 and job 97). `boot-p2` is the control for this section: same binary and
  environment without the poke probe, to separate a tree shift from a probe-timing shift.
- The cdb negative is about the debugger configuration (14.2), not about title execution.
- No game data: RVAs, hashes, ids, field offsets, counts, handles and pointers-as-pointers only.

### 14.9 Attribution (this section)

Same attribution and policy as section 10: this follow-up was implemented from this project's own measurements, no
upstream code or behaviour claim is used, and no git writes were made by this slice.
