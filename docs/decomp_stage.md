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

## 11. Measured: the loading record's completion dword is written only by the record reset

**Status: measured, diagnostic only, negative for the producer; the writer of the dword is named.** Section 10 left the
store that would complete slot 4 unnamed and named a hardware write watchpoint on `image+0x78ACB20` as its next
falsifier. This section adds one opt-in probe, `PALMETTO_SLOT_WATCH`, to `port/src/main.cpp`, and reports two boots. The
probe arms four hardware data breakpoints in every thread: the target completion dword, a live control on the record's
own slot 0 tick, the target slot's poll tick, and a probe-owned control slot. In the one arm that reached session 3 the
completion dword was written exactly once, with value zero, by the loading record's own reset `0x22310D0` (store at
`0x223111E`), invoked through the streaming subscriber method `0x2355DD0` from the loading update `0x2257700`. No
producer store followed before the title stopped, and none arrived before the process exited. The probe therefore names
the reset as the dword's only writer and measures the completing producer as not dispatched in the window; it changes no
shim behaviour.

### 11.1 What ran

| | |
|---|---|
| probe | `PALMETTO_SLOT_WATCH=1` in `port/src/main.cpp` (unset: one cached `getenv`, no thread and no handler) |
| boots | two, one at a time under `_setup/boot_lock.ps1` (`-Who slotwatch`), harness `tooling/boot_report.py`, 300 s timeout |
| environment | section 10.2's standard arm: the deferred-compile environment of [3d_textures.md](3d_textures.md) section 11.1, `PALMETTO_STATE_TRACE=2,300`, `PALMETTO_STAGE3_PUMP=1,2,4,6,9,14,20,30`, `PALMETTO_WAIT_TRACE=105,110,115`, `PALMETTO_DS_RECORD=500,300`, `PALMETTO_TIMELINE=1`, plus the slot watch; `slot2` also ran `PALMETTO_DS_LOAD_TRACE=0,300` |
| binary | SHA-256 `F0A45CD2...` (first 8) in both arms |
| logs | `D:\leonida-work\slotwatch\boot-slot1` and `boot-slot2` (outside the repository) |

| arm | wall | stage 3 / session 3 | stop | slot-watch counters at the last reporter |
|:--|--:|:--|:--|:--|
| `slot1` | 79.5 s | machine current 3 at 63.1 s, init 3 at 64.0 s, session 3 at 64.0 s | first exception trace t 66.3 s, `game+0x515ACF5` read `0x18` | 70 s: 109 threads armed; completion 1, live-control 281, poll-tick 149, method-control 1,327 |
| `slot2` | 158.1 s | not reached (machine 2/2, init 2 at 20.0 s, loading 2 at 70.0 s, session 1 from 70.0 s) | first exception trace t 73.6 s, `0x0` execute | 80 s: 105 threads armed; completion 0, live-control 8, poll-tick 0, method-control 1,373 |

The arm table's `t` is the startup trace's clock, which opens at process start; the watch times in 11.3 and the state
changes are on the state trace's process-relative clock, which starts when the first sampler starts, later in the same
startup. The two clocks are adjacent but not the same timebase, so comparisons between them are read to the nearest
second, not to the decimal.

The second arm is the stage-3 spread section 10.2 and [3d_textures.md](3d_textures.md) section 11.1 already record: it
stopped pre-stage in the `0x0`-execute family (the same t-versus-wall split that page's `record5` arm shows, t 75.8 s,
wall 142.0 s), so one arm carries the session-3 measurement and the other is a pre-stage negative with the live control
still firing. Both arms recorded their D3D12 call counts and appeared in the ordinary trace: 172,654 calls and 9,615
events with one exception (`slot1`), and 162,721 calls and 9,204 events with three exceptions (`slot2`).

### 11.2 The probe

`port/src/main.cpp` gains the section `PALMETTO_SLOT_WATCH`. Windows debug registers are per-thread, so the probe arms
every thread of the process with the same enumeration and two-scan rule the executor watch documents (a thread is only
suspended once it has been alive across two scans, because suspending a thread between creation and its first
instruction can leave a suspend count its owner never balances), and keeps arming threads created later. Four data
breakpoints exist and all four are used, each as an aligned 8-byte write watch:

| breakpoint | address | role |
|---|---|---|
| DR0 | `image+0x78ACB20` (slot 4, `+0x18`) | the target: the dword the session-3 poll's predicate `0x41E8C0` reads |
| DR1 | `image+0x78AC9C8` (slot 0 tick) | the same-boot live control: a title store into this record that section 10.3 measured moving during the loading pass |
| DR2 | `image+0x78ACB08` (slot 4 tick) | the session-3 poll's own tick store, which dates its first pass without sampling the state word |
| DR3 | a port-owned slot written every 50 ms | the method control: if it never fires, the debug registers are not honoured here and a silent DR0 is a method failure |

A hit prints the steady clock (the `PALMETTO_TIMELINE` clock) and the process-relative second (the state trace's
clock), the writer's RIP with the sixteen bytes before it (a data breakpoint fires after the store, so those bytes are
the storing instruction), the value now and before, the three state words with the machine's current/requested pair,
the loader object's armed and in-flight fields, all five slots' tick/completion/state dwords, the poll index, the
registers and the call chain from `RtlVirtualUnwind` (`RtlLookupFunctionEntry`, image frames as game+RVA, other modules
by name). It is bounded: a per-watch log cap, a 96-line total cap, and a 10 s counter reporter, so a boot without a hit
still shows what the watch saw. `PALMETTO_SLOT_WATCH=1` watches slot 4; `=<slot>` (2, 3 or 4) selects another slot. Off
when unset or `0`; with the switch unset no thread starts, no handler is installed and the default path is unchanged.

### 11.3 What the watch caught in `slot1`

| t (s) | watch | writer | value | what the stack says |
|---:|:--|:--|:--|:--|
| 1.1 | method control | `host:palmetto.exe+0x7814f` | increments | data breakpoints are honoured in this environment; the control slot is the one writer |
| 60.2 | live control | `game+0x41E8FD` (inside `0x41E8C0`) | a title tick | tid 51832, the loading task: `0x41E8C0` <- `0x85E892`/`0x85E823` (`0x85E860`) <- `0x22347F3` <- `0x22577AD` (`0x2257700`) <- `0x11F749` (loading poll `0x11F740`) - the loading pass polling slot 0 |
| 63.1 | poll tick | `game+0x41E8FD` | a title tick | tid 43580, the frame thread: `0x41E8C0` <- `0x21B7097` (session-3 poll `0x21B7070`) <- `0x1D83996` <- `0x81AFB9` <- `0x53EC079` <- `0x53EBF66` <- `0x661F0` <- `0x1D109C9` <- `0x5A91C0D` <- `0x56A1E5A` - the same chain section 10.4 measured |
| 65.1 | **completion** | `game+0x2231126` (inside the reset `0x22310D0`) | `0x0` (was `0x0`) | tid 51832, the loading task: `0x22310D0` <- `0x2355DF2` (`0x2355DD0`) <- `0x22FE404` <- `0x22FDDF8` <- `0x5AB5914` <- `0x31F721` <- `0x51D99FE` <- `0x51D9842` <- `0x31F6C1` <- `0xCAB143` <- `0x14EACE5` <- `0x2257807` (`0x2257700`) <- `0x11F749` (loading poll) <- `0x4FD1AE0` (loading task) <- `0x23CF247`/`0x23CF104` (task executor) |

The completion hit is one line in the whole boot. Its state read is `init 3`, machine `3/3`, `loading 2`, `session 3`,
poll index 4; the loader object at `game+0x79F6D70` read armed `0` and in-flight `0`; all five slots read `0/0/0`. The
sixteen bytes before the writer's RIP are `00 00 89 81 10 01 00 00 c5 fc 11 81 40 01 00 00`, i.e. the last executed
instruction is `vmovups [rcx+0x140], ymm0` with `rcx` = the record base: a 32-byte zero store that covers slot 4's tick,
`+0x10` and the completion dword at `+0x18`. The counters at 70 s read one completion hit, and no later reporter ran
before the process exited, so the dword was written once in the boot and written zero.

After the reset the record was rebuilt by title code, not completed: the dstorage shim's record trace (section 10.1's
`PALMETTO_DS_RECORD`) sampled slot 4's tick at `29503570` and `29507143` in its +67.05 s and +70.61 s lines with the
completion dword still `0`, session 3 and machine 3/3, while the poll kept writing ticks. No `SLOT n COMPLETION dword`
line was printed by that trace in either arm.

### 11.4 The writer, statically

The function containing the writer RIP is `0x22310D0`-`0x223120B` [M, disassembly and Ghidra decompile]. Its first
instructions read the record's own-slot pointer at `record+0x190`, dereference it (the index dword at
`image+0x78ACBA0`, value 4) and compare it with the function's second argument; when they match it zeroes the five
0x50-byte slots at record offsets `+0x0`, `+0x50`, `+0xA0`, `+0xF0` and `+0x140` with 32-byte stores and the five state
dwords at `+0x20`/`+0x70`/`+0xC0`/`+0x110`/`+0x160`, then walks and calls the record's callback list at
`+0x1A8`/`+0x1C0`. So the dword's measured writer is the record's **reset/initialisation**, not a completion: it writes
a zero over the completion field.

