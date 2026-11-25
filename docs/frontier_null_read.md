# The null after the read repair: the cached stream, the file-range read, and the frontier it opens

**Status: measured, implemented, tested, measured again; the stop is gone and the boot runs its frame loop to the
130 s timeout.** The slice before this one removed the silent `ReadFile` retry loop and the boot then stopped at
`game+0x259393a` on a read of address 0 ([readfile_first_touch.md](readfile_first_touch.md), section 5). This slice
reverses that stop with static analysis and two in-process diagnostic boots, names the zero: a **file object the
title's own handle table still holds, whose cached stream its open path never attached** - and names the runtime
failure that kept the cache empty: a **DirectStorage file-range read into first-touch title memory**, the class the
earlier page measured as negative only because the loader was stalled. The repair is the same first-touch primitive
the file shim already uses, shared through an include-only header and applied to `read_file_range`; the A/B boot now
runs 130 s with **0 exceptions**, no DirectStorage entry failure, 400 loader begin/end hook lines, 8,421 submits and
4,210 presents by the +120 s marker. The presented image is still **black**: the frontier is now a repeating,
non-fatal `0x80070057` from the console compute-PSO slot.

## 1. Why this stop is worth a slice

The boot that named it (from [readfile_first_touch.md](readfile_first_touch.md)) stops 1.1-1.3 s after the repaired
read with an access violation reading address 0, at `game+0x259393a`, stack `game+0x259393a` / `game+0x2592d69` /
`game+0x2215eca` / a host thread start, exit `0xC0000005`. It reproduces in every loader-resumed boot of this tree (two
in the earlier A/B, one fresh diagnostic boot here). A stop that is the title's own named code site - not a stalled
kernel wait - is a place to measure what the runtime handed it, because the runtime now runs the loader far enough to
ask for something new.

## 2. Measured shapes

### 2.1 The stop, instruction by instruction

One fresh boot with the standard environment plus `PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1`,
`PALMETTO_FAULT_MEM_RVA=98d4b28,8fdce28,4abcd1f8,7bc7f78` and `PALMETTO_CRASH_REPORT` gives the fault context:

| Field | Measurement |
|---|---|
| fault RIP | `game+0x259393a`, instruction `mov rax, qword ptr [rax]` |
| rax / rsi | 0 / 0 - the instruction loads a vtable through a null object pointer |
| rcx, rdx | image RVA `0x5ebdac0` (a 5-byte function: `mov rax,[rcx+0x58]; ret`) |
| r8 | 0xe4 |
| r14 | the request entry the batch loop was processing |
| stack | `game+0x259393a` / `game+0x2592d69` / `game+0x2215eca` / host thread start |

The containing functions from the image's exception table (`.pdata`): `0x2593560-0x25944ea` (the fault is at
`+0x3da`), `0x2592c50-0x259344e` (the caller, at `+0x119`), `0x2215ea0-0x2215ee7` (the thread entry, at `+0x2a`). The
thread entry calls a function pointer stored in its argument; the batch function walks a pointer array and calls the
per-request state machine `0x2593560` once per element. `0x2593560` is a 16-state machine on `request[2] & 0x1f`; this
request is in state 0.

### 2.2 The request entry

`r14` at the fault reads as a small record: `{0x0004d86e, 0x00e40001, 0x000ff8e0, 0x08000001, ...}` - a handle
(0x0004d86e), a table key (0x00e40001), a service id whose low five bits are the state (0x000ff8e0, state 0) and a
flag word. State 0 validates the handle through the runtime's generic handle manager and, when that fails, resolves
the key through the object table:

1. `0x4e70150` tests the handle against a range singleton at image RVA `0x8fdce30` (`+8` base, `+0x10` size).
2. `0x1292b0` returns a handle type; type `0x10` takes a different path.
3. `0x4efc8e0` tests one bit of the allocation bitmap whose base is the image global at RVA `0xabcc5b0`
   (`bt [base + (handle>>5)*4], handle`).

All three said no for the measured handle, so the code falls through to the key lookup, which is where the fault is.

