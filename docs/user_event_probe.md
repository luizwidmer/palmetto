# The delivered user-change event: consumed and integrated, but the loading state step does not move

One paired 300 s measurement - arm A control, arm B with the runtime's opt-in delivered user-change
event (`PALMETTO_USER_CHANGE_EVENT=1`, [user_events.md](user_events.md)) - on the same runtime binary,
with the story-probe measurement layer of [story_load_probe.md](story_load_probe.md) and its external
2 s `ReadProcessMemory` probe of the title's state words.

**Short answer: the event is consumed and it does change the title's user-state handling, but the
measured state step does not move.** Arm B's delivered `SignedInAgain` (0) runs the registered
callback and the five follow-up user calls the earlier 130 s probe predicted, and it also makes the
title run its own user-ready refresh at 27.0 s (gamertag, ids, handle duplicates, guest/age, ten-plus
privilege checks, and the first `XGameSave` class request, all `S_OK`) - 70 platform lines that the
control never makes. Yet the words the tracker issue samples do not move in either arm: after the
ordinary boot transition the game/init word `0x7446510` is **2**, the session word `0x98f828c` is
**0** in every one of the 148 successful samples, and the loading word `0x7453ed0` is **1**. The
loader does not restart (no re-arm step, counters balanced), no new DirectStorage request shape and
no file open appears after the load, and all five presenter captures in both arms are uniformly
black.

## 1. What ran

