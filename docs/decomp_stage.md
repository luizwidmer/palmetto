# Stage and state machine from stage 3 on: writers, transitions, the boot-media gate, the job graph and the runtime answers it reads

**Status: sections 1-9 are static decompilation only; section 10 is measured.** The static part of this page maps
what the title does after its game/stage word reaches 3: the exact phase dispatch of the stage machine object, every
writer of the three state words with its store site, the boot-media gate bytes `0x71B90AC`/`0x71B90AD` and the
title path they select, the stage-3 search handlers, the 16-slot job graph the stage-3 update dispatches to, the
session state machine that runs in the same call tree (`0` -> `1` -> `3` -> `4` -> ...), and the runtime answers
(IAT imports and console-object vtable calls) read on those paths with the RVA of each read. It closes with a
ranked list of runtime-answer hypotheses that could keep stage 3 alive or make the first visible content
reachable, each with the shim where we answer it. Section 10 reports the boot that measured ranked hypothesis 1 and
the diagnostic switch it added.

Marking: **[M]** measured statically in this slice (decompiled C or instruction bytes); **[I]** inferred from the
call graph, a measured page elsewhere, or a structure's use. Where this page corrects an earlier one it says so.
Every claim is RVAs, offsets, values and counts only - no strings, asset bytes, package paths or hashes of game
data.

## 1. What ran

| | |
|---|---|
| Project | read-only copy of `D:\leonida-work\classa\ghidra-proj.rep` at `D:\leonida-work\decomp-stage\ghidra-proj` (the analyzed `game_scarlett_master.exe` import; no re-import, never opened read-write) |
| Decompiler | Ghidra 12.1.4 PUBLIC headless, `D:\leonida-work\rea-tooling\ghidra_scripts\DecompileRanges.java` over exact `.pdata` ranges; 69 range decompiles in 4 batches (13 + 28 + 16 + 12), split into per-function files |
| Static scans | validated RIP-relative reference scan over `.text` (instruction-boundary validated, `[M]`), direct `E8`/`E9` call/jmp scan, qword pointer scan for VA and RVA forms, raw disassembly of thunks outside `.pdata` |
| Call-graph walk | bounded depth-2 direct-call walk from three root sets, reporting every IAT read reached: 197 functions from the stage-3 path, 247 from the session handlers, 243 from the media/boot-media/job modules |
| Boots | none for sections 1-9; section 10 is a measured runtime slice on the same page |
| Work notes | `D:\leonida-work\decomp\stage-machine.md` (outside the repository, raw tables) |

## 2. The machine object and its four phases

The game/stage word `0x7446510` is written by the machine's transition executor during the stage chain (the
module reset `0x614030` also zeroes it once at initialisation). The machine object
itself is reached through the image global `0x71273C8`, written by the stage-2 entry handler at **`0x81B278`**
[M]; its vtable lives in `.rdata` at `0x61862F0` and slot 1 (`0x61862F8`) holds the transition executor
`0x53EBFE0` [M].

| object field | offset | writer (RVA) | meaning |
|---|---|---|---|
| current stage | `+0x10` (u16) | `0x53EBEA0` (loop) | the stage the machine is in |
| requested stage | `+0x12` (u16) | `0x813660` (request writer) | the stage to enter; a u16 store, any caller may request |
| pending stage | `+0x14` (u16) | `0x813660` / cleared at `0x53EBFC8` | non-`0xFFFF` = a retry/pending stage path |
| phase flag | `+0x16` (byte) | set bit 2 at `0x53EBF8F`, cleared at `0x53EBF01` | selects the phase the executor runs this pump pass |
| last-change tick | `+0x8` (qword) | `0x53EBED3` | title tick of the last phase change |

The pump is `0x53EBE50` (precondition + one pass) and `0x53EBEA0` (loop); the executor `0x53EBFE0` writes the
stage word through `0x53EC0C0` and then runs the stage handler for the phase it was given [M]. The phase values
are not enter/exit: the executor switch shows a **four-phase** protocol [M]:

| phase `param_4` | when the pump passes it | stage-3 handler | stage-1/2/4/5 handler |
|---:|---|---|---|
| 2 | the pump pass where a new request differs from current (store at `0x53EBF76`, call at `0x53EBF8C`), argument is the **old** stage | none | none |
| 0 | the next pump pass after a transition (phase flag bit 2 was set) | `0x81AFB0` (post-transition, runs once) | none |
| 1 | every later pump pass while current == requested | `0x812620` (per-frame update) | `0x81B200` / `0x81B270` / `0x21B68D0` / `0x21B6910` |
| 3 | the pending stage at `+0x14` (call at `0x53EBF9C`, cleared at `0x53EBFC8`) | none | none |

This corrects the naming in [state_machine.md](state_machine.md) section 2: `0x812620` is the **per-frame
update** (phase 1) and `0x81AFB0` is the **post-transition** handler (phase 0), not an enter/exit pair. A stage
change therefore needs two pump passes: one transition pass and one post pass [M].

The pump is run from the title's frame loop (`0x1D109C9` -> `0x661E0` -> pump) and from three registries: the UI
module caller `0x73C950`, the story module caller `0x810E20` (walks the device registry at `0x147134ED8` /
`0x147134F08`), and the event/handler registry caller `0x1D66670` [M, call sites in the decompiled handlers].

## 3. The three state words

| word | only writer (RVA) | store site | values written statically | readers |
|---|---|---|---|---|
| game/stage `0x7446510` | `0x53EC0C0` | `0x53EC0E3` (qword tick `0x7446508` at `0x53EC0DC`) | the requested stage 0..6 | the writer's own compare `0x53EC0C5`; the stage-reset function `0x614030` writes 0 directly at `0x61404C`; the debug status formatter `0x222C880` reads it at `0x222C8E4` |
| loading `0x7453ED0` | `0x22572D0` | `0x22572F0` (notify `0x5B5D3D0` first) | 0, 1, 2..5 (load mode) | `0x11F740` poll, `0x1D5D040` stop, `0x1D67570` start, `0x4F862D0` test |
| session `0x98F828C` | `0x1AC7760` (2-instruction setter: `mov dword ptr [rip+0x98F828C], ecx; ret`) | `0x1AC7760` | 0,1,3,4,5,6,7,9,11,12,13,16 | dispatcher `0x1D83960` and 11 other readers (see 3.3) |

