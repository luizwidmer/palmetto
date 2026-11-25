# Long boot: what the title does between 130 s and 600 s

One 600 s boot under the boot lock, with the timeline, loader-hook and DirectStorage trace switches on and a
presenter capture every 60 s, measures what happens after the 130 s window we had been sampling. The public
fork's own report (its `docs/linux_proton.md`, the "What a healthy boot looks like" table) places its story
load at roughly 240-300 s after start and its end at roughly 540-630 s, so a 130 s boot is pre-load and this
boot was meant to cross both checkpoints.

**Short answer: nothing new appears.** The title's own work ends by ~30.8 s and never resumes: the loader's
last begin/end entry calls are at ~30.8 s, the loader status arrays stop changing by 24.0 s, the DirectStorage
trace's last request is enqueued by 7.4 s, all 51,786 graphics pipelines are converted by the first 30 s
marker, and none of it moves again. Between 130 s and 600 s the only log lines are the stationary frame
loop's own counters (submissions, presents, console calls, capture milestones, page-copy milestones) and
scheduled diagnostics. Ten presenter captures from frame 1 (31.9 s) to frame 54,001 (588.5 s) are uniformly
black, the cdb all-thread dumps at 120/240/300/360/480 s show the same 35 title-stack threads with the same
frames and the same 102 loaded modules, and no traced D3D12 call kind is first seen after 130 s. The fork's
240-300 s story-load onset and 540-630 s end therefore have **no correlate in this boot**: the 600 s boot is
still in the pre-content stationary two-pass loop that [frame_capture.md](frame_capture.md) measured at
300 s, 600 s and 1,200 s on the fork tree, and wall-clock time is not what advances it.

## 1. What ran

| | |
|---|---|
| Machine | Windows 11 Pro 10.0.26200, Core i9-13900K (24C/32T), GeForce RTX 4090, driver 32.0.16.1714, 63.7 GB RAM |
| Tree | `D:\leonida` at `a90cd9f98`, no local changes at start; `port/build/palmetto.exe` built 2026-11-25 10:23:56, SHA-256 `0570EFBC0E174945C8DF0223BF269E35F74AE66843FAEF3BB47F610E4F8BB6D7` |
| Boot | started 10:37:49, wall 600.2 s, killed by the harness timeout at 600 s (`taskkill /T /F`), started-process exit code 1 |
| Trace | 102,543 startup-trace events, **0 exceptions** |
| Capture window | 10 presenter captures, presented frames 1..54,001, i.e. 31.9 s to 588.5 s |
| cdb dumps | 5 of 5, exit 0 each, 3.0-5.9 s per attach (23.4 s of paused process in the 600 s) |
| Boot lock | `_setup/boot_lock.ps1 -Acquire -Who longboot`, released after the kill |
| Log size | 141.6 MB stderr, 104.2 MB of captures |