| | |
|---|---|
| Machine | Windows 11 Pro 10.0.26200, Core i9-13900K, GeForce RTX 4090, driver 32.0.16.1714 |
| Boot lock | `_setup/boot_lock.ps1 -Acquire -Who ueprobe`, held for one boot per arm and released before the next arm |
| Boots | A started 13:52:32, B started 13:59:52 (local), 300 s each, killed by the harness at 300 s |
| Environment | the standard set (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_PIPELINE_TIMELINE=30`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_FILE_TRACE=1`, presenter capture (`PALMETTO_CAPTURE_EVERY=2800`, `PALMETTO_CAPTURE_LIMIT=8`); B adds only `PALMETTO_USER_CHANGE_EVENT=1` |
| Probe | external `ReadProcessMemory` at the fixed title image base `0x140000000` + RVA, every 2 s, read-only |

### Boot identity and summary

| | A control | B user-change event |
|---|---|---|
| repository HEAD at boot | `8daa4a6c3e815c8ffce1b108fa0d9238928feb4e` | `8daa4a6c3e815c8ffce1b108fa0d9238928feb4e` |
| git status at boot | `M port/shims/d3d12_xs_device.cpp; M port/shims/d3d12_xs_device.h; M port/test/xs_proxy_mock_test.cpp` | `M port/shims/d3d12_xs_device.cpp; M port/shims/d3d12_xs_device.h; M port/test/xs_proxy_mock_test.cpp;?? docs/range_assert.md` |
| extra arm switch | none | `PALMETTO_USER_CHANGE_EVENT=1` |
| `palmetto.exe` SHA-256 | `45DF506F3009A76156F40D9103B07EA63F811BE3BEFC3649D4540E497B1003BF` | `45DF506F3009A76156F40D9103B07EA63F811BE3BEFC3649D4540E497B1003BF` |
| `palmetto.exe` mtime | 2026-11-25T13:50:06.4568583-03:00 | 2026-11-25T13:50:06.4568583-03:00 |
| boot started | 2026-11-25 13:52:31 | 2026-11-25 13:59:49 |
| stderr log | 38,256,722 bytes | 40,065,458 bytes |
| startup-trace events | 50,679 | 51,657 |
| trace last event | 300.07 s | 299.98 s |
| trace exceptions | 0 | 0 |
| `int3_skipped` | 0 | 0 |
| DirectStorage shapes (final) | 8,273 | 8,273 |
| DirectStorage completions | 8,192 requests, 498.9 MiB | 8,192 requests, 498.9 MiB |
| file opens | 121 | 121 |
| window (kill at) | 300.2 s | 300.1 s |
| presenter captures | 5 | 5 |

The tree moved during this slice (other agents work in the same checkout): both arms booted with
HEAD `8daa4a6c3` and the same three modified runtime/test files, and the concurrent display-window
slice committed its slot-127 change as `ebe181e14` at 14:00:18, inside arm B's boot. No build ran
during either boot, and the `palmetto.exe` SHA-256 in the two arms' metadata is identical, so the
pair compares one binary. That binary is **not** the one behind [story_load_probe.md](story_load_probe.md)
(that boot's `int3_skipped` and 115.14 s sign-in are absent here - see section 8).

## 2. What the delivered event did (arm B)

The registration is at boot 1.59 s and the runtime delivers one `SignedInAgain` (0) event to the
title's registered callback `game+0x2211c80` 0.06 s later:

```
[rt] xgameruntime: +1.28s XUser change event SignedInAgain (0) delivered to the registered callback
```

It is consumed. The title's callback runs the five follow-up calls the 130 s probe predicted, at boot
1.78-2.10 s, all answered `S_OK` (`-> 0x0`):

| boot s | process s | call | answered |
|---:|---:|---|---|
| 1.78 | 1.28 | `XUserFindUserByLocalId #1 from game+0x55de309 -> 0x0` | `0x0` |
| 1.86 | 1.28 | `XUserGetId #1 from game+0x55dd09f -> 0x0` | `0x0` |
| 1.94 | 1.28 | `XUserGetLocalId #1 from game+0x55dd117 -> 0x0` | `0x0` |
| 2.02 | 1.28 | `XUserDuplicateHandle #1 from game+0x55dcb8b -> 0x0` | `0x0` |
| 2.10 | 1.28 | `XUserCloseHandle #1 from game+0x55dcbf4 -> 0x0` | `0x0` |

**New relative to that 130 s probe:** the title then runs a second, larger user-ready refresh at boot
26.97-27.11 s, immediately after its `XNetworking`/`XGame`/`XSystem` block (which the control makes
too, without the user calls). Arm A makes none of these calls in the whole 300 s.

| call printed in the refresh block | printed calls |
|---|---:|
| `QueryApiImpl XUserImpl` | 12 |
| `XUserGetGamertag` | 9 |
| `XUserCheckPrivilege` | 9 |
| `XUserDuplicateHandle` | 8 |
| `XUserGetId` | 8 |
| `XUserGetLocalId` | 8 |
| `QueryApiImpl XGameSaveImpl` | 2 |
| `QueryApiImpl XNetworkingImpl` | 1 |
| `QueryApiImpl XGameImpl` | 1 |
| `XUserCloseHandle` | 1 |
| `XUserGetIsGuest` | 1 |
| `XUserGetAgeGroup` | 1 |
| **total lines** | **61** |

Answered values: `0x0` x61. The trace prints the first 8 calls of an entry point and then every tenth-power one, so a method with printed ordinals 1-8 and 10 was called at least ten times.

The platform-count difference between the arms is therefore not the event singleton but the whole
refresh path it enables:

| platform entry point (printed calls) | A control | B user-change event |
|---|---:|---:|
| `QueryApiImpl` | 35 | 55 |
| `XSpeechSynthesizerCreate` | 1 | 1 |
| `XUserCheckPrivilege` | 0 | 9 |
| `XUserCloseHandle` | 0 | 2 |
| `XUserDuplicateHandle` | 0 | 9 |
| `XUserFindUserByLocalId` | 0 | 1 |
| `XUserGetAgeGroup` | 0 | 1 |
| `XUserGetGamertag` | 0 | 9 |
| `XUserGetId` | 0 | 9 |
| `XUserGetIsGuest` | 0 | 1 |
| `XUserGetLocalId` | 0 | 9 |
| `XUserRegisterForChangeEvent(queue=given` | 1 | 1 |
| **total platform lines** | **37** | **107** |

So the delivered event is not a no-op that the title ignores: it is integrated into the title's user
state machine, and the title treats the offline user as resolved and signed in - yet section 3 shows
that this state does not feed the loading step, and section 4 shows it starts no streaming, loader or
file work.

## 3. The state words do not move

The external probe samples six title words and three 32-byte windows every 2 s. The two arms' series
are identical in what they show: one ordinary boot transition, then no state movement for the rest of
the 300 s.

### Arm A (control)

| state (init, session, loading) | first sampled at boot | last sampled at boot | samples |
|---|---:|---:|---:|
| 1/0/0 | 3.01 s | 21.07 s | 10 |
| 2/0/0 | 23.08 s | 23.08 s | 1 |
| 2/0/1 | 25.09 s | 298.95 s | 137 |

Successful in-window samples: 148; first at 3.01 s, last at 298.95 s. Distinct values: `0x7446510` 2, `0x98f828c` 1, `0x7453ed0` 2. 32-byte windows around the words: `0x7446500` 2 distinct, `0x98f8280` 1 distinct, `0x7453ec0` 136 distinct (that last window carries a counter that keeps advancing; the two state windows do not move after the boot transition).

### Arm B (user-change event)

| state (init, session, loading) | first sampled at boot | last sampled at boot | samples |
|---|---:|---:|---:|
| 1/0/0 | 3.02 s | 17.09 s | 8 |
| 2/0/1 | 19.10 s | 298.95 s | 140 |

Successful in-window samples: 148; first at 3.02 s, last at 298.95 s. Distinct values: `0x7446510` 2, `0x98f828c` 1, `0x7453ed0` 2. 32-byte windows around the words: `0x7446500` 2 distinct, `0x98f8280` 1 distinct, `0x7453ec0` 138 distinct (that last window carries a counter that keeps advancing; the two state windows do not move after the boot transition).

In both arms the session word is **0 in every sample** (148/148 in each), the init word is **2 in
every sample after the boot transition**, and no state transition happens after 25.1 s (A) / 19.1 s
(B). The only window that keeps changing is the 32-byte window around the loading word, which carries
a title-internal counter; the windows around the init and session words are constant after the boot
transition (A: 2 and 1 distinct values; B: same).

### Per-30 s series (timeline mark, DirectStorage, loader, files, captures)

Arm A:

| bin | timeline mark at | submits | presents | console | threads | DS shapes | loader prints | files | captures |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | no mark in this bin | - | - | - | - | 4199 | 7/7 | 24 | - |
| 30 | 33.8 s | 147 | 73 | 111268 | 103 | 8273 | 193/193 | 95 | 1(0.00)@32.0s |
| 60 | 63.8 s | 2972 | 1485 | 132319 | 102 | 8273 | 0/0 | 0 | 2801(0.00)@88.8s |
| 90 | 93.8 s | 6122 | 3060 | 155967 | 98 | 8273 | 0/0 | 2 | - |
| 120 | 123.8 s | 8896 | 4447 | 176818 | 98 | 8273 | 0/0 | 0 | - |
| 150 | 153.8 s | 11531 | 5765 | 196595 | 97 | 8273 | 0/0 | 0 | 5601(0.00)@150.1s |
| 180 | 183.8 s | 13945 | 6972 | 214715 | 97 | 8273 | 0/0 | 0 | - |
| 210 | 213.8 s | 16540 | 8269 | 234211 | 96 | 8273 | 0/0 | 0 | 8401(0.00)@217.8s |
| 240 | 243.8 s | 19437 | 9718 | 255963 | 96 | 8273 | 0/0 | 0 | - |
| 270 | 273.8 s | 22629 | 11314 | 279919 | 96 | 8273 | 0/0 | 0 | 11201(0.00)@271.5s |
| 300 | after the kill | - | - | - | - | 8273 | 0/0 | 0 | - |
| 330 | after the kill | - | - | - | - | 8273 | 0/0 | 0 | - |

Arm B:

| bin | timeline mark at | submits | presents | console | threads | DS shapes | loader prints | files | captures |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 0 | no mark in this bin | - | - | - | - | 8273 | 200/200 | 121 | 1(0.00)@26.6s |
| 30 | 32.8 s | 545 | 272 | 114409 | 104 | 8273 | 0/0 | 0 | - |
| 60 | 62.8 s | 3333 | 1666 | 135339 | 100 | 8273 | 0/0 | 0 | 2801(0.00)@85.6s |
| 90 | 92.8 s | 6219 | 3109 | 156995 | 99 | 8273 | 0/0 | 0 | - |
| 120 | 122.8 s | 9331 | 4665 | 180383 | 99 | 8273 | 0/0 | 0 | 5601(0.00)@141.5s |
| 150 | 152.9 s | 12211 | 6105 | 201996 | 97 | 8273 | 0/0 | 0 | - |
| 180 | 182.8 s | 14699 | 7349 | 220672 | 97 | 8273 | 0/0 | 0 | 8401(0.00)@203.8s |
| 210 | 212.9 s | 17601 | 8800 | 242471 | 96 | 8273 | 0/0 | 0 | - |
| 240 | 242.8 s | 20820 | 10409 | 266634 | 96 | 8273 | 0/0 | 0 | 11201(0.00)@258.5s |
| 270 | 272.8 s | 23792 | 11895 | 288936 | 96 | 8273 | 0/0 | 0 | - |
| 300 | after the kill | - | - | - | - | 8273 | 0/0 | 0 | - |
| 330 | after the kill | - | - | - | - | 8273 | 0/0 | 0 | - |

## 4. Loader, DirectStorage and files: no restart, no new work

**Loader** (`PALMETTO_LOADER_HOOK=1`, 16 tracked objects, 5 detours installed in both arms). The hook
prints the first 200 calls per site and keeps counting; the three race summaries are the counted
state.

Arm A:

| summary label | printed at boot | objects | begin / end | cross-thread end | object armed flags |
|---:|---:|---:|---:|---:|---|
| 30 s | 31.35 s | 4 | 21 / 20 | 0 | all `last armed 1` |
| 60 s | 91.25 s | 16 | 288 / 288 | 8 | all `last armed 1` |
| 90 s | 181.26 s | 16 | 288 / 288 | 8 | all `last armed 1` |

Printed loader calls: 200 begin, 200 end, 8 cross-thread end (the hook prints the first 200 calls of each site and keeps counting).

Arm B:

| summary label | printed at boot | objects | begin / end | cross-thread end | object armed flags |
|---:|---:|---:|---:|---:|---|
| 30 s | 31.22 s | 16 | 291 / 291 | 8 | all `last armed 1` |
| 60 s | 91.22 s | 16 | 291 / 291 | 8 | all `last armed 1` |
| 90 s | 181.24 s | 16 | 297 / 297 | 8 | all `last armed 1` |

Printed loader calls: 200 begin, 200 end, 8 cross-thread end (the hook prints the first 200 calls of each site and keeps counting).

Reading it: the loader does the same batch in both arms - 200/200 printed begin/end calls by about
31 s, 8 cross-thread ends, cumulative counters balanced - and there is **no re-arm step** in either
arm (`last armed` is 1 in every summary for every object; no summary records an object left at 0, the
state the earlier page's pre-repair boot showed). A's
cumulative counters are frozen at **288 begin / 288 end** from the 91.25 s summary to the 181.26 s
one; B's are 291/291 at 91.22 s and 297/297 at 181.24 s - a +6 growth on the global object, still
balanced, with no new printed call (the 200-call print cap) and no new batch. So the delivered event
does not restart the loader; the only late loader change is six counted pairs on one object, and it
is not matched by any state-word, DirectStorage or file movement.

**DirectStorage** (`PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_DS_ARRAYS=1`):

Arm A:

| measurement | value |
|---|---:|
| shapes at 8.08 s | 4199 (zlib 4199, refusals 0) |
| shapes at 8.43 s | 4199 (zlib 4199, refusals 0) |
| shapes at 8.68 s | 4199 (zlib 4199, refusals 0) |
| shapes at 8.91 s | 4199 (zlib 4199, refusals 0) |
| shapes at 31.88 s | 5123 (zlib 5002, refusals 0) |
| shapes at 33.07 s | 6996 (zlib 5120, refusals 0) |
| shapes at 33.35 s | 8273 (zlib 5121, refusals 0) |
| shapes at 33.52 s | 8273 (zlib 5121, refusals 0) |
| completions at 8.07 s | 1024 request(s), 64.0 MiB decompressed |
| completions at 8.43 s | 2048 request(s), 128.0 MiB decompressed |
| completions at 8.67 s | 3072 request(s), 192.0 MiB decompressed |
| completions at 8.91 s | 4096 request(s), 256.0 MiB decompressed |
| completions at 31.88 s | 5120 request(s), 354.3 MiB decompressed |
| completions at 33.07 s | 6144 request(s), 480.1 MiB decompressed |
| completions at 33.35 s | 7168 request(s), 487.2 MiB decompressed |
| completions at 33.52 s | 8192 request(s), 498.9 MiB decompressed |

Arm B:

| measurement | value |
|---|---:|
| shapes at 6.95 s | 4199 (zlib 4199, refusals 0) |
| shapes at 7.27 s | 4199 (zlib 4199, refusals 0) |
| shapes at 7.45 s | 4199 (zlib 4199, refusals 0) |
| shapes at 7.50 s | 4199 (zlib 4199, refusals 0) |
| shapes at 26.52 s | 5123 (zlib 5002, refusals 0) |
| shapes at 27.74 s | 6996 (zlib 5120, refusals 0) |
| shapes at 27.93 s | 8273 (zlib 5121, refusals 0) |
| shapes at 28.09 s | 8273 (zlib 5121, refusals 0) |
| completions at 6.94 s | 1024 request(s), 64.0 MiB decompressed |
| completions at 7.27 s | 2048 request(s), 128.0 MiB decompressed |
| completions at 7.45 s | 3072 request(s), 192.0 MiB decompressed |
| completions at 7.49 s | 4096 request(s), 256.0 MiB decompressed |
| completions at 26.52 s | 5120 request(s), 354.3 MiB decompressed |
| completions at 27.74 s | 6144 request(s), 480.1 MiB decompressed |
| completions at 27.93 s | 7168 request(s), 487.2 MiB decompressed |
| completions at 28.09 s | 8192 request(s), 498.9 MiB decompressed |

Both arms create the same **8,273** request shapes and complete the same **8,192** requests /
**498.9 MiB**; no request shape is created after 33.52 s (A) / 28.09 s (B), and no new request kind
appears. The only DirectStorage activity after that is a single late burst of enqueues on the
existing status array #17 (the loading spinner's array, caller `game+0xe8943`), in both arms and at
different times: six enqueue lines at boot 284.01-284.44 s in A and six at boot 144.54-144.74 s in B; the same status array #17 is enqueued many more times earlier in both boots (89 and 90 #17 enqueue lines in the whole boot), so these are enqueues on an existing array - the loading spinner's poll path - and not new requests. 
They are not new request shapes, no streaming follows them, and they are not attributable to the
event (both arms have them).

**Files** (`PALMETTO_FILE_TRACE=1`):

- Arm A: File opens: **121**; first at 2.83 s, last at 90.81 s; opens after 130 s: **0**; opens after 115.2 s: **0** (no file name is recorded on this page; the trace that carries them is in the evidence directory).
- Arm B: File opens: **121**; first at 1.88 s, last at 27.99 s; opens after 130 s: **0**; opens after 115.2 s: **0** (no file name is recorded on this page; the trace that carries them is in the evidence directory).

Both arms open the same count (121); the measured load ordering differs by boot speed (see section 9), and no
file is opened in either arm after its load ends - in particular not after 115 s and not in the
250-300 s window.

## 5. The presented image is still black

`PALMETTO_CAPTURE` wrote five captures per arm (every 2,800 presented frames). The runtime's own
index line and an independent read of every PPM agree.

Arm A:

| frame | written (boot s) | extent | index mean rgb | PPM mean | peak | non-black |
|---:|---:|---|---:|---:|---:|---:|
| 1 | 32.0 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 2801 | 88.8 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 5601 | 150.1 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 8401 | 217.8 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 11201 | 271.5 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |

Arm B:

| frame | written (boot s) | extent | index mean rgb | PPM mean | peak | non-black |
|---:|---:|---|---:|---:|---:|---:|
| 1 | 26.6 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 2801 | 85.6 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 5601 | 141.5 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 8401 | 203.8 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |
| 11201 | 258.5 | 2564x1421 | 0.00 0.00 0.00 | 0.0000 | 0 | 0/3643444 |

Every capture in both arms is uniformly black: mean 0.0000 over all three channels, peak 0, and
0 non-black pixels of 3,643,444. The delivered event moves nothing on the present path.

## 6. Every platform line, with its time

`PALMETTO_USER_TRACE=1`. The line is `method #ordinal from caller -> answered`; the `QueryApiImpl` rows also carry the
interface id. Cardinal `#N` is the trace's calling ordinal (first 8, then tenth powers).

Arm A - 37 lines:

| boot s | process s | line |
|---:|---:|---|
| 1.79 | 1.30 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #1 from game+0x55de08c -> 0x0` |
| 1.91 | 1.30 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #2 from game+0x55de08c -> 0x0` |
| 2.02 | 1.30 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #1 from game+0x55de08c -> 0x0` |
| 2.14 | 2.25 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #3 from game+0x55de08c -> 0x0` |
| 2.47 | 2.25 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #1 from game+0x55de08c -> 0x0` |
| 2.79 | 2.25 | `XUserRegisterForChangeEvent(queue=given context=given callback=game+0x2211c80) #1 from game+0x55dcdcb -> 0x0` |
| 3.19 | 3.25 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #1 from game+0x55de08c -> 0xffffffff80004002` |
| 3.20 | 3.25 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #2 from game+0x55de08c -> 0xffffffff80004002` |
| 3.20 | 3.25 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #4 from game+0x55de08c -> 0x0` |
| 3.20 | 3.25 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #3 from game+0x55de08c -> 0xffffffff80004002` |
| 3.20 | 3.26 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #5 from game+0x55de08c -> 0x0` |
| 3.20 | 3.26 | `QueryApiImpl XPersistentLocalStorageImpl iid 41a4e10c-5a7e-41d9-8c37-37bde62a07d6 #1 from game+0x55de08c -> 0x0` |
| 3.21 | 3.32 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #6 from game+0x55de08c -> 0x0` |
| 3.21 | 3.32 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #1 from game+0x55de08c -> 0x0` |
| 4.21 | 3.85 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #7 from game+0x55de08c -> 0x0` |
| 4.22 | 3.85 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #2 from game+0x55de08c -> 0x0` |
| 23.78 | 23.80 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #8 from game+0x55de08c -> 0x0` |
| 23.78 | 23.80 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #3 from game+0x55de08c -> 0x0` |
| 31.22 | 31.00 | `QueryApiImpl XAccessibilityImpl iid d722b373-8c4d-4692-9e51-d6ad9b37aa7d #1 from game+0x55de08c -> 0x0` |
| 31.22 | 31.00 | `XSpeechSynthesizerCreate #1 from game+0x55dced0 -> 0xffffffff80004005` |
| 31.23 | 31.00 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #10 from game+0x55de08c -> 0x0` |
| 31.23 | 31.00 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #4 from game+0x55de08c -> 0x0` |
| 32.09 | 32.26 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #2 from game+0x55de08c -> 0x0` |
| 32.11 | 32.28 | `QueryApiImpl XGameImpl iid 2549f142-6419-4a06-97b5-931aab7c2f34 #1 from game+0x55de08c -> 0x0` |
| 32.11 | 32.28 | `QueryApiImpl XSystemImpl iid 1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf #1 from game+0x55de08c -> 0x0` |
| 32.11 | 32.28 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #3 from game+0x55de08c -> 0x0` |
| 32.11 | 32.28 | `QueryApiImpl XGameImpl iid 2549f142-6419-4a06-97b5-931aab7c2f34 #2 from game+0x55de08c -> 0x0` |
| 32.11 | 32.28 | `QueryApiImpl XSystemImpl iid 1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf #2 from game+0x55de08c -> 0x0` |
| 32.12 | 32.28 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #4 from game+0x55de08c -> 0x0` |
| 32.12 | 32.28 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #5 from game+0x55de08c -> 0x0` |
| 32.13 | 32.30 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #6 from game+0x55de08c -> 0x0` |
| 32.14 | 32.30 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #7 from game+0x55de08c -> 0x0` |
| 32.17 | 32.32 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #8 from game+0x55de08c -> 0x0` |
| 32.19 | 32.33 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #10 from game+0x55de08c -> 0x0` |
| 33.13 | 33.08 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #100 from game+0x55de08c -> 0x0` |
| 40.52 | 40.55 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #1000 from game+0x55de08c -> 0x0` |
| 115.19 | 115.23 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #10000 from game+0x55de08c -> 0x0` |

Arm B - 107 lines:

| boot s | process s | line |
|---:|---:|---|
| 1.34 | 1.26 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #1 from game+0x55de08c -> 0x0` |
| 1.39 | 1.26 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #2 from game+0x55de08c -> 0x0` |
| 1.44 | 1.26 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #1 from game+0x55de08c -> 0x0` |
| 1.49 | 1.28 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #3 from game+0x55de08c -> 0x0` |
| 1.54 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #1 from game+0x55de08c -> 0x0` |
| 1.59 | 1.28 | `XUserRegisterForChangeEvent(queue=given context=given callback=game+0x2211c80) #1 from game+0x55dcdcb -> 0x0` |
| 1.68 | 1.28 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #4 from game+0x55de08c -> 0x0` |
| 1.73 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #2 from game+0x55de08c -> 0x0` |
| 1.78 | 1.28 | `XUserFindUserByLocalId #1 from game+0x55de309 -> 0x0` |
| 1.81 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #3 from game+0x55de08c -> 0x0` |
| 1.86 | 1.28 | `XUserGetId #1 from game+0x55dd09f -> 0x0` |
| 1.89 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #4 from game+0x55de08c -> 0x0` |
| 1.94 | 1.28 | `XUserGetLocalId #1 from game+0x55dd117 -> 0x0` |
| 1.97 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #5 from game+0x55de08c -> 0x0` |
| 2.02 | 1.28 | `XUserDuplicateHandle #1 from game+0x55dcb8b -> 0x0` |
| 2.05 | 1.28 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #6 from game+0x55de08c -> 0x0` |
| 2.10 | 1.28 | `XUserCloseHandle #1 from game+0x55dcbf4 -> 0x0` |
| 2.13 | 2.28 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #1 from game+0x55de08c -> 0xffffffff80004002` |
| 2.14 | 2.28 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #2 from game+0x55de08c -> 0xffffffff80004002` |
| 2.14 | 2.28 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #5 from game+0x55de08c -> 0x0` |
| 2.14 | 2.28 | `QueryApiImpl XPackageImpl iid 0b9d556e-ab70-43f4-baa0-9999ac4cb040 #3 from game+0x55de08c -> 0xffffffff80004002` |
| 2.14 | 2.29 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #6 from game+0x55de08c -> 0x0` |
| 2.14 | 2.29 | `QueryApiImpl XPersistentLocalStorageImpl iid 41a4e10c-5a7e-41d9-8c37-37bde62a07d6 #1 from game+0x55de08c -> 0x0` |
| 2.15 | 2.34 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #7 from game+0x55de08c -> 0x0` |
| 2.15 | 2.34 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #1 from game+0x55de08c -> 0x0` |
| 3.16 | 2.86 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #8 from game+0x55de08c -> 0x0` |
| 3.17 | 2.86 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #2 from game+0x55de08c -> 0x0` |
| 19.13 | 19.15 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #3 from game+0x55de08c -> 0x0` |
| 25.94 | 25.64 | `QueryApiImpl XThreadingImpl iid 073b7dcb-1fcf-4030-94be-e3c9eb623428 #10 from game+0x55de08c -> 0x0` |
| 25.94 | 25.64 | `QueryApiImpl XAccessibilityImpl iid d722b373-8c4d-4692-9e51-d6ad9b37aa7d #1 from game+0x55de08c -> 0x0` |
| 25.94 | 25.64 | `XSpeechSynthesizerCreate #1 from game+0x55dced0 -> 0xffffffff80004005` |
| 25.95 | 25.65 | `QueryApiImpl XDisplayImpl iid 35f07670-706e-4bfb-9476-090798c5ebf3 #4 from game+0x55de08c -> 0x0` |
| 26.83 | 26.91 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #2 from game+0x55de08c -> 0x0` |
| 26.86 | 26.92 | `QueryApiImpl XGameImpl iid 2549f142-6419-4a06-97b5-931aab7c2f34 #1 from game+0x55de08c -> 0x0` |
| 26.86 | 26.92 | `QueryApiImpl XSystemImpl iid 1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf #1 from game+0x55de08c -> 0x0` |
| 26.86 | 26.92 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #3 from game+0x55de08c -> 0x0` |
| 26.86 | 26.92 | `QueryApiImpl XGameImpl iid 2549f142-6419-4a06-97b5-931aab7c2f34 #2 from game+0x55de08c -> 0x0` |
| 26.86 | 26.92 | `QueryApiImpl XSystemImpl iid 1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf #2 from game+0x55de08c -> 0x0` |
| 26.86 | 26.93 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #4 from game+0x55de08c -> 0x0` |
| 26.86 | 26.93 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #5 from game+0x55de08c -> 0x0` |
| 26.88 | 26.95 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #6 from game+0x55de08c -> 0x0` |
| 26.88 | 26.95 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #7 from game+0x55de08c -> 0x0` |
| 26.89 | 26.96 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #8 from game+0x55de08c -> 0x0` |
| 26.90 | 26.98 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #10 from game+0x55de08c -> 0x0` |
| 26.97 | 27.07 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #7 from game+0x55de08c -> 0x0` |
| 26.97 | 27.07 | `XUserDuplicateHandle #2 from game+0x55dcb8b -> 0x0` |
| 26.97 | 27.07 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #8 from game+0x55de08c -> 0x0` |
| 26.97 | 27.07 | `XUserDuplicateHandle #3 from game+0x55dcb8b -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XGameImpl iid 2549f142-6419-4a06-97b5-931aab7c2f34 #3 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XGameSaveImpl iid 1bfff3af-f14a-40a3-8e35-9ada906593f9 #1 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserDuplicateHandle #4 from game+0x55dcb8b -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid 01acd177-91f9-4763-a38e-ccbb55ce32e0 #10 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetId #2 from game+0x55dd09f -> 0x0` |
| 26.98 | 27.07 | `XUserGetLocalId #2 from game+0x55dd117 -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #1 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetGamertag #1 from game+0x55de49a -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #2 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetGamertag #2 from game+0x55de49a -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #3 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetGamertag #3 from game+0x55de49a -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #4 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetGamertag #4 from game+0x55de49a -> 0x0` |
| 26.98 | 27.07 | `XUserDuplicateHandle #5 from game+0x55dcb8b -> 0x0` |
| 26.98 | 27.07 | `XUserGetId #3 from game+0x55dd09f -> 0x0` |
| 26.98 | 27.07 | `XUserGetLocalId #3 from game+0x55dd117 -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #5 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `XUserGetGamertag #5 from game+0x55de49a -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XGameSaveImpl iid 1bfff3af-f14a-40a3-8e35-9ada906593f9 #2 from game+0x55de08c -> 0x0` |
| 26.98 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #6 from game+0x55de08c -> 0x0` |
| 26.99 | 27.07 | `XUserGetGamertag #6 from game+0x55de49a -> 0x0` |
| 26.99 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #7 from game+0x55de08c -> 0x0` |
| 26.99 | 27.07 | `XUserGetGamertag #7 from game+0x55de49a -> 0x0` |
| 26.99 | 27.07 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #8 from game+0x55de08c -> 0x0` |
| 26.99 | 27.07 | `XUserGetGamertag #8 from game+0x55de49a -> 0x0` |
| 26.99 | 27.08 | `XUserDuplicateHandle #6 from game+0x55dcb8b -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #4 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #4 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `QueryApiImpl XUserImpl iid cef4fac0-7676-4a94-a119-4c43f9eb5b74 #10 from game+0x55de08c -> 0x0` |
| 26.99 | 27.08 | `XUserGetGamertag #10 from game+0x55de49a -> 0x0` |
| 26.99 | 27.08 | `XUserDuplicateHandle #7 from game+0x55dcb8b -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #5 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #5 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `XUserDuplicateHandle #8 from game+0x55dcb8b -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #6 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #6 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #7 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #7 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `XUserDuplicateHandle #10 from game+0x55dcb8b -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #8 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #8 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `XUserGetId #10 from game+0x55dd09f -> 0x0` |
| 26.99 | 27.08 | `XUserGetLocalId #10 from game+0x55dd117 -> 0x0` |
| 26.99 | 27.08 | `XUserCloseHandle #2 from game+0x55dcbf4 -> 0x0` |
| 27.00 | 27.08 | `XUserGetIsGuest #1 from game+0x55de383 -> 0x0` |
| 27.00 | 27.08 | `XUserGetAgeGroup #1 from game+0x55de522 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #1 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #2 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #3 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #4 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #5 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #6 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #7 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #8 from game+0x55de5c1 -> 0x0` |
| 27.03 | 27.11 | `XUserCheckPrivilege #10 from game+0x55de5c1 -> 0x0` |
| 27.76 | 27.73 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #100 from game+0x55de08c -> 0x0` |
| 35.17 | 35.20 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #1000 from game+0x55de08c -> 0x0` |
| 109.83 | 109.85 | `QueryApiImpl XNetworkingImpl iid bf2346b2-39af-4658-b5ea-44713c7e83b3 #10000 from game+0x55de08c -> 0x0` |

In A the last platform line is the network poll print `QueryApiImpl XNetworkingImpl #10000` at boot
115.19 s; in B it is the same print at boot 109.83 s. **No platform call of any class is made after
115.2 s in either arm**, no `XUserAddAsync` / `XUserAddResult` sign-in sequence runs at all in either
arm, and no `XStore` or `XGameSave` slot call is made (B requests the `XGameSaveImpl` class twice at
27.0 s but calls no game-save slot).

## 7. Activity first seen after 130 s

| arm | new D3D12 call kinds | new user-trace methods | file opens | captures written | status-array #17 enqueue lines |
|---|---|---|---:|---:|---|
| A | none | methods: none | 0 | frame 5601 at 150.1 s, frame 8401 at 217.8 s, frame 11201 at 271.5 s | 6 lines at boot 284.01-284.44 s |
| B | none | methods: none | 0 | frame 5601 at 141.5 s, frame 8401 at 203.8 s, frame 11201 at 258.5 s | 6 lines at boot 144.54-144.74 s |

The new patterns are the captures and the late status-array enqueue bursts of section 4; no new
D3D12 call kind, no new user-trace method and no file open appears after 130 s in either arm.

## 8. Verdict

**Does the delivered signed-in user-change event move the title's post-sign-in state step? No.**

- The event is delivered and consumed: the registered callback runs the five follow-up user calls at
  1.78-2.10 s, and the title's full user-ready refresh runs at 26.97-27.11 s (all `S_OK`). Arm A
  makes none of those calls, so the difference is the switch's effect and not boot noise.
- The state words are unmoved: `0x7446510` stays 2, `0x98f828c` is 0 in all 148 samples, `0x7453ed0`
  stays 1, and the init/session windows are constant after the ordinary 1 -> 2 boot transition.
- The loader does not restart: no re-arm step in the race summaries, counters balanced, and the
  batch's printed calls stop at about 31 s in both arms. The +6 counted pairs of arm B's global
  object between 91 s and 181 s are not a restart and have no correlate in any other stream.
- No new DirectStorage request shape and no file open appear after the load (8,273 shapes, 8,192
  completions / 498.9 MiB, 121 files in both arms); the late status-array enqueues exist in both arms.
- The presented image is black in all ten captures (five per arm).

**Which measured signal is absent next:** the title-side transition that would take the game/init
word off 2 and the session word from 0 to 1. Everything this pair can measure on the platform side
during that path is answered on the successful path, and delivering the signed-in event - the
cheapest platform-side candidate - resolves and refreshes the title's user object without producing
that transition. The next useful measurement is therefore not another platform switch in this slice
but the title-side stage dispatcher after the 27 s user refresh (the same gate
[loading_gate.md](loading_gate.md) narrowed to the title's own dispatcher), or a boot in which the
title's own sign-in stage runs on the current runtime (section 9's baseline limit). No further switch
was added in this slice.

## 9. Limits

- **The baseline is the current in-flight binary, not the task-82 one.** This pair boots the runtime
  binary built at 13:50:06 while the concurrent display-window slice's change was uncommitted
  (SHA-256 `45DF506F3009A76156F40D9103B07EA63F811BE3BEFC3649D4540E497B1003BF`); that change was
  committed as `ebe181e14` at 14:00:18, inside arm B's boot, and no build happened during either
  boot. The change is active in the measured boots: the log records the display surface allocated
  through the title's own XMem hook (`[rt] d3d12_xs: slot 127: display-surface memory is allocated
  through the title's XMem hook ... 33177600 byte(s)`), and `int3_skipped` is 0 in both arms where
  the task-82 boot recorded the `game+0x247af2d` range check being skipped. This binary also does
  **not** run the title's own 115.14 s sign-in sequence that [story_load_probe.md](story_load_probe.md)
  measured on the older binary: neither arm calls `XUserAddAsync`, and both arms' platform surface
  ends with the network poll print at 109.8-115.2 s. The pair therefore answers the event question
  on the current runtime; it cannot literally re-measure a "post-sign-in" window, because on this
  binary neither arm reaches a sign-in. That sign-in difference belongs to the display-window
  change's own slice, not to the switch under test (the control lacks it too).
- **The two boots ran under different machine load.** Arm A's boot overlapped another slice's
  build (two test executables relinked at 13:53:06 and 13:53:09); arm B's overlapped that slice's
  commit only. A's stages are 4-5 s later (DirectStorage final 33.52 s vs 28.09 s; init transition
  23.08 s vs 19.10 s). The event-attributable difference (70 platform lines, all after the event,
  plus the 27 s refresh) is not a timing artifact: the control makes no user calls at all.
- **One pair, one machine, one arm each, own overlay and capture directories.** The load stage is
  timing-sensitive, which is exactly why the two arms are reported with their own measured times.
- The delivered event is **fabricated**: the offline user never changes, so `SignedInAgain` is the
  only transition compatible with the measured state. It is a switch, not an observed console
  transition; the title's `SignedOut` and `Privileges` branches are not exercised.
- **Probe limits.** Every 2 s a sample can miss a transient between samples; the 32-byte windows
  around the three words were sampled so a neighbouring state word would not be missed (none moves).
  Arm A's probe kept running after the kill because the per-arm script's teardown aborted on a
  non-zero `taskkill` exit before it killed the probe; from 306.99 s it read the process of the
  *next* boot that had already taken the boot lock, so the arm-a CSV's rows after 300 s (the
  `2 -> 0 -> 1` transitions in it) belong to another boot and are excluded here. Arm B's probe
  reported `title process gone` and stopped by itself; its in-window rows are clean. The same
  teardown abort is why the launcher's wall/exit summary line is absent from the two `monitor.log`
  files (the kill time is recorded there).
- **No input was injected.** This is the self-driven path only; a landing prompt a player accepts is
  not tested.
- The page contains RVAs, sampled integers, hex bytes of small windows, counters, HRESULTs, timings
  and image means only: **no game data and no package paths**.

## 10. Evidence and reproduction

Outside the repository, under `D:\leonida-work\userchange-probe`:

- `arm-a\boot` and `arm-b\boot` - per-arm `stderr.log`, `stdout.log`, `boot.jsonl`,
  `samples.csv` (1 Hz process/file-growth series), `monitor.log`, `arm_meta.txt` (HEAD, git status,
  `palmetto.exe` and shim hashes, the exact `PALMETTO_*` environment, boot wall clock),
  `analysis.json` / `analysis.txt` (the extracted series of this page).
- `arm-a\state_probe.csv` and `arm-b\state_probe.csv` - the external probe's rows, including the
  windows; `arm-a\capture` and `arm-b\capture` - the ten PPMs and the runtime's `index.txt`.
- `arm.ps1` (one arm: boot lock, probe, boot, kill, release), `state_probe.py` (the read-only
  probe), `analyze_arm.py` (per-arm extraction), `compare.py` (the arm comparison),
  `facts.py` / `statefacts.py` (the in-window statistics), `build_page.py` (this page). The probe's
  SHA-256 is recorded in each arm's `arm_meta.txt`.
- `compare.txt`, `doc-tables.md`, `facts` output and the two arm `analysis.txt` files hold the full
  platform lines (with interface ids and callers), the loader/DirectStorage/file series and the
  capture statistics quoted above.

Reproduce one arm: acquire `_setup\boot_lock.ps1 -Acquire -Who <name>`, start the probe
(`python -I state_probe.py --out <work>\state_probe.csv --seconds 330 --interval 2`), run one boot
with the environment of section 1 and `--timeout 300`, kill at 300 s, stop the probe and release the
lock. The second arm is the same with `--env PALMETTO_USER_CHANGE_EVENT=1`. No runtime change is
involved and no git write is made by this slice; the only file this slice writes in the repository is
this page (the checkout had another slice's changes while the pair ran, and it still had
`port/shims/xmem.cpp` modified by that slice when this page was written).
