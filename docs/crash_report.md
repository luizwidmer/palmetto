# Crash report: the faulting thread's stack, and every other thread

**Status: implemented, wired into the trace's exception path, and tested on synthetic images — including the
every-thread pass.** The code is `port/src/crash_unwind.{h,cpp}` and its test is `port/test/crash_unwind_test.cpp`
(CTest name `crash_unwind`).

## What it does

When the title faults inside the mapped image, the existing trace names only the faulting RVA (and, with
`PALMETTO_DUMP_REGS`, some registers). One PC cannot say *which frame* left a callee-saved register wrong, so this
module walks the faulting thread's stack through the image's own `.pdata` and writes one line per frame:

```
[rt] crash report: rip 0x000002325d6d1008 rsp 0x000002325d3f99c0
image base 0x000002325d6d0000 size 0x4000, 3 .pdata entries
frame 0: image+0x1008 (routine image+0x1000) stack: ... 8 words ...
         rbx=... rbp=... rsi=... rdi=... r12=... r13=... r14=... r15=...
frame 1: image+0x1108 (routine image+0x1100) stack: ...
frame 2: image+0x1208 (routine image+0x1200) stack: ...
frame 3: 0x000002325d6d5000 (outside the image) stack: ...
[rt] crash report: 4 frames; walk stopped: the next return address is outside the image
```

Per frame: the image-relative PC (RVA), the containing routine's begin RVA, up to eight qwords read at RSP (shadow
space and stack arguments of a frame), and the eight callee-saved registers as that frame saw them. The callee-saved
column is the point: RtlVirtualUnwind restores the nonvolatiles as it unwinds, so the first frame where a value
differs from the frame below names the call that did not preserve it.

For each frame the walker searches the image's `RUNTIME_FUNCTION` array itself (PE `.pdata` is sorted by
`BeginAddress`) and then calls `RtlVirtualUnwind` with a copy of the `CONTEXT`. The mapped title is not an HMODULE,
so the image base, size and table are passed in explicitly instead of being looked up from the process. A return
address one byte past a routine's end (a call that is the last instruction) is looked up at `pc - 1`, the same
adjustment a debugger makes.

## How it is enabled

The integrator adds `src/crash_unwind.cpp` to the `palmetto` target and calls these functions:

```cpp
// Once, after Loader::load() and before guest code runs:
rt::crash_report::set_image(main_module.base, main_module.size,
                            static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(main_module.function_table),
                            main_module.function_count);

// From the exception handler, for the faulting thread's CONTEXT. The third argument is optional: it is copied into the
// report as a `pass:` line under the header.
rt::crash_report::dump(context, L"<work-dir>\\crash_report.txt", "exception 0xc0000005");

// From the same handler, for every other thread of the process:
rt::crash_report::dump_all(L"<work-dir>", /*summary_path=*/nullptr, "exception 0xc0000005");
```

The trace's own exception path reads two switches (`shims/startup_trace.h`, `log_exception`):
`PALMETTO_CRASH_REPORT=<file>` writes the faulting thread's report, `PALMETTO_CRASH_REPORT_ALL=<directory>` runs the
every-thread pass. They are independent and can be set together; the faulting thread's report is written first. Both
values are read with `GetEnvironmentVariableW(..., 512)`, so a value of 512 characters or more is ignored and the
report or the pass it asked for does not run (`shims/startup_trace.h`, `wants_own` and `wants_all`).

Both switches are gated on the exception code, because the handler is first-chance and the trace's job is to survive
exceptions the title handles for itself:

- Only a code with the SEVERE bit (`0xC0000000`) takes a report, and six routinely handled codes are excluded by
  name: a C++ throw (`0xE06D7363`, which does carry the SEVERE bit), `0x40010006` and `0x4001000A` (`DBG_PRINTEXCEPTION`
  and its wide form), `0x80000003` (breakpoint), `0x80000004` (single step) and `0x406D1388` (`SetThreadName`). Without
  the gate each handled C++ exception wrote a fresh set of reports, so the directory described the last throw instead
  of the fault.
- The every-thread pass runs **once per process**. The first qualifying code takes it; later ones are logged as
  `crash_report_all_suppressed` in the trace and do not touch the directory, whose files would otherwise be a mix of
  two passes that cannot be told apart.

`set_image` is also the reset: calling it with no image unregisters the previous one. The pointers must stay valid for
the process lifetime; they live in the mapped image (`Module::function_table` / `Module::function_count`, the same
table `Loader::load` registers with `RtlAddFunctionTable`).

`dump(context, path, tag)` creates or overwrites `path` (UTF-16) and returns the number of frames written, 0 if there
was nothing to walk or the file could not be created. A null `path` writes to stderr instead. `tag` is ASCII, is not
interpreted, and is skipped when null.