The four loading-word write call sites are `0x2257209` (init -> 1), `0x1D675C7` (start -> mode), `0x1D5D188`
(stop -> 0) and `0x11F78A` (poll 1 -> 2) [M].

### 3.1 Stage-word transitions (request site, handler, condition)

| from -> to | request site | handler that runs | condition |
|---|---|---|---|
| reset (>1) -> 0 | `0x53EBE8E` (pump precondition requests 0 when current > 1 and `[obj+0x60] != 0`) | stage-0 update `0x810F00`, which requests 1 at `0x810F04` | module init [M] |
| 0 -> 1 | `0x810F04` (`mov dx,1; jmp 0x813660`) | - | unbounded [M] |
| 1 -> 2 | `0x81B25A` (`mov dx,2; jmp 0x813660`) | stage-1 update `0x81B200` first runs `0x14222D390(0)`, `0x1422161F0` and `0x142240DA0` | unbounded [M] |
| 2 -> 3 | `0x81B2EF` (`mov dx,3; call 0x813660`) | stage-2 update `0x81B270`: `0x1421B7A40`, `0x14222D390(2)`, deferred registration `0x1421B8940` when `0x141C1ACE0()==0`, `0x142237D50`, boot sequence `0x1421B8960` | the boot sequence must return; it blocks on the tracker event measured in [stage2_blocker.md](stage2_blocker.md) [M] |
| 3 -> 4 (path A) | `0x812671` (`mov dx,4; jmp 0x813660`) | - | `0x450920() != 0` and `0x38D7C0() != 0` and `0x23096D0(obj)` true, then `0x14235A4E0(1)` and `0x1414EAB80(0)` [M sites, I semantics] |
| 3 -> 4 (path B) | `0x812732` (`mov dx,4; call 0x813660`) | - | `0x38D7C0() != 0` and `*(u32*)([0x9E24DF8]+0x14) == 0` and `0x1421B6D70() != 0` [M sites, I semantics] |
| 3 -> 5 | `0x8126FE` | - | `0x2E3EE0() != 0` (byte at `0xA943A01`) and `0x1421B6D70() != 0` [M sites, I semantics] |
| 4 -> 2 | handler `0x21B68D0`: frees `0x71273D0` when non-null, calls `0x21B6960`, stores `+0x12 = 2` (`0x21B690A`) | - | unbounded [M] |
| 5 -> 6 | handler `0x21B6910`: same free, calls `0x21B6960`, stores `+0x12 = 6` (`0x21B694A`) | - | unbounded [M] |
| 6 -> teardown | handler `0x21B6950` = thunk to `0x2246820` | - | subsystem teardown, no further request [M] |

`2 -> 3` is the transition the campaign measures as missing before the shadow-buffer fix and reached after it
([3d_textures.md](3d_textures.md)); the request store is not the change - the executor applies it when the pump
runs again, which needs the stage-2 handler to return first [M].

### 3.2 The per-frame stage-3 update (`0x812620`, phase 1)

Call order in the decompiled function [M]. Gated calls are shown with their gate.

| # | call site | target | what it does |
|---:|---|---|---|
| 1 | `0x812628` | `0x450920` | reads the loading struct byte `0x7453ED5` (returns it) |
| 2 | `0x812631` | `0x38D7C0` | reads byte `+0x1B3` of the object at global `0xAAE3B70` |
| 3 | `0x81263A`-`0x812660` | `0x2258DE0`, `0x4EC8B40`, `0x4EC8B50`, `0x23096D0`, `0x235A4E0(1)`, `0x14EAB80(0)` | only on 1 and 2 true; then requests **stage 4** at `0x812671` |
| 4 | `0x812676`-`0x812680` | `0x2361910`, `0x2246930`, `0x23C1540` | frame/title subsystems |
| 5 | `0x812691` | `0xA1C290(stream,1,0)` | phase marker 1 of the job timeline |
| 6 | `0x812696` | **`0x21B6CA0`** | the story/session tick (section 3.4): runs the session dispatcher and the story setup |
| 7 | `0x81269B`-`0x8126B8` | `0x23CB230`, `0x23CF6A0(1)`, `0x23C1660`, `0xA1C290(...,2,0)` | media/streaming tick, phase marker 2 |
| 8 | `0x8126C6`, `0x8126D4` | `0x22264D0` if `0x7453CC0==1`; `0x2434490` if `0x1497DFE50==1` | optional subsystems |
| 9 | `0x8126D9` | `0x21C2E60` | - |
| 10 | `0x8126E5`, `0x8126EE` | `0x2E3EE0`, `0x1421B6D70` | both true -> requests **stage 5** at `0x8126FE` |
| 11 | `0x812703`-`0x812722` | `0x38D7C0`, `0x4EC8B40`, `0x53D54B0`, `0x50903F0(*(u32*)(obj+0x14))`, `0x1421B6D70` | gate of stage-4 path B; when it passes, requests **stage 4** at `0x812732` |
| 12 | `0x812737`-`0x81274F` | `0x23C15D0`, `0x2246970`, `0x1DF4DC0` if `0x75DEDC1==1` | frame tail |
| 13 | `0x81275F` | `jmp 0x9EF0E0` | **job-graph dispatch** of the current job id `[0x6CEE300]` (section 5.1) |

### 3.3 Session-word transitions

The dispatcher `0x1D83960` compares `0x98F828C` and indexes a switch jump table at `0x674C120` [M]. Every write
goes through the one setter `0x1AC7760`; the table lists the request sites and the value they pass [M].