Its caller is `0x2355DD0` [M, decompiled C]: `FUN_142355dd0` first calls the reset with the literal record
`game+0x78AC9C8` and the poll index `[game+0x78ACBA0]`, then builds a command block from the event payload's `+0x18`
(`0x2309DB0`), calls `0x5BB4DE0`, and posts event type `0xe` into the global at `[game+0x147F00518]`. That function is
the virtual method at vtable slot `+0x68` of a streaming subscriber object, and the dispatch chain above it is the
loading update: `0x11F740` (loading poll, on the loading task) -> `0x2257700` (loading update) -> `0x14EABB0` ->
`0x31F670`/`0x51D9800`/`0x51D9990`/`0x31F700` -> `0x5AB58E0` (walks 0x68-byte subscriber entries and calls each
entry's `+0x50`) -> `0x22FDDE0`/`0x22FE3F0` (vtable call `+0x68`) -> `0x2355DD0` -> reset. In one sentence: a queued
streaming event delivered by the loading update starts a subscriber, and that subscriber's start method resets the
loading record to zero with the current poll slot.

### 11.5 Attribution

- **The writer is reachable title code, and it ran.** `slot1` executed it at 65.1 s on the loading task thread (tid
  51832): 3.1 s after the state trace first read the loading word 2 (62.0 s), 2.0 s after the session-3 poll's first
  slot 4 tick write (63.1 s) and 1.1 s after the session word first read 3 (64.0 s). The hit is not a method failure:
  the live control and the method control both fired in the same boot, and the poll tick shows the target slot's own
  bookkeeping.
- **It is not the media/boot-media dead-code cluster.** That cluster is `0x2218180` and its selector thunks
  `0x2217C70`/`0x2217C90`, unreferenced in this image ([decomp_first_ui.md](decomp_first_ui.md) section 11.2). Every
  frame of the measured writer stack is the loading/streaming family; no frame is in the media subscriber path and the
  boot-media AC chain was not involved.
- **No runtime answer of ours is in this event.** The hit read the loader object armed 0 and in-flight 0, and section
  10.5 measured the storage path with every request delivered and nothing outstanding in the session-3 arm. The reset
  is title bookkeeping when the loading update delivers a queued event to a subscriber; no shim answer produces it and
  none is implied to change it.
- **The completion producer did not run in the window.** From the poll's first pass (63.1 s) to the title's first
  exception (trace t 66.3 s, the allocator free path `0x515ABB0`, whose measured `0x515ACF5` site is its first read of
  the resolved tree node at `+0x18`; [decomp_memory.md](decomp_memory.md) section 4), and for the rest of the process,
  no non-zero write to `game+0x78ACB20` arrived. The predicate `0x41E8C0` therefore never read true, which is why the
  session word stayed 3.

### 11.6 Verdict, limits and the next falsifier

The verdict on the producer is a negative with the writer named: the completion dword's only writer in the measured
boot is the record reset `0x22310D0`, which writes zero, and the completing store never happened before the title
stopped. This is consistent with section 10's producer-side measurement (nothing outstanding, no shim producer) and
adds the missing attribution: the write the poll waits for is a title job-completion path, not the reset and not our
storage layer.

- **The session-3 window was short.** The session word first read 3 at 64.0 s (state trace clock) and the title's first
  exception is at trace t 66.3 s, so the poll ran a couple of seconds and the negative covers that window plus the rest
  of the process. The dword was watched from 1.1 s and the watch stayed armed to process exit; a second completion write
  would have been logged (the per-watch cap is 48 lines and one was used), so the one hit is the boot's only write.
- **One boot carries the session-3 evidence.** The second arm stopped pre-stage in the `0x0`-execute family; stage 3
  reached in one of two arms is the same spread section 10.2 reports for six arms.
- **Watched threads are threads that lived long enough.** A thread created and destroyed between two sweeps is never
  armed; a writer on such a thread would be missed. Every thread in the measured writer stack was armed (the counters
  show 109 threads by 70 s) and a title store into the record was caught, so the method was live.
- **The probe adds debug exceptions.** The method control traps ~20 times a second, and the live control and poll tick
  trap on each title store into the record (281 and 149 by 70 s in `slot1`); the boot's timings can shift by the cost of
  those traps. The stop site is the title's own allocator free-path crash family ([decomp_memory.md](decomp_memory.md)
  section 4), not a fault in the probe's path, but no probe-off arm was run in this environment.