### 2.3 The key lookup: a 1000-slot object table

`0x4efc9f0(key)` is three instructions' worth of policy:

```text
entry = table[key >> 16]                       # 0x4efca30
if entry == null: return null
if entry->vtable[0x78] != 0x5ebdac0: return entry->vtable[0x78](entry)   # tail call
return entry[+0x58]
```

The table itself is a 0x30-byte object at image RVA `0x98d4b30` (a global pointer): a `u16` count of 1000 at `+0`,
and two 1000-entry pointer arrays at `+0x10` and `+0x20`; `0x4efca30` indexes `+0x10` when `key >> 16 < 1000` and
`+0x20` with the index reduced by 1000 above that (the second array exists for a grown table; the count is 1000 here,
so the first array is the one in use). The fault's key is 0x00e40001, so the entry was slot 0xe4 = 228.

Entry 228 was **not null**: the fault executed the `+0x58` branch, and the register state (`rcx`/`rdx` equal to the
5-byte default implementation at RVA `0x5ebdac0`, whose bytes are on record in the fault-memory dump) is that branch's
signature. The entry's **`+0x58` field was zero**, and the caller used the returned pointer without a null test.

### 2.4 What the table holds, and who writes `+0x58`

The table has exactly one direct registrar, `0x55b2550`: it scans the table's first array from index 1 and
`lock cmpxchg`s the object into the first zero slot, returning the index (and `int3` + `0xffff` when full). Its only
direct caller is the constructor at `0xfc2240`, which builds a **0x60-byte virtual-file object**:

| Offset | Field |
|---|---|
| `+0x00` | vtable at image RVA `0x62fd890` |
| `+0x08`, `+0x09` | byte flags |
| `+0x0a` | `0xffff0000` |
| `+0x0c` | table index (the registrar's return, written after registration) |
| `+0x0e` | 16-bit generation |
| `+0x10` | the file-system interface the open resolved |
| `+0x18` | the file handle the open returned (`-1` = open failed) |
| `+0x28` | a size/offset the open resolved |
| `+0x30` | a size the open resolved |
| `+0x38` | the path string |
| `+0x47` | a flag byte |
| `+0x58` | the cached stream object |

The key the lookup uses is `(index << 16) | generation` (the measured 0x00e40001 is index 0xe4, generation 1). The
class's other virtual methods are `CreateFileW` (slot 0x278, `0x28c8220`), the title's `ReadFile` helper (slot 0x2b0,
`0x314200`), `WriteFile` (slot 0x2b8, `0x28c83a0`) and `CloseHandle` (slot 0x2e0, `0x28c8400`): the table is the
engine's file-handle table and the entry is the file object for one opened file.

The constructor zeroes `+0x58` and **no code in the class writes it afterwards**. The writer is the class's second
virtual method, `0xfc2490` (the open/attach call): it constructs an **0x80-byte stream object** over the file
(constructor `0xfc2540`, vtable at image RVA `0x62fd480`), asks it to open (slot `0x3a0`), and - only when its sixth
argument, the cache flag, is non-zero - stores that stream at `file+0x58`:

```text
stream = new(0x80); stream->construct(file, ...)
if (!stream->vtable[0x3a0](stream)) { out = 0; delete stream; }
else { if (cache_flag) file[+0x58] = stream;         # the +0x58 writer
       out = stream; }
```

So the zero the lookup returns is the **title's own cache miss**: the request's key names a file object that was
opened without a cached stream (or whose stream open failed), and the state machine that consumes `entry[+0x58]`
assumes the cache is always filled. Our runtime does not create title objects and cannot write this field; what it can
do is keep the *stream open* from failing.

### 2.5 The runtime failure that kept the cache empty: a file-range read into first-touch memory

The earlier page predicted this exact re-opening: [readfile_first_touch.md](readfile_first_touch.md), section 2.4,
measured the DirectStorage destination-page class as **negative** only because in the stalled boot every request was
`dest-pages=0` and no read failed, and section 6 named the falsifier - "a later request shape whose destination is
page-mapped re-opens it; the `read_error` mapping already names what it would look like". With the loader resumed,
that is what the boots measure:

* every loader-resumed boot logs **one** `[rt] dstorage: an entry failed with 0x89240009` line (command type 0),
  before this slice;
* `0x89240009` is `E_DSTORAGE_ACCESS_VIOLATION`, and `read_file_range` in
  [`port/shims/dstorage.cpp`](../port/shims/dstorage.cpp) produces it from `ERROR_NOACCESS` (998) or
  `ERROR_INVALID_USER_BUFFER` (1784) - the two errors a kernel-mode copy into a page that is committed only on first
  touch reports. Before this slice the shim returned that error without touching the destination pages, exactly the
  class the file shim's `ReadFile` repair already removes.

A stream whose file-range read fails is a stream that does not come up; a file whose stream never came up carries the
zero at `+0x58` that the batch consumer later dereferences. The timing agrees: the pre-fix diagnostic boot logs the
failed entry and, 1.5 ms before the fatal fault, an `int3_skipped` at `game+0x23d8f03` (an assert on a failed
cache/insert path) that no post-fix boot logs.

## 3. What we implemented

One include-only header, one repair in the DirectStorage read path, and the file shim's existing repair moved onto the
same primitive. No new target, no CMakeLists change, no change to xmem, main.cpp or another agent's files.

* **[`port/shims/first_touch.h`](../port/shims/first_touch.h)** - the shared primitive, stated once:
  `rt::first_touch::commit_range(buffer, size)` reads one byte of every 4 KiB page of the destination that is not
  committed yet from user mode (a `VirtualQuery`-filtered walk; a page whose touch faults is swallowed so the walk
  continues), with `kPageLimit` = 262,144 pages = 1 GiB per attempt, and no C++ objects in the `__try` frame; and
  `is_kernel_write_blocked(error)` for the two errors above. It exists so the two shims that copy into title memory
  cannot drift apart in page size, bound or gating.
* **[`port/shims/dstorage.cpp`](../port/shims/dstorage.cpp)** - `read_file_range` now repairs the class it already
  mapped to `E_DSTORAGE_ACCESS_VIOLATION`: on a first-touch error, commit the destination pages of the pending chunk
  with the shared primitive and re-issue that chunk **once**; a destination whose retry failed is remembered
  (one atomic pair) and returns the host's error unchanged instead of being walked again. The measurement is bounded
  and named: one stderr line per run plus the `dstorage_read_noaccess` startup-trace event (API, error, size, pages,
  repaired, count, caller). Off the error path the function pays one branch; a successful read is byte-for-byte the
  previous behaviour.
* **[`port/shims/file_system.cpp`](../port/shims/file_system.cpp)** - the `ReadFile` repair
  (RT-READFILE-NOACCESS) now calls the shared primitive. Its gating (`ERROR_NOACCESS` only, the measured A/B), one
  retry, failed-destination memo, trace event and stderr line are unchanged.

The repair is **default on and error-gated**: it can only change a call the host already failed, and the A/B below
shows no regression in a successful read. Deliberately not done: no destination-page resolver (the DirectStorage
page-list path still refuses by name in [directstorage_advanced.md](directstorage_advanced.md)), no repair for
asynchronous/overlapped copies that are not measured, and no attempt to write title-side state.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`. The commit consulted is
`779f901084774418f1f83d55c6d7fa075e71e98b` (`fs, dstorage: a ReadFile into first-touch title memory commits the pages
and reads again (RT-READFILE-NOACCESS)`, Tyrannys, 2026-11-24, `Co-Authored-By: Claude Opus 5.5`), touching
`runtime/shims/file_system.cpp`, `runtime/shims/dstorage.cpp` and `runtime/test/file_system_test.cpp`.

| Topic | Reference | This measurement | Why |
|---|---|---|---|
| Class | The same commit repairs `read_file_range` because reads into title memory came back `E_DSTORAGE_ACCESS_VIOLATION` | Our tree measured the class **negative** before the read repair and **positive** after it: one failed entry (`0x89240009`) in every loader-resumed boot, two repaired reads after the fix | **Agree once the loader runs.** The earlier page's "differ: not implemented" was a measurement of the stalled boot, not a disagreement about the mechanism |
| Mechanism | Touch every 4 KiB destination page on the kernel-write error, then read again once | Our own primitive: `VirtualQuery`-filtered walk, 1 GiB per-attempt cap, failed-destination memo, bounded named trace | **Agree on the mechanism; differs in structure and bounds.** The primitive is ours and shared by both shims; the reference's code and text were not copied |
| Structure | Repair lives inside the reference's file and dstorage shims | One include-only header used by both of ours, so the two walks cannot drift | **Differ by design**, and the difference is the point of the header |
| Result | Its commit body reports the title stopped before its first frame loop; later fork work reached the story load | The pre-fix boot stopped at `game+0x259393a`; the post-fix boot runs the frame loop to the 130 s timeout with 0 exceptions, presents black frames, and moves the frontier to a non-fatal compute-PSO failure | **Agree on direction, differ where the boot is**: our tree still presents black, so this slice clears a stop and does not reach content |

The reference remains the copyright of its contributors and its licence continues to apply to its own tree.

## 5. A/B measurement

Two 130 s boots of this repository under the boot lock, one at a time, through `tooling/boot_report.py` from
`D:\leonida-work\nullread\` (outside the repository). The environment is the standard one - `PALMETTO_SHADERS`,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8` - plus the measurement switches
`PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DS_STACK=1`,
`PALMETTO_DS_ARRAYS=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_FILE_TRACE=1`, and (A only) the fault dump
(`PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1`, `PALMETTO_FAULT_MEM_RVA=...`, `PALMETTO_CRASH_REPORT`); B adds the
presenter captures (`PALMETTO_CAPTURE`, every 2,000 frames, at most 8).

* **A (before)**: `port/build/palmetto.exe` of the tree of its moment, SHA-256 `7CBFAB84...D835E82A`, built 12:06,
  boot 12:14. This is the pre-fix measurement: it fails.
* **B (after)**: the same target rebuilt with the shared first-touch header and the `read_file_range` repair, SHA-256
  `55DB1C0E...868A5E26`, built 12:31, boot 12:34. `dstorage.cpp`, `first_touch.h`, `file_system.cpp` and the test are
  the files this slice changed; the working tree also carried other agents' in-flight `xgameruntime_*` changes in both
  builds, a different subsystem, so the pair is a real A/B for this class but not a pristine one-file pair.

| | A: before | B: after |
|---|---|---|
| boot | **STOP at 25.6 s**, exit `0xC0000005` | **ALIVE 130.3 s** (killed by the 130 s timeout) |
| startup trace | 863 events, 1 exception | **19,866 events, 0 exceptions** |
| `d3d12-trace` calls | 58,853 | **218,490** |
| DirectStorage entry failure | **1** `0x89240009` (command type 0) | **0** |
| `dstorage_read_noaccess` | - (no repair) | **2**: 998, 752 B at +26.38 s, 1 page, repaired; 998, 65,392 B at +112.9 s, 1 page, repaired |
| `file_read_noaccess` | 1 (271,044,608 B, 4,136 pages, repaired) | 1 (same) |
| loader begin/end hook lines | 400 begin / 400 end (the in-flight lines stop at the fault) | 400 / 400, all pairs complete |
| timeline +30 s | - (fault at 25.6 s) | graphics 51,786; compute 249 calls / 160 created; submits 697; presents 348 |
| timeline +120 s | - | graphics 51,786; compute 381 / 226; **submits 8,421; presents 4,210** |
| `int3_skipped` | 5 (the last at `game+0x23d8f03`, 1.5 ms before the fault) | **4** (that site is gone) |
| captures | none (switch off) | 3 (frames 1, 2,001, 4,001), extent 2564x1421, **mean RGB 0.00, 0.00, 0.00** |
| verdict | fatal title null read | **advanced**: the stop is cleared, the loader and frame loop run; the image is still black |

The two repaired reads are the class the pre-fix boot failed: 752 B at +26.38 s (the moment the pre-fix boot faults
at 25.6 s) and 65,392 B late in the run. The `862` extra trace events are almost all `audio` (19,701) because the
boot lives 100 s longer; no new failure event appears.

### 5.1 The new frontier, named

The boot no longer stops. The next failure is a repeating, non-fatal one in the device-proxy slot 67
(`DeserializeComputePipelineStateX` routed to PC compute-PSO creation): **147 logged `0x80070057`
(`E_INVALIDARG`) failures** (calls 43-189), matching `compute calls 381 created 226` at the last marker - about 155
refused compute PSOs, on the frame loop's path. It does not end the run; it is the next slice's target, not this
page's claim.

## 6. Limits

* **The crash consumer is titled-side code we cannot instrument further without a semantic patch.** We measured the
  null field and the cache-miss chain in the image, and we measured the DirectStorage failure that disappears
  together with the crash - the mechanism matches and the timing matches (the failed entry, the assert 1.5 ms before
  the fault, the two repairs), but no single trace line proves the repaired read is the read that fills this file's
  cache. The falsifier that would close that gap is a title-side watch on `entry+0x58`; a shim cannot write there.
* **The presented image is black.** This slice clears a stop and takes the boot into its frame loop; it does not
  claim content. The pre-fix and post-fix captures are both mean RGB 0.00.
* **One machine, one copy, one Vulkan backend.** Counts and RVAs are for the 1.0.2.1 target image; the A/B pair is
  the tree of its moment (see section 5).
* **The 1 GiB per-attempt page cap and the one-destination memo are bounds, not measurements.** The two observed DS
  destinations are one page each (752 B and 65,392 B); a larger page-mapped destination gets one retry over its first
  1 GiB.
* **Asynchronous copies are not measured.** The repair runs on the immediate error of the synchronous chunk read the
  queue worker makes; no overlapped/async request shape was exercised.
* **No destination-page resolver.** The DirectStorage page-list path still refuses by name; this slice only makes the
  ordinary into-destination read repair itself.
* No game data: this page contains RVAs, structure offsets, counts, Win32/HRESULT codes, byte sizes, frame counts and
  outcomes only - no package path, file name, hash, extracted table or shader byte.

## 7. Tests

[`port/test/file_system_test.cpp`](../port/test/file_system_test.cpp) extends the existing target and exercises the
shared primitive directly on its own reserve-only view and vectored handler (no xmem, no game data, no GPU):
`file_system_test` runs **512 checks, 0 failures** (508 before this slice, 4 added).

| Check | What it proves |
|---|---|
| the shimmed `ReadFile` commits every destination page and reads the whole file | the refactored file repair still works end to end |
| the shared helper commits exactly a destination's pages through the user-mode touch | the primitive performs the walk the file and DirectStorage repairs share |
| a destination whose pages are committed is not walked again | a second call touches nothing |
| the shared error gate accepts the two first-touch errors and nothing else | `ERROR_NOACCESS`/`ERROR_INVALID_USER_BUFFER` only, so a successful read never enters the repair |

No new test target was added: `dstorage.cpp` is not linked by an existing test (`dstorage_test` covers the
`dstorage_advanced.h` mechanisms only), and wiring a new one would need a `CMakeLists.txt` change, which is outside
this slice's write scope. The DirectStorage half of the repair is covered by the shared primitive's checks plus the
two repaired reads the boot measures.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, licence
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/shims/file_system.cpp`,
`runtime/shims/dstorage.cpp` and `runtime/test/file_system_test.cpp` at commit `779f9010847744`
(`RT-READFILE-NOACCESS`) by Tyrannys (2026-11-24; `Co-Authored-By: Claude Opus 5.5`); fork HEAD when this page was
written `ed94fbef9888`. No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) also puts this sentence in
[NOTICE.md](../NOTICE.md). That file is outside this slice's write scope, so the lead adds the entry there; the
sentence above is its content.