| from -> to | request site | value | notes |
|---|---|---:|---|
| any -> 1 | `0x1C99520` (`mov ecx,1`), called from the task-completion path `0x59965B0` at `0x59965D2` | 1 | the 0 -> 1 step measured at 60 s in [3d_textures.md](3d_textures.md) is this call [I] |
| 1 -> 3 | `0x582A8D0` (`mov ecx,3; jmp`), dispatched by case 1 at `0x1D839EC` | 3 | explains the measured session 3 at stage 3 [M/I] |
| 2 -> 1 | `0x21B7060` (wrapper `mov ecx,1`) | 1 | - |
| 2 -> 16 | `0x21B7020` when `[0x7127438] != 0` | 16 (`0x21B703E`) | else `0x154C9A0` -> `0x2931DA0` |
| 3 -> 4 | `0x21B7182` (end of the `0x21B7070` poll) | 4 | the state-3 poll completed |
| 4 -> 5 | `0x21B71D1` (end of handler `0x21B71A0`) | 5 | after `0x1D24C80(...,1)` and the `+0x30` service call (section 5.3) |
| 5 -> 6 | `0x21B71F2` (end of handler `0x21B71E0`) | 6 | after `0x1428BB970` sets the media state |
| 6 -> 13 | `0x5946370` (`mov ecx,0xD`; also writes `0x6842894`) | 13 | the state-6 handler polls `0x1478B2818` first |
| 7/8 -> 0 | `0x21B72EA` | 0 | after the media-ready chain of `0x21B6DA0`/`0x21B6E20` |
| 9 -> 10 | `0x21B7305` | 10 | - |
| 11 -> 12 | `0x21B736F` when `0x289E7B0()` returns 2 | 12 | else `0x21B7406` sets 0 |
| 12 -> 0 | `0x21B7505` | 0 | after the `0x26` command poll |
| 13 -> 16 | `0x21B75A3` when `0x145EA7CE0() != 0` | 16 | else falls through to 0 |
| 14 -> 0 | `0x21B765F` | 0 | - |
| 16 -> 0 | `0x21B7672` | 0 | - |
| default (> 0x10) | dispatcher `0x1D839BD` | 0 | - |

States **2, 8 and 15 have no static setter** in this image: nothing writes those values, so they are not reached
by title code as far as the image shows [M]. This corrects [state_machine.md](state_machine.md) section 5, which
reported "no writer of the session word in the image": the writer is the 2-instruction leaf `0x1AC7760`, missed
there because it is not in `.pdata`; the measured session `0 -> 1 -> 3` of that campaign is consistent with the
table above.

### 3.4 The story/session tick `0x21B6CA0`

Called from the stage-3 update every frame at `0x812696` [M]. In order: `0x21B6FC0` (byte `0x7127450`),
`0x21B6FD0` (media-ready chain, `0x23097B0` with command `0x9AD090D9`/`0x79DFE9A1`, and `0x2AFEFE0` when byte
`0x7127450`),
`0x21B7020` (session 2 -> 16 when `[0x7127438]` is non-null), **`0x1D83960` the session dispatcher**, then
`0x54267A0(...,1)`, `0x1428BC4A0`, `0x54267A0(...,2)`, `0x14221C4C0(0,1)`/`(1,0)`, `0x1407B5A80`,
`0x1406C5340`, `0x142622120`, `0x140B3E560`, and `0x142E7D530` when `0x14ABBE2E0==0 && 0x144F7B6A0()==0 &&
0x14ABBE2E1==0` [M]. So the whole session machine runs inside the stage-3 update, not on a separate thread.

## 4. The boot-media gate: what opens it and what it selects

The boot-media module's state block starts at `0x71B8C50`; the bytes the campaign probes are its tail [M].

| field | RVA | writer (RVA or site) | value | reader (site) |
|---|---|---|---|---|
| block word | `0x71B9040` | ctor `0x5BF9A40` at `0x5BF9CD9` | `0x7F` | - |
| **gate byte** | `0x71B9048` | ctor `0x5BF9A40` at `0x5BF9CE0` | **2** | predicate `0x5EA6930` (`cmp byte [0x71B9048],2`) - the stage-2 boot wait |
| dword | `0x71B90A8` | ctor at `0x5BF9CEF` (`mov dword [...],esi`, esi=0) | 0 | `0x810E10` (`dword != 0`) |
| **flag AC** | `0x71B90AC` | ctor zeroes it (with AD, one word store at `0x5BF9CF5`); only non-zero setter is `0x2218910` (`mov byte [0x71B90AC],1`) | 0 -> 1 | tick half B `0x22185A0` at `0x22185B2`; dispatcher `0x50070F0` at `0x50070F4` |
| **active AD** | `0x71B90AD` | `0x22184AB` = 1 on phase (2,0); `0x2218689` = 0 after the media pass; ctor zero | 0/1 | tick half B at `0x22185A5`; tick half A at `0x2218458` |
| semaphore A | `0x71B9078` | created by the semaphore wrapper `0x1662430` (IAT `CreateSemaphoreExW` at `0x1662454`/`0x1662492`) [I] | handle | released in the media pass `0x22185DF` via `0x2A1BE0` -> `0x569F0` |
| semaphore B | `0x71B9088` | same wrapper [I] | handle | released in the media pass `0x221863D`; `ReleaseSemaphore` (`0x7330` -> slot `0x6793B08`) in `0x2490080` |
| callback | `0x71B8DA0` | `0x1D5A530`; ctor clears at `0x5BF9A8D` | - | `0x50070F0` calls it at `0x500711C` when AC==1 and non-null |
| sub-object | `0x71B90B0` | passed to `0x141F67540` at `0x220FEC4` in module init | - | - |

Openers [M]: the module initialiser `0x220FCA0` calls the ctor `0x5BF9A40` at `0x220FEBF`; the ctor latches
`0x71B9048 = 2` and clears AC/AD. That latch is the byte the stage-2 boot wait polls, so the wait is a
"module initialised" latch, not the media gate.

The tick is `0x313560` = `call 0x2218410; jmp 0x22185A0`; it is called from the stage-2 wait loop thunk
`0x313550` and from `0x10D6511`, `0x63AD78`, `0x21B79D5`, `0x21B7A29`, `0x23CB275` [M].

- Half A `0x2218410` drains the callback list at `0x149AEA338` and, when its phase pair is (2,0), sets
  `0x71B90AD = 1` at `0x22184AB`; phases 3/4/5 call `0x2218890`/`0x2218870`/`0x2218880` [M].
- Half B `0x22185A0` only runs its body when **both** `0x71B90AD == 1` and `0x71B90AC == 1`, and the body is the
  boot-media present path: `0x140B5B6B0`, `0x1422177F0`, `0x1422186B0`, `0x1422186F0`,
  release semaphore `0x71B9078` (`0x2A1BE0`), `0x142218710`, callback `[0x71B8CD0]`, `0x142218720`,
  callback `[0x71B8D38]`, release `0x71B9080` (`0x4DAA220`), release `0x71B9088` (`0x2A1BE0`),
  `0x142218080`, `0x142218730`, `0x142217A50`, `0x142217B80`, then `0x71B90AD = 0` at `0x2218689`,
  `0x140B5B6F0`, and half A again [M].