- **Next falsifier.** Name the completing path by its event, not its address: an execution breakpoint on `0x2355DD0`'s
  call site would print the event payload's command id, and a write watch on the slot state dword (`+0x20`) and on the
  record's callback list (`+0x1A8`/`+0x1C0`) would date the job the reset started. A boot with a longer session-3
  window (section 10.4's `record1` arm ran 9.9 s) is the other half: a non-zero completion store there would name the
  producer in one line; none there would move the question to whatever dispatches the job.

### 11.7 Tests and gates

- No test target is added: the slice is a hardware-observation path in `port/src/main.cpp` with no pure helper to
  exercise, and the switch is off in every default run (one cached `getenv`, no thread, no handler, no allocation).
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the built
  targets exit 0, and every `tests/test_docs_*.py` check passes.

### 11.8 Attribution

This section was implemented from this project's own measurement: the writer watch, the two boot arms, the hit logs,
the state-word timeline, the disassembled and decompiled reset and dispatch functions, and the dstorage record
sampler's lines. The static check reused the read-only Ghidra project copy section 1 describes
(`D:\leonida-work\decomp-stage\ghidra-proj`), with decompiled C under `D:\leonida-work\slotwatch\decomp-writer`; no
peer code was used and no game data is added: RVAs, field offsets, instruction bytes, values, counts and outcomes only.

## 12. Measured: the record-completion writer is live in the boot for two other loading records, and the session-3 record's two dispatchers never run

**Status: measured, diagnostic only, negative for the session-3 record; the producer is named and section 17's
semaphore reading is corrected.** Section 11 named the completion dword's only writer as the reset and left two
falsifiers: an execution probe on the streaming subscriber start `0x2355DD0` and the reset `0x22310D0`, and a
wait-object trace of the loading task's semaphore (section 11.6). This section runs both in two clean 300 s light arms
and adds the producer's static attribution: the dword is written by the record-completion function `0x5862370` when a
caller passes the record's own poll index. In the second arm that function's **execution probe fired twice for two
other loading records but never for `0x78AC9C8` slot 4**, and the two call sites that pass this record - the streaming
subscriber's completion/stop virtual method `0x2355D40` and the task callback `0x27ED3A0` - never ran. The dword
therefore stayed zero for the whole window (the reset's zero store is its only write), the session word stayed 3, the
frame thread stayed in the 33 ms session-3 poll, the movie player held state `0x9` with `buffers 0`, the per-frame
decode `0x2EF0E0` was never entered, and every capture and full-target readback is black. The wait trace refutes
section 17's reading of the loading task: its semaphore is the task system's **worker idle wait**, released 11,791
times in the same boot by title code at `0x6380`, not a wait that stops the loader. No runtime answer of ours is
measured on the path, so no shim is changed and the default path is unchanged.

### 12.1 The arms and the environment

Two boots, one at a time under `_setup/boot_lock.ps1` (`-Who s3record`), `-Light`, through `tooling/boot_report.py`,
300 s timeout. One binary: SHA-256 `2120E306743D2DBD...`, the section-16/17 gate build (unmodified tree). The
environment is section 17.1's content environment - the template's deferred-compile/state-trace/stage-3-pump/alloc
watch lines, `PALMETTO_CONTENT_PROBE=1`, `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, `PALMETTO_SLOT70_PUBLISH=1`,
`PALMETTO_READBACK_FULL=1`, captures every 100 frames to 30, `PALMETTO_USER_TRACE=1`, `PALMETTO_INPUT_TRACE=1` - plus
the mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1` in every arm and these diagnostics:

| switch | arm 1 `record1` | arm 2 `record2` |
|---|---|---|
| completion-dword watch | `PALMETTO_SLOT_WATCH=1` | `PALMETTO_SLOT_WATCH=1` |
| execution probes | `PALMETTO_POKE_LOG=2355DD0,22310D0` | `PALMETTO_POKE_LOG=5862370,58623E0,2355D40,2EF0E0` |
| record sampler | `PALMETTO_DS_RECORD=500,300` | `PALMETTO_DS_RECORD=500,300` |
| stage-3 pump | `PALMETTO_STAGE3_PUMP=5,20,60,120,180,240` | same |
| wait-object trace | `PALMETTO_WAIT_TRACE=70,100,130,170,220,280` | `PALMETTO_WAIT_TRACE=130,170,220,280` |

| arm | wall | stage 3 (state clock) | session word | stop | device loss | captures / readbacks |
|---|---|---|---|---|---|---|
| `record1` | 300.2 s (timeout) | 96.0 s | 3 from 96.0 s to 298.0 s | ALIVE, 0 exceptions | 0 | 30, all mean rgb `0.00 0.00 0.00`; 4, all mean 0 max 0 |
| `record2` | 300.2 s (timeout) | 76.0 s | 3 from 76.0 s to 296.0 s | ALIVE, 0 exceptions | 0 | 30, all mean rgb `0.00 0.00 0.00`; 4, all mean 0 max 0 |

This is the first pair in the stage-3 programme with two clean long arms in a row: the stage-3 window is 202 s
(`record1`) and 220 s (`record2`), and both boots kept submitting after the entry (13,741 and 17,691
`ExecuteCommandLists` in the timeline; 9,135 and 13,885 of them after the stage-3 entry; the last at relative 299.2 s
and 299.1 s). Both arms report the mitigation line "image+0x1555c10 redirected at 3 direct call site(s)".

### 12.2 The completion producer, statically

Section 10.6 left the dword's writer unnamed because the store goes through a register. It is now decompiled: the
function `0x5862370` reads the title tick `0x4E4BA80`, and when its second argument (the record pointer's poll index)
equals the record's own index (`**(int**)(record+0x190)`) it stores the tick at `record+0x18+index*0x50`, then walks
the record's callback list at `+0x1A8` and notifies each callback with event code 6 [M, decompiled C]. Its entry gate
`0x58623E0` takes `*(record+0x1A0)`: a null pointer allows the write, otherwise a flag word at `+0x198` and the
record poll `0x41E8C0` decide [M]. The function has ten direct call sites; the two that pass the session-3 record
`0x78AC9C8` with index `&0x1478ACBA0` (value 4) are the ones that matter here:

| call site | caller | what the caller does | record it passes | measured in `record2` |
|---|---|---|---|---|
| `0x2355D6D` | `0x2355D40`, the streaming subscriber's virtual method at vtable `+0x60` | calls the job stop helper, then the writer, then posts event type `0xe` and calls the media state object's `+0x70` | `0x78AC9C8` / index 4 | **0 execution-probe hits** |
| `0x27ED3BB` | `0x27ED3A0`, a task callback with no direct caller (function-pointer dispatch) | calls the writer, then the streaming update, then posts event type 9 | `0x78AC9C8` / index 4 | **0 hits, proved by the writer probe** |
| `0x58622B4` | `0x58621D0` | pairs with the media event check | `0x147BB4CA0` / index 4 | not probed |
| `0x1CFC349` | `0x1CFC320` (through `0x5A68B20`) | load-completion callbacks of another channel | `0x1474511B8` / index 0 | **1 writer hit at relative 26.6 s** |
| `0x2207863` | `0x2207680` | state-checked completion of another channel | `0x147BD2320` / index 2 | **1 writer hit at relative 71.9 s** |
| `0x21B741E` | `0x21B7310`, session handler 11 | media command check; the tail jump's record operand was not recovered | not recovered | not reached (session stays 3) |
| `0x21B751D` | `0x21B7430`, session handler 12 | media command check | `0x147BD2140` / index 0 | not reached (session stays 3) |
| `0xDCBC08`, `0x289E5B2`, `0x29DF27F` | `0xDCBAC0`, `0x289E470`, `0x29DF250` | other loading families | `0x147BB4978`, `0x1478B2A58`, `0x147BC4A00` | not probed |

The start side is unchanged from section 11.4: the subscriber start `0x2355DD0` (vtable `+0x68`) calls the reset
`0x22310D0` with the record literal and the poll index, executes the event's command block through `0x5BB4DE0`, and
posts event type `0xe` into the loading event queue `0x147F00518`. So the only two dispatches that can store the
session-3 record's completion are the subscriber's completion method and the task callback, and the second one's call
is excluded by the writer probe itself (a call would have shown in it).

### 12.3 What the two boots measured

The completion watch and the execution probes, per arm. `record1` probes the start/reset pair named in section 11.6;
`record2` probes the writer, its gate, the subscriber completion method and the movie per-frame decode. Every line is
from the same boot as its controls.

| measurement | `record1` | `record2` |
|---|---|---|
| completion-dword watch (DR0 on `image+0x78ACB20`) | 1 hit, value `0x0`, writer `game+0x2231126` (the reset), state `init 3, machine 3/3, loading 2, session 3`, poll index 4, loader armed 0/in-flight 0, all five slots `0/0/0`, at state clock 97.6 s | 1 hit, value `0x0`, writer `game+0x2231126` (the reset), at state clock 77.8 s |
| same-boot live control (DR1, slot 0 tick) | 8,804 hits by 290 s | 13,404 hits by 290 s |
| same-boot poll control (DR2, slot 4 tick) | 5,765 hits by 290 s | 6,325 hits by 290 s |
| method control (DR3) | 5,681 hits; 110 threads armed | 5,624 hits; 110 threads armed |
| subscriber start `0x2355DD0` | 1 hit at relative 97.5 s: `rcx` = subscriber object (vtable `game+0x61B2790`), `rdx` = event payload, payload qwords `0,0,0,0x100000000`, the dword it reads at `+0x18` is 0; return address `game+0x22FE404` | not probed |
| reset `0x22310D0` | 1 hit: `rcx` = record `0x1478AC9C8`, `rdx` = 4, return address `game+0x2355DF2` | not probed |
| writer `0x5862370` | not probed | **2 hits**: record `0x1474511B8` / index 0 at relative 26.6 s (caller `game+0x1CFC349`), record `0x147BD2320` / index 2 at relative 71.9 s (caller `game+0x22076863`); **no hit for `0x78AC9C8`** |
| writer gate `0x58623E0` | not probed | 2 hits, `rcx` (the `record+0x1A0` object) `0x0`, `rdx` 0 and 2 - both calls allowed |
| subscriber completion `0x2355D40` | not probed | **0 hits** |
| movie per-frame decode `0x2EF0E0` | not probed | **0 hits** |
| DirectStorage record sampler | 480 samples to +241.7 s: five completion dwords all 0, max `0x0`, slot 0 and slot 4 ticks advancing; header lists `+0x1A8`/`+0x1C0` 0 | 480 samples to +241.8 s: same, max `0x0` |

The writer probe is the decisive control: in `record2` the completion writer was entered for two other loading records
50 s and 5 s apart (the second one at relative 71.9 s, i.e. 4 s before this arm's stage-3 read at 76.0 s), and its gate
returned "allowed" at both calls, so the debug-register-observed dword's silence is not a method failure and the store
path is title-side and live. Those two calls passed other entries of the same eight-record loading table registered by
`0x22D3B0`; the session-3 record `0x78AC9C8` shares poll index 4 with one other record, `0x147BB4CA0`, whose
completion helper `0x58621D0` passes that record and not this one.

The wait trace answers section 11.6's second falsifier and corrects section 17.2. The loading worker thread (tid 59404
in `record1`, 38316 in `record2`) does wait on a semaphore with an infinite timeout, at `game+0x56A0D`, and the wait
object table shows that semaphore released by title code `ReleaseSemaphore` at `game+0x6380`: handle `0x94c` had
12,004 waits / 11,634 sets, handle `0x958` had 11,841 waits / 11,791 sets at the 130 s dump in the two arms. Seven
other worker semaphores (`0x948`, `0x950`, `0x95c`, `0x1b78`, `0x1c28`, `0x1c40`, `0x1c8c`, `0x1c90`) sit at the same
site with the same release site, and in `record2` the worker was running inside `loading-update` at the 60/120/180 s
stage-3 pump samples. So the semaphore is the task executor's per-worker idle wait - the loader is not parked on a
missing signal, and the loading tick keeps advancing in both arms (1,000 counts/s from the 96 s to the 296 s sample).
The thread that really is parked is the frame thread: `Sleep(33)` at `game+0x21B70A5` (the session-3 poll) at every
stage-3 pump sample of both arms, and no thread's blocked wait at any dump is an object our shims answer.

| state/pixel witness | `record1` | `record2` |
|---|---|---|
| state changes after stage 3 | none: init 3 / loading 2 / session 3 from 96.0 s to 298.0 s | none: init 3 / loading 2 / session 3 from 76.0 s to 296.0 s |
| movie player state `entity+0x1c4` | `0x3` at 92.0 s, `0x9` from 98.0 s to 298.0 s, handle non-null, `buffers 0` | `0x9` from 78.0 s to 296.0 s, handle non-null, `buffers 0` |
| movie decode / frame-buffer registration | not probed (section 17 measured 0 hits) | per-frame decode `0x2EF0E0` 0 hits |
| presenter captures | 30, 2564x1421, every mean rgb `0.00 0.00 0.00` | 30, same |
| full-target readbacks | 4, draws 4636-4639, 3840x2160, mean 0, max 0 | 4, draws 3860-3863, mean 0, max 0 |
| renderer submission | 13,741 command lists, 9,135 after the entry, last relative 299.2 s | 17,691 command lists, 13,885 after the entry, last relative 299.1 s |

### 12.4 Verdict, limits and the next falsifier

The verdict on the producer is a negative with both dispatches named and excluded in the same boot: the completion
dword's producer is `0x5862370`, the session-3 record's two callers into it are the subscriber completion method
`0x2355D40` (probe: 0 hits) and the task callback `0x27ED3A0` (excluded by the writer probe's silence on this record
while the probe fired for two other records), and neither ran in the measured window. This is consistent with section
10.5's producer-side measurement (storage delivered everything, nothing outstanding): no shim producer is in the
path, so no shim behaviour changes and the default path stays byte-identical.

- **A correction to section 17.2.** The loading task's semaphore wait is the task executor's idle wait (released by
  title code `game+0x6380`, shared by eight workers), not a wait that stops the loader: the loading update keeps
  running and its tick keeps advancing. The only parked thread on the path is the frame thread in the session-3 poll,
  which is the consumer of the record this section leaves incomplete.
- **The completion store path itself is proven live.** The writer/gate probes fired for other loading records in the
  same boot; the negative is about which dispatcher runs for `0x78AC9C8`, not about the store code or our probe.
- **Limits.** Two boots, one stage-3 window each; the writer probe covers the writer entry but not the gate's input for
  this record (the record's `+0x1A0`/`+0x198` were not sampled when the store was absent); `0x27ED3A0` is excluded by
  the writer probe rather than by a probe of its own entry; the completion method's name ("stop/finish") is inferred
  from its body; and the vtable `+0x60` dispatch that should call `0x2355D40` has no named caller yet.
- **Next falsifier.** Name the dispatch that should call the subscriber completion `0x2355D40` (or the task callback
  `0x27ED3A0`): an execution probe on the `+0x60` call itself, on the loading event queue `0x147F00518` consumer that
  receives the start's event type `0xe`, and on the job object the start built (`0x5BB4DE0`'s block) would separate
  "the job finished and nobody called the completion" from "the job never finished". A boot in which a caller of the
  completion runs should store a non-zero slot-4 dword and move session 3 -> 4 in one 33 ms pass.

### 12.5 Tests and gates

- No test target is added and no code file is edited: this slice uses the existing opt-in probes only
  (`PALMETTO_SLOT_WATCH`, `PALMETTO_POKE_LOG`, `PALMETTO_DS_RECORD`, `PALMETTO_WAIT_TRACE`,
  `PALMETTO_STAGE3_PUMP`), and every one of them is off in a default run.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the GPU-free
  gate targets exit 0, and every `tests/test_docs_*.py` check passes.
- Evidence outside the repository: `D:\leonida-work\fsrstable\boot-record1`, `boot-record2` (stderr/stdout/summary/
  meta), `capture-record1`, `capture-record2`, `readback-record1`, `readback-record2`, and the analysis scripts under
  `D:\leonida-work\s3record` (`run_arm.ps1`, `analyze_record.py`, `find_calls_to.py`, `pe_rva.py`). No game data:
  RVAs, offsets, register/pointer values, counts, times and image means only.

### 12.6 Attribution

This section was implemented from this project's own measurement: the two long arms, the completion watch and its
controls, the execution probes, the wait-object dumps, the record sampler, the state-word timeline, the captures and
readbacks, and the disassembled/decompiled start, reset, writer, gate and subscriber functions. The static check
reused the read-only Ghidra project copy section 1 describes (`D:\leonida-work\decomp-stage\ghidra-proj`), with
decompiled C under `D:\leonida-work\s3record\decomp-b6` through `decomp-b10`; no peer code was used and no game data
is added: RVAs, field offsets, instruction bytes, values, counts and outcomes only.

## 13. Measured: the session-3 stream job is dispatched and stays pending; the completion dispatch and the event bus never run in two clean 300 s arms

**Status: measured, diagnostic only, negative for the producer; the case split is "the job never finished".** Section
12 named the record-completion writer `0x5862370`, its two callers that pass the session-3 loading record (the streaming
subscriber's completion method `0x2355D40` at vtable `+0x60` and the task callback `0x27ED3A0`), and left one falsifier:
an execution probe on the `+0x60` dispatch, on the loading event queue `0x147F00518` and on the job block the start
builds through `0x5BB4DE0`. This section adds one opt-in probe, `PALMETTO_S3DISPATCH`, with twelve int3 sites, a 2 s
state sampler and a 100 ms fine change log, and reports two clean 300 s light arms with the class-0xa mitigation on. In
both arms the subscriber start ran once (reset, command id 0, third argument 1), the streaming subsystem's async request
(vtable `game+0x658DE18`, key `0x9837FB2E`) was created and attached to the subscriber's stream object (vtable
`game+0x658DE48`), whose pending slot `+0x120` then held it; the request's owner slot `+0x30`, sub-operation count
`+0x18`, callback array/count `+0x20`/`+0x28`, key `+0x38` and flag `+0x3c` did not change again for the rest of the
boot. The consumer that the completion method runs first (`0x4043300`), the completion method, the task callback, the
event-bus raise `0x224CD60` (zero events of any id in the whole boot) and the subscriber's registered id-`0xe` handler
`0x2355EF0` all stayed at zero hits, and the completion writer fired only for the two other loading records section 12
measured. Session and stage stayed 3, the movie player stayed at state `0x9` with `buffers 0`, every capture and every
full-target readback is black. No runtime answer of ours is on the path, so no shim changes and the default path is
unchanged.

### 13.1 The probe

`port/src/main.cpp` gains the section `PALMETTO_S3DISPATCH`. It is opt-in (unset or `0`: one cached `getenv`, no
handler, no thread, no patch), and it uses the allocation watch's trap discipline: each site gets one `int3` over the
function's first byte, a hit restores the original byte, single-steps it and re-arms, so the title executes exactly the
code it would have executed. Twelve sites are armed once, after the image is mapped and before the entry point:

| site | RVA | what it measures |
|---|---|---|
| `subscriber-start` | `0x2355DD0` | the subscriber's start method (vtable `+0x68`): the object, the event payload, the record reset |
| `job-exec` | `0x5BB4DE0` | the start's command execution: the subscriber object and the command block's dwords |
| `async-submit` | `0x1DBC0F0` | the command dispatch that submits the stream job: command id, flag, handle before/after |
| `subscriber-completion` | `0x2355D40` | the completion method (vtable `+0x60`) that would write the record completion |
| `task-callback` | `0x27ED3A0` | the other caller that would write this record's completion |
| `event-bus-raise` | `0x224CD60` | the global event bus raise/dispatch entry; the event id is `*(u32*)(event+8)` |
| `subscriber-bus-handler` | `0x2355EF0` | the handler the start registers on that bus for id `0xe` |
| `completion-writer` | `0x5862370` | the writer itself, logged with the record and the poll index it is called for (control) |
| `async-request-consume` | `0x4043300` | the SDK routine the completion method runs first; it reads the request's completion fields |
| `owner-attach-request` | `0x402F0F0` | the attach that registers the request into the stream object's pending slot |
| `owner-attach-notify` | `0x402F110` | the attach's notification step (skipped when the attach pre-check says so) |
| `async-attach` | `0x1D1B8D0` | the attach check itself: request, owner, owner slot |

The two samplers read title memory only: every 2 s the subscriber object, its async handle and the request's fields,
the record's index, header gate fields `+0x198`/`+0x1A0`, callback lists `+0x1A8`/`+0x1C0` and the five slots' tick and
completion dwords, the global event bus (`*image+0x7F00518`, its owner token and its bucket count), the three state
words with the machine's current/next pair, and the movie player's state, handle and buffer count. A second thread polls
the request's first 0x40 bytes and the stream object's `+0x100..+0x120` block every 100 ms and prints only a change, so a
job that never moves is visible as a single line. The log is bounded: per-site caps, three logged raises per event id, a
600-line total cap, 170 state lines and 150 fine-change lines.

### 13.2 The arms

Two clean arms are the measurement; the first measured attempt and a 60 s smoke arm are quoted for completeness. All
boots are one at a time under `_setup/boot_lock.ps1` (`-Who s3dispatch`), `-Light`, 300 s (60 s for the smoke arm) via
`tooling/boot_report.py`, logs outside the repository. The environment is section 17.1's content environment (deferred
compile, 2 s state trace, stage-3 pump, alloc watch, console graphics root, slot-70 publication, content probe,
full-target readback, user and input traces, captures every 100 frames to 30) with the mitigation
`PALMETTO_TITLE_CLASS0A_SIZE=1`, `PALMETTO_S3DISPATCH=1` and the DirectStorage record sampler in every probed arm; the
refined arm also runs `PALMETTO_FILE_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1` and `PALMETTO_DS_LOAD_TRACE=0,300`.

| arm | binary SHA-256 (first 16) | wall | stage 3 (state clock) | session word | stop | device loss | exec entries in the timeline | captures / readbacks |
|---|---|---|---|---|---|---|---|---|
| `dispatchek` (smoke) | `7526D314E76EDFC6` | 60.3 s (timeout) | not reached | 0 -> 1 | ALIVE, 0 exceptions | 0 | 3,645 | 20 / 0 |
| `dispatch1` | `7526D314E76EDFC6` | 51.0 s | not reached (init 2, loading 2) | 0 | STOP `game+0x28CF870` read `0x5c` | 1 | 2,528 | 14 / 0 |
| `dispatch2` | `7526D314E76EDFC6` | 300.2 s (timeout) | 66.0 s | 3 from 66.0 s to 298.0 s | ALIVE, 0 exceptions | 0 | 18,800 | 30, all mean rgb `0.00`; 6, all mean 0 |
| `dispatch3` | `F99B54163D4EE3D9` | 300.3 s (timeout) | 58.0 s | 3 from 58.0 s to 298.0 s | ALIVE, 0 exceptions | 0 | 23,521 | 30, all mean rgb `0.00`; 6, all mean 0 |

`dispatch1` is the pre-stage crash family sections 10.2 and 12.1 record (`game+0x28CF870` is a new member of it), so it
carries no post-stage-3 measurement and its only loss was one device. The two clean arms ran two probe builds: `dispatch2`
the eight-site build, which is also the smoke and failed arms' binary, and `dispatch3` the twelve-site build with the two
extra sampler threads. Stage 3 arrived at 66.0 s and 58.0 s, the same 52-116 s spread the whole stage-3 programme
records, and both boots kept submitting command lists to the kill (18,800 and 23,521 timeline executions, the last
within 0.5 s of the timeout).

### 13.3 What the sites measured

Every value below is from the same boot as its control. `dispatch2` and `dispatch3` agree wherever both probed a site.

| measurement | `dispatch2` | `dispatch3` |
|---|---|---|
| `subscriber-start` | 1 hit | 1 hit |
| `job-exec` | 1 hit | 1 hit |
| `async-submit` | 1 hit | 1 hit |
| `async-attach` | not probed | 1 hit |
| `owner-attach-request` | not probed | 1 hit |
| `owner-attach-notify` | not probed | 0 hits |
| `subscriber-completion`  | **0 hits** | **0 hits** |
| `task-callback` | **0 hits** | **0 hits** |
| `async-request-consume` | not probed | **0 hits** |
| `event-bus-raise` | **0 hits**, 0 events of any id | **0 hits**, 0 events of any id |
| `subscriber-bus-handler` | **0 hits** | **0 hits** |
| `completion-writer` | 2 hits, both other records | 2 hits, both other records |
| DirectStorage record sampler | 480 samples, max completion dword `0x0` | 480 samples, max completion dword `0x0` |
| fine change log | - | 1 line (the attach snapshot); no change after it |
| state words at the last sample | init 3 / loading 2 / session 3, machine 3/3 | init 3 / loading 2 / session 3, machine 3/3 |
| movie player at the last sample | state `0x9`, handle non-null, `buffers 0` | state `0x9`, handle non-null, `buffers 0` |

The start hit reads the same shape in both arms: `rcx` is the subscriber object (vtable `game+0x61B2790`), `rdx` the
event payload whose qwords are `0,0,0,0x100000000` (the dword the start copies at payload `+0x18` is 0, the command id),
`r8` is `0xFFFFFFFF00000001` (the third argument is 1). At the hit the record is already reset: poll index 4, header
`+0x198`/`+0x1A0`/`+0x1A8`/`+0x1C0` all zero, slot 0 tick carrying a title tick, slot 4 tick carrying a title tick, and
all five completion dwords zero. The `job-exec` hit reads the command block on the stack (`command-id 0`, the rest zero)
and the subscriber handle still zero. The `async-submit` hit reads `rdx 0` (command id 0), `r8b 1` and the handle at
subscriber `+0x58` still zero, i.e. the submission had not happened yet at the entry. A `subscriber-completion` hit
would have printed the unwound caller chain and so named the `+0x60` dispatch site itself; the site never fired in
either arm, which is why section 12's dispatch caller is still unnamed by call chain rather than measured.

The attach hits then name the job: `async-attach` reads request `0x25443F4DF80` with vtable `game+0x658DE18`, refcount
2, empty completion fields and owner slot `+0x30` = the stream object; `owner-attach-request` reads the stream object
`0x2542A7B5D00` (vtable `game+0x658DE48`), the request and key `0x9837FB2E`, and the stream's `+0x100` fields. The
`owner-attach-notify` site did not run because the attach's pre-check returned "already attached". The writer's two hits
are records `0x1474511B8` index 0 and `0x147BD2320` index 2, the same pair section 12 measured, and never the session-3
record `0x78AC9C8`.

The subscriber's bus entry is registered from the first post-start sample to the end: entry owner `+0x80` = the
subscriber, handler `+0x88` = `0x142355EF0`, and a non-zero bucket pointer at `+0xA0`, so the start's registration is
live and the event id it waits for (`0xe`) simply never arrives.

### 13.4 The job's state: the case split

The refined arm's 100 ms fine log printed exactly one line, the post-attach snapshot at trace t 58.9 s; after it, for the
remaining 240 s, neither the request's first 0x40 bytes nor the stream object's `+0x100..+0x120` block changed. The
request's layout is the one the attach measured: vtable `game+0x658DE18`, refcount at `+8` = 2, `+0x10`/`+0x18` zero,
callback array `+0x20` zero with count `+0x28` zero, owner `+0x30` = the stream object, key `+0x38` = `0x9837FB2E`, flag
`+0x3C` zero. The stream object's pending slot `+0x120` held the same request and did not move either.

The case split follows from which fields a finished job would have moved. The routine that a completion first runs is
`0x4043300` (reached from the completion method through `0x22FEF70`, which loads `rcx = *(subscriber+0x50)` and
`rdx = *(subscriber+0x58)` and tail-jumps to the consume path): it reads the sub-operation count `+0x18`, walks the
callback array `+0x20`/`+0x28`, consumes the owner `+0x30` and clears the flag `+0x3C`. So a job that finished has to
have its completion fields filled **before** any dispatch can consume them, and the measured boot never filled one. The
alternative case, "the job finished and its completion was never dispatched", would show the request's fields filled and
no consumer; that did not happen. What is measured is the third state: the request was created, keyed, attached to the
stream object and left pending, and the only consumer stayed at zero for 240 s. The session-3 record's completion dword
therefore stays zero because the job it waits for never completed, not because its completion was misrouted.

The event side says the same. The global at `image+0x7F00518` is an event bus object with three API entries, all of them
measured statically in this slice: subscribe `0x58A6510(bus, id, entry)`, unsubscribe `0x2209460(bus, id, entry)` and
raise/dispatch `0x224CD60(bus, event)`; the event structure is `{vtable, id at +8, payload at +0x18}`, and the subscriber
entry is `{owner at +0x80, handler at +0x88}` plus the list links at `+0x90..+0xA0`. The start registers that entry for
id `0xe` and the completion method unregisters it; **neither the raise nor the handler ran in either clean arm**, and the
raise counters show zero events of any id in 300 s. Section 12.2's sentence "posts event type `0xe` into the loading
event queue `0x147F00518`" is therefore corrected here: the start subscribes the entry, it does not post; a posted event
would have shown in the raise site's counters, and none did.

### 13.5 What the job waits on, and the storage/file reach

The request is one of the streaming subsystem's own async requests: the manager is the streaming manager's request-cache
object at `*image+0x999ABE0` (used through the submit `0x44043570`, consumed through `0x4043300`, and cleared by the
streaming stop `0x22306070`), and the stream object passed as the attach context is the subscriber's `+0x50` object. The
refined arm ran the file and storage reach switches in the same boot as the probe:

| trace | measurement |
|---|---|
| file opens (`PALMETTO_FILE_TRACE`) | 121 opens, all in the loading burst; the last is at trace t 18.62 s |
| file first-touch repairs | last at trace t 55.46 s (84.8 MiB `ReadFile` into uncommitted title memory, repaired) |
| DirectStorage first-touch repairs | 10, last at trace t 55.13 s |
| DirectStorage load heartbeat | 8,685 requests enqueued / 8,685 done at its last +20 s sample |
| subscriber start | trace t 58.72 s (timeline origin 18,178.304, the start hit at steady 18,237.022) |

The start is 3.3 s after the last file IO event and 3.6 s after the last storage repair, and no open, read or enqueue
follows it. So the stream job did not issue an IO that any shim of this tree serves: it is waiting on its own producer,
not on a storage answer. That is consistent with section 10.5's loader-side measurement (everything delivered) and adds
the missing step: the awaited stream never reaches a storage call at all.

### 13.6 Verdict, limits and the next falsifier

- **Verdict: negative for the producer, with the case split resolved to "the job never finished".** The subscriber start
  runs, the record resets, the command executes and the job is submitted and attached; from then on nothing moves: no
  request completion field changes, no consumer runs, no bus event is raised or delivered, and the completion writer
  never sees this record. The session word stays 3 because no completion was ever written, not because a written
  completion was dropped.
- **No runtime answer is on the path.** The reach switches show no file, ReadFile or DirectStorage call at or after the
  start; the manager and the request are title-side objects (`0x44043570` and `0x4043300` are the title's stream
  request cache), so no shim producer is measured to fix and the default path stays byte-identical.
- **Corrected attribution.** Section 12's loading event queue `0x147F00518` is the global event bus, and the start's
  `FUN_1458A6510(bus, 0xe, subscriber+0x80)` is a subscription, not a post; the three bus entries and the entry/event
  layouts are named in 13.4. That also means the "queue consumer" section 12 named is not a queue consumer at all: an
  id-`0xe` delivery would have run `0x2355EF0` and it never did.
- **Limits.** Two clean arms, one stage-3 window each (66.0 s and 58.0 s); the fine log samples at 100 ms and could
  miss a change between two samples, though the 2 s sampler's request fields agree with it; the "finished" reading rests
  on the request's own completion fields, whose documented marker is the consumer's read order, not a published ABI;
  and the probe's traps shift timings by their cost, so the stage-3 arrival spread is not compared between arms.
- **Next falsifier.** Probe the stream request cache's completion producer directly: the request class vtable
  `game+0x658DE18` and the stream class vtable `game+0x658DE48` slot functions, and the streaming manager
  `_DAT_147455F40` vtable slot that reaps finished requests (the handler family around `0x234A870`, which looks a request
  up by key and calls into it). A change in the request's `+0x18`/`+0x20`/`+0x28` or the stream's `+0x120` without a
  completion hit would flip the case split to "finished but not dispatched"; no change at all keeps the job's producer
  as the wall. A write watch on those fields would date the change without polling.
- **The picture is unchanged.** No session advance means no movie draw: the player holds state `0x9` with `buffers 0`
  in both arms, and all 30 presenter captures and all 6 full-target readbacks are mean 0 in both boots.

### 13.7 Tests and gates

- No test target is added: the slice is an opt-in diagnostic path in `port/src/main.cpp` with no pure helper to
  exercise, and the switch is off in every default run.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the GPU-free gate targets exit 0, and every `tests/test_docs_*.py` check passes.
- Evidence outside the repository: `D:\leonida-work\fsrstable\boot-dispatch1`, `boot-dispatch2`, `boot-dispatch3`,
  `boot-dispatchek`, with `stderr.log`/`stdout.log`/`summary.txt`/`boot.jsonl`; `capture-dispatch2`,
  `capture-dispatch3`, `readback-dispatch2`, `readback-dispatch3`; and `D:\leonida-work\s3dispatch` (`run_arm.ps1`,
  `analyze.py`). No game data: RVAs, offsets, register/pointer values, counts, times and image means only.

### 13.8 Attribution

This section was implemented from this project's own measurement: the twelve-site probe, the two clean 300 s arms and
the smoke/failed arms, the site hits and register reads, the two samplers and the fine change log, the file/storage reach
switches in the refined arm, and the disassembled/decompiled start, completion, attach, request-consume and event-bus
functions. The static check reused the read-only Ghidra project copy section 1 describes
(`D:\leonida-work\decomp-stage\ghidra-proj`, decompiled ranges under `D:\leonida-work\decomp-stage\decomp-q1` through
`decomp-q16` and `D:\leonida-work\s3record`); no peer code was used and no game data is added: RVAs, field offsets,
instruction bytes, values, counts and outcomes only.

## 14. Measured: the session-3 stream request is stepped by the loading update every frame but its command engine stops after the attach, and every completion entry point stays at zero

**Status: measured, diagnostic only, negative for a runtime producer; the case split is "the request is attached,
cached and stepped by the loading update, but the stream's command engine is entered only in the attach window and
never again".** Section 13 left one falsifier: probe the request-class and stream-class vtable slots, the streaming
manager's reaping family, the manager global `_DAT_147455F40` and a write watch on the request completion fields with a
same-boot control. This section adds one opt-in probe, `PALMETTO_STREAMREAP`, with execution probes on the request,
the request cache, the manager's job reap and the async command engine, a hardware write watch on the measured request
and stream objects, and two samplers that read the request, the stream, the request cache and the manager's per-stream
table. Three arms are reported: one clean 300 s build with sixteen sites (`stream1`), one pre-stage loss with the
twenty-site build (`stream2`), and the clean 300 s arm of that build (`stream3`). In both clean arms the session-3
request is created and attached once (subscriber start, submit, command start and pending registration exactly one hit
each), stays in the request cache for the whole window, and is stepped by the loading update 9,588 times in
`stream3`; but the async engine step the stream pump calls it with runs 16 times, all inside the 0.7 s attach window,
and zero times afterwards. The manager-table slot the engine's entry gate reads holds an entry with state 0 (not the
"open" state 2) during those sixteen steps and reads null in all 118 two-second samples from 62 s to 296 s. The request
consume `0x4043300`, the subscriber completion `0x2355D40`, the cache completion `0x4043390`, the manager job reap
`0x234A870`, the other two cache-completion handlers and the completion-event raise all stay at zero hits in both
clean arms, and the request's completion fields (+0x18, +0x20/+0x28, +0x30, +0x38, +0x3c) and the stream's pending slot
+0x120 never change except for the loading update's own `+0x20 = 0` cleanup every frame. No runtime answer of ours and
no D3D12 fence, copy or queue submission is on this path, so no shim changes and the default path is unchanged. The
session word stays 3, the movie player holds state `0x9`, and every capture and readback is unchanged: black title
pixels, no session 4, no movie draw.

### 14.1 The probe

`port/src/main.cpp` gains the section `PALMETTO_STREAMREAP`. It is opt-in (unset or `0`: one cached `getenv`, no
handler, no thread and no patch) and it uses the section 13 trap discipline: each site is a one-byte `int3` that is
restored, single-stepped and re-armed, so the title executes the code it would have executed. The write watch is
`PALMETTO_STREAMREAP_WATCH=1` and arms when the attach registers the request, with the slot-watch arming discipline
(three scans, one suspend per thread, threads created later re-armed).

The first build armed sixteen sites; the refined build adds four inside the pipeline the first build's write watch
found. Two of the first build's cache sites were transcribed with the wrong image offset and failed to arm
(`cache-lookup` and `cache-key`); the refined build fixes their RVAs, so its table is the reference.

| site | RVA | what it measures | stream1 hits | stream3 hits |
|---|---|---|---:|---:|
| `subscriber-start` | `0x2355DD0` | the loading subscriber's start (vtable `+0x68`): resets the record, runs the command, registers the bus entry | 1 | 1 |
| `stream-submit` | `0x1DBC0F0` | the command dispatch: stream ready check, command start, cache key and request store | 1 | 1 |
| `stream-pending-register` | `0x402F0F0` | the attach onto the stream: stream `+0x1c`, `+0x30`, `+0x120` | 1 | 1 |
| `stream-command-start` | `0x402F120` | the stream command start: handler lookup, `stream+0xe8`, the open check, the start event | 1 | 1 |
| `cache-complete` | `0x4043390` | the async cache-entry completion (the only path that can complete a request) | 0 | 0 |
| `cache-lookup` | `0x402D580` | the request cache's hash lookup by key | not armed | 1 |
| `cache-key` | `0x402D560` | the cache key builder from a handle | not armed | 0 |
| `job-reap` | `0x234A870` | the streaming manager's job reap (class vtable `game+0x61B4170` slot `+0x40`) | 0 | 0 |
| `async-cmd-handler` | `0x131C480` | the async command handler that completes a job's handle from a command | 27 | 27 |
| `async-complete-handler` | `0x2329110` | one async completion handler (event flag 8) | 0 | 0 |
| `manager-complete` | `0x230BB90` | the streaming manager's completion entry (event flags `0x48`) | 0 | 0 |
| `stream-event-raise` | `0x3065AF0` | the completion raise `0x4043390` uses on the stream queue `+0x128` | 0 | 0 |
| `stream-start-event` | `0x864100` | the shared event-queue raise the command start and the engine use | 36 | 35 |
| `request-consume` | `0x4043300` | the request consume (callbacks `+0x20`/`+0x28`, sub-ops `+0x18`, owner `+0x30`, flag `+0x3c`) | 0 | 0 |
| `subscriber-completion` | `0x2355D40` | the subscriber completion that writes the loading record's completion dword | 0 | 0 |
| `streaming-stop` | `0x2306070` | the streaming subsystem stop | 0 | 0 |
| `stream-pump` | `0x45E570` | the per-frame stream pump inside the request step | not armed | 9,588 |
| `stream-step` | `0x402E260` | the async step the pump calls with the pending request `stream+0x120` | not armed | 9,592 |
| `async-engine-step` | `0x402E340` | the async engine step, entered only when the stream's open check passes | not armed | 16 |
| `stream-open-check` | `0x45E630` | the manager-table open check `state(+0xAC) == 2` that gates the engine step | not armed | 38,357 |

The write watch uses all four data breakpoints, each an 8-byte write watch, armed after the attach names the
request and the stream:

| breakpoint | address | role |
|---|---|---|
| DR0 | request+0x18 | the sub-operation count dword |
| DR1 | request+0x20 | the callback array pointer |
| DR2 | stream+0x120 | the stream's pending request slot |
| DR3 | `game+0x78ACB08` | the same-boot live control: the session-3 poll's slot-4 tick write (section 11.3 measured its writer) |

The two-second sampler prints the three state words, the machine pair, the loading tick, the record's slot-4 tick and
completion dword, the manager object, the request cache (bucket count, entry count and whether the measured request
is still in it), the request's full `0x40` bytes, the stream's `+0xd8..+0x128` block, the manager-table gate fields
(id, table, entry pointer, entry state `+0xAC`) and the site counters. A second thread polls the request, the stream
block, the cache entry count and the record's completion dword every 100 ms and prints one line per change; a third
reports the counters every 10 s. The log is bounded: per-site caps, 900 hit lines, 150 sampler lines, 140 fine-change
lines and 120 watch lines.

### 14.2 The arms

Two clean arms and one pre-stage loss, one at a time under `_setup/boot_lock.ps1` (`-Who streamreap`), `-Light`, 300 s
via `tooling/boot_report.py`, logs outside the repository. The environment is section 17.1's content environment
(deferred compile, state trace, stage-3 pump, alloc watch, console graphics root, slot-70 publication, content probe,
full-target readback, captures every 100 frames to 12) with the mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1`,
`PALMETTO_STREAMREAP=1`, `PALMETTO_STREAMREAP_WATCH=1` and `PALMETTO_THREAD_STACKS=100,160,220,280` (`stream1`) or
`130,190,250` (`stream2`/`stream3`).

| arm | binary SHA-256 (first 16) | wall | stage 3 (pump read) | session word | stop | device loss | captures / readbacks |
|---|---|---|---|---:|---|---|---:|---|
| `stream1` | `835D37600A030E76` | 300.3 s (timeout) | 62.1 s | 3 from 64.0 s to 298.0 s | ALIVE, 0 exceptions | 0 | 12, all mean rgb `0.00 0.00 0.00`; 6 readbacks, 3 mean 0 and 3 the layer's `51 102 204` control |
| `stream2` | `208D769914F17D50` | 62.1 s | not reached (last state sample: loading 2, session 1) | not reached | STOP `game+0x28CF870` read `0x5c` | 1 | 12, all mean rgb `0.00 0.00 0.00`; 0 readbacks |
| `stream3` | `208D769914F17D50` | 300.2 s (timeout) | 58.8 s | 3 from 60.0 s to 298.0 s | ALIVE, 0 exceptions | 0 | 12, all mean rgb `0.00 0.00 0.00`; 6 readbacks, 3 mean 0 and 3 the layer's `51 102 204` control |

`stream2` is the host's pre-stage crash family sections 10.2 and 12.1 record (`game+0x28CF870` reading `0x5c`): its
`stderr.log` carries one `replay: the Vulkan device was lost` line, it stopped before stage 3, and it is a loss, not a
measurement. One device loss is a retry, so `stream3` ran; a second loss would have ended the arms. Both clean arms
kept submitting to the timeout: 24,483 `exec entry` timeline lines in `stream1` and 24,504 in `stream3`, the last
entries about 0.1 s before the kill, and D3D12 recorded 1,009,867 and 1,018,685 calls.

### 14.3 What the sites measured

Every value below is from the same boot as its control. The two clean arms agree wherever both armed the same site.

| measurement | `stream1` | `stream3` |
|---|---|---|
| subscriber start / submit / command start / pending registration | 1 / 1 / 1 / 1 hit | 1 / 1 / 1 / 1 hit |
| command id at the start and submit | 0 | 0 |
| request object / vtable | measured by the attach hit | measured by the attach hit |
| request key | `0x9837FB2E` | `0x9837FB2E` |
| request cache | 11 buckets, 1 entry, request in cache at every sample | 11 buckets, 1 entry, request in cache at every sample |
| cache lookup | site not armed | 1 hit at the attach: key `0x9837FB2E`, 0 buckets (the cache is allocated on this insert) |
| per-frame stream pump `0x45E570` | site not armed | 9,588 hits by 290 s (about 41 per second) |
| per-frame stream step `0x402E260` | site not armed | 9,592 hits by 290 s |
| async engine step `0x402E340` | site not armed | **16 hits by 70 s, 0 after**: all eight logged hits fall between 60.5 s and 60.7 s |
| stream open check `0x45E630` | site not armed | 38,357 hits by 290 s |
| cache completion `0x4043390` | 0 hits | 0 hits |
| manager job reap `0x234A870` | 0 hits | 0 hits |
| manager completion `0x230BB90` | 0 hits | 0 hits |
| async completion handler `0x2329110` | 0 hits | 0 hits |
| completion raise `0x3065AF0` | 0 hits | 0 hits |
| request consume `0x4043300` | 0 hits | 0 hits |
| subscriber completion `0x2355D40` | 0 hits | 0 hits |
| async command handler `0x131C480` | 27 hits by the 60 s sample (the six logged hits at 59.5 s), before the attach | 27 hits by the 60 s sample (the six logged hits at 56.0 s), before the attach |
| shared event raise `0x864100` | 36 hits; all raises before the 70 s sample | 35 hits; all raises before the 70 s sample; six logged, all between 60.4 s and 60.6 s |
| hardware watch DR0 request+0x18 | 0 hits | 0 hits |
| hardware watch DR1 request+0x20 | 9,348 hits by 290 s | 9,557 hits by 290 s |
| hardware watch DR2 stream+0x120 | 0 hits | 0 hits |
| hardware watch DR3 live control | 6,662 hits by 290 s, writer `game+0x41E8FD` | 6,769 hits by 290 s, writer `game+0x41E8FD` |
| threads armed | 107 | 107 |
| state words at the last sample | init 3, loading 2, session 3, machine 3/3 | init 3, loading 2, session 3, machine 3/3 |
| record slot-4 completion dword | 0 at every sample | 0 at every sample |
| movie player state | `0x9` in all 115 samples | `0x9` in all 118 samples |
| captures / readback means | 12, all mean rgb `0.00 0.00 0.00`; 6 readbacks, 3 mean 0 and 3 the layer's `51 102 204` write control | same |

The start and attach hits read the same shape in both clean arms. The start is called once on the loading task
(`rcx` = subscriber, vtable `game+0x61B2790`; `rdx` = the event payload whose qwords are `0,0,0,0x100000000`;
`r8` = `0xFFFFFFFF00000001`). The submit is called on the same thread with command id 0 and flag 1, and reads the
stream object out of subscriber `+0x50` before its `+0x58` request is stored. The command start reads the stream
object (vtable `game+0x658DE48`), its command id 0, its identity pair `+0xFC`/`+0x100` (the pair the open check
reads) and raises the start event on the stream's event queue at `+0x128`. The attach then reads the request with
vtable `game+0x658DE18`, owner `+0x30` = the stream object, key `+0x38` = `0x9837FB2E`, flag `+0x3C` = 0, and writes
the stream's `+0x120` = request.

The only completion-field write in either clean arm is the loading update's own cleanup: `mov qword [rsi+0x20], 0`
at `game+0x45E488`, whose unwound chain is `game+0x45E488 <- game+0x528CD72 <- game+0x14EAC80 <- game+0x2257807 <-
game+0x11F749 <- game+0x4FD1AE0 <- game+0x23CF247 <- game+0x23CF104`, i.e. the loading task's loading poll
`0x11F740` -> loading update `0x2257700` -> streaming manager update `0x14EABB0` -> request-cache walk `0x528CD20` ->
request step `0x45E430` -> loading task. It writes the value the field already holds (0), so no request field changes;
the fine-change poller printed exactly one line in each clean arm (the post-attach snapshot) and no change after it.

### 14.4 The pipeline the write watch found

The write's chain names the per-frame pipeline, and three decompiled functions close it (all read from the image's
own bytes; RVAs and offsets only):

| RVA | what it does |
|---|---|
| `0x14EABB0` | the streaming manager's per-loading-update tick: calls the manager's vtable slots with the frame delta; the manager object is at `game+0x7455F40` |
| `0x528CD20` | walks the request cache's bucket table (`cache+0x348`, bucket count `cache+0x350`): for each node the request is `node+0x8`, and when `0x299090(request+0x30)` says the owner slot is non-null it calls the request step |
| `0x45E430` | the request step: if the owner check `0x88B3F0(owner)` reads `owner+0x11A` bit 0 clear, it steps the owner (`0x45E570`), walks the callback array `+0x20`/`+0x28`, then frees and clears that array and the count (the measured `+0x20 = 0`) |
| `0x45E570` | the stream pump: checks the stream's `+0x118` state byte, `0x45E5D0`, then loops `0x45E630` -> `0x402E260` while the open check and the step return work, and finally stores the manager-table lookup in `stream+0x138` |
| `0x402E260` | the stream step: if `0x45E630(stream+0xFC, stream+0x100)` is true it loops the async engine step `0x402E340(stream+0xE8, ..., &out, *(u64*)(stream+0x120))` and, when an out record appears, calls the completion dispatch `0x402E8A0` and releases through the handler at `+0x40`; if the check is false it returns 0 without calling the engine |
| `0x402E340` | the async engine step: resolves the queue's key pair `queue+0x14`/`queue+0x18` through the async registry, drives the queue entry's state `+0x58`, its result `+0x40` and its event records, and writes the produced out record |
| `0x45E630` | the open check: true when the async registry lookup `0x45E690(id, table)` is non-null, or when the manager-table entry `*(qword*)(table + 0x18 + (id-1)*0xC0)` is non-null and its state dword `+0xAC` is 2; `0x45E5D0` uses the same check to decide whether the pump's loop is entered |

So the request is not parked on a thread that never runs: the loading task's loading update walks the request cache
and steps the request's owner every frame, and the stream pump and the stream step are entered every frame (9,588 and
9,592 hits by 290 s). The only frame-level gate is `0x45E630`.

### 14.5 The open gate: the engine step stops after the attach

The refined arm's sampler reads the gate's inputs every two seconds: the stream's id at `+0xFC` is 1 and its table
pointer at `+0x100` is a stream-owned address in every sample; the manager-table entry slot
`table + 0x18 + (id-1)*0xC0` reads null in **all 118 samples from 62 s to 296 s**. The attach-window checks catch
the slot before it is emptied: the two logged gate hits at 60.4 s and 60.5 s read the entry slot as `table + 0xD0`
with state dword `+0xAC` = **0**, and the probe's own print labels it `open no` (the state the gate accepts is 2).

The counters agree with the gate: the stream step `0x402E260` was entered 9,592 times, but the engine step
`0x402E340` only 16 times, and the counter readings show those 16 were complete by the 70 s sample: the per-frame
loading update continues for the remaining 230 s, the pump keeps running, and the engine step never runs again. The
35 shared event raises and the 27 async command handler hits are also complete by then. In other words, the async
command engine is entered for 0.7 s around the attach, then the gate closes and the request is left pending.

What the request waits on is therefore measured one level past section 13: the request is not missing a dispatch and
not waiting on a storage, GPU or thread event; the per-frame update reaches it, but the stream's manager-table
registration is missing (or its state never reaches 2) after the attach, so the command engine is not stepped. The
gate reads title bookkeeping (`table+0x18` and the entry's `+0xAC` state word); no shim of this tree writes either
field, and no runtime answer of ours is on the path.

### 14.6 The D3D12 timeline and the storage reach

The timeline (`PALMETTO_TIMELINE`) is the same in both clean arms: `stream1` origin `t=20422.670`, start at
`t=20486.641`; `stream3` origin `t=21032.321`, start at `t=21092.705`. The event kinds are only `exec entry`,
`exec return`, `queue signal` and `compile wait` (107,898 and 108,104 lines); there is no fence, copy or queue event
that corresponds to the request. The first ten seconds after the start carry 647 (`stream1`) and 701 (`stream3`)
`exec entry` events, i.e. the renderer keeps its normal submission cadence through the request's whole life; the
request itself emits nothing on the timeline. So the awaited job is not behind a D3D12 fence or copy of ours.

The storage/file reach was not re-run with its trace switches in these arms; section 13.5 measured the same start in
an arm that ran `PALMETTO_FILE_TRACE`, `PALMETTO_DSTORAGE_TRACE` and `PALMETTO_DS_LOAD_TRACE` in the same boot and
found no open, read, enqueue or repair after the start (the last file event 3.3 s and the last storage repair 3.6 s
before it). The request is not waiting on a storage answer.

### 14.7 Verdict, limits and the next falsifier

- **Verdict: negative for a runtime producer, with the wait attributed one level deeper.** The session-3 request is
  created, cached and attached once; the loading update walks the request cache and steps it every frame; the stream
  pump and stream step run every frame; but the stream's open gate closes after the attach (manager-table slot null,
  entry state 0 not 2), so the async command engine is entered only in the attach window. Every completion entry point
  stays at zero, the request stays in the cache with its fields unchanged, and the session word stays 3.
- **No runtime answer is on the path.** The gate reads title bookkeeping (a manager-table pointer and a state dword);
  the request cache, the request, the stream and the manager are all title-side objects. The D3D12 timeline has no
  event tied to the request and the storage reach was measured empty in section 13.5's same-boot arm. No shim change
  is implied and the default path is byte-identical; with the switches unset the probe installs nothing.
- **The picture is unchanged.** No session advance means no movie draw: the player holds state `0x9`, all 12
  presenter captures are mean rgb `0.00 0.00 0.00` in both clean arms, and of the six 128x128 readbacks three are
  mean 0 and three are the replay layer's own `51 102 204` write control (16,384/16,384 non-black), not title
  content.
- **Limits.** Two clean arms, one stage-3 window each (58.8 s and 62.1 s pump reads), plus one pre-stage loss; the
  write watch samples four 8-byte fields only (the owner `+0x30`, key `+0x38` and flag `+0x3C` are covered by the
  100 ms poller, not by a breakpoint); the gate sampler reads one table slot and one state word, and the 16 engine
  steps' out record and queue operations are not identified; `stream1`'s two cache sites failed to arm because their
  RVAs were transcribed with the wrong image offset, so only `stream3` measures the cache lookup; and the probes'
  traps shift timings by their cost, so the stage-3 arrival spread is not compared between arms.
- **Next falsifier.** Name the writer of the manager-table slot and the state transition: a hardware write watch on
  `table + 0x18 + (id-1)*0xC0` and on the entry's `+0xAC` state dword (replacing the two request watches that measured
  zero), with the request consume `0x4043300` and the subscriber completion `0x2355D40` as zero-hit controls, plus
  execution probes on the registry lookup `0x45E690` and the command-start handler lookup `0x402F1C0` / queue
  registration `0x402D010`. If the slot writer runs before the attach and the state word never reaches 2, the wall is
  the open transition itself; if the slot is never written again after it is emptied, the wall is whatever code is
  supposed to open the stream in this build's loading sequence.

### 14.8 Tests and gates

- No test target is added: the slice is an opt-in diagnostic path in `port/src/main.cpp` with no pure helper to
  exercise, and every switch is off in a default run.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the GPU-free gate targets exit 0, and every `tests/test_docs_*.py` check passes.
- Evidence outside the repository: `D:\leonida-work\fsrstable\boot-stream1`, `boot-stream2`, `boot-stream3` with
  `stderr.log`/`stdout.log`/`summary.txt`/`meta.txt`/`boot.jsonl`, `capture-stream1`/`capture-stream3` and
  `readback-stream1`/`readback-stream3`; and `D:\leonida-work\streamreap` with the arm log, the analysis script, the
  cross-reference and vcall scanners and the decompiled ranges (`decomp-sr1` through `decomp-sr9`). No game data:
  RVAs, offsets, register/pointer values, counts, times and image means only.

### 14.9 Attribution

This section was implemented from this project's own measurement: the two clean 300 s arms and the pre-stage loss,
the twenty execution probes and their register reads and unwound chains, the hardware write watch and its live
control, the two samplers and the fine-change log, the three state-word timelines, the capture and readback means,
and the disassembled/decompiled request step, stream pump, stream step, async engine step, open check, manager update
and request-cache walk. The static check reused the read-only Ghidra project copy section 1 describes
(`D:\leonida-work\decomp-stage\ghidra-proj`), with the new decompiled ranges under
`D:\leonida-work\streamreap\decomp-sr1` through `decomp-sr9`; no peer code was used and no game data is added: RVAs,
field offsets, instruction bytes, values, counts and outcomes only.


## 15. Measured: the stream's open gate opens through the registry engine pointer, and the completion dispatch re-registers the engine's out record because its state reads 0, not 2

**Status: measured, diagnostic only, negative for a runtime producer; the "ready state 2" the section-14 gate tests is a
state word this build never writes, and the wall one level deeper is named.** Section 14 left one falsifier: a hardware
write watch on the manager-table slot and the entry's state block, execution probes on the registry lookup
`0x45E690`, the command-start handler lookup `0x402F1C0`/registration `0x402D010`, plus a fine sampler, with a same-boot
live control. This section adds one opt-in probe, `PALMETTO_STREAMOPEN`, with 30 int3 sites, four hardware write
watchpoints and a 10 ms change sampler, and reports three arms with the class-0xa mitigation on. In both arms that
reached the session-3 window the subscriber start, the command start, the registration and the attach ran once each; the
open gate's first condition then passed because the registry engine pointer at `table + (id-1)*0xc0 + 0x10` was written
by the title setter `0x5AEAC50` (writer `game+0x5AEAC5F`) on the registration, and the gate's second condition (entry
pointer non-null and `entry + 0xac == 2`) was never the one that opened: **no title store ever wrote the entry's `+0xac`
state block in the measured windows** (zero data-breakpoint hits on it while the gate was evaluated 55 times). The
completion never ran for a second reason, also measured: when the engine's step produced an out record, the dispatch
`0x402E8A0` read that record's `+0xac`, found 0, and took its re-registration branch (four `0x402D010` calls, four
`0x5AEABF0` closes) instead of the state-2 completion branch `0x402E050` (zero hits); the cache completion `0x4043390`,
the request consume `0x4043300` and the record-completion writer stayed at zero. No shim write, storage answer, GPU
event or runtime-visible call is on the path, so no shim behaviour changes and the default path is unchanged. Session
stayed 3, the movie player held state `0x9` with `buffers 0`, and every capture and readback stayed black. The next
falsifier is the decisive diagnostic named in 15.6.

### 15.1 The probe

`port/src/main.cpp` gains the section `PALMETTO_STREAMOPEN`. It is opt-in (unset or `0`: one cached `getenv`, no
handler, no thread and no patch) and uses the section 13/14 trap discipline: one-byte `int3`, restore, single-step,
re-arm. `PALMETTO_STREAMOPEN_WATCH` (on by default when the probe is on; `=0` turns it off) arms the hardware write
watch. Thirty sites are armed once, after the image is mapped and before the entry point:

| site | RVA | what it measures | `open2` | `open3` |
|---|---|---|---:|---:|
| `subscriber-start` | `0x2355DD0` | the loading subscriber's start; its stream object at subscriber `+0x50` | 1 | 1 |
| `stream-submit` | `0x1DBC0F0` | the stream submit (command id, flag) | 1 | 1 |
| `stream-pending-register` | `0x402F0F0` | the attach that stores the request at stream `+0x120` | 1 | 1 |
| `stream-command-start` | `0x402F120` | the command start: handler lookup, registration, the gate re-check and the start event | 1 | 1 |
| `handler-lookup` | `0x402F1C0` | the stream's command-handler array lookup (`count` at stream `+0x3c`, array at `+0x40`, stride `0xb8`) | 1 | 1 |
| `register-open` | `0x402D010` | the registration/open of the stream's async queue (`stream+0xe8`) | 4 | 4 |
| `register-cancel` | `0x402D320` | the cancel/teardown the engine step runs on an out record | 1 | 1 |
| `stream-open-gate` | `0x45E640` | the gate: `0x45E690` engine pointer or `0x45E6D0` entry with `+0xac == 2` | 55 | 55 |
| `registry-engine-lookup` | `0x45E690` | `*(u64*)(table + (id-1)*0xc0 + 0x10)` | 175 | 175 |
| `queue-vtable-site` | `0x402D15D` | just before the registration's queue-object virtual call at vtable `+0x18` | 4 | 4 |
| `engine-state` | `0x402D430` | the engine object's state machine (`+0x58` 1/2/3, `+0x40` job) | 2 | 2 |
| `record-key-lookup` | `0x1C9B8D0` | `*(u64*)(table + (id-1)*0xc0 + 0x8)`, the record's job key | 95 | 95 |
| `registry-update` | `0x1C9B8E0` | the registration's registry update that can store the `+0x10` engine pointer | 11 | 11 |
| `registry-close` | `0x5AEABF0` | the close path that clears the record's `+0x8`/`+0x10`/`+0x18` | 4 | 4 |
| `registry-set-job` | `0x5AEAC40` | stores `+0x8 + (id-1)*0xc0` | 16 | 16 |
| `registry-set-engine` | `0x5AEAC50` | stores `+0x10 + (id-1)*0xc0` (the gate's first condition) | 16 | 16 |
| `registry-set-entry` | `0x5AEAC60` | stores `+0x18 + (id-1)*0xc0` (the gate's second condition) | 8 | 8 |
| `registry-set-tail` | `0x5AEACE0` | stores `+0xb8 + (id-1)*0xc0` | 14 | 14 |
| `registry-set-records` | `0x5AEAD00` | commits the record's out-record vector (`+0x20`/`+0x28`/`+0x30`, count `+0xb0`) | 8 | 8 |
| `stream-pump` | `0x45E570` | the per-frame stream pump inside the request step | 5 | 5 |
| `stream-step` | `0x402E260` | the stream step the pump runs with the pending request | 8 | 8 |
| `async-engine-step` | `0x402E340` | the async engine step (always with the session-3 request) | 12 | 12 |
| `completion-dispatch` | `0x402E8A0` | the engine step's dispatch of a produced out record | 3 | 3 |
| `cache-complete` | `0x4043390` | the async cache completion (the only request-completion path) | **0** | **0** |
| `owner-busy-check` | `0x88B3F0` | `*(u8*)(owner+0x11a) & 1`, the request step's owner check | 5 | 5 |
| `request-step` | `0x45E430` | the loading update's request-cache step | 5 | 5 |
| `request-consume` | `0x4043300` | the request consume (callbacks, sub-ops, owner, flag) | **0** | **0** |
| `completion-run` | `0x402E050` | the state-2 completion branch of the dispatch | **0** | **0** |
| `queue-process` | `0x599B7B0` | the queue holder's vtable `+0x18` method the registration calls | 21 | 21 |
| `queue-poll` | `0x599B790` | the queue holder's vtable `+0x20` method | 7 | 7 |

The data watch arms when the subscriber start names the stream, installs on the loading task's own thread first (so
the registration's stores are caught), and a sweep arms every other thread for later writers:

| breakpoint | address | role |
|---|---|---|
| DR0 | `table + (id-1)*0xc0 + 0x10` | the registry engine pointer the gate's first lookup reads |
| DR1 | `table + (id-1)*0xc0 + 0x18` | the entry pointer the gate's second lookup reads |
| DR2 | `stream + 0x118` | the stream state byte, the `+0x11a` owner-busy bit and the pending request pointer at `+0x120` |
| DR3 | `game+0x78acb08` | the session-3 poll's slot-4 tick: the same-boot title-store live control section 11.3 measured |

The 10 ms thread prints one change line for the gate's two inputs, the entry's state/handler fields, the engine's state
and job, the stream's `+0x118` block, the request's first five qwords and the site counters; a 2 s sampler prints the
state words, the movie player, the record and the manager-table record, and a 10 s reporter prints the counters. The log
is bounded: per-site caps, 800 hit lines, 260 fine lines, 130 sampler lines and 160 watch lines.

### 15.2 The arms and the environment

Three arms, one at a time under `_setup/boot_lock.ps1` (`-Who streamreg`), `-Light`, through `tooling/boot_report.py`,
300 s timeout, logs outside the repository. The environment is section 17.1's content environment (deferred compile,
2 s state trace, stage-3 pump, alloc watch, console graphics root, slot-70 publication, content probe, full-target
readback, captures every 100 frames) with the mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1`, `PALMETTO_STREAMOPEN=1` and
the template's write watch; the two session-3 arms also ran `PALMETTO_USER_TRACE=1` and `PALMETTO_INPUT_TRACE=1`.

| arm | build | wall | session 3 | stop | device loss | captures / readbacks |
|---|---|---|---:|---|---|---:|
| `open1` | 24 sites, `9BFB7B0C...` | 62.4 s | not reached (stage 2 candidate only) | STOP `game+0x4064926` read `0x1c` | 0 | none assembled |
| `open2` | 24 sites, `9BFB7B0C...` | 103.1 s | reached 54.5 s | STOP `game+0x43AE303` read `0x10000007e` | 1 | 15 captures, all mean 0; 4 readbacks, mean 0 |
| `open3` | 30 sites, `2A553C49...` | 97.8 s | reached 51.3 s | STOP `Bink2Scarlett.dll+0x5de9` read `0x1660000005c` | 1 | 13 captures, all mean 0; 4 readbacks, mean 0 |

No arm is a clean 300 s arm: one stopped before stage 3 and the two that reached session 3 stopped about 45 s later,
each after one `present failed (VkResult -4)` device loss. The programme's rule is to stop after two device losses, so
the measurement ends with these three. The two session-3 arms agree wherever both armed a site, and each stop is in a
family this programme already records: `open1` in the pre-stage read family ([3d_textures.md](3d_textures.md) section
11.1), `open2` at `game+0x43AE303` after the device loss, and `open3` inside the movie middleware after the device
loss.

`open2`'s `meta.txt` was rewritten by a refused rerun after the probe rebuild (the runtime refuses to overwrite an
existing `boot.jsonl`); the arm itself was not re-run, and its own arming line records the 24-site build it ran. The
exe hash in `open1`'s `meta.txt` (`9BFB7B0CEB8AE10D...`) is the value recorded when `open2` started too.