Environment: the standard set (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus the measurement switches this task names —
`PALMETTO_PIPELINE_TIMELINE=30`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_DSTORAGE_TRACE=1`,
`PALMETTO_DS_ARRAYS=1` — and `PALMETTO_CAPTURE` with `PALMETTO_CAPTURE_EVERY=6000`,
`PALMETTO_CAPTURE_LIMIT=12`. No runtime change of any kind; every switch is off by default and logs only.
The boot was launched exactly as `tooling/boot_report.py` does (`--run`, `--game-dir`, `--overlay`,
`--trace`, `--trace-d3d12`) so the run is comparable with the 130 s boots, and `PALMETTO_CAPTURE_EVERY=6000`
is ~60 s at the measured ~100 presents/s.

## 2. The per-30 s series

Counters are from `PALMETTO_PIPELINE_TIMELINE=30` (the printed `+N s` mark is the thread's own nominal time;
each line lands ~3.7 s later in boot seconds) and from the interval's other lines. `d3d12` is the
`[d3d12-trace]` call count in the interval, `loader` the loader begin/end entry calls, `DS req` the
DirectStorage requests the shim's shape counter has seen (`shapes file=4199`), `caps` the presenter captures
(their mean channel value), and `milestones` the frame loop's own bounded milestone prints. The pipeline
population is frozen from the first row: graphics 51,786, slot-57 records 10,241 converted / 0 refused,
slot-55 derived calls 41,545, compute 95 calls / 95 created.

| t (s) | submits | presents | console | threads | DS req | loader | caps | d3d12 | milestones |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 30 | 322 | 160 | 108,887 | 90 | 4,199 | 7 begin/7 end | 0 | 38,227 | loader burst 6.4-8.5 s; all DS requests enqueued by 7.4 s |
| 60 | 6,421 | 3,210 | 154,594 | 86 | 4,199 | 12 begin/12 end (at ~30.8 s) | 1 (0.00) | 117,450 | last loader/queue burst ~30.8 s; first present 31.9 s (frame 1) |
| 90 | 12,453 | 6,226 | 199,848 | 85 | 4,199 | 0 | 0 | 117,496 | — |
| 120 | 18,090 | 9,045 | 242,139 | 85 | 4,199 | 0 | 1 (0.00) | 116,796 | loader race summary printed 90.3 s |
| 150 | 23,678 | 11,838 | 284,056 | 85 | 4,199 | 0 | 0 | 93,958 | cdb dump 120.2-126.1 s; `PresentX call 10000` at 134.8 s |
| 180 | 29,548 | 14,773 | 328,095 | 85 | 4,199 | 0 | 1 (0.00) | 111,975 | — |
| 210 | 35,690 | 17,844 | 374,176 | 83 | 4,199 | 0 | 0 | 115,387 | race summary printed 180.3 s; first page-copy milestone 199.3 s |
| 240 | 41,191 | 20,595 | 415,450 | 83 | 4,199 | 0 | 1 (0.00) | 114,508 | — |
| 270 | 47,451 | 23,725 | 462,415 | 83 | 4,199 | 0 | 0 | 109,123 | cdb dump 240.2-243.2 s |
| 300 | 52,790 | 26,394 | 502,464 | 83 | 4,199 | 0 | 1 (0.00) | 113,794 | race summary printed 299.5 s |
| 330 | 58,479 | 29,239 | 545,148 | 83 | 4,199 | 0 | 0 | 96,586 | cdb dump 300.1-304.7 s |
| 360 | 63,274 | 31,636 | 581,119 | 83 | 4,199 | 0 | 1 (0.00) | 103,521 | — |
| 390 | 69,014 | 34,506 | 624,182 | 83 | 4,199 | 0 | 0 | 95,993 | cdb dump 360.1-365.1 s; second page-copy milestone 376.9 s |
| 420 | 75,358 | 37,678 | 671,779 | 83 | 4,199 | 0 | 1 (0.00) | 119,759 | — |
| 450 | 81,581 | 40,790 | 718,467 | 83 | 4,199 | 0 | 0 | 118,858 | — |
| 480 | 86,904 | 43,451 | 758,399 | 81 | 4,199 | 0 | 1 (0.00) | 114,349 | — |
| 510 | 92,737 | 46,368 | 802,163 | 82 | 4,199 | 0 | 0 | 98,431 | cdb dump 480.1-485.0 s |
| 540 | 98,920 | 49,460 | 848,555 | 82 | 4,199 | 0 | 1 (0.00) | 116,688 | — |
| 570 | 105,168 | 52,583 | 895,424 | 82 | 4,199 | 0 | 0 | 119,768 | third page-copy milestone 540.6 s |
| 600 | killed | — | — | 82 | 4,199 | 0 | 1 (0.00) | 111,959 | last capture 588.5 s (frame 54,001); last replay milestone 106,496 at 579.9 s |

Steady state, read off the table: ~5,700-6,300 submissions, ~2,900-3,100 presents and ~41,000-47,000 console
calls per 30 s; ~96,000-120,000 traced D3D12 calls per 30 s (lower in the intervals that contain a cdb
attach, which pauses the process); working set 6.5 GB and private 12.1 GB, flat from +30 s to the kill;
threads 90 at +30 s drifting down to 82 (host threads exiting, see section 4). Nothing in the pipeline,
loader or DirectStorage columns changes after the ~30.8 s boundary.

## 3. Loader and DirectStorage: the last activity is at ~30.8 s

**DirectStorage.** `PALMETTO_DSTORAGE_TRACE=1` records the whole request stream: 4,199 requests, all
file-source, all zlib, all "source up to 64 KiB, destination up to 64 KiB" file→buffer copies. The factory is
served at 2.14 s, the first request is enqueued at 6.38 s, and the advanced line prints `shapes file=4199
zlib=4199` from 7.43 s onward: **every request is enqueued by +7.43 s**. Completions print at 1,024 / 2,048 /
3,072 / 4,096 requests at boot 7.43 / 7.43 / 8.48 / 8.48 s; the advanced counters show every opt-in mechanism
inert on this path (`memory=0 bcpack=0 swizzle=0 dest-pages=0 staging=0 pins=0 unshuffle=0 pages planned=0
predictor=off`). The queue's submit lines (bounded to 8 per site) are 43 entries at 6.38 s, 7 + 2 + 475 +
3,677 at 7.43 s, 2 at 23.36 s and 2 + 6 at ~30.8 s; the later batches are status entries — the request shape
count does not grow after 7.43 s.

**Loader status arrays.** `PALMETTO_DS_ARRAYS=1` dumps every live status array 150 times over the first
~76 s. There are 17 arrays, created at +0.000 to +0.011 s; only #17 and #3 are ever used and the other
15 are never enqueued to. Array #17 is where the load spinner polls: **31,200,161 polls** in total
(3,534,805 at +0.254 s — 18.8M polls/s during the submission burst — 31,116,930 by +16.4 s, 31,200,161 by
+24.0 s, and exactly that value at every dump through the last one at +75.9 s), with 14 enqueues, 14
completions and 0 pending. Array #3: 1 enqueue, 10,009 polls, 0 pending. So the title's own status polling is
over by +24.0 s.

**Loader begin/end detours.** `PALMETTO_LOADER_HOOK=1` installs 5 detours at 1.09 s (verified at 5.33 s) and
prints per-call state. In the whole 600 s: **19 begin calls and 19 end calls**, and the completion entry
called **exactly once** (first hit at 6.38 s, caller `game+0x43a492c`); the spinner entry **never fires**
(the four race summaries all report `spinner depth 0, last spinner tid 0 object 0x0`). The per-call states
(`armed` / `in-flight` from the loader object) are the load's shrinking batches: `in-flight 42` then 6, 1,
474, 3,676 in the 6.4-8.5 s burst, always `end … armed 1 in-flight 0`; one pair at 23.36 s; a last burst of
**12 begin/12 end at ~30.8 s** on four different loader objects. Every call is balanced (`begin = end` on
every object, 0 cross-thread ends). After that burst the loader object counters stay
`begin/end = 1, 16, 1, 1` at every later race summary — i.e. **no loader entry call after ~30.8 s**.

## 4. The title's threads: cdb all-thread dumps at 120/240/300/360/480 s

Each dump is `cdb -p <pid> -c "~*k; .detach; q"`; the attach suspends the process for 3.0-5.9 s (a limit,
section 8). The title is mapped by palmetto, so its frames have no symbols and appear as raw VAs; they are
reported here as RVAs from the image base.

| boot time | threads | top frame is a wait | threads with title-image frames | top frame is title code | same 102 modules |
|---:|---:|---:|---:|---|---|
| 120.2 s | 86 | 83 | 35 | `2d0` at title+0x4E8AA90 | yes |
| 240.2 s | 84 | 80 | 35 | none in this sample | yes |
| 300.1 s | 84 | 81 | 35 | `2d0` at title+0x4FB0AF2 | yes |
| 360.1 s | 84 | 80 | 35 | `e62c` at title+0x4D9F72F | yes |
| 480.1 s | 82 | 77 | 35 | `2d0` at title+0x4FB0C00 | yes |

Host-side frame groups are the same at all five dumps: 33-34 threads whose first non-system frame is title
code (blocked in a system wait called from the title), 17-18 waiting in the runtime's own condition variable
(`MSVCP140!_Cnd_wait`, the shim-side workers), 5-8 with only system frames, 6-8 NVIDIA driver threads
(`nvoglv64`), 6 GameInput threads, 4 waiting on a raw non-title range, the runtime's entry thread, and the two
`palmetto` `sleep_until` threads (the pipeline-timeline and DS-arrays samplers).

The important part is what is *not* there: the 35 threads with title-image frames are the **same thread IDs at
all five dumps**, and 31 of the 35 have an identical title-frame RVA sequence at 120/240/300/360/480 s (the
other 4 differ only at the frame that happened to be executing). The single thread whose top frame is title
code is a different TID/RVA in each sample (`2d0`, none, `2d0`, `e62c`, `2d0`, at four different RVAs), so it
is whichever thread was executing at the instant of the attach, not a fixed spinner. Thread count falls
86→82 because host-side (driver/worker) threads exit; the title's 35-thread working set does not change, and
the loaded-module list is identical (102 modules) at every attach, so no module is loaded after 120 s.

## 5. The presented image

`PALMETTO_CAPTURE` wrote 10 captures, one per 6,000 presented frames (the capture is bound to presented
frames, so intervals are 58-67 s in boot time):

| capture | presented frame | boot time | extent | mean rgb | non-black | peak |
|---:|---:|---:|---|---:|---:|---:|
| 1 | 1 | 31.9 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 2 | 6,001 | 91.3 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 3 | 12,001 | 155.9 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 4 | 18,001 | 215.1 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 5 | 24,001 | 276.3 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 6 | 30,001 | 342.9 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 7 | 36,001 | 407.7 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 8 | 42,001 | 466.0 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 9 | 48,001 | 530.0 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |
| 10 | 54,001 | 588.5 s | 2564x1421 | 0.00 0.00 0.00 | 0 / 3,643,444 | 0 |

The runtime's own `index.txt` lines and an independent read of every PPM agree: every capture is uniformly
black, 0 non-black pixels of 3,643,444 and peak 0. The captured extent is the swapchain the title presents
(2564x1421), not the composed 4K target.

## 6. Phase verdict against the fork's published timeline

| fork's healthy-boot table | Linux machine | Windows A machine | this 600 s boot (RTX 4090) |
|---|---|---|---|
| package splash | 0-10 s | — | startup trace only |
| launch movie, landing prompt | ~60-90 s | — | not seen: the first presented frame is at 31.9 s and is already the black loading loop, and no capture or log line names a movie or a prompt |
| initialisation done | ~180 s | ~70 s | loader + DirectStorage bursts finish 6.4-8.5 s; 51,786 pipelines by the +30 s marker |
| story load starts (slideshow with artwork) | ~240-300 s | ~110 s | **no change at 240-300 s**: DS requests still 4,199 (all enqueued by 7.4 s), pipelines still 51,786, first capture after 240 s is frame 24,001 at 276.3 s and black |
| story load ends (the fork reports ~38,000 streamed images registered), world phase | ~540-630 s | ~190 s | **no change at 540-630 s**: capture at 588.5 s (frame 54,001) is black; the window's last lines are the replay submission milestone at 579.9 s and that capture; loader/DS/pipeline counters unchanged |

The boot is in exactly one measured phase from 31.9 s to the kill: the stationary two-pass blit/clear loop
that [frame_capture.md](frame_capture.md) describes ("what reaches the swapchain is the same two-pass loop
over the same 4K target"). The title's own work — the streaming load, the pipeline conversion, the loading
manager's status polling — is complete by ~30.8 s and does not restart; after that the only growth is the
frame loop's linear counters. Wall-clock time is therefore not what gates the fork's story-load phase in this
build: 568 s of the 600 s window pass with the loader, DirectStorage, pipeline and thread evidence unchanged,
even though this boot's own initialisation-equivalent stages finish faster than either fork machine's.

Two readings are compatible with the measurement, and this boot cannot separate them: (a) the fork's
240-300 s story load is gated on something our port does not satisfy — the fork's healthy-boot table shows a
landing prompt, and the only input work measured here was the synthetic pad of [input_gate.md](input_gate.md),
whose presses were all before 40 s; or (b) the fork's timeline belongs to its own runtime, which has advances
this port does not carry, and the phase does not exist here yet. What the measurement does settle is the
negative: no timer, subsystem or thread in the title starts the story load between 130 s and 600 s on its
own.

## 7. The first new activity after 130 s

The whole boot logs 61 `[rt]` lines after 130 s, and every one of them belongs to a category already seen
before 130 s:

| after 130 s | lines | what it is |
|---|---:|---|
| replay submission milestones | 22 | every 4,096 submissions (24,576 at 137.9 s ... 106,496 at 579.9 s boot time) |
| pipeline timeline | 15 | the 30 s sampler |
| loader race-summary body lines | 10 | the diagnostic's own 30/60/90/120 s prints (see the label quirk in section 8) |
| presenter captures | 8 | frames 12,001 ... 54,001, all black |
| page-content copy milestones | 3 | 65,536 pulls at 199.3 s, 131,072 at 376.9 s, 196,608 at 540.6 s (136.5 / 273.0 / 409.5 MiB) |
| `PresentX` bounded print | 3 | the `PresentX call 10000` milestone at 134.8 s (planes/params/resource lines) |

So the first genuinely *new* prints after 130 s are the frame loop's own bounded milestones, and their trigger
is simply the loop running: the present counter reaching 10,000 (`PresentX call 10000`, +134.8 s) and the
replay's page-content counter reaching 65,536 title-page pulls (`page content: 65536 pull(s), 136.5 MiB
copied`, +199.3 s — the replay copying descriptor-table buffer pages into its arena stand-ins, ~2 pulls per
submission, steady to +540.6 s). Neither is title-side progress: no traced D3D12 call kind is first seen
after 130 s (37 distinct call names in the boot, all present before 130 s), no module is loaded after 120 s,
no thread is added, no pipeline is created and no DirectStorage request is enqueued.

The measured negative, stated as numbers: the last loader entry call is at ~30.8 s, the last status-array
change at 24.0 s, the last DirectStorage request enqueue at 7.43 s, the last DS queue submit at ~30.8 s
(bounded log; the array dump closes that gap to +75.9 s and the loader counters to +299.5 s), and from 31.9 s
to 588.5 s every one of 10 captures is black. The title did not do anything new for the last 569 s of the
boot.

## 8. Limits

- **The cdb attaches pause the title.** Each attach suspends the process for 3.0-5.9 s (23.6 s over five
  dumps); the traced-call counts dip in the intervals that contain one (e.g. 93,958 in 120-150 s against
  ~110,000-120,000 typical). The dumps are the task's measurement, not part of the steady state.
- **`PALMETTO_DSTORAGE_TRACE`'s per-site lines are capped at 8.** The last logged submit (30.8 s) is not by
  itself proof of the last submit; the status-array dump (frozen from 24.0 s to 75.9 s) and the loader
  object counters (unchanged at summaries printed at 90.3/180.3/299.5 s) are what close the gap. After
  299.5 s the loader-hook print cap (200 calls per site, 19 used) means further begin/end calls would still
  have been logged; none were.
- **`PALMETTO_DS_ARRAYS` is bounded by design** (150 dumps, 250 ms for the first 25 s then 1 Hz, ~76 s
  total), so the array state is only observed to +75.9 s.
- **The loader race-summary labels are the diagnostic's own numbers, not boot seconds.** Its four summaries
  are labelled "at 30/60/90/120 s" but print at boot 30.8, 90.3, 180.3 and 299.5 s (the sleep accumulates);
  they were used here only for their per-object counters, which are boot-time independent.
- **The pipeline timeline prints late.** The `+N s` mark is the sampler thread's nominal time; each line lands
  ~3.7 s later in boot seconds (first present 31.9 s, `+30 s` line at 33.7 s). The series uses the nominal
  time; the cdb/DS events use the byte-offset-mapped boot time (±1 s).
- **One boot, one machine, one overlay.** Frame rate and thread count drift (90→82) are host variance: driver
  and worker threads exit, and the title's 35-thread set does not. The boot's own first present is at 31.9 s,
  ~12 s later than a 130 s `boot-timeline1` boot with fewer switches (its sampler already had 1 present at the
  +20 s mark); that shifts the series but not the verdict, because nothing happens after the load in either
  boot.
- The 130 s baseline quoted in the task (loader busy to ~16.5 s, all 51,786 pipelines by ~25 s) is this
  tree's short-boot shape; this boot's own load burst is 6.4-8.5 s and its pipeline population is complete at
  the first 30 s marker. Both end far before 130 s, which is the point of the long boot.
- A 600 s window cannot exclude a phase that would start after 600 s. It can exclude the fork's 240-300 s and
  540-630 s checkpoints, and it does.

## 9. What the observations ask for next

- **An input scripted after the loop is running.** The fork's healthy-boot table shows a landing prompt that a
  player accepts, and our synthetic-pad work injected presses at 10-20 s and 30-40 s only. The next
  measurement is one 600 s boot with `PALMETTO_FAKE_INPUT` (the documented switch,
  [input_gate.md](input_gate.md)) pressing A at, say, 200 s and 400 s, and the same series: does the loader
  re-arm (begin/end counters), do DS requests resume, does the pipeline population grow, do captures leave
  black? A negative there makes the "the gate is not input" statement measured rather than inferred.
- **The title-side stage dispatcher.** The native stacks are stationary (section 4) and the platform calls are
  excluded ([loading_gate.md](loading_gate.md)), so the next stack work is the guest/emulated stack of the
  stage dispatcher, as [streamed_consumer.md](streamed_consumer.md) section 9 names — a stack/RVA series over
  130-600 s rather than a single sample.
- **Capture cost.** One 130 s boot with the capture off, same switches otherwise, to attribute the ~10 s later
  first present to the capture staging rather than to the loader hook or DS arrays; the phase verdict does not
  depend on it.

## 10. Evidence and reproduction

The boot's own files (outside the repository): `D:\leonida-work\boot-long600` — `stderr.log` (141.6 MB),
`stdout.log`, `boot.jsonl` (102,543 events, 0 exceptions), `samples.csv` (1 s process/file-growth series),
`monitor.log`, `analysis.json` / `analysis.txt` (the extracted series and pattern analysis),
`events_loader.txt` / `events_dstorage.txt` (the per-event timeline), `cdb\dump-120.log` ... `cdb\dump-480.log`
with `cdb\summary.txt`, `cdb_summary.json` and `dumps.csv`; captures in `D:\leonida-work\capture-long600`
(`frame_00001.ppm` ... `frame_54001.ppm`, `index.txt`) and their statistics in `capture_stats.txt`.
The run script is `D:\leonida-work\long_boot600.ps1`; the analysis scripts are
`D:\leonida-work\analyze_long600.py`, `analyze_cdb.py`, `analyze_captures.py` and
`extract_long600_events.py`.

Reproduce: acquire `_setup/boot_lock.ps1 -Acquire -Who <name>`, run one boot through
`tooling/boot_report.py` with `--timeout 600` and the environment of section 1, take the five cdb dumps with
`cdb -p <pid> -c "~*k; .detach; q" -logo <file>`, then release the lock. No runtime change is involved.