**What opens AC** [M]: the only instruction that sets it non-zero is `0x2218910`, reached through the thunk
`0x2218900`, which is tail-jumped from `0x2484A90`. `0x2484A90` is called from `0x2484A50` when bit 0 of `bl`
is set; that function is `0x24846D0`, the title's media/frame tick, which drains the Win32 message queue and
processes two job rings, and calls `0x2484A90` when the byte `0x75DF0D8` has bit 0 set [M]. `0x75DF0D8` is
written by `0x247B500` (`1` on event type `0x24`, `0` on type `0x25`, both with a zero payload at `+8`) and
cleared by `0x2490080`; `0x247B500` is registered as a callback by the address-taken `lea` at `0x5CABBB7` into
`0x75DF0B8` [M].

So the title path the gate selects is the **boot-media/UI present path** `0x22185A0`: it is the only statically
decoded producer of "boot media" frames, and its second key (AC) is inert unless an event of type `0x24` reaches
the handler `0x247B500` in the same process. The producer of that event is not resolved statically; the likely
family is the title's app-lifecycle/platform (PLM) notification path, which is the same conclusion the
first-run/UI slice reaches from its own side [I]. This is why the campaign's first-visible question reduces to
whether the media module runs at all: [movie_path.md](movie_path.md) measured the movie library mapped but never
entered, and [first_visible.md](first_visible.md) section 4 names the 2D UI/video path as the peers' first visible
producer.

## 5. The job graph after stage 3

### 5.1 The per-frame job dispatch

The stage-3 update ends at `0x81275F` with `jmp 0x9EF0E0`, which loads the current job id `[0x6CEE300]` and jumps
to `0x9EF0F0` [M]. `0x9EF0F0` reads the 16-slot frame-event/media table at `0x75DE4B8`, stride `0x90`, state byte
`job+0x18`, and dispatches [M]. (This table is the frame-event/media scheduler, not the render-command job
system whose objects are `creator`/`job` in the renderer slices.)

| state `job+0x18` | handler | what it does |
|---:|---|---|
| 1 | `0x2484540` | computes a scaled time from `job+0x18` fields and jumps to `0x2482BC0` |
| 2 | `0x24845B0` | calls the **device** object's vtable slot `0x280` (console device slot 80 = `WaitFrameEventX(event,timeout,reserved,flags,&token)`) with `(device, 0, 0xFFFFFFFF, 0, 0, &token)` at `0x2484637`; `int3` on negative HRESULT at `0x2484642`; the dispatcher then stores the returned token into the slot's first qword at `0x9EF0F0` |
| 4 | `0x2484660` | computes a scaled time and jumps to `0x2482BC0` |

The dispatcher is called from the stage-3 tail and from the media tick `0x2483F40` at `0x2483FFC` and from
`0x5BFC36B` [M]. The slot state byte is written at `0x2470DB4`; the slot base qword at `0x2489E8B` [M]. The same
media module also owns a second dispatch at `0x2490080`: device vtable slots `0xB8` and `0xC0` at `0x249009F` /
`0x24900B4`, then device slot `0x278` (slot 79 = `ScheduleFrameEventX`) at `0x24900E3` with `(device,2,0,0,0)`
and `int3` on failure at `0x24900ED`, then `ReleaseSemaphore` [M]. Treat `0x2487...`/device-object identity as
inferred from the slot numbers against the measured console slot table in [console_slots.md](console_slots.md)
[I]; the calls themselves are measured.

### 5.2 The session-3 poll (`0x21B7070`)

Dispatched from the session machine when `0x98F828C == 3` [M]. It first polls
`0x41E8C0(0x78AC9C8, [0x78ACBA0])` in a `Sleep(0x21)` = 33 ms loop at `0x21B7092`/`0x21B70A0` (with
`0x21B7970`, `0x5AB12E0(1)`), then `0x2208760`, `0x5EA6EB0`, `0x2894220` (mode getter `0x5268610` writing
`0x78CCE28`; values above 3 route to `0x388490`), then `0x41E8C0` again. On completion it calls the service
object `0x149AB3020`'s vtable slot `+0x30` at `0x21B70E4` (out-parameters at `[rsp+0x30]`/`[rsp+0x40]`), the mode
getter, slot `+0x38` at `0x21B710C` with `(object, mode, out, 1)`, `0x4DAA2B0` (QPC timestamp), then a QPC-timed
loop (`QueryPerformanceCounter` at `0x21B7125`) until `0x2894220` or `_DAT_146159AE0`, and finally requests
session **4** at `0x21B7182` [M].

`0x41E8C0(record, index)` [M]: the second argument (the dword read from `0x78ACBA0`) is a ring index; it writes
a tick into `record + index*0x50`, then reads its own index `own = *(int*)(*(int**)(record+0x190))` and returns
`*(int*)(record + 0x18 + own*0x50) != 0`. The record is the title's loading/streaming progress table: 0x50-byte
entries, entry `+0x18` is the completion dword. Its readers include
`0x1D24C80` at `0x1D24DEE`/`0x1D24DF4` (session-4 media start), `0x1D67570` at `0x1D67587` (load start) and
eleven streaming/asset systems (`0x222D3B0`, `0x222EBA0`, `0x2234800`, `0x22AF6A0`, `0x2355D40`, `0x2355DD0`,
`0x263C6D0`, `0x27E9030`, `0x27ED3A0`, `0x58621D0`, `0x85E860`) [M].

### 5.3 Session 4..16 in one table