### 15.3 The registration and the open transition, measured

Times below are `open3`'s trace clock; `open2` measured the same order with its own times (+3.2 s on the start).
Every value is from the same boot as its control.

| t (s) | measurement |
|---:|---|
| 51.27 | `subscriber-start`: subscriber `0x1667e248450`, stream `0x1667e248520` (vtable `game+0x658de48`), id 1, table `0x1667e248890` |
| 51.28 | `stream-submit`: command id 0 on the same task; `stream-command-start`: the stream's handler array count is 2 and its first entry is at table `+0xD0` (`vtable game+0x658de38`) |
| 51.29 | `register-open`: queue `stream+0xe8`, id 1, table `0x1667e248890`, queue holder at `stream+0x10` with vtable `game+0x658de60`; its virtual methods are `game+0x599b7b0` (`+0x18`) and `game+0x599b790` (`+0x20`) |
| 51.29 | first gate hit: engine 0, entry 0, `open no`; the registration's initial gate call reads the same |
| 51.31 | `registry-set-engine`: `record+0x10 <- 0`; `registry-set-entry`: `record+0x18 <- table+0xD0`; `registry-set-job`: `record+0x8 <- 0` |
| 51.33 | second gate hit: engine `stream+0x58`, entry `table+0xD0`, entry state `+0xac` 0, **`open yes`** (the first condition) |
| 51.35 | `stream-pending-register`: the request is attached at stream `+0x120` (vtable `game+0x658de18`, key `0x9837FB2E`, owner = stream, flag `0x45003c00`) |
| 51.4-52.0 | `async-engine-step` 5 times, each with the session-3 request in `r9`; the engine object moves from `stream+0x58` to a heap object `0x1667e32c280` with vtable `game+0x658de90`, its `+0x58` state walking 0 -> 1 -> 2 |
| 52.0-74.9 | the request step stops being run: the pump and request-step counters stay at 4 and no engine step arrives; the loading word's tick does not advance (22678982 since 48.0 s) |
| 74.9-76.7 | three `completion-dispatch` hits: out records `0x1667e24a498`, `0x16690448028`, `0x1667e426dc8`, vtable `game+0x658df20`, **`+0xac` 0** in all three; each is re-registered (`register-open` 2-4) and closed (`registry-close`) |
| 78-94 | the counters move once more (pump and request-step 5, engine step 12 by 80 s); the record's slot-4 completion dword stays 0 and the session word stays 3 |

