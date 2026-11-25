# State machine: the loading stage chain, its three words, and the transition that is missing

**Status: decompiled and measured.** The title's init/loading state machine is reconstructed from the mapped image
with the local Ghidra workflow (24 functions decompiled, plus static reference scans) and measured in one 130 s boot
with an opt-in in-process probe. The three tracker words are **not** three independent gates: they are

* `0x7446510` - the **game/stage state** word (1 = initial stage, 2 = init/load stage, 3 = running; 4..6 = later
  stages). Its only writer is `FUN_1453EC0C0`, and every write is a stage transition executed by the state-machine
  pump `FUN_1453EBE50` / executor `FUN_1453EBFE0`.
* `0x7453ED0` - the **loading/streaming subsystem state**, not a progress counter. 0 = stopped/idle, **1 = subsystem
  initialised and idle** (set once by `FUN_1422571E0`), 2..5 = a load is running (started by `FUN_141D67570`). The
  measured value 1 is the healthy pre-story value; it does not mean "loading in progress".
* `0x98F828C` - the **session/online state** dispatched by `FUN_141D83960` (states 0..16; 0 and 10 are "nothing to
  do"). No writer of this word exists in the image's static code: state 0 is the offline value.

The stage chain is linear: the stage-1 entry handler ends by requesting stage 2, and the stage-2 entry handler
`FUN_14081B270` runs the boot sequence `FUN_1421B8960` and then ends by requesting stage 3 with
`call 0x813660` at **RVA `0x81B2EF`**. Inside that boot sequence a wait loop at **`0x21B8A3E`** blocks until the byte
`0x71B9048` equals 2 (`FUN_145EA6930`, called at `0x21B8A43`); that byte is written **only** by the boot-media
module constructor `FUN_145BF9A40` at **RVA `0x5BF9CE0`**, which `FUN_14220FCA0` calls during module initialisation.
The named missing transition is the last line of this chain - the request to stage 3 (`0x81B2EF`) being executed by
the pump - and the probe added in this slice distinguishes "still inside the boot wait" from "stage 3 requested but
never executed" by sampling the state-machine object's current (`+0x10`) and requested (`+0x12`) stage.

Everything below is RVA, counter, state number and API-name output only; no game bytes, package paths or shader data.

## 1. What ran

| | |
|---|---|
| Tree | `D:\leonida` at HEAD `5fe9094af` plus this slice's `port/src/main.cpp` probe (uncommitted), built `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` |
| Decompiler | Ghidra 12.1.4 PUBLIC headless at `D:\tools\ghidra_12.1.4_PUBLIC`, project copy under `D:\leonida-work\state2\ghidra-proj` (the previous session's `slotmap` project for `game_scarlett_master.exe`, imported `-noanalysis`); the external workflow is `D:\leonida-work\rea-tooling` (`DecompileRanges.java`, `rva2func.py`, `find_xrefs.py`) |
| Functions decompiled | the transition writer + executor + pump, the six stage entry/exit handlers, the boot sequence `FUN_1421B8960` and the boot-media module (`FUN_14220FCA0`, `FUN_145BF9A40`, its tick `FUN_142213...`/`0x313560`, the predicate), the loading subsystem (`FUN_1422571E0`, `FUN_1422572D0`, `FUN_14011F740`, `FUN_141D67570`, `FUN_141D5D040`), the session dispatcher `FUN_141D83960` |
| Static scans | all RIP-relative references to the three words and to 64 bytes around them, and to the boot-media gate; direct-call caller graphs; absolute-pointer search for the three addresses (none: all three are only reached RIP-relatively) |
| Boot | one 130 s boot under `_setup\boot_lock.ps1 -Acquire -Who state2`, standard environment plus `PALMETTO_PIPELINE_TIMELINE=30` and `PALMETTO_STATE_TRACE=2,130`; harness `tooling/boot_report.py`, package and overlay outside the repository; verdict `ALIVE 130.3 s`, 0 exceptions, 223,820 D3D12 calls recorded |

## 2. The game/stage state machine (`0x7446510`)

The word is a 32-bit enum. Every write goes through `FUN_1453EC0C0` (RVA `0x53EC0C0`), which is also the only code
that reads the word back:

```c
void FUN_1453ec0c0(int new_state) {          // RVA 0x53EC0C0
  if (DAT_147446510 != new_state) {          // 0x7446510
    DAT_147446508 = FUN_144e4ba80();         // 0x7446508: title tick (ms) of the change
    DAT_147446510 = new_state;
  }
}
```

The only caller is `FUN_1453EBFE0` (RVA `0x53EBFE0`), the transition executor: it calls the writer and then runs the
entered/exited stage handler. Its per-stage handlers, in the `param_4 == 1` (enter) direction, are:

| stage | enter handler | what it does |
|---:|---|---|
| 0 | `func_0x140810f00` / `func_0x1408163b0` | start/stop the front end |
| 1 | `FUN_14081B200` (RVA `0x81B200`) | runs `func_0x14222D390(0)`, `func_0x1422161F0`, `FUN_142240DA0`, then **requests stage 2**: `call 0x813660` at `0x81B25A` |
| 2 | `FUN_14081B270` (RVA `0x81B270`) | runs `FUN_1421B7A40`, `func_0x14222D390(2)`, registers the deferred `FUN_1421B8940` when `func_0x141C1ACE0()` is 0, then runs the boot sequence `FUN_1421B8960` and **requests stage 3**: `call 0x813660` at `0x81B2EF` |
| 3 | enter `FUN_140812620` (RVA `0x812620`), exit `FUN_14081AFB0` (RVA `0x81AFB0`) | the running stage; its exit handler waits on the loading subsystem event `0x7453ED8` when the loading flags at `0x7453ED4` say a load is in flight |
| 4 | `FUN_1421B68D0` (RVA `0x21B68D0`) | resets the loading object and sets the machine's "next" field to 2 |
| 5 | `FUN_1421B6910` (RVA `0x21B6910`) | same, "next" = 6 |
| 6 | `func_0x1421B6950` (RVA `0x21B6950`) | full subsystem teardown |

The executor is reached through the state-machine object's vtable slot 1, run by the pump `FUN_1453EBE50` (RVA
`0x53EBE50`; loop body `FUN_1453EBEA0`). The pump compares the object's current stage at `+0x10` with the requested
stage at `+0x12` and, when they differ, calls the executor for the request. Three code paths call the pump - a UI
module (`FUN_14073C950`), the story module (`FUN_140810E20`, which walks a registry at `0x147134ED8`/`0x147134F08`
and calls the pump for every registered device) and an event/handler registry (`FUN_141D66670`) - and those are the
only code that can execute a requested stage change.

`fn_813660` (RVA `0x813660`) is the shared tail that **writes the request**, not the change itself:

```c
void fn_813660(longlong object, ushort next_stage) { *(ushort *)(object + 0x12) = next_stage; }
```

So a stage request is a store into the state-machine object; the change of `0x7446510` happens only when the pump is
run again and the executor applies it. This is the structural reason a stage can be "requested but not entered" and
is what the probe's `machine current/next` line measures (the object pointer lives at image RVA `0x71273C8`; the
stage-2 entry handler stores it there).