| state | handler RVA | what it does | next |
|---:|---|---|---|
| 4 | `0x21B71A0` | `0x1D24C80(_DAT_14AAD4500,1)` - the media start state machine, which reads the loading record and the flags at `param+0x21..0x27`; then the object at `[_DAT_14AAD3CE0+8]`'s vtable slot `+0x30` with `0x9C40` (40000) at `0x21B71C5` | 5 |
| 5 | `0x21B71E0` | `0x1428BB970`: sets `0x1478B2818` to 5, or to `3*(flag^1)+2` after `0x1458E20F0(0x1478B2760)` | 6 |
| 6 | `0x21B7200` | polls `0x1478B2818 == 2` in a 33 ms loop; if it is 3, sets `0x1471275E8 = 1`; stores `0x6842894` | 13 |
| 7/8 | `0x21B7240` | media-ready chain `0x21B6DA0`+`0x21B6E20`, `0x5B4C030`, `0x21B6AB0`, `0x21B6BA0`, `0x21B7680`, `0x21B77C0`; on state 7 calls `0x2654F00`; `0x35D1F20(1)` | 0 |
| 9 | `0x21B7300` | no work | 10 |
| 11 | `0x21B7310` | checks media command `0x1C46AD2` via `0x6E640`, `0x28BBA20`, `0x28BBC90`, `0x289E7B0`; on 2 | 12, else 0 |
| 12 | `0x21B7430` | checks media command `0xB16658E1`; else issues command `0x26` (`0x28948B0`) and polls `0x2894270(0x26)` with `Sleep(33)`, then command `0x31EA800E` (`0x23098A0`) or teardown | 0 |
| 13 | `0x21B7530` | command loop `0x6E5D0` with `0xBAAB6434` + media-ready chain `0x21B6FC0`/`0x21B6FD0`/`0x21B7020`; then polls `0x41E8C0(0x7BB4978, [0x7BD2318])`; if `0x145EA7CE0()==0` it may wait on `0x1478B2818==3`; `0x5B48EF0` | 16 or 0 |
| 14 | `0x21B7620` | if `0x21B6DA0`, tears down and clears the media state | 0 |
| 16 | `0x21B7670` | no work | 0 |

The command ids (`0x1C46AD2`, `0xB16658E1`, `0xBAAB6434`, `0x9AD090D9`, `0x79DFE9A1`, `0x31EA800E`) are 4-byte
instruction immediates of the title's own command wrappers `0x6E640`/`0x6E5D0`/`0x2309DB0`; the page records them
only as gate values [M].

## 6. Milestones from "stage 3" to first visible content

The order below is static; the measured pages supply the runtime observations [I].

| # | milestone | gate read (RVA) | where the title is |
|---:|---|---|---|
| 1 | stage 3 entered | stage word 3; machine current==requested | the pump applies `0x81B2EF`; phase 2 transition pass, then phase 0 |
| 2 | phase-0 post-transition `0x81AFB0` | `0x7453ED4` flag: if 1, `WaitForSingleObject(0x7453ED8)` | calls `0x5B69660` (= session dispatcher) and clears `0x7453ED5`, the byte the stage-4 path-A gate reads |
| 3 | session 0 -> 1 | task completion `0x59965B0` at `0x59965D2` -> `0x1C99520` | measured session 1 at 60 s in [3d_textures.md](3d_textures.md) |
| 4 | session 1 -> 3 -> 4 | poll `0x41E8C0` on the loading record `0x78AC9C8` | the state-3 poll; if the record completes, state 4 |
| 5 | session 4 -> 5 -> 6 -> 13 | media start `0x1D24C80`; media state `0x1478B2818`; command `0xBAAB6434` | the title's own media/boot-media state machine is being asked to run |
| 6 | boot-media open chain (parallel) | `0x75DF0D8` bit 0 (writer `0x247B500`) -> `0x2484A90` -> `0x2218910` sets AC -> `0x22185A0` media present pass | the only static producer of boot-media/UI frames is unlocked here |
| 7 | stage 4/5 request | path A `0x450920`+`0x38D7C0`+`0x23096D0`; path B `0x38D7C0`+`[0x9E24DF8]+0x14==0`+`0x1421B6D70`; stage 5 `0x2E3EE0`+`0x1421B6D70` | the machine leaves the running stage (4 returns to stage 2 = reload; 5 goes to 6 = teardown) |
| 8 | first visible first-run/UI/movie frame | the `0x22185A0` body and the `0x21B6CA0` media tick | the producers the peer ports show first ([first_visible.md](first_visible.md) section 2) |

## 7. Runtime answers read in these paths

Depth-2 static reachability: 197 functions from the stage-3 handlers, 247 from the session handlers, 243 from the
media/boot-media/job modules. The IAT reads found on them [M]: `QueryPerformanceCounter` (20 + 11 + 9 sites),
`QueryPerformanceFrequency` (1 each), `WaitForSingleObject` (`0x1784D5`, the wait helper reached from the
phase-0 tail call `0x2258DE0`), `Sleep` (`0x92ECA0`, the 33 ms polls), `CreateSemaphoreExW`
(`0x1662454`/`0x1662492`), `ReleaseSemaphore` (`0x7330` -> `0x6793B08`), `GetCommandLineA` (`0x22107BC`),
`GetCurrentThreadId` (`0x10D65A0`), and `memcpy`/`memset` (VCRUNTIME). **No GDK, D3D12X, display or network
import is read inside the depth-2 stage-3 path**: those answers are reached through the title's console-object
vtables and the loading record, not through the import table [M]. The only non-CRT answers read directly there
are the Win32 user/sync surfaces below.

| category | read (site RVA) | answer the title consumes | what the title does with it |
|---|---|---|---|
| timing | `0x21B7125`, `0x9EF117`/`0x9EF17F`, `0xA1C2D1`, `0x4DAA2B0` -> `0x4DAA2C2` | `QueryPerformanceCounter`/`Frequency` | per-job phase markers 0xB/0xC and the session-3 timeout loop `_DAT_146159AE0` [M] |
| waiting | `0x1784D5` (wait helper), `0x92ECA0` at `0x21B70A0`, `0x21B714E`, `0x21B721B`, `0x21B748D`, `0x21B75DC`, `0x21B7601` | `WaitForSingleObject`, `Sleep` | 33 ms polls of the loading/media records; the phase-0 loading-event wait on `0x7453ED8` (tail call `0x2258DE0`, helper `0x1784D0`) [M sites, I which frame owns the wait] |
| console D3D12X device | `0x2484637` slot `0x280` (80 `WaitFrameEventX`); `0x24900E3` slot `0x278` (79 `ScheduleFrameEventX`); `0x249009F`/`0x24900B4` slots `0xB8`/`0xC0` | device methods through the object at `[_DAT_148759D00]` / `[_DAT_148759FF8]` | one token per job step; `int3` assert on a negative HRESULT (`0x2484642`, `0x24900ED`) [M calls; slot names from console_slots.md] |
| session service | `0x21B70E4` slot `+0x30`, `0x21B710C` slot `+0x38` on `0x149AB3020`; `0x21B71C5` slot `+0x30` on `[_DAT_14AAD3CE0+8]` | question/answer pair + 40000 ms timeout | decides whether the loading progress record is ready and whether to leave session 3/4 [M calls, I meaning] |
| storage/streaming | `0x21B7082`/`0x21B7089` (`0x78AC9C8`), `0x1D24DEE`/`0x1D24DF4`, `0x1D67587` (`0x78ACBA0` mode) | the loading/streaming completion table, entry `+0x18` | session 3 -> 4 and the load start; the table is filled by the streaming systems listed in 5.2 and answered by our DirectStorage/ReadFile shims [M reads, I answer] |
| Win32 user | message helper `0x4DBD6C0` -> `0x4DBDA30` called at `0x248474D`/`0x2484797`; `GetCommandLineA` `0x22107BC`; `GetCurrentThreadId` `0x10D65A0` | window messages, command line, thread id | the media tick drains messages and opens the boot-media path via `0x75DF0D8`; module init reads the command line [M calls, I intent] |
| network | none in the depth-2 stage-3 path | - | the connectivity poll measured in [networking_gate.md](networking_gate.md) is not reached from these functions |
| display/present | none in the depth-2 stage-3 path | - | presentations come from the frame loop measured in [present_wiring.md](present_wiring.md), not from these handlers |

