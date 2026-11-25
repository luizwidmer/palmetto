# Story-load probe: one 300 s boot with the title's state words sampled

One 300 s boot under the boot lock, on the tree where the renderer setup returns success, with the standard
measurement layer plus an external `ReadProcessMemory` probe that samples the title's own state words every 2 s.
This page answers one question: now that the silent stall is crossed and the renderer setup succeeds, does the
title leave its init state and reach the story load? It also says exactly what changed against the older
600 s page, [long_boot.md](long_boot.md).

**Short answer: no.** The boot now goes further than the 600 s page measured - the loader's ended pairs go from
19 in 600 s to 302 begin/end pairs by 181 s, DirectStorage request shapes go from 4,199 to 8,272, the title
opens 121 package files where the pre-fix 130 s measurement traced 25 and performs its first repaired streamed
reads at ~27 s, and at 115.1 s it runs its **first user sign-in** (`XUserAddAsync`/`AddResult`/`GetId`/
`GetLocalId`/`DuplicateHandle`/`CloseHandle`, all `S_OK`). But the words the tracker issue measured do not move:
the game/init word at `0x7446510` is **2** from 21.1 s to the last sample at 298.9 s, the session word at
`0x98f828c` is **0** for the whole boot, and the loading word `0x7453ed0` is 1. All four presenter captures are
uniformly black, no DirectStorage request is created after 31.2 s, no file is opened after 35.3 s, no platform
call is made after 115.16 s and no new D3D12 call kind appears after 130 s. The fork's story-load window
(240-300 s) passes with only the stationary frame loop.

## 1. What ran

| | |
|---|---|
| Machine | Windows 11 Pro 10.0.26200, Core i9-13900K (24C/32T), GeForce RTX 4090, driver 32.0.16.1714, 63.7 GiB RAM |
| Tree | `D:\leonida` at `583b91195`, working tree clean at boot time; the only uncommitted file after this slice is this page |
| Binary | `port/build/palmetto.exe` built 2026-11-25 13:18:30, SHA-256 `1864FE173C95B6346ADDD265EF29371EF4EC5092EC15288F35B1DF2FB5DEE7B4` |
| Boot | one boot, started 13:29:37, wall 300.2 s, killed by the harness timeout at 300 s (`taskkill /T /F`), launcher exit code 1 |
| Trace | 51,482 startup-trace events, 0 exceptions, last event 300.00 s; exactly one `int3_skipped` (`game+0x247af2d` at 4.95 s) |
| Probe | external read-only `ReadProcessMemory` at the fixed title image base `0x140000000` + RVA, every 2 s, 153 rows (148 successful) |
| Captures | 4 presenter captures (`frame_00001`, `02801`, `05601`, `08401`), every 2,800 presented frames |
| Boot lock | `_setup/boot_lock.ps1 -Acquire -Who storyprobe`, released after the kill |
| Log size | 33.0 MB stderr, 43.7 MB captures, 4.6 MB startup trace |