The exception handler is the caller's: a vectored handler is fine as long as it does not return
`EXCEPTION_CONTINUE_EXECUTION` past the report. The report itself does not repeat the exception code or address unless
`tag` carries them; the trace's `exception` event already logs both, and the two files can be cross-read.

## All threads

**Status: measured.** One crash shows what one thread was doing; a title that stops making progress shows it across
several. The loading gate (`docs/loading_gate.md`) is the case that asked for this: the main thread waits in the
loading loop while other threads stream, compile and submit, and which of them is waiting on which is not answerable
from the faulting thread's stack alone.

`dump_all(directory, summary_path = nullptr, reason = nullptr)` walks every thread of this process except the caller:

1. `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` + `Thread32First`/`Thread32Next` collect the ids of this process's
   threads into a fixed 256-entry stack array (`kMaxDumpedThreads`). The snapshot is closed before any thread is
   touched, so a failed suspend cannot leave the toolhelp handle open on the fault path.
2. The index file and the per-thread report files are opened here, while nothing is suspended — all of them at once,
   up to `kMaxDumpedThreads` (256) handles. They are *not* opened in batches; see the paragraph after step 4 for the
   probe that turned a 32-file batching revision back into a single burst.
3. For each id, `OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT)` is called with nothing suspended, so a lock
   it takes can still be released by whoever holds it. Then, in a short suspend window: `SuspendThread`,
   `GetThreadContext` with `CONTEXT_CONTROL | CONTEXT_INTEGER`, then the same walker `dump()` uses, writing through
   `dump_to_handle` to the handle opened in step 2. Inside the window there is no `CreateFileW`, no path joining and no
   allocation left: only `SuspendThread`, `GetThreadContext` and `WriteFile` on a handle somebody else already opened.
4. The suspension is released in a `__finally`, and one `ResumeThread` is the only thing in that block.

Steps 2 and 3 are split for a reason that is not tidiness. `CreateFileW` takes the process heap lock; a pass that opens
its files from inside its own suspend window can park the thread holding that lock and then wait for it forever. Both
halves are measurable: with a thread that takes `HeapLock(GetProcessHeap())` 15 ms into the pass (60 parked threads,
lock never released), the reviewed implementation stopped dead 28 reports in, while this one finishes all of them
(`considered 61, written 61`) — and with the same lock taken *before* the pass starts, both hang at the first
`CreateFileW`, which is what proves the lock is the object being waited on. `port/src/main.cpp` follows the same rule
for the wait-state sampler ("allocated before any thread is suspended" / "everything below runs with every thread
resumed").

Opening them "all at once" rather than in batches is part of that same finding, and it was learnt the hard way. A
revision that opened 32 files per batch — arguing that the batches are serialised, so each open still happens with
nothing of the pass parked — regressed the probe: **3 of 3 runs hung** at 33 files, inside the second batch's
`CreateFileW`, with no thread of the pass suspended. The same probe with one burst finished 3 of 3 runs, `considered 61,
written 61`, 62 files in 293–343 ms. Serialisation bounds *when* the later burst runs; it does not stop the later burst
from running after 32 threads have already been through the suspend window, which is exactly when a lock taker gets its
chance. The frame this costs (2 KiB of handles instead of 256 B) is paid out of `kStackGuaranteeBytes`; a hang is not
payable at any frame size.

The case the first measurement left open is the one where the lock's holder is itself suspended by the pass. A probe
that builds it (a thread takes `HeapLock`, publishes a counter in a named shared
section so a parent process can tell "suspended" from "running", and is the first thread the pass reaches) separates the
two implementations: with the reviewed `dump_all` it hangs inside the pass with the counter stopped — the holder *is*
suspended, 3 of 12 runs — while the current one either finishes every report (`considered 40, written 40`, 41 non-empty
files) or blocks in a `CreateFileW` that runs with no thread of the pass parked, its counter still moving. `WriteFile`
on an already-open handle needs no heap lock (1 MiB written in 404 µs with the lock held by another thread), which is
what makes the window safe rather than merely shorter.

The `__finally` in step 4 is not decoration either. A `ResumeThread()` written as the next statement after the walk
would be skipped by an SEH fault on the reporting path, because the caller's `__except(EXCEPTION_EXECUTE_HANDLER)`
does not unwind the stack and the process keeps running: one crash would become a live process with a thread nobody can
wake.

`CONTEXT_INTEGER` is not optional. `CONTEXT_CONTROL` gives the `Rip`/`Rsp` the walk starts from; without the integer
group every frame's `rbx..r15` column would read zero, and that column is the reason the report exists (it names the
frame whose call did not preserve a nonvolatile).

### Files

`<directory>\crash_report.<tid>.txt` per thread, and `<directory>\crash_report.threads.txt` as the index (or the name
passed as `summary_path`). The index is ASCII like the reports, and it now names the pass that produced them:

```
[rt] crash report: all threads of process 24680, 19 besides the reporting one
pass: exception 0xc0000005
thread 24684: 12 frames
thread 24688: 0 frames
thread 24692: not captured (OpenThread failed)
considered 19, written 18, skipped 1, truncated 0
```

A truncated pass says so in the index instead of silently counting the threads it left out:

```
[rt] crash report: all threads of process 24680, 300 besides the reporting one
pass: exception 0xc0000005
44 thread(s) past the cap of 256 are not in this pass
```

`written + skipped + truncated == considered` holds for every pass, and `considered` is every thread the snapshot named
besides the caller.

The per-thread frame count is the index's only job; *why* a walk stopped is in that thread's own report, where the
walker already wrote it. Summarising it in the index would mean walking twice. A failure is reported as the one thing
that actually failed — `OpenThread failed`, `SuspendThread failed`, `GetThreadContext failed`, `report file could not
be created`, `the report path would be longer than 511 characters`, `the report file rejected the write` — rather than
one sentence that covers all of them and names the wrong one.

### Allocation

The ids, the handles and the per-thread open state live in fixed stack arrays and every path and line buffer is a
fixed local, so the pass performs no C++ allocation — `crash_unwind_test` counts `operator new` across `dump_all` and
requires zero. It does call `CreateToolhelp32Snapshot`, `OpenThread`, `CreateFileW` and `WriteFile`, whose kernel-side
allocations are unavoidable for enumerating threads and writing files.

That promise is about `dump_all` alone. The path it is wired into is not allocation-free: `shims/startup_trace.h`
builds its JSON `exception` event out of `std::string` before it reaches the report code. "The crash-report path does
not allocate" is therefore false for the trace, and the zero-allocation assertion in the test is not evidence about
it.

### Stack

Both halves of the report run on the stack of the thread that faulted, so their frames are part of what a
`STATUS_STACK_OVERFLOW` leaves them. Measured with `dumpbin /disasm:nobytes` on the real object
(`port/build/crash_unwind_test.dir/RelWithDebInfo/crash_unwind.obj`, MSVC 14.44.35207, x64) — the immediate before
`__chkstk`, or the `sub rsp` when the frame is under a page. Two numbers per function: the local frame (`sub rsp`), and
how far `RSP` really drops once the prologue's `push`es are counted:

| function | locals now | with its pushes | 32-file batching round | last reviewed round |
|---|---|---|---|---|
| `dump_all` | **5 504 B** (`mov eax,1580h` + `__chkstk`) | 5 560 B (7 pushes) | 3 744 B (`sub rsp,0EA0h`) | 4 456 B (`mov eax,1168h`) |
| `walk` | 2 032 B (`sub rsp,7F0h`) | 2 088 B (7 pushes) | 2 032 B | 3 264 B |
| `dump_to_handle` | 424 B (`sub rsp,1A8h`) | 456 B (4 pushes) | 424 B | — |
| `dump` | 80 B (`sub rsp,50h`) | 88 B (1 push) | 80 B | 400 B |
| `write_frame` | 624 B (`sub rsp,270h`) | 632 B (1 push) | 624 B | 624 B |
| `appendf` | 80 B (`sub rsp,50h`) | 104 B (3 pushes) | 80 B | 80 B |

Peak chain `dump_all → dump_to_handle → walk → write_frame → appendf → vsnprintf` is **8 664 B** of local frames,
**8 840 B** counting the pushes (6 904 B in the batching round, 10 152 B when the deadlock fix first added a 256-slot
handle array, 8 824 B in the reviewed build). 1 760 B of that is the batching round being undone: 32 handles (256 B)
went back to 256 handles (2 KiB) — see the ordering note above for the probe that made that trade obvious. Dropping the
second `CONTEXT` copy in `walk` (1 232 B) and the second `wchar_t[512]` path buffer (1 KiB) are still in place; they
were never the problem.

Two things keep that chain usable after a stack overflow, and both were measured:

1. `dump_all` asks for a **128 KiB** stack guarantee at its entrance (`SetThreadStackGuarantee`, `kStackGuaranteeBytes`),
   and `startup_trace.h` asks for the same when the trace is installed, because the guarantee is per thread. A guarantee
   is not a reserve: it grows no stack, repairs nothing and cannot move the stack limit — it only decides how much of the
   stack stays usable to the handler once the overflow has happened.
2. 128 KiB is chosen against the measured peak chain, not against a measured survival threshold. With a real overflow
   (recursion on a 1 MiB stack, first-chance vectored handler, the shape `startup_trace.h` installs) the pass does run to
   the end and writes all its reports, but the smallest guarantee at which it still does depends on the binary and on how
   the stack was exhausted — separate runs of the same probe gave different boundaries, so no figure is claimed here.
   What matters for this change is that the guarantee is generous against the chain (8 664 B, 8 840 B counting the
   pushed registers) rather than shaved to the frame.

