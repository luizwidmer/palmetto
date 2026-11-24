# Crash report: the faulting thread's stack

**Status: implemented and tested on synthetic images; the exception hook itself is not wired yet.** The code is
`port/src/crash_unwind.{h,cpp}` and its test is `port/test/crash_unwind_test.cpp` (CTest name
`crash_unwind`).

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

The integrator adds `src/crash_unwind.cpp` to the `palmetto` target and calls two functions:

```cpp
// Once, after Loader::load() and before guest code runs:
rt::crash_report::set_image(main_module.base, main_module.size,
                            static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(main_module.function_table),
                            main_module.function_count);

// From the exception handler, for the faulting thread's CONTEXT:
rt::crash_report::dump(context, L"<work-dir>\\crash_report.txt");
```

- `set_image` is also the reset: calling it with no image unregisters the previous one. The pointers must stay valid
  for the process lifetime; they live in the mapped image (`Module::function_table` / `Module::function_count`, the
  same table `Loader::load` registers with `RtlAddFunctionTable`).
- `dump(context, path, /*user_data=*/nullptr)` creates or overwrites `path` (UTF-16) and returns the number of frames
  written, 0 if there was nothing to walk or the file could not be created. A null `path` writes to stderr instead.
  `user_data` is reserved and never read.
- The exception handler is the caller's: a vectored handler is fine as long as it does not return
  `EXCEPTION_CONTINUE_EXECUTION` past the report, and a handler that runs more than once overwrites the file each
  time. The report does not include the exception code or address; the startup trace's `exception` event already
  logs those, and the two files can be cross-read.

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
  cannot save is a fault that repeats inside the report itself (a stack overflow or double fault): the walker runs on
  the faulted thread's own stack.
- **No heap, no exceptions, no formatting library on the path.** Every buffer is a fixed-size stack local and the
  report is streamed one frame at a time, so a 64-frame walk allocates nothing; the test counts `operator new` calls
  around `dump` and requires zero. The only allocations are inside the kernel calls that open and write the file
  (`CreateFileW`/`WriteFile`), which are unavoidable for a file report and happen outside this module's code.
- **RVAs, not names.** The report cannot name a routine beyond its begin RVA because the image has no symbol table;
  a symbol map can be applied offline.
- **Not re-entrant per call.** Two threads faulting at once write two interleaved reports if they pass the same path;
  give each thread its own path, or call `dump` from a handler that serialises.