The hardware watch names the writers of the two gate inputs and measures the state block:

| watch | hits | writer | values |
|---|---:|---|---|
| `TABLE +0x10 ENGINE` | 16 | `game+0x5AEAC5F` (inside the setter `0x5AEAC50`) | 0 and engine object pointers (`stream+0x58`, then heap objects with vtable `game+0x658de90`) |
| `TABLE +0x18 ENTRY` | 8 | `game+0x5AEAC6F` (inside the setter `0x5AEAC60`) | 0 and the command-handler/out-record pointers (`table+0xD0`, `game+0x658df20` records) |
| entry `+0xac` state block | **0** | - | never written in `open2`'s 24-site watch (the section-14 candidate) |
| `STREAM +0x118 STATE` | 2 | `game+0x58D6913` / `game+0x58D691A` | qword `0x1667e000100` -> `0x1667e000101`: the stream `+0x118` byte 0 -> 1; the `+0x11a` owner-busy byte stays 0 |

Three static facts complete the attribution, all from the image's own bytes:

- **The gate's two conditions** are `0x45E690(id, table)` = `*(u64*)(table + (id-1)*0xc0 + 0x10)` non-null and
  `0x45E6D0(id, table)` = `*(u64*)(table + (id-1)*0xc0 + 0x18)` non-null with `*(u32*)(entry + 0xac) == 2`
  [M, disassembly and decompile]. The registry setters are one-store helpers: `0x5AEAC40` stores `+0x8`, `0x5AEAC50`
  stores `+0x10`, `0x5AEAC60` stores `+0x18`, `0x5AEACE0` stores `+0xb8`, `0x5AEAD00` builds the record's out-record
  vector, and `0x5AEABF0` clears `+0x8`/`+0x10`/`+0x18` and the vector (the close) [M, decompiled C].