## 8. Ranked runtime-answer hypotheses

Ranked by how directly each one stands between the decoded stage-3 path and visible content. "Answer" is the
shim that owns the runtime value; none of these is a code change made by this slice.

| rank | hypothesis | static evidence (RVA) | where we answer it | falsifier |
|---:|---|---|---|---|
| 1 | **Complete the session-3 loading record.** The only static exit from session 3 is `0x41E8C0` reporting entry `+0x18 != 0` on `0x78AC9C8`; if the streaming systems never fill that entry the machine stays in the 33 ms poll and stage 3 never advances | `0x21B7082`, `0x41E8C0`, `0x21B7182` | `port/shims/dstorage.cpp`, `port/shims/file_system.cpp` (the read/completion path) | the record's entry is non-zero and session still does not leave 3: the next gate is the `0x149AB3020` service pair |
| 2 | **Open the boot-media AC chain.** AC is set only by `0x2218910` from `0x2484A90` when `0x75DF0D8` bit 0 is set; that bit is written by the event handler `0x247B500` on event type `0x24` (producer unresolved; an app-lifecycle/PLM notification is the candidate). This is the only static producer of boot-media/UI frames | `0x247B500`, `0x5CABBB7`, `0x24846D0`, `0x2218910`, `0x22185B2` | `port/shims/xmem.cpp`, `port/shims/library_loader.cpp` (movie library reach), `port/src/main.cpp` (window/message pump) | AC reads 1 but the `0x22185A0` body still presents nothing: the next wall is inside the media pass |
| 3 | **Do not leave a load in flight at the stage-3 transition.** The phase-0 handler waits on `0x7453ED8` when `0x7453ED4 == 1`; a load left running by the pre-transition path parks the pump exactly at the transition | `0x81AFB0`, `0x81AFE8` (tail call), `0x7453ED4`, `0x7453ED8` | `port/shims/dstorage.cpp`, `port/src/main.cpp` probes | a boot with `0x7453ED4 == 1` at the transition shows the pump inside the wait helper `0x1784D0` while the heartbeat stops |
| 4 | **Satisfy stage-4 path B.** It needs `*(u32*)([0x9E24DF8]+0x14) == 0` and the media-ready chain; the object behind `0x9E24DF8` is not named statically | `0x812703`, `0x53D54B0`, `0x50903F0`, `0x812732` | unresolved; name it with a one-shot probe at `0x812711` (object pointer) | the object's `+0x14` is zero and stage 4 still is not requested: the media-ready chain `0x1421B6D70` is the gate |
| 5 | **Run the media state machine to phase 2.** The stage-5 gate reads a byte at `0xA943A01` and `0x1421B6D70`; `0x1428BB970` sets `0x1478B2818` from `0x1458E20F0(0x1478B2760)` | `0x2E3EE0`, `0x8126E5`, `0x8126EE`, `0x1428BB970`, `0x21B6DA0`, `0x21B6E20` | `port/shims/xmem.cpp` (movie reach signal), `port/src/main.cpp` | both gates read 1 and the request store at `0x8126FE` is still not written: the byte/gate mapping is wrong and needs a probe |
| 6 | **Keep the console frame-event answers successful.** Job case 2 calls slot 80 and asserts on failure; the media teardown calls slot 79 and asserts too. Our device proxy already answers slot 80 with `S_OK` and a fresh token | `0x2484637`, `0x24900E3` | `port/shims/d3d12_xs_device.cpp` (slots 79/80) | an `int3` at `0x2484642`/`0x24900ED` in a boot means the device answer regressed |
| 7 | **Service the Win32 message queue.** The media tick's message loop (`0x4DBD6C0`) runs before the AC opener; an unserviced queue changes what the media module sees | `0x248474D`, `0x2484797`, `0x2484A50` | `port/src/main.cpp` (window adoption and pump) | the queue is serviced and the flag stays clear: the producer `0x247B500` is the missing side |
| - | **Negatives.** D3D12X/display/network imports are not read in the depth-2 stage-3 path, so a layer answer there cannot by itself move stage 3 or produce the first visible frame | depth-2 walk of section 1 | - | a measured read of one of those imports inside these functions |

## 9. Limits, evidence, files

- **Static only.** No boot was run for this slice; every runtime value quoted is another page's measurement
  ([state_machine.md](state_machine.md), [stage2_blocker.md](stage2_blocker.md), [3d_textures.md](3d_textures.md),
  [vertex_buffer_va.md](vertex_buffer_va.md), [first_visible.md](first_visible.md)). The decompiler's signatures
  lose arguments at vtable calls and inlines the 2-instruction session setter into its callers; the store site
  quoted is always the disassembled site.
- **Depth bound.** The runtime-answer table is a depth-2 call-graph result from the named roots. Deeper GDK and
  D3D12X answers exist and are measured in the pages that own them; they are not reachable as IAT reads within
  this bound.
- **Not resolvable statically.** The object behind `0x9E24DF8`, the service object `0x149AB3020`, the manager
  object `[_DAT_14AAD3CE0+8]`, and the media event payload of type `0x24` are named by their use only.
- **Evidence.** Decompiled C per function and the four headless logs are in `D:\leonida-work\decomp-stage\decomp-b1`,
  `-b2`, `-b4`, `-b5` (outside the repository); scan tools `pe_scan.py`, `scan_branch.py`, `runtime_answers.py`
  in `D:\leonida-work\decomp-stage\tools`; raw tables in `D:\leonida-work\decomp\stage-machine.md`.