Environment: the standard set (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus the measurement switches this task names - `PALMETTO_PIPELINE_TIMELINE=30`,
`PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_DS_ARRAYS=1`,
`PALMETTO_FILE_TRACE=1` - and presenter capture `PALMETTO_CAPTURE` with `PALMETTO_CAPTURE_EVERY=2800`,
`PALMETTO_CAPTURE_LIMIT=8`. No runtime change of any kind; every switch logs only. The boot was launched
directly with the same arguments `tooling/boot_report.py` passes (`--run`, `--game-dir`, `--overlay`, `--trace`,
`--trace-d3d12`), with a 1 Hz process monitor writing the counters beside it.

The external probe is the issue's method: it keeps the `palmetto.exe` process in which the read at the fixed
base succeeds and samples, per row, six title words and three 32-byte windows around them:

| RVA | what it is | sampled as |
|---|---|---|
| `0x7446510` | game/init state word (tracker issue #1: 2 = init, 3 = running) | u32 |
| `0x7446514` | adjacent word, sampled so a companion transition is not missed | u32 |
| `0x98f828c` | session word (tracker issue #1) | u32 |
| `0x98f8290` | adjacent word | u32 |
| `0x7453ed0` | loading word (tracker issue #1) | u32 and low byte |
| `0x7453ed4` | adjacent word | u32 |

It writes nothing, injects nothing and reads no game data: RVAs, integers, hex bytes of the windows, elapsed
seconds and liveness only.

## 2. The per-30 s series

The `PALMETTO_PIPELINE_TIMELINE=30` mark `+N s` is the sampler thread's nominal time; each line lands 3.7-3.9 s
later in boot seconds (measured: `+30` printed at 33.9 s, `+270` at 273.9 s). The table places the counters at
the mark's **print** time and bins the interval counters between successive prints. `d3d12` is the
`[d3d12-trace]` line count in the interval, `files` the startup trace's `file_open` events, `ldr` the printed
loader begin/end calls (the print cap is explained in section 4), `caps` the presenter captures with their mean
channel value.

| mark t | printed at | submits | presents | console | threads | d3d12 | files | ldr prints begin/end | caps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| +30 | 33.9 s | 670 | 334 | 114,019 | 104 | 60,298 | 119 | 200 / 200 | frame_00001 (0.00) |
| +60 | 63.8 s | 3,430 | 1,714 | 134,796 | 100 | 52,447 | 2 | 0 / 0 | - |
| +90 | 93.9 s | 6,039 | 3,019 | 154,421 | 100 | 49,593 | 0 | 0 / 0 | frame_02801 (0.00) |
| +120 | 123.9 s | 7,939 | 3,969 | 168,833 | 99 | 36,091 | 0 | 0 / 0 | - |
| +150 | 153.9 s | 9,511 | 4,755 | 180,769 | 98 | 29,884 | 0 | 0 / 0 | - |
| +180 | 183.9 s | 11,155 | 5,577 | 193,126 | 98 | 31,228 | 0 | 0 / 0 | - |
| +210 | 213.9 s | 12,986 | 6,492 | 206,969 | 96 | 34,798 | 0 | 0 / 0 | frame_05601 (0.00) |
| +240 | 243.8 s | 16,035 | 8,017 | 229,979 | 96 | 57,931 | 0 | 0 / 0 | - |
| +270 | 273.9 s | 18,842 | 9,420 | 251,354 | 95 | 53,345 | 0 | 0 / 0 | frame_08401 (0.00) |
| kill | 300.2 s | - | - | - | - | 47,346 | 0 | 0 / 0 | - |

Steady state, read off the table: the frame loop runs at ~790-1,530 presents per interval (26-51 presents/s;
the first interval includes the slow first presents), console calls and submissions grow linearly, the traced
D3D12 calls stay at 30,000-60,000 per 30 s (452,961 lines in the boot), and thread count drifts 104 -> 95. The
last title-side milestones are `PresentX call 10000` at 285.3 s and `replay: submission 20480` at 290.7 s; the
harness killed the boot at 300.2 s.

One measured counter is new relative to the old page: memory. The monitor's 1 Hz process sample is no longer
flat; working set and private bytes at the same marks:

| mark t | working set | private bytes |
|---:|---:|---:|
| +30 | 11.2 GB | 16.7 GB |
| +60 | 27.6 GB | 34.4 GB |
| +90 | 30.0 GB | 36.9 GB |
| +120 | 30.6 GB | 37.1 GB |
| +150 | 41.7 GB | 49.4 GB |
| +180 | 40.5 GB | 67.7 GB |
| +210 | 44.6 GB | 70.8 GB |
| +240 | 48.0 GB | 73.6 GB |
| +270 | 47.5 GB | 78.0 GB |
| kill | 48.5 GB | 83.6 GB |

The older 600 s boot was flat at 6.5 GB working set / 12.1 GB private from +30 s to the kill. This boot's
private-byte count reaches 83.6 GB at 300 s - about 94% of the machine's ~89 GB commit limit (68.4 GB RAM plus
a 20.5 GB page file) - and it is still rising at the kill. The measurement does not attribute the growth: no
file open, DirectStorage request or platform call starts after 115.2 s, so it is not the same event stream as
the load. It is the largest measured difference in the stationary window, and it is why the frame rate is lower
than the older page's (26-51 presents/s against ~97 presents/s).

## 3. The title's state words do not move

148 successful probe samples, from boot 3.05 s to boot 298.93 s. Exactly two transitions were measured, both at
boot 21.14 s, and none after that:

| word | before 21.14 s | from 21.14 s to 298.93 s | transition |
|---|---|---|---|
| `0x7446510` game/init | 1 | **2** | 1 -> 2 at 21.14 s |
| `0x98f828c` session | 0 | **0** | none in the boot |
| `0x7453ed0` loading | 0 | **1** | 0 -> 1 at 21.14 s |
| `0x7446514` adjacent | 0 | 0 | none |
| `0x98f8290` adjacent | 1,181,168,080 | 1,181,168,080 | none (constant from the first sample) |
| `0x7453ed4` adjacent | 0 | 0 | none |

The session word in particular is 0 in every one of the 148 samples: there is no session 0 -> 1 step anywhere in
the 300 s window. The two 32-byte windows around the init and session words are constant after the 21.14 s
transition (2 and 1 distinct values respectively), so no companion word in them changes later either. The only
sampled word that keeps moving is a counter, not a state word: the u32 at `0x7453ecc`, one word below
`0x7453ed0`, is 0 until the 21.14 s transition, jumps to ~55.9 million with it and then advances ~1,000 per
second through the boot (138 distinct window values). The loading word at `0x7453ed0` itself stays 1.

The probe also shows the title process's exit: reads fail at boot 300.94 s and 302.95 s (the harness killed it at
300.2 s, the process takes a moment to unmap the image) and the process is gone at 304.96 s.

This is the same value the tracker issue measured at an older commit (2/0/1 for the whole run) and the same value
[issue1_baseline.md](issue1_baseline.md) re-measured at HEAD `8142fd7f9` to 130 s. The renderer-setup success and
the streaming repairs did not change it: it holds for 300 s now, not just 130 s.

The renderer setup's own return value has no memory mirror this probe can read; the measured form of "the setup
returned success" is that the `0x19f4f58` assert no longer fires. The boot records exactly one `int3_skipped`,
the unrelated `game+0x247af2d` range check at 4.95 s, where the older tree recorded four.

## 4. Loader activity after the 16.5 s check

The 130 s baseline this task compares against completed the loader's pairs by about 16.5 s; this boot's loader
is busy far past that point. `PALMETTO_LOADER_HOOK=1` installs 5 detours (verified) and prints the first 200
calls of each begin/end site, then keeps counting.

| measurement | value |
|---|---|
| printed begin calls | 200, first 7.54 s, last 30.68 s; only **6** at or before 16.5 s, **194** after it |
| printed end calls | 200, last 30.68 s; 8 cross-thread ends, all 27.9-30.4 s |
| completion entry | called exactly once, 7.53 s |
| spinner entry | never fires (all three race summaries report spinner depth 0, object 0x0) |
| cumulative begin/end counters (16 tracked objects) | **284/284 at 31.25 s**, **289/289 at 91.28 s**, **302/302 at 181.25 s** |
| objects | 16 tracked objects: 15 block objects 0x80 apart (0x...1ee80 ... 0x...1f580) plus the global object 0x...1f780; every tracked object is balanced (begin = end) |
| last counter sample | 181.25 s; the fourth summary would print at ~301 s, after the 300 s kill |

So the loader does not stop at the old ~30.8 s boundary: 194 of its 200 printed begin calls come after the
16.5 s check, and at least 296 of the 302 counted calls by 181.25 s do too (only the 6 printed calls at or
before 16.5 s do not). The counters are still growing 150 s later (+18 calls from 31.25 s to 181.25 s). The
counters cannot be localised after 181.25 s (the print cap hides later calls and the next summary falls after
the kill), but the status-array event log below puts the loader's last measured action at 115.13 s.

`PALMETTO_DS_ARRAYS=1` shows the same shape on the title's own status arrays: all **17** arrays are used in this
boot, where the old page's boot used only #17 and #3. Arrays #1-#16 each take 12-14 enqueues and 12-14
completions, all by the array sampler's +23.2 to +23.5 s mark (about boot 31 s). Array #17, the one the loading
spinner polls, takes **98 enqueue events** in the whole boot: 5 at 7.72-7.90 s, 1 at 19.52 s, 78 at
26.99-35.73 s, 1 at ~75.9 s, 6 at 94.6-94.8 s and 7 at 114.2-115.1 s - the last at **boot 115.13 s**, 10 ms
before the first `XUserAddAsync` (section 6). At the last dump (sampler +75.5 s, written at boot 83.0 s) array
#17 has 85 enqueues and **40,211,664 polls**, against 14 enqueues and 31,200,161 polls in the old page's
600 s boot.
Every array is pending 0 at every dump.

## 5. DirectStorage and file activity

**DirectStorage request shapes** (`PALMETTO_DSTORAGE_TRACE=1`): 4,199 shapes at 8.1-8.6 s, then the repaired
path adds more - 5,123 at 27.69 s, 6,996 at 30.87 s and **8,272 at 31.07 s**, the final count (5,120 zlib,
refusals 0, predictor off, all opt-in mechanisms inert: `memory=0 bcpack=0 swizzle=0 dest-pages=0 staging=0
pins=0`). The new shapes are the post-load streaming that [command_signature.md](command_signature.md) records.

**Completions** (progress milestones): 1,024 / 2,048 / 3,072 / 4,096 requests at 8.1 / 8.4 / 8.6 / 8.6 s (queue
times 0.4-1.7 s), then 5,120 at 27.69 s, 6,144 at 30.87 s, 7,168 at 31.07 s and **8,192 requests / 498.9 MiB
decompressed at 31.20 s** (queue time 23.6 s). No request shape is created after 31.2 s and no completion
milestone is printed after 31.2 s.

**First-touch repairs** (the read and DirectStorage repairs this tree landed): `ReadFile` into uncommitted title
memory at `game+0x314275` at boot 27.1 s, 4,136 destination pages touched from user mode, 271,044,608 of
271,044,608 bytes read; `IDStorageQueueX::EnqueueRequest` first-touch repairs at boot 27.9 s (752 bytes, 1 page)
and 30.2 s (65,392 bytes, 1 page), caller `host:palmetto+0x105a79`. These are the first post-load streamed
reads measured on this frontier.

**File opens** (`PALMETTO_FILE_TRACE=1`, startup-trace `file_open` events): 121 in the boot. 24 are the twelve
package `.rpf` files opened twice each (host and title call site) at 2.88-3.0 s, one is the telemetry cache at
27.55 s, and the other 96 are content files opened at 30.26-35.31 s (the `ttdb` database set, `core_bvh4.rpf`,
`core_licenses_data.rpf` and their pairs). **No file is opened after 35.31 s** - not after 115.2 s and not in the
240-300 s story-load window.

## 6. User, package and store calls: the first sign-in

`PALMETTO_USER_TRACE=1` records 49 platform calls, every one at or before boot 115.16 s:

| when (boot) | call | answered |
|---|---|---|
| 1.78-4.23 s | `QueryApiImpl` XThreading ×7, XNetworking ×1, XUserImpl ×1, XPackageImpl ×3, XPersistentLocalStorage ×1, XDisplay ×2 | `S_OK`, except the three XPackageImpl calls |
| 2.79 s | `XUserRegisterForChangeEvent(queue, context, callback)` | `S_OK` (callback never invoked) |
| 3.20-3.21 s | `QueryApiImpl` XPackageImpl `IXPackageImpl4` ×3 | **`E_NOINTERFACE` (0x80004002)** - the opt-in package interface switch is not enabled by the standard environment; [loading_gate.md](loading_gate.md) already measured that completing that chain changes nothing |
| 20.2-36.2 s | `QueryApiImpl` XThreading/XDisplay/XAccessibility/XNetworking/XGame/XSystem | `S_OK` |
| 26.99 s | `XSpeechSynthesizerCreate` | `E_FAIL` (0x80004005) - the opt-in silent-speech switch is not enabled |
| **115.14-115.16 s** | `QueryApiImpl` XUserImpl ×6, `XUserAddAsync`, `XUserAddResult`, `XUserGetId`, `XUserGetLocalId`, `XUserDuplicateHandle`, `XUserCloseHandle` | **all `S_OK`** |

The sign-in sequence at 115.14-115.16 s is the last platform activity in the boot: no platform call, of any
class, is made in the remaining 185 s. No `XStore*` call, no `XGameSave*` call and no `XPackage*` slot call is
made at all. This is new against the 300 s acceptance boot of [loading_gate.md](loading_gate.md), which recorded
no `XUserAddAsync`/`AddResult` in 300 s: the boot now gets to the sign-in stage on its own, with every call
answered `S_OK`, and nothing follows it.

## 7. The presented image is still black

`PALMETTO_CAPTURE` wrote 4 captures, every 2,800 presented frames (at this boot's presented rate that is 61-97 s
apart). The runtime's own `index.txt` line and an independent read of every PPM agree:

| capture | presented frame | written at | extent | index mean rgb | PPM mean | peak | non-black |
|---:|---:|---:|---|---:|---:|---:|---:|
| 1 | 1 | 27.7 s | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0 / 3,643,444 |
| 2 | 2,801 | 88.7 s | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0 / 3,643,444 |
| 3 | 5,601 | 186.1 s | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0 / 3,643,444 |
| 4 | 8,401 | 252.3 s | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0 / 3,643,444 |

No loading artwork, prompt or first-run page appears in any capture. The captured extent is the swapchain the
title presents (2564x1421), not the composed 4K target.

## 8. New title-side activity after 130 s: none

Categorising every `[rt]` line by its boot time, the only categories with a first occurrence after 130 s are the
frame loop's own counters and the diagnostic's own scheduled prints:

| category | before 130 s | after 130 s | first after 130 s |
|---|---:|---:|---:|
| D3D12 `slot 67` compute refusals | 410 | 133 | 130.2 s (continues a pre-130 line stream) |
| `no translated compute shader` lines (same calls) | 409 | 133 | 130.2 s |
| pipeline timeline (30 s sampler) | 4 | 5 | 153.9 s |
| replay submission milestone | 6 | 3 | 205.6 s |
| presenter capture | 3 | 2 | 186.1 s |
| loader race summary (diagnostic) | 2 | 1 | 181.2 s |
| `PresentX call 10000` milestone | 3 | 1 | 285.3 s |
| DirectStorage trace / status arrays | 54 / 3,019 | 0 / 0 | - |
| loader hook, user trace, file opens | 805 / 49 / 121 | 0 / 0 / 0 | - |

The boot logs 37 distinct traced D3D12 call kinds and **all of them are first seen before 130 s**; no user-trace
method is first seen after 130 s; no `file_open` event occurs after 35.31 s; no DirectStorage request shape is
created after 31.2 s. After 115.16 s the only growing quantities are the frame loop's counters and the
private-byte count of section 2.

## 9. Verdict against the 600 s page

[long_boot.md](long_boot.md) measured one 600 s boot at tree `a90cd9f98` - before the read, DirectStorage and
command-signature repairs - and concluded that nothing new appears after ~30.8 s. This boot is the same
measurement on the fixed tree, with a 300 s window. Exactly what changed:

| measurement | old 600 s page (`a90cd9f98`) | this 300 s boot (`583b91195`) |
|---|---|---|
| DirectStorage requests | 4,199, all by 7.43 s, frozen | 8,272, 5,123 by 27.7 s and the last 8,272 by 31.1 s, frozen after |
| DirectStorage completions | 4,096 by 8.48 s | 8,192 requests / 498.9 MiB by 31.2 s |
| loader begin/end calls | 19/19 in 600 s, last at 30.8 s | 284/284 by 31.25 s -> 302/302 by 181.25 s (200/200 printed by 30.68 s) |
| loader completion entry | once, 6.38 s | once, 7.53 s |
| loader spinner | never | never |
| DirectStorage status arrays | #17 and #3 used; #17 14 enqueues, last 23.95 s, polls frozen 31.2 M | all 17 used; #17 98 enqueue events, last at 115.13 s, polls 40.2 M |
| post-load streamed reads | none | `ReadFile` repair 27.1 s (271 MB / 4,136 pages), DS repair 27.9 s and 30.2 s |
| package file opens | not traced on that boot | 121, last at 35.31 s |
| platform calls | not traced on that boot | 49, last at 115.16 s; first user sign-in all `S_OK` |
| `int3_skipped` | 4 (`0x247af2d`, `0x19f4f58`, `0x1e6c2c5`, `0x344196`) | 1 (`0x247af2d`) |
| presented frames | 10 captures, all black, frames 1 .. 54,001 | 4 captures, all black, frames 1 .. 8,401 |
| state words `0x7446510`/`0x98f828c`/`0x7453ed0` | not sampled on that boot; [issue1_baseline.md](issue1_baseline.md) measured 2/0/1 to 130 s | 2/0/1 from 21.1 s to 298.9 s |
| after-130 title-side activity | only frame-loop counters and scheduled diagnostics | only frame-loop counters and the slot-67 compute refusals |
| memory | flat 6.5 GB working set / 12.1 GB private, 30 s -> 600 s | 11.2 -> 48.5 GB working set, 16.7 -> 83.6 GB private, 33.9 -> 299.9 s |
| story load (fork's 240-300 s start) | no correlate | **no correlate**: black loop, no DS/file/loader/state change |

The load stage itself is now much longer and much more complete than the old page's: the old boot's title-side
work ended by ~30.8 s, this one reaches a user sign-in at 115.1 s and had loader calls still being counted at
181.25 s. What did not change is the boundary that matters: the game/init word never leaves 2, the session word
never leaves 0, no loading artwork appears, and the story-load window has no correlate. The old page's reading
that "wall-clock time is not what advances it" still holds, and this boot adds that neither the renderer-setup
success nor the repaired streamed reads nor the completed offline sign-in advances it either.

## 10. What has to change first

Measured state at the 300 s kill: init word `0x7446510` = **2**, session word `0x98f828c` = **0**, loading word
`0x7453ed0` = **1**; the frame loop is alive (9,420 presents at the +270 mark, `PresentX call 10000` at 285.3 s,
`replay: submission 20480` at 290.7 s, 0 exceptions); the image is black in 4/4 captures; the last title-side
activity is the user sign-in at 115.14-115.16 s, and in the 185 s after it no loader entry, DirectStorage
request, file open, platform call or new D3D12 kind starts.

The first thing that must change is the title's own state step after that sign-in: `0x7446510` must leave 2
(and then `0x98f828c` must go 0 -> 1). Everything the measured path asks of the port in this window is being
answered on the successful path - the loader's ended pairs are balanced, all 8,272 DirectStorage requests
complete `S_OK`, the 121 files open, and the six user calls of the sign-in return `S_OK` (the two opt-in
refusals of section 6 are the package interface query and the speech synthesizer, and neither is re-read after
27 s) - and no timer, no further loader/DirectStorage/file work and no frame-loop milestone moves the two words
in the remaining 185 s. The next measurement that can discriminate the gate is therefore the title-side stage
dispatcher after `XUserCloseHandle` at 115.16 s (the gate [loading_gate.md](loading_gate.md) already narrowed to
the title's own dispatcher), not another wall-clock boot: no new title-side stage activity appears in the
window from 115.2 s to 300 s.

## 11. Limits

- **One boot, one machine, one overlay.** The boot's own timing moves run to run: the two 130 s fix boots of
  [command_signature.md](command_signature.md) drained the same 8,192 DirectStorage requests by 98.7 / 89.1 s of
  queue time, this one by 23.6 s, and their ~31 s race summaries counted 110 begin calls on 16 objects and 50 on
  2, against this boot's 284 on 16. The load stage is timing-sensitive, which is why this boot's series is not
  interchangeable with theirs.
- **The capture interval is frame-bound.** `PALMETTO_CAPTURE_EVERY=2800` is ~60 s at the older page's ~97
  presents/s but 61 / 97 / 66 s here, at the measured 29-46 presents/s of the capture intervals; the capture
  times in section 7 are the measured ones.
- **The loader print cap hides later calls.** `PALMETTO_LOADER_HOOK` prints 200 calls per site; the counters in
  the race summaries keep counting (to 181.25 s here), and the fourth summary would print at ~301 s, after the
  300 s kill. Activity between 181.25 s and the kill cannot be counted from the hook; the status-array event log
  (to 115.13 s) and the DirectStorage shape count (frozen from 31.2 s) bound it.
- **The status-array sampler's `+N s` marks are not boot seconds.** Its clock starts when the title creates the
  arrays (~7.6 s into this boot) and drifts; the page converts every mark through the 1 Hz stderr-growth clock
  (for example the `+75.474 s` last dump was written at boot 83.0 s). The same is true of the pipeline timeline
  marks (3.7-3.9 s late) and the race-summary labels (printed at 31.25 / 91.28 / 181.25 s for the labels
  30 / 60 / 90 s, the diagnostic's own accumulated sleep).
- **The memory growth is measured, not attributed.** No event stream measured here explains it; it is reported
  because it is the largest difference in the stationary window and because it puts the process at 94% of the
  machine's commit limit at 300 s. A second boot with a smaller measurement layer would say whether the switches
  contribute.
- **No input was injected.** The fork's healthy-boot table has a landing prompt that a player accepts; this boot
  tests the self-driven path only, so "the gate is not input" remains untested (the old page's next-step
  suggestion).
- **The package and speech answers are opt-in and off here.** The three `IXPackageImpl4` queries return
  `E_NOINTERFACE` and `XSpeechSynthesizerCreate` returns `E_FAIL`; [loading_gate.md](loading_gate.md) measured
  that turning the package chain on changes nothing, but this boot does not re-test it on the fixed tree.
- **State words are RVAs, not game data.** This page contains RVAs, sampled integers, hex bytes of three small
  windows, counters, HRESULTs and outcomes only: no package path, no game byte, no extracted table.

## 12. Evidence and reproduction

The boot's own files (outside the repository), under `D:\leonida-work\storyprobe`: `boot\stderr.log` (33.0 MB),
`boot\stdout.log`, `boot\boot.jsonl` (51,482 events), `boot\samples.csv` (1 Hz process/file-growth series),
`boot\monitor.log`, `boot\table.txt` and `boot\analysis.txt` / `boot\analysis.json` (the extracted series), and
`boot\events.txt` / `boot\events.json` (the title-side event timeline at boot seconds). Captures are in
`D:\leonida-work\storyprobe\capture` (`frame_00001.ppm` ... `frame_08401.ppm`, `index.txt`) with their
independent statistics in the analysis. `state_probe.csv` is the external probe's 153 rows. The scripts are
`D:\leonida-work\storyprobe\boot300.ps1` (the single boot, lock held and released inside it), `state_probe.py`
(the read-only probe), and `analyze_story_probe.py`, `extract_events.py`, `build_table.py` (the tables of this
page). No game bytes are read by any of them.

Reproduce: acquire `_setup/boot_lock.ps1 -Acquire -Who <name>`, start the probe
(`python -I state_probe.py --out <work>\state_probe.csv --seconds 330 interval option 2`), then run one boot the way
`tooling/boot_report.py` does with the environment of section 1 and `--timeout 300`, sample the process at 1 Hz,
kill at 300 s, stop the probe and release the lock. No runtime change is involved and no git write is made by
this slice; the only uncommitted file is this page.