- **The registration** `0x402D010` sets the three record fields, commits a record vector, calls the queue object's
  virtual `+0x18` twice, runs the registry update `0x1C9B8E0` (which stores the `+0x10` engine pointer), and finally
  clears the registration when the record's `+0x8` equals its `+0x10` [M, decompiled C]. The command start
  `0x402F120` calls it, re-reads the gate itself, and raises the start event only when the gate is true [M].
- **The completion dispatch** `0x402E8A0` checks its out record's `+0xac`: state 2 runs `0x402E050` (the completion),
  any other state runs `0x402D010` again with the out record as the handler [M, decompiled C]. Its caller is the
  stream step `0x402E260`, which runs it only when the engine step produced an out record [M, decompiled C].

### 15.4 Verdict: the state-2 path is not the open path, and the completion path needs a state the engine never sets

- **The "ready state 2" is not what opens this stream.** The gate's second condition reads the command-handler/out
  record's `+0xac`, and no measured title code writes that word at all: the write watch on the state block caught zero
  stores in a boot where the gate was evaluated 55 times and the registration ran four times. The open transition is
  the gate's first condition, the registry engine pointer at `table + 0x10`, written by `game+0x5AEAC5F` (the
  `0x5AEAC50` setter) on the registration and cleared by the same setter when the engine step's out record is handled.
  Section 14's "manager-table entry never reaches ready state 2" is therefore true but is not the wall: that word is
  not part of this build's open path.