- **Files the static slice touched:** this page only, in sections 1-9. No other page, no port source, no test, no
  build file and no git write. The repository also carries other slices' uncommitted files; none of them was touched
  there. Section 10 came later and touched `port/shims/dstorage.cpp`, `port/shims/dstorage_advanced.h`,
  `port/test/dstorage_test.cpp` and this page (10.7).

## 10. Measured: the loading record in a boot, and the completion the session-3 poll waits for

**Status: measured, diagnostic only, negative.** Sections 1-9 are static decompilation. This section adds one opt-in
probe to the DirectStorage shim and reports six boots. The record at `image+0x78AC9C8` was sampled once per interval:
its own-slot pointer came back as `image+0x78ACBA0` (the poll slot, value 4), the five 0x50-byte slots' completion
dwords stayed zero in every sampled window, and the one arm that reached session 3 sat in the 33 ms poll from
`0x21B7070` until the title's own crash 9.9 s later, so the predicate `0x41E8C0` never returned true. On the producer
side the storage path answered every request it was given: the loader's first burst of 4,199 requests completed by
+3 s, no request was refused and every first-touch repair succeeded. So no producer fix in
`port/shims/dstorage.cpp` or `port/shims/file_system.cpp` is implied by this measurement. The next falsifier is named
in section 10.6.

### 10.1 The record, as the probe reads it

The probe is `PALMETTO_DS_RECORD` in `port/shims/dstorage.cpp`. It starts on the first queue call, resolves the title
image base from that call's return address (no base, no thread) and prints one line per sample: the poll slot, each
slot's tick/completion/state dwords, the record's list-head and tick fields, and the three state words with the stage
machine's current/requested pair. It is off by default (one cached boolean, no thread, no allocation without the
switch), bounded (480 lines, at most 600 s), and its grammar is pure and tested (`parse_record_trace_spec` in
`port/shims/dstorage_advanced.h`). The measured shape:

| field | address | value in the six boots | what it is |
|---|---|---|---|
| poll slot | `record+0x1D8` (`image+0x78ACBA0`) | 4, constant | the `param_2` the session-3 poll passes to `0x41E8C0` |
| own-slot pointer | `record+0x190` | `image+0x78ACBA0` | dereferenced by `0x41E8C0`; `own` equals the poll slot |
| slots 0-4 | `record+slot*0x50` | tick `+0x0`, completion `+0x18`, state `+0x20` | the five job slots the reset `0x22310D0` zeroes at load start |
| list heads | `record+0x188`, `+0x1A8`, `+0x1C0` | 0 in every sample | the reset's outstanding-job list; empty here |
| header ticks | `record+0x1CC`, `+0x1D0`, `+0x1D4` | a title tick, `0x22F01073`, `0x22F01073` | written by the record's own init |

So in this build the dword the session-3 predicate tests is `image+0x78ACB20` (slot 4, `+0x18`), and the five slots'
tick dwords are `image+0x78AC9C8`, `+0x78ACA18`, `+0x78ACA68`, `+0x78ACAB8`, `+0x78ACB08`.

### 10.2 The arms

Six boots, one at a time under `_setup/boot_lock.ps1`, harness `tooling/boot_report.py`, 300 s timeout, logs under
`D:\leonida-work\sessionload` (outside the repository). Every arm is the standard deferred-compile environment of
[3d_textures.md](3d_textures.md) section 11.1 plus `PALMETTO_STATE_TRACE=2,300`, `PALMETTO_STAGE3_PUMP`, the record
switch and `PALMETTO_WAIT_TRACE`; Record 1 also ran `PALMETTO_DSTORAGE_TRACE`, `PALMETTO_DS_LOAD_TRACE` and
`PALMETTO_DS_ARRAYS`; Record 5 added `PALMETTO_FILE_TRACE=1`, and Record 6 armed the presenter captures and the
full-frame readback (`PALMETTO_CAPTURE`, `PALMETTO_READBACK_PPM`).

| arm | binary SHA-256 (first 8) | record window | stage 3 | verdict |
|:--|:--|:--|:--|:--|
| record1 | `F22439B6` | 500 ms, 80 s | 107.8 s | STOP `game+0x13360A3` read `0xFFFFFFFFFFFFFFFF`, t 117.7 s, wall 124.0 s |
| record2 | `1B67BBEF` | 250 ms, 300 s | not reached | STOP `game+0x43ADFEA` read `0x26500000006`, t 90.0 s, wall 105.0 s |
| record3 | `1B67BBEF` | 100 ms, 300 s | not reached | STOP host `VCRUNTIME140+0x1CCA7` read `0x21100000100`, t 70.0 s, wall 88.8 s |
| record4 | `1B67BBEF` | 100 ms, 300 s | not reached | STOP `game+0x43ADFEA` read `0x21600000000`, t 63.2 s, wall 66.1 s |
| record5 | `E900132B` | 500 ms, 300 s | not reached | STOP `0x0` execute, t 75.8 s, wall 142.0 s |
| record6 | `E900132B` | 500 ms, 300 s, captures on | not reached | STOP `game+0x43AE026` read `0x18`, t 78.0 s, wall 90.0 s |

Stage-3 arming has the same spread the earlier pages record: it ranged 52-110 s, and five of these six arms stopped
in the pre-stage crash family the content slice already named ([vertex_buffer_va.md](vertex_buffer_va.md) section
10.5), so one arm carries a session-3 window.

### 10.3 What the record showed

- **All five completion dwords stayed zero in every sample of every arm.** No store into `slot+0x18` was observed:
  slots 1-4 are zero in tick, completion and state in every sample, and slot 0 carries a tick only. No sample shows
  the value that would make `0x41E8C0` return true.
- **Slot 0's tick dword is the only slot field that moves.** It is zero through the boot's first phase, then starts
  carrying a title tick when the loading pass begins (record4's first non-zero value at +45.77 s of its clock, record5's
  at +46.11 s, record3's at +50.73 s, record6's at +56.24 s), and it advances while the loading worker runs. That is a
  title-side progress write, not a completion.
- **The poll slot (4) never carries a tick in any sampled window.** The session-3 poll would write it; the one arm
  with a session-3 window (record1) had already ended its 80 s record window when session 3 began, so the witness for
  that arm is the state word, not a record sample (10.4).
- **The header lists are empty** (`+0x188`, `+0x1A8`, `+0x1C0` all zero), so the reset/owner list walk had no
  outstanding job either.