What the guarantee does not cover: a thread that faults before it ever reaches `dump_all` needs its own guarantee
(hence the call in `startup_trace.h`), and no guarantee helps a stack that is already exhausted past it.

### What it measured (synthetic fixture, no game files)

`crash_unwind_test` parks one thread inside the image's own code page (`EB FE`, `jmp $`, in a page made
`PAGE_EXECUTE_READWRITE`) and one in a kernel wait, then reports both:

```
dump_all: considered 5, written 5, skipped 0, truncated 0
dump_all allocation: 0 operator new call(s) over 4 thread(s)
stale report: considered 4, written 3, skipped 1, truncated 0
```

The spinning thread's report names an image frame (`image+0x...`); the waiting thread's report ends on a `walk stopped:`
line. The reporting thread itself is absent from the directory, which is the point: it is the handler, not a suspect.

The same test asks each parked thread `ResumeThread()` after the pass and requires the count it reports to be 0, with
the probe calibrated first by taking a suspension and requiring it to be seen. Deleting the `ResumeThread()` inside
`dump_all` fails two checks; without that calibration the assertion would also pass for a pass that never suspended
anything.

The third line is the stale-report case: a read-only report left at the same name by an earlier pass must be counted
as `skipped (report file could not be created)`, not as `written`, and the file itself must still hold the earlier
content. The reviewed code asked the filesystem `GetFileAttributesW` instead and produced an index that said `0 frames`
next to a file holding three.

## Limits

- **One image.** Only the registered image is unwound. A frame in another mapped module (for example the
  `Bink2Console.dll` the title loads) or in a host shim ends the walk with `the next return address is outside the
  image` (or `the faulting PC is outside the image`); frames above that point are not named. That is deliberate: the
  question the report answers is which *guest* frame set a register.
- **`.pdata` is required per frame.** A leaf routine with no exception-directory entry ends the walk with
  `the image's .pdata has no entry for the frame`; the frame itself is still printed with its RVA. The same happens
  for a recycled or unsorted table because the lookup is a binary search.
- **The `.pdata` must be the mapped copy.** `RUNTIME_FUNCTION::UnwindInfoAddress` is an RVA, so the table has to
  describe the image as mapped at the address passed to `set_image`.
- **A corrupt stack can only be bounded, not repaired.** The walk stops at 64 frames, on a repeated control PC, on a
  stack pointer that is unreadable or does not advance, and on unwind info that falls outside the image, has an
  unknown version, or chains too deeply. The read that cannot be checked in advance — `RtlVirtualUnwind` following
  the unwind data — is inside `__try`/`__except`, so an access violation there is reported as
  `RtlVirtualUnwind faulted while reading the unwind info` and the frames before it stay on disk. What the report
  cannot save is a fault that repeats inside the report itself (a double fault). A stack overflow no longer belongs to
  that group: the pass measured its own peak chain against the guarantee it asks for (see **Stack** above), so a report
  of a stack overflow is exactly what the guarantee is spent on.
- **No heap, no exceptions, no formatting library on the path.** Every buffer is a fixed-size stack local and the
  report is streamed one frame at a time, so a 64-frame walk allocates nothing; the test counts `operator new` calls
  around `dump` and requires zero. The only allocations are inside the kernel calls that open and write the file
  (`CreateFileW`/`WriteFile`), which are unavoidable for a file report and happen outside this module's code.
- **RVAs, not names.** The report cannot name a routine beyond its begin RVA because the image has no symbol table;
  a symbol map can be applied offline.
- **Not re-entrant per call.** Two threads faulting at once write two interleaved reports if they pass the same path;
  give each thread its own path, or call `dump` from a handler that serialises.
- **The every-thread pass has six limits of its own.** A process with more than `kMaxDumpedThreads` (256) threads gets
  256 reports; the rest are counted in `truncated` and named in the index. A thread created after the snapshot is not
  included. A thread inside a kernel transition reports its last user-mode frame, not the kernel state it is in. Only
  the thread whose window is open is suspended — the pass never stops the whole process, which is exactly why nothing
  inside the window may wait on a lock another thread has to release. The directory must exist and the per-thread path
  must fit in 511 characters (`<directory>` plus `\crash_report.<tid>.txt`); otherwise the file is not created and the
  index says so. A crash report describes the moment it ran; none of these is a reason to re-run it.
- **The pass does not decide for you whether the process can be reported.** It is a diagnostic for a fault, not a
  sampling profiler: a title that stops making progress without a fault does not reach an exception handler at all,
  so `PALMETTO_CRASH_REPORT_ALL` produces nothing until a code with the SEVERE bit faults. Attaching to a live hang is a
  separate tool.