- **The deeper wall is the out record's state.** The engine step does produce out records (three of them, between
  `open3`'s 74.9 s and 76.7 s), and the dispatch reads their `+0xac`: all three read 0, so the state-2 completion
  branch `0x402E050` never runs, the framework re-registers each record through `0x402D010` (four registrations, four
  closes), and the cache completion `0x4043390`, the request consume `0x4043300` and the record-completion writer
  never see the session-3 request. The record class vtable `game+0x658df20` and its `+0x28`/`+0x40` job fields are the
  title's own async command objects; whatever should store 2 into `+0xac` is title-side and did not run.
- **No runtime answer of ours is on the path.** Every writer the watch named is a title function (`game+0x5AEAC5F`,
  `game+0x5AEAC6F`, `game+0x58D6913`); the gate and the dispatch read title bookkeeping only; the file, DirectStorage
  and GPU reach switches from sections 13.5 and 14.6 measured nothing at or after the start; and the section-13.5
  finding (no IO after the start) still holds. No shim is changed and the default path stays byte-identical.
- **The picture is unchanged.** Session stayed 3 with the machine at 3/3 in both session-3 arms, the movie player held
  state `0x9` with `buffers 0`, the record's slot-4 completion dword stayed 0, and every presented capture is mean
  rgb `0.00 0.00 0.00` (`open3`: 13 captures and four 3840x2160 readbacks; `open2`: 15 captures and four readbacks). No session 4 and no movie draw.

