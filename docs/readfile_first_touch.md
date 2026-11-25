# The silent file-reader retry loop: `ReadFile` into first-touch title memory

**Status: measured, implemented, tested, measured again; target proven, boot advanced to a new stop.** The retail
title's own loader reads package data straight into working buffers that `xmem` hands out as views of a reserve-only
section; a page is committed only when user-mode code first touches it. A kernel-mode copy raises no such fault, so
`ReadFile` into an untouched page fails with `ERROR_NOACCESS` (998) - and the title's read loop treats that as
transient and retries forever. This slice measures that failure in our own boot, implements our own repair in
[`port/shims/file_system.cpp`](../port/shims/file_system.cpp) (on the error, touch one byte of every uncommitted
destination page from user mode, then retry the read once, bounded and named), and measures the effect: the failing
read at `game+0x314275` is repaired (4,136 pages committed, all 271,044,608 bytes read), the loader resumes and runs
another 92 begin/end pairs, the DirectStorage request counter grows from 4,199 to 5,123 - and the boot then ends ~9 s
later in the title's own access violation reading address 0 at `game+0x259393a`, with no content presented. The
silent infinite wait is gone; the next stop is a named title code site.

This slice follows [upstream_port.md](upstream_port.md). The public GPL-3.0 fork
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) was the third source, used to cross-check the
mechanism (section 4); the implementation, the test and every number are ours.

## 1. Why this read is the stop

The boot's own 130 s measurement (`boot-a-measure`, the baseline of section 5) shows one sustained failure and nothing
else moving after it:

- **One caller retries one read forever.** `game+0x314275` calls `ReadFile` for 271,044,608 bytes and gets
  `ERROR_NOACCESS` (998) every time. The first failure is at **+25.760 s**, the 68,608th and last logged one at
  **+129.403 s** - 662 failures per second, about 1.51 ms apart, which is the title's `Sleep(1)` plus the call.
- **The loader stops at the same moment.** Its entry detours (`PALMETTO_LOADER_HOOK=1`) record 18 begin/end pairs in
  the whole boot, all before the `+30 s` timeline marker, and none after the read starts; the last line before the stall
  is an `end` with the loader armed and in-flight 0, and no `begin` follows it.
- **Everything else is already frozen or is the frame loop.** The pipeline population is complete by `+30 s` (51,786
  graphics PSOs, 95/95 compute), the DirectStorage request stream is 4,199 file requests with 0 failures, and the
  title keeps presenting a black image (7 captures, mean RGB 0.00). The 600 s measurement in
  [long_boot.md](long_boot.md) showed no new activity between 130 s and 600 s, so the retry loop is the last thing the
  loader does, not a pause between batches.

The first read into the destination fails before any byte is copied: the error is about the destination's pages, not
about the file. That is the signature of first-touch memory receiving a kernel-mode write.

## 2. Measured shapes

### 2.1 The import and the callers