- **The samplers stop reporting at the loading-pass phase in four arms.** In record3, record4, record5 and record6 the
  record sampler's last line is at +55.62 s, +50.67 s, +51.69 s and +68.21 s of its clock while the boot ran on to its
  crash, and the page's own `PALMETTO_STATE_TRACE` sampler stopped within a few seconds of it (60.0 s, 63.2 s, 58.0 s,
  74.0 s). Both are poll-based detached-thread samplers that read title memory; this is a tooling limit of the
  measurement, not a property of the record. Record1 ran both samplers through stage 3 (state lines to 120.7 s), so the
  sampler cannot explain the session word staying 3 there.

### 10.4 The arm that reached session 3

`record1` reached stage 3 at 107.8 s (machine current 3, requested 3), the loading word moved 1 -> 2 at 106.0 s, the
session word 0 -> 1 -> 3 and read 3 from 108.0 s. The stage-3 pump probe's samples at +1, +2, +4, +6 and +8 s after
entry name the frame thread inside the session-3 poll: `game+0x21B70A5` is the `Sleep(33)` of `0x21B7070`, reached
through `0x1D83996` <- `0x81AFB9` <- `0x53EC079` <- `0x53EBF66` <- `0x661F0` <- `0x1D109C9` <- `0x5A91C0D` <-
`0x56A1E5A`, and the heartbeat advanced 0 times after stage 3 entry in the first sample. The loading task thread
(`0x4FD1AD0`'s worker) was alive in `loading-update`/`loading-task` and the loading word's tick kept advancing
(`27423423` at 110.0 s, `27423968` at 112.0 s), so the loader was still being driven while the frame thread polled.

The state trace read session 3 with "last change 108.0 s" at 108.0, 110.0, 112.0, 114.0, 116.0, 118.0 and 120.7 s,
and the machine word current/requested 3/3 at each of those samples. A true predicate is consumed in one 33 ms pass
and stores 4 at `0x21B7182`; that store never happened before the title stopped at `game+0x13360A3` (read of `-1`)
9.9 s after stage 3 entry. The session word is the witness that the record's completion dword was still zero.

### 10.5 The producer path, measured on the storage side

Record1's `PALMETTO_DSTORAGE_TRACE` and `PALMETTO_DS_LOAD_TRACE` lines answer what the storage path did:

| measure | value |
|---|---|
| first load burst | 4,199 `EnqueueRequest` calls, all 4,199 done by +3 s of the trace's clock |
| status arrays | all complete, pending 0 in the 38.7 s dump; last poll value `0x0`; no `EnqueueSignal`/`EnqueueSetEvent` |
| second burst (pipeline precompile) | requests up to 8,297 by +20 s, done 8,297 |
| DirectStorage repair events | 10 `dstorage_read_noaccess` enqueues in the load window, every one repaired |
| `ReadFile` repair events | 2 in the load window (271 MiB at t 35.4 s, 84.8 MiB at t 106.5 s), both repaired |
| storage refusals / errors | 0 request refusals, 0 `unknown resource`, no device-lost line |

Every read the title's loader asked the storage shims for was delivered, or was a first-touch repair that then
succeeded. A read left in flight, a completion event never signalled or a queued request never drained is not what
this measurement shows. The record is reset and walked by title code (`0x22310D0` and the record's callback list), and
no shim on the storage path holds its completion: the completion the poll waits for is produced by a title subsystem,
which this slice did not name.

Record6 is the capture-armed arm, and its content numbers are the same negative the content slice reports: one
presenter capture (2564x1421, mean rgb 0.00 0.00 0.00, i.e. black), no full-frame readback because no content draw was
reached, one device-lost line, and four `ExecuteCommandLists` refusals after it. At 74.0 s its state words read
garbage (init `260240686`, loading `260561804`, boot-media gate `0x62`), five exceptions followed, and the first was
`game+0x43AE026` reading `0x18`. So this arm stopped in the pre-stage family and adds no content evidence either way.

### 10.6 Verdict, limits and the next falsifier

The verdict is a negative. In the measured boots the loading/streaming record's completion dword never became
non-zero, the one session-3 window shows the poll spinning until the title's own crash, and the storage path answered
everything it was asked. The two shims named by the roadmap row are not the producer of that value, so this slice
changes no shim behaviour beyond the diagnostic switch.

- **Sampling, not watching.** The probe polls once per 500 ms (250/100 ms in the earlier builds); it can miss a store
  and a later clear between two samples. The state word closes that gap for session 3: a true predicate is consumed
  in one 33 ms pass and stores 4, and the word never moved, so the title never observed a completion.
- **The poll-based samplers stop at the loading phase in some boots.** 10.3 names the three arms where they did and
  the one arm where they ran through stage 3; the stall is in the measurement threads, after their last reported
  sample, and does not change any state word.
- **One boot per arm, and the pre-stage crash family.** Stage 3 was reached in one of six arms; the other five
  stopped before it at `game+0x43ADFEA` (twice), in host `VCRUNTIME140`, at `0x0` (execute) and at `game+0x43AE026`.
  That family is the content slice's stop, not this probe's: the same environment without the probe stops there too.
- **The record's writer is not named.** `0x22310D0` resets the slots and `0x41E8C0` is the reader; the store that
  would write slot 4's `+0x18` has no static call site in this slice's search because the record is passed as a
  pointer and the store is through a register.
- **Next falsifier.** A hardware write watchpoint on `image+0x78ACB20` (slot 4, `+0x18`), armed from the load start,
  would name the storing title function and the subsystem that owns it. If it fires around the boot-media/AC
  handshake, the media slice owns the fix; if it never fires before the crash, the title stops short of dispatching
  the job at all. That names the producer instead of guessing it, in one boot.

### 10.7 Tests and gates

- `port/test/dstorage_test.cpp` gains `test_record_trace_spec`: a null/empty/`0`/unparsable spec is off, `1` is the
  documented 500 ms / 300 s default, an interval and an interval+duration are read, the interval and duration are
  clamped, and a zero duration keeps the default (10 new `check` lines; the target prints 234 checks, 0 failed).
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the
  `dstorage_test` target exits 0, and every `tests/test_docs_*.py` check passes.

### 10.8 Attribution

This section was implemented from this project's own measurement: the record trace, the five arms, the state-word
timeline and the storage counters. No peer code was used and no game data is added: RVAs, field offsets, values,
counts and outcomes only.