## 3. The stage-2 boot sequence and its wait (`FUN_1421B8960`, RVA `0x21B8960`)

`FUN_14081B270` calls `FUN_1421B8960` (at `0x81B2E3`). In order, that function initialises the streaming/loading
subsystem (`FUN_1422571E0` at `0x21B8994`, which sets loading = 1), registers step callbacks, and then **blocks**:

```c
// RVA 0x21B8A3E
do {
  func_0x140313550();                        // boot-media tick (RVA 0x313550 -> 0x313560)
  cVar1 = func_0x145ea6930();                // RVA 0x5EA6930: return DAT_1471b9048 == 2;
  if (cVar1) break;                          // exit at 0x21B8A58
  func_0x14092eca0(1);                       // Sleep(1) per attempt
} while (true);
```

The predicate is a single byte: **`0x71B9048 == 2`**. Its only static writer is `FUN_145BF9A40` (RVA `0x5BF9A40`),
the constructor of the boot-media/first-run module, at **RVA `0x5BF9CE0`**:

```asm
0x5bf9cd9: mov qword ptr [rip+...], rax   ; 0x71B9040
0x5bf9ce0: mov byte  ptr [rip+...], 2     ; 0x71B9048 = 2
```

`FUN_145BF9A40` is called from `FUN_14220FCA0` (RVA `0x220FCA0`, the module's initialiser) at call site `0x220FEBF`.
The same module's tick (`FUN_140313560`, called through the thunk at `0x313550`) only does work while
`0x71B90AD == 1 && 0x71B90AC == 1`; it drives the boot media/UI present path and nothing else writes the gate byte.
The rest of `FUN_1421B8960` after the loop (`0x21B8A58` onwards) registers more boot pieces, tests the loading state
with `FUN_144F862D0` (returns `loading != 0`) and, **only if the loading state is 0**, starts a load with
`FUN_141D67570(1)` at `0x21B8ABB`; then `FUN_14081B270` requests stage 3.

Two more blocking forms live in the same function: four gated loops
`while (func_0x1423CB280() == 0) { Sleep(1); ... }` inside the block that runs when the platform byte
`0xA210D81 == 1` (the block at `0x21B8A20`-`0x21B8AD1`). The probe samples that gate too.

So the stage chain has exactly two ways to stay at 2: the boot wait `0x71B9048 == 2` never becomes true (or the
tick/loop never runs), or the request to stage 3 is made and the pump is never run again. The probe's
`machine current/next` and `boot-media gate` fields separate them in one boot.

**Measured: the wait is not the block.** The boot-media gate is 2 from 4 s (section 6), 18 s before stage 2 is
entered, and the requested stage is still 2 at the kill (section 9). So the loop at `0x21B8A3E` - if it is reached at
all - exits on its first iteration and the stage-2 entry handler is still running when the machine is killed: the
block is between the loading init at `0x21B8994` (which did run, loading = 1 at 24 s) and the request at `0x81B2EF`.

## 4. The loading/streaming state machine (`0x7453ED0`)

The word's only writer is `FUN_1422572D0` (RVA `0x22572D0`), which also calls the notification hook
`FUN_145B5D3D0(old, new)`:

```c
void FUN_1422572d0(int new_state) {          // RVA 0x22572D0
  if (DAT_147453ED0 != new_state) {
    FUN_145b5d3d0(DAT_147453ED0, new_state);  // notify subscribers
    DAT_147453ED0 = new_state;
  }
}
```

Its four callers are the whole life cycle:

| caller | site | transition |
|---|---|---|
| `FUN_1422571E0` (subsystem init, RVA `0x22571E0`) | `0x2257209` | -> **1** ("initialised, idle"); also sets `0x7453EBD = 1`, creates the loading event `0x7453ED8`, registers the task `FUN_144FD1AD0` |
| `FUN_141D67570` (start a load, RVA `0x1D67570`) | `0x1D675C7` | 0 -> **mode** (1..5); requires state 0; sets `0x7453EBD = 1`, `0x7453EC0 = QPC` |
| `FUN_141D5D040` (stop a load, RVA `0x1D5D040`) | `0x1D5D188` | any -> **0**; tears down `0x7453F10`, clears `0x7453F18` |
| `FUN_14011F740` (the poll task, RVA `0x11F740`) | `0x11F78A` | 1 -> **2**, only when `0x7453EBD == 0`; the registered task `FUN_144FD1AD0` (`0x4FD1AD0`) calls this poll |

The per-frame update `FUN_142257700` returns immediately unless the flags byte at `0x7453ED4` has bit 0, and the
loading state 3/4/5 branches of `FUN_141D5D040` run the end-of-load path (`FUN_142257DA0`). The story load itself is
started from the *stage that comes after this one*: `FUN_142754DC0` (RVA `0x2754DC0`) first stops any load
`FUN_141D5D040(1)`, waits on the boot update `FUN_145B2AC00`, then starts `FUN_141D67570(mode)`, where `mode` comes
from the title's own map/level record (`[object+0x84]` read in `FUN_142754D40`). The healthy sequence is therefore
`1 -> 0 (stop) -> 3/4/5 (load) -> 0 (stop)`, and the measured **1 is the idle value before the story load** - not a
stalled load.

## 5. The session state (`0x98F828C`)

`FUN_141D83960` (RVA `0x1D83960`) is a dispatcher that loops while the word changes, with a handler per state:

| state | handler |
|---:|---|
| 0, 10 | nothing (return) |
| 1 | `func_0x14582a8d0` |
| 2 | `func_0x1421b7060` |
| 3 | `FUN_1421b7070` |
| 4 | `FUN_1421b71a0` |
| 5 | `FUN_1421b71e0` |
| 6 | `FUN_1421b7200` |
| 7, 8 | `FUN_1421b7240` |
| 9 | `func_0x1421b7300` |
| 11 | `FUN_1421b7310` |
| 12 | `FUN_1421b7430` |
| 13 | `FUN_1421b7530` |
| 14 | `FUN_1421b7620` |
| 16 | `func_0x1421b7670` |
| other | `func_0x141ac7760(0)` |

It is called once from the story-stage boot `FUN_1421B6CA0` (RVA `0x21B6CA0`, call site `0x21B6CBC`) and once from
`0x5B69660`. The word itself has **no writer anywhere in the image**: a scan of every RIP-relative reference into the
`0x98F8280` structure (the whole page, 1,849 candidate sites) plus a dedicated immediate-store scan (`mov
byte/dword/qword [rip+disp32], imm`) finds only reads of `0x98F828C` - the dispatcher's own compare/load and
`FUN_1457D1FF0`'s load - and no absolute pointer to the word exists in the image either. The two byte stores near
it are `0x21B78C5`/`0x21B78D1` (writing 1 then 0 to the flags byte `0x98F8288`, in the story-stage boot cluster), not
the state word. In an offline boot the value 0 is therefore the "no session requested" state set at module
initialisation, and the dispatcher's own 0/10 branches are no-ops. This word is a read-only observation in the
measured boot, not the gate that keeps the loading stage open; the story-stage boot only calls its dispatcher once.
The companion word at `0x98F8290` is the constant `0x466731D0` (1,181,168,080) in every sample - the same value
[story_load_probe.md](story_load_probe.md) recorded as the session-adjacent word, so the two measurements agree on
that structure.

## 6. Measured timeline (one 130 s boot)

`PALMETTO_STATE_TRACE=2,130` samples the words every 2 s, and every sample also censuses the threads that hold
title-image frames (their RIP when running title code, else their nearest title stack word) and prints the full
title stack of a thread whose stack names one of the state-machine owners (section 8).

| measure | value |
|---|---|
| boot | 130 s, killed by the harness timeout (`ALIVE 130.3 s`, 0 exceptions, 223,820 D3D12 calls recorded); `PALMETTO_OK` build |
| `0x7446510` (stage) | 0 -> **1 at 4 s**, 1 -> **2 at 22 s**, then 2 to the kill; never 3 |
| `0x7453ED0` (loading) | 0 -> **1 at 24 s** (inside `FUN_1421B8960`'s `FUN_1422571E0` at `0x21B8994`), then 1 to the kill; never 0 or 2..5 |
| `0x98F828C` (session) | 0 in every sample |
| boot-media gate `0x71B9048` | 0 -> **2 at 4 s** (the module constructor `FUN_145BF9A40` runs early), then 2 to the kill |
| platform gate byte `0xA210D81` | 0 -> **1 at 22 s**, with a transient `0x1000001` (word at `0xA210D84` also 1) at 24 s, then 1 |
| state-machine object (`[0x71273C8]` = 0x1471273a0) | created by the stage-2 entry handler; **current 2, next 2** at every sample from 22 s to the kill |
| loading tick `0x7453ECC` | advances ~1,000/s from 24 s (the loading poll runs, state stays 1) |
| title-image thread census | one thread at 8 s (`game+0x4F657F1`, the loader completion entry); every later census found no thread with a title RIP or a title return address in 8 stack pages - see the limit in section 11 |
| the transition that would move it | `call 0x813660` at RVA `0x81B2EF` (request stage 3) written to the object's `+0x12`; the probe shows `+0x12` stays 2, so the request is never made, and the pump therefore never runs a transition |

So the measurement refutes the "the boot-media wait loop holds the machine" candidate: the gate it waits on is already 2
from 4 s, 18 s before stage 2 is entered. The stage-2 entry handler is still running (it set loading = 1 at 24 s) but
never returns to request stage 3, so the request is never made: `next` stays 2. The block is between the loading init
(`0x21B8994`) and the request (`0x81B2EF`).


## 7. What this says about the next wall

After first light (the frame path can be made to draw), the title is still in stage 2, so the next wall is the
stage-3 request, not the frame and not the loading word. The decompile names the chain and the boot measures where
it stops:

1. **The stage-2 entry handler `FUN_14081B270` is the only thing that can request stage 3**, and it does so on its
   last line: `call 0x813660` at RVA `0x81B2EF` (the shared predicate store `fn_813660(object, 3)` writes the u16 at
   the state-machine object's `+0x12`). The probe reads that field every 2 s: it is 2 from the entry at 22 s to the
   kill, so **the request is never made**.
2. **The boot-wait loop at `0x21B8A3E` is not the block.** Its predicate is the byte `0x71B9048 == 2`, and the probe
   measures that byte becoming 2 at **4 s** (the boot-media module constructor `FUN_145BF9A40`, `0x5BF9CE0`), 18 s
   before stage 2 is entered. The loop, if reached, exits on its first iteration.
3. **The loading init inside the same handler did run** (loading = 1 at 24 s, at `0x21B8994`), so the block is between
   that call and the request at `0x81B2EF`: the remainder of `FUN_1421B8960` (`0x21B8999`..`0x21B8AFC` - the
   `FUN_145AE8810`/`FUN_1421B8B00`/`FUN_145AD7630`/`FUN_145A79A60` init group, then `FUN_145ABCD00`, the loader/TLS
   check at `0x21B8A00`, `FUN_145CC6D80`, `FUN_142763010`, the wait loop, then the post-loop group
   `FUN_145BF0B10`, `FUN_1421B8B20`, `FUN_1421B8BD0`, `FUN_1421B8C80`, `FUN_142225870(0x147127458,2)`,
   `FUN_145C3DC10`, `FUN_141D24C80`, `FUN_14282F3C0`, `FUN_14282F810`, `FUN_1426B1E60`) or in the two calls
   before it (`FUN_1421B7A40`, `FUN_142237D50`), and it is a blocking wait, not a loop that exits.
4. **The loading word cannot move before stage 3.** 1 means "initialised and idle"; the story-load start is a later
   stage's own call (`FUN_142754DC0` -> `FUN_141D67570(mode)`), so its value cannot change while the machine sits at
   2.
5. **The session word is not involved.** It has no writer in the image and stays 0 in every sample.

The 27.7 s boundary [wait_forensics.md](wait_forensics.md) measured on an older tree (the entry thread blocked on the
title lock `game+0x258DB70`, and a `ReadFile` retry loop) is consistent with this: the stage-2 entry handler's
post-loading work starts at 24 s and the machine never returns. On the current tree the read is repaired
([readfile_first_touch.md](readfile_first_touch.md)), so the next measurement must name which call in the list above
blocks, not assume the old one.

In fork terms the public work's `linux_proton.md` gives the healthy Linux sequence: package splash 0-10 s, launch
movie and landing prompt ~60-90 s while initialisation finishes, initialisation done ~180 s (the A button is
accepted), story load starts ~240-300 s with its loading slideshow and artwork, and the world phase begins ~540-630 s
(~70 s / ~110 s / ~190 s on its Windows machine). Our stage 2 is exactly that "initialisation" window: the stage-2
entry handler runs the boot sequence and never finishes it. The public work's `d3d12_xs_console_abi.md` records the
boot split at the same place: "up to the story load (~150 s): about 12 000 `write32` ... 16 000 `write64` ...
4 100 `wait64`", and "from the story load on: job chains across the queues" - a completely different queue shape
after the break. We measure the pre-break shape. So the missing transition is the same break the fork's healthy boot
makes after its landing prompt: stage 2 -> 3, and only then can the loading state leave 1.

## 8. The probe (`PALMETTO_STATE_TRACE`)

`port/src/main.cpp` gains one opt-in diagnostic, off when the variable is unset (the default path does not start a
thread and does not touch the image). It is deliberately a **sampler**, not a hook: the writers are named statically
above, and what a sampler must add is the timeline plus which thread is holding the machine.

```
PALMETTO_STATE_TRACE=<interval-seconds>[,<stop-seconds>]     (e.g. 2,130; "1" means 2 s to process end)
```

Each interval it prints two lines and, while its census budget lasts, one line per sampled thread group:

* the three words with their change times and the title's own last-change tick (`0x7446508`), the loading tick
  (`0x7453ECC`), the "load started" byte (`0x7453EBD`) and the load-start QPC (`0x7453EC0`), the flags at
  `0x7453ED4`;
* the boot-media gate (`0x71B9048`), the boot-media active/flag bytes (`0x71B90AD`/`0x71B90AC`), the platform gate
  `0xA210D81`, the loading-active byte `0x7453EE0`, the session companion word `0x98F8290`, and the state-machine
  object (`[0x71273C8]`) with its current stage (`+0x10`) and requested stage (`+0x12`);
* a census of every other thread: suspended, RIP read, up to 10 title-image frames scanned from its stack, grouped
  by title wait site (RIP if in title code, else nearest title frame) with up to 8 sites shown, and the full title
  stack of up to two threads whose stack or RIP names a state-machine owner (pump, executor, writer, stage handlers,
  boot sequence, loading init/poll/start/stop, session dispatcher - the RVA list is in the source).

Bounds: interval clamped to 0.2..60 s, stop time optional, census at most 24 samples (the first 12 every interval,
then every tenth), values are RVAs/counters/handles only. A change of any sampled word prints a one-line
`[rt] state change at ...` with the old and new value before the next sample, so the store is dated to the interval.

## 9. Per-sample timeline

The boot's `stderr.log` carries 64 word samples (2 s interval, 0.0-128.0 s) and 17 thread censuses. The session word
is 0 and the boot-media gate is 2 in every sample after 4 s; the table keeps the transition points and every 10th
later sample (the full file is in the boot directory, section 11). Byte-valued fields are shown as the byte they
are read from (the probe's 32-bit read at a byte address is masked here).

| t (s) | stage `0x7446510` | loading `0x7453ED0` | loading tick `0x7453ECC` | gate `0x71B9048` | platform byte `0xA210D81` | machine cur/next | thread census |
|---:|---:|---:|---:|---:|---:|---|---|
| 0 | 0 | 0 | 0 | 0 | 0 | 0/0 (object not created) | 0 title threads |
| 4 | **1** | 0 | 0 | **2** | 0 | 0/0 | 0 |
| 8 | 1 | 0 | 0 | 2 | 0 | 0/0 | 1 thread at `game+0x4F657F1` |
| 22 | **2** | 0 | 0 | 2 | **1** | **2/2** | 0 |
| 24 | 2 | **1** | 62022108 | 2 | 1 | 2/2 | 0 |
| 26 | 2 | 1 | 62026056 | 2 | 1 | 2/2 | 0 |
| 40 | 2 | 1 | 62039919 | 2 | 1 | 2/2 | 0 |
| 60 | 2 | 1 | 62060047 | 2 | 1 | 2/2 | 0 |
| 80 | 2 | 1 | 62080051 | 2 | 1 | 2/2 | 0 |
| 100 | 2 | 1 | 62100061 | 2 | 1 | 2/2 | 0 |
| 120 | 2 | 1 | 62120054 | 2 | 1 | 2/2 | 0 |
| 128 | 2 | 1 | 62128057 | 2 | 1 | 2/2 | (census budget spent) |

Read as a statement: the title enters stage 2 at 22 s and the stage-2 entry handler immediately initialises the
loading subsystem (state 1 at 24 s, whose poll then keeps `0x7453ECC` advancing), but the handler never returns:
the requested stage stays 2 for the remaining 106 s. The title's own change record agrees: the stage word's
last-change tick `0x7446508` is 62,022,057 from 22 s to the kill (the loading tick runs to 62,128,057, so the title's
clock itself kept moving while the stage did not).

## 10. Smallest next experiment

The probe's own boot already picked between the candidates: the boot-media gate is 2 before stage 2 starts (so the
wait loop is out), and the state-machine object's requested stage stays 2 (so the stage-3 request is never made).
The next measurement is therefore to find the blocking call inside the stage-2 entry handler, and it is one boot
with the samplers this tree already has:

**One 130 s boot with `PALMETTO_STATE_TRACE=2,130` plus `PALMETTO_WAIT_SAMPLE=30,60,90,120` (and, if the wait
sampler classifies the entry thread as "busy", `PALMETTO_THREAD_STACKS=30,60,90,120`).** The wait sampler prints, for
every waiting thread, the wait API, the nearest title-image frame (its wait site) and the object type; its
[wait_forensics.md](wait_forensics.md) usage already named `game+0x258DBA6` and the parked pool threads on an older
tree. With the probe's two lines in the same boot, the entry thread is identified by elimination (the thread whose
title site is inside `FUN_1421B8960`'s call range `0x21B8999`..`0x21B8AFC` or inside `FUN_1421B7A40` /
`FUN_142237D50`) and the blocking API/object is named. That is the only missing fact before a fix can be routed.

Two smaller follow-ups once the call is named:

1. If the site is a wait on a title-created event/lock (the wait forensics pattern), the producer side is the
   subsystem the init call drives; the stack's remaining title frames name it, and the fix belongs to that
   subsystem's shim, not to the state machine.
2. If the site is a host shim call that never returns, the probe's own line `[rt] state trace ... current 2 next 2`
   plus the boot log's last `[rt]` lines bracket it; the fix is the shim's.

Only that measurement is needed; no runtime semantics should be changed before it. The loading word and the session
word do not need instrumentation beyond the sampled values: neither can move before stage 3.

## 11. Evidence, limits, files

* Boot directory (outside the repository): `D:\leonida-work\state2\boot-state1\` - `stderr.log` (16.3 MB; the probe
  lines are `[rt] state trace` / `[rt] state change`), `stdout.log`, `boot.jsonl` startup trace, `summary.txt`
  (`ALIVE 130.3 s`, 0 exceptions, 223,820 D3D12 calls). The extracted table is
  `D:\leonida-work\state2\trace_table.md` (from `parse_state_trace.py`).
* Decompile logs and per-function `.c` files: `D:\leonida-work\state2\ghidra-decomp*.log`, `decomp*/`; the static
  scan drivers are `scan_state_refs.py`, `scan_absolute.py`, `find_callers_local.py`, `rva_ranges.py`,
  `dis_range.py`, `split_my_decomp.py` in the same folder.
* Ghidra project copy: `D:\leonida-work\state2\ghidra-proj\slotmap.*` (imported with `-noanalysis`; the shared
  `D:\tools\ghidra-projects` project was copied so a read-only decompile cannot race another agent).
* Limits: one boot, one machine, one overlay; the sampler has 2 s resolution, so a store inside an interval is dated
  to the interval (the `[rt] state change` lines are printed from the probe's own clock). The state-machine object is
  a heap object whose pointer is read from the image, so its fields are valid only while the pointer is non-zero and
  page-committed (the probe checks both). **The thread census found no title-image thread after 8 s** - one title
  thread at 8 s (`game+0x4F657F1`) and none in later samples, although the boot obviously keeps running title code.
  The likely reason is the 8-page stack window and the census's `RIP-in-image OR title-frame-on-stack` criterion; it
  is a real limitation of this probe's stack part, not evidence that no title thread exists (the 130 s boot's frame
  loop and the loader papers show otherwise). The waiting-thread stacks this slice wanted are therefore to be taken
  from the existing `PALMETTO_WAIT_SAMPLE`/`PALMETTO_THREAD_STACKS` samplers, which is exactly what section 10
  proposes. The fork timeline quoted in section 7 is the public work's measurement on its own machines, used as
  evidence only, not as a target for our timings.
* Files this slice touched: `port/src/main.cpp` (the probe only; no other behaviour changed) and this page. No shim,
  build file, test or other page was modified; the tree also carries other slices' uncommitted files.