The retail executable imports `ReadFile` from `api-ms-win-core-file-l1-1-0.dll`; the loader also binds our shim through
`KERNEL32.dll!ReadFile` (the boot's binding report lists both). Two distinct title call sites call it in the measured
boot (`file_read` events, first call per caller, bounded to 16):

| Caller (module+RVA) | First call | Shape |
|---|---|---|
| `game+0xf43690` | +2.871 s | 4,096 bytes, succeeds and keeps succeeding |
| `game+0x314275` | +2.871 s | 4,096 bytes at first, then the failing 271,044,608-byte read at +25.760 s |

The failing call passes a null `OVERLAPPED` (synchronous read); no title call in the window uses asynchronous I/O.

### 2.2 The failing read

| Field | Measurement |
|---|---|
| size | 271,044,608 bytes (258.5 MiB) |
| error | `ERROR_NOACCESS`, Win32 998 |
| caller | `game+0x314275` |
| first failure | +25.760 s (same read attempted again every ~1.51 ms to +129.403 s) |
| fails before any data | yes - the same destination page is never written, so no byte of the file is copied |

The size is constant across every retry, so the title is not walking a chunked read; it is retrying one whole-buffer
read, which is why the loop is silent: there is no progress to log.

### 2.3 The destination memory

The repair's own diagnostic names where the pages come from. In boot B the first (and only) failure reports

```text
[rt] fs: ReadFile into uncommitted title memory (ERROR_NOACCESS 998) at game_scarlett_master.exe+0x314275;
touched 4136 destination page(s) from user mode and the read then succeeded (271044608 of 271044608 byte(s) read; ...)
```

4,136 committed 64 KiB units for 271,044,608 bytes (271,044,608 / 65,536 = 4,135.5, rounded up) is exactly the shape
of the `xmem` physical-page windows: `XMemAllocatePhysicalPages` is measured at +2.936 s (11,360 pages, attributes
0x280) and +6.695 s (99,776 pages, attributes 0x380), each 64 KiB page backed by a private reserve-only section, and
`port/shims/xmem.cpp` commits a page from the vectored handler `physical_page_fault` only when user-mode code touches
it. An earlier 130 s boot with `PALMETTO_STREAM_CONSUMER=1` shows the same class: the title maps almost 100,000
physical pages before the load window. The retry loop is therefore exactly the documented first-touch/kernel-write
interaction, measured on our tree with our switches.

### 2.4 The DirectStorage read path: measured negative

The same class can surface in the shim's DirectStorage file reads, and the reference reports it as
`E_DSTORAGE_ACCESS_VIOLATION` there. Our tree does not show it in any measured boot:

| Boot | DirectStorage request shapes | Page-list destination requests | Read failures |
|---|---|---|---|
| `boot-a-measure`, 130 s | `file=4199 memory=0 zlib=4199`, `refusals=0` | `dest-pages=0 source-pages=0` | 0 failure lines, 0 exceptions |
| 600 s measurement ([long_boot.md](long_boot.md)) | `file=4199 zlib=4199`, all enqueued before +7.43 s | `dest-pages=0` | 0 `[rt] dstorage` failure lines |

`read_file_range` has no other source of `ERROR_NOACCESS`, and `read_error` already turns one into the named
`E_DSTORAGE_ACCESS_VIOLATION`; with zero failures observed, this slice leaves
[`port/shims/dstorage.cpp`](../port/shims/dstorage.cpp) unchanged. The class stays a latent hazard, not a measured one,
and that is what a later request-shape change would re-open.

## 3. What we implemented

One function per concern in `port/shims/file_system.cpp`, plus the registry entry that binds the title's import to it;
no other runtime file changed.

- **`read_file`, served from `kernel32.dll`, `kernelbase.dll` and the `api-ms-win-core-file-l1-1-0/1-2-0/l2-1-0`
  sets** (the same table shape every file API already uses). It calls the host `ReadFile` first. On success it returns
  the host's answer untouched.
- **The repair is error-gated.** Only when the host reports `ERROR_NOACCESS` and the buffer is non-null and non-empty
  does it walk the destination. The successful path pays one branch and, behind the existing `PALMETTO_FILE_TRACE`
  switch, the first-caller diagnostic; no switch changes a successful call.
- **The page walk**: for each 4 KiB page of the destination, `VirtualQuery` skips pages already committed; an
  uncommitted page gets one volatile byte read from user mode, which runs the first-touch handler that commits it, and
  a read that raises an access violation is swallowed so the walk continues (`__try`/`__except` with no C++ objects in
  the frame). Then the read is re-issued **once** - never a loop.
- **Bounds, in code and in the log**: one retry only; at most `kReadTouchPageLimit` = 262,144 pages (1 GiB) walked per
  attempt; a destination whose retry also failed is remembered
  (`g_repair_failed_buffer`/`g_repair_failed_size`) so a title retrying that same read forever is answered from memory
  instead of being walked again; the startup-trace event (`file_read_noaccess`: API, error, size, pages touched, bytes
  read, repaired, count, caller) is bounded to the first 256 failures and then one in every 1,024; one stderr line per
  run names the first repaired read. A second failure returns the host's error unchanged.
- **Deliberately not done**: no `Sleep`, no title-side state, no change to `dstorage.cpp` (section 2.4), no change to
  `xmem` (its commit helper is not needed - the user-mode touch is the same path the title's own code takes).

The comment block next to the code states these facts, including the attribution to the reference commit that pointed
at the behaviour.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`. The commit consulted is
`779f901084774418f1f83d55c6d7fa075e71e98b` (`fs, dstorage: a ReadFile into first-touch title memory commits the pages
and reads again (RT-READFILE-NOACCESS)`, Tyrannys, 2026-11-24, `Co-Authored-By: Claude Opus 5.5`), touching
`runtime/shims/file_system.cpp`, `runtime/shims/dstorage.cpp` and `runtime/test/file_system_test.cpp`. The fork's HEAD
when this page was written was `ed94fbef9888`.

| Topic | Reference | This measurement | Why |
|---|---|---|---|
| Mechanism | A kernel write into a page of a reserve-only section raises no user-mode fault; `ReadFile` fails with 998 and the title's loop treats it as transient and retries after `Sleep(1)` | The same failure is measured on our tree: 998, same retry cadence (1.51 ms), one caller, loader dormant, black presents | **Agree**, and this is the first independent confirmation on our tree that the reference's description is the observed signature, not its title's alone |
| Repair | Touch every 4 KiB page of the destination on 998, then read again once | Same mechanism, our own implementation: `VirtualQuery`-filtered walk, 1 GiB per-attempt cap, failed-destination memo, bounded named trace, one stderr line | **Agree on the mechanism; differs in bounds and logging.** Ours never re-walks a destination whose repair failed and never lets a title retrying forever fill the trace |
| DirectStorage | The same commit repairs `read_file_range` because reads into the same memory came back `E_DSTORAGE_ACCESS_VIOLATION` | Our boot measures 4,199 file requests, `dest-pages=0`, zero failures (130 s and 600 s) | **Differ: not implemented here.** Our DirectStorage destinations are not page-mapped in the measured window, so the task's scope rule (repair it only if proven) keeps `dstorage.cpp` unchanged; the reference's fix stays its own tree's |
| Test | Modifies `runtime/test/file_system_test.cpp` with a reserve-only section and a first-touch handler | [`port/test/file_system_test.cpp`](../port/test/file_system_test.cpp) extends the existing target the same way, written independently (different fixture, sizes, checks; 12 new checks) | **Agree on the shape.** A reserve-only section with a committing vectored handler is the only way to reproduce a first-touch kernel-write failure without `xmem` |
| Result | Its commit body reports the title stopped before its first frame loop; later fork work reached the story load | The loop is real (68,608 failures) but repairing it does not reach content here: the loader resumes and the boot ends at `game+0x259393a` (section 5) | **Differ where the boot is, not in the shim.** Our tree carries a different set of later gaps; the reference reached content after more slices than this one, and none of them are in this diff |

The disagreement recorded on the policy's behalf: the reference's account makes the ReadFile loop *the* pre-content
stop; on our tree it is a real and now-removed stop, and the next code path behind it faults on a null pointer in the
title - so the loop was *a* gate, not the last one.

## 5. A/B measurement

Two 130 s boots of this repository under the boot lock, one at a time, through `tooling/boot_report.py --timeout 130`,
from `D:\leonida-work\readfile\` (outside the repository). The standard environment is `PALMETTO_SHADERS`,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, plus the measurement switches
`PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DS_STACK=1`,
`PALMETTO_DS_ARRAYS=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_FILE_TRACE=1`, and presenter captures
(`PALMETTO_CAPTURE`, every 2,000 frames, at most 8).

- **A** is the measurement build: `port/build/palmetto.exe` built with the `ReadFile` shim in place but with only the
  counted diagnostic on the error path (no repair), SHA-256 `B6A0CE9799A944B6A1C55F72F86A21EAAF318C4E573875722716CCC566FFF47B`, 11:45 local time. It measures the
  failure without changing the host's answer, so it is the "before" arm.
- **B** is the same target rebuilt with the repair, SHA-256 `3AD8C875F7363D1BCFF14DF5D2460E9104BE4EE6F5C9F1A28BD3F28BD974E925`, 11:52 local time. `file_system.cpp` plus
  the test are the only files that differ from A; the repair changes nothing on a call that does not fail.
- A third boot, **B2**, is the build with the bytes-read field added, SHA-256
  `A77796599D78779C9B8B214A7F4A599A60B35F196EBD59AE409653D9EA982149`, 11:55 local time; it reproduces B and supplies
  the full-byte count.

| | A: before (no repair) | B: after (repair) |
|---|---|---|
| boot | ALIVE, killed by the 130 s timeout | STOP at 37.2 s; process exit code `0xC0000005` |
| startup trace | 19,959 events, 0 exceptions | 944 events, 1 exception (first-chance, logged +27.02 s) |
| `file_read_noaccess` | 323 bounded records; counter **68,608** | **1** record: repaired, 4,136 pages, 271,044,608 bytes requested |
| failing read | `game+0x314275`, 271,044,608 bytes, every ~1.51 ms from +25.760 s to +129.403 s | same caller and size at +26.048 s; retry returns TRUE, and B2 records **271,044,608 of 271,044,608 bytes read** |
| loader begin/end hook pairs | 18 / 18; none after the read starts; no loader activity after ~+26 s | 111 / 111; **184 of 222 hook line records after the repair line**, in-flight up to 210 in that tail (B2: 182 after) |
| DirectStorage shapes | `file=4199 zlib=4199`, `refusals=0` | `file=5123 zlib=5002`, `refusals=0` |
| pipeline population | 51,786 graphics, 95/95 compute by +30 s, frozen after | 21,907 graphics at the last `+20 s` marker (the boot ends before `+30`), 95/95 compute |
| presents/captures | presented frames past 12,000; 7 captures, all mean RGB 0.00 | 1 capture (frame 1), mean RGB 0.00 |
| verdict | silent infinite retry loop; loader dormant; black | target repaired and the loader resumes; **new stop**: title AV read of address 0 at `game+0x259393a`, then `0xC0000005` |

The exception B logs is first-chance and the process keeps running for several more seconds (trace events to +35.30 s;
B2 +38.00 s); the fatal exit is the same code path at the same site 1.1-1.3 s after the repaired read (B:
`game+0x259393a`, stack `game+0x259393a` / `game+0x2592d69` / `game+0x2215eca` / a host thread start; B2: identical
site). B2 additionally shows the read completed with the full requested byte count, so the crash is not a short read
and not zero-filled tail data: the title received the whole file and then dereferenced NULL in its own code.

Two exact notes for reproduction. The `file_read_noaccess` counter is the last logged `count` field: the log keeps the
first 256 failures individually and one per 1,024 after that, so the last field is the true total to within 1,024. The
loader hook counter is line-based: each pair prints a begin record and an end record, so 18/18 is 36 lines and 111/111
is 222 lines, of which 184 follow the `[rt] fs: ...` repair line in `stderr.log`.

## 6. Limits, and the next slice

- **The repair is proven; content is not.** The measured effect stops at the title's access violation at
  `game+0x259393a`. Nothing in this page claims the boot advances past it. A parallel page (the wait-forensics slice)
  measured the loader entry thread's wait in the un-repaired boot and predicted that a repaired read would let that
  thread leave its wait at `game+0x258DBA6`; the stop this slice measures is ~24 KiB further into the same function
  family, which is consistent but is not the same measurement.
- **The fatal exception is not fully attributed.** The vectored tracer logs the first exception it sees (the
  first-chance one at +27.02 s) and the process then exits `0xC0000005` without a second logged exception; the fatal
  fault's own context was not captured in this pair. That is the next falsifier: one boot with a debugger attach at
  the stop, or the runtime's register dump switch on the second-chance fault, naming the null source.
- **The 1 GiB walk cap is a bound, not a measurement.** No measured read exceeds it (the observed one is 258.5 MiB);
  a larger destination gets one retry over its first 1 GiB and then the host error, memoized.
- **Asynchronous reads are not measured.** Both call sites are synchronous; the repair runs on any immediate
  `ERROR_NOACCESS` regardless of `OVERLAPPED`, but no title path exercises the asynchronous form in this window.
- **The DirectStorage half of the reference commit is deliberately absent** because our boot measures the class as
  negative (section 2.4). If a later request shape makes destination pages page-mapped, that is the measurement that
  re-opens it; the `read_error` mapping already names what it would look like.
- **Default on, honestly.** The repair only changes behaviour when the host already failed the read; leaving it off
  means a `ReadFile` that can never succeed is retried forever in silence. The visible consequence of honesty here is
  that the boot now ends at a named title fault instead of idling black - recorded as the result, not hidden.
- No game data: this page contains sizes, counts, page counts, Win32/HRESULT codes, RVAs and outcomes only - no package
  path, file name, hash, extracted table or shader byte.

## 7. Tests

[`port/test/file_system_test.cpp`](../port/test/file_system_test.cpp) extends the existing target; `file_system_test`
runs **508 checks, 0 failures** (496 before this slice, 12 added). The fixture is synthetic and GPU-free: a temporary
file of generated bytes, a `PAGE_READWRITE | SEC_RESERVE` section mapped as a view, and a vectored handler that
commits a page when user code touches it - the same shape `xmem` presents, without `xmem`.

| Check | What it proves |
|---|---|
| `ReadFile` resolves through `api-ms-win-core-file-l1-1-0.dll` and `kernel32.dll` | the import the title uses is served by the shim, same function |
| the reserve-only view is created | the fixture reproduces first-touch memory |
| host `ReadFile` into the untouched view fails with `ERROR_NOACCESS` | the failure is the host's own behaviour, not a shim invention |
| shimmed `ReadFile` succeeds, reads all bytes, bytes match | the repair commits the destination and the retry copies the file |
| the handler counted the committed pages | the commit happened through the user-mode path |
| a committed destination reads the same through host and shim | the success path is unchanged |
| a refused page keeps the host error after one retry | a second failure is returned, not swallowed or looped |
| the handler's fault count is bounded by the page count | one touch per page, no retry loop inside the shim |
| a refused page does not stop the other pages being touched | the walk continues and reports honestly |

No DirectStorage test was added: `dstorage_test.cpp` covers the `dstorage_advanced.h` mechanisms only (its target does
not link `dstorage.cpp`), the shim's read path is unchanged by this slice, and the measured DirectStorage class is
negative. Adding a target would need a `CMakeLists.txt` change, which is outside this slice's write scope.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, license
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/shims/file_system.cpp`,
`runtime/shims/dstorage.cpp` and `runtime/test/file_system_test.cpp` at commit `779f9010847744`
(`RT-READFILE-NOACCESS`) by Tyrannys (2026-11-24; `Co-Authored-By: Claude Opus 5.5`); fork HEAD when this page was
written `ed94fbef9888`. No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) also puts this sentence in
[NOTICE.md](../NOTICE.md). That file is outside this slice's write scope, so the lead adds the entry there; the sentence above is
its content.


