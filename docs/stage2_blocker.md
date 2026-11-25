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