### 15.5 Limits, and the movie-middleware stop

- **No clean 300 s arm.** Three arms, one pre-stage stop and two session-3 stops at 103.1 s and 97.8 s, each after one
  device loss; the programme's two-device-loss rule ended the boots. The session-3 window is about 45 s per arm, and
  the two arms agree on every site both measured.
- **The 22 s loading pause is not attributed.** Between the first registration and the out records the loading task
  runs no request step (the pump and request-step counters stay at 4) and no engine step arrives; its stack was not
  sampled in these arms (no `PALMETTO_THREAD_STACKS`/`PALMETTO_WAIT_TRACE` line), so whether the pause is title work
  or a wait is open. The owner check is not the cause: `0x88B3F0` returned "not busy" on all five calls and the
  `+0x11a` byte was never set.
- **The state-block watch arms at the entry's publication.** In the 24-site arm the third breakpoint is armed when
  `0x5AEAC60` first stores the entry pointer, so a store into `+0xac` before that store would not be caught; its
  measured effect would be the state 0 the gate read, and the same boot's DR0/DR1 hits and live control prove the
  debug registers were honoured.
- **The probes shift timing** by their trap cost (about 150 k traps per session-3 arm across 30 sites), and no
  probe-off A/B was run in this environment; the stops are in families the programme records without the probe.
- **`open3`'s stop is in the movie middleware**, not on the stream path: `Bink2Scarlett.dll+0x5de9` read
  `0x1660000005c` after the device loss, inside a Bink decode attempt on a pointer derived from the console address
  range. That is the movie/Bink path section 12 named, and it is the next slice's subject; this section does not
  attribute it.

### 15.6 The next falsifier

The decisive diagnostic is one opt-in title-side patch and it needs the lead's routing before implementation: at
`0x402E8A0`, make the state test treat the engine's out record as state 2 when it reads 0 (a one-byte conditional
redirect over the compare, installed after the image is mapped and before the entry point, off by default). If the
session-3 request then completes - `0x402E050`, then `0x4043390`, then the record-completion writer, then session 3 ->
4 and the movie player leaving state `0x9` - the missing producer is the out-record state store and the next slice
chases that store. If it still does not complete, the wall is upstream of the dispatch and the next probe should
re-target the state watch on the out record `+0xa8`/`+0xac` and arm `0x402E050` and `0x599B7B0` as the completion and queue-process controls, with the 22 s loading pause sampled by
`PALMETTO_THREAD_STACKS` at 60/70 s to name the blocked site.

### 15.7 Tests and gates

- No test target is added: the slice is an opt-in diagnostic path in `port/src/main.cpp` with no pure helper to
  exercise, and every switch is off in a default run.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the GPU-free gate targets exit 0, and every `tests/test_docs_*.py` check passes.
- Evidence outside the repository: `D:\leonida-work\fsrstable\boot-open1`, `boot-open2`, `boot-open3` with
  `stderr.log`/`stdout.log`/`summary.txt`/`meta.txt`/`boot.jsonl`, the matching `capture-open*` and `readback-open*`
  directories, and `D:\leonida-work\streamopen` (`run_arm.ps1`, `analyze.py`, the two analysis transcripts). The
  static work reused the read-only Ghidra project copy section 1 describes, with decompiled ranges under
  `D:\leonida-work\streamreap\decomp-sr10` through `decomp-sr17`. No game data: RVAs, offsets, register/pointer
  values, counts, times and image means only.

### 15.8 Attribution

This section was implemented from this project's own measurement: the three arms, the 30 execution probes and their
register reads, the four hardware write watchpoints and their live control, the 10 ms fine-change log and the 2 s
sampler, the capture means, and the disassembled/decompiled gate, registry setters, registration/open, command start,
engine step, completion dispatch and stream-step functions. No peer code was used and no game data is added: RVAs,
field offsets, instruction bytes, values, counts and outcomes only.
