# The 10^9 in the fence proxy's vtable slot (stop `game+0x4fb0aec`)

**Status: measured on one machine, 2026-11-24**, against the 1.0.2.1 target console executable, one `--run` per
configuration of the Vulkan backend with the shader corpus. This page answers the tracker's O10: who wrote
`0x3b9aca00` (10^9) into the object at `game+0x4fb0aec`, whose first qword must be a vtable pointer. Nothing here quotes
executable bytes, game data, package paths or extracted tables: RVAs, structure field offsets, register values, counts
and the project's own wording only.

**Answer: our own runtime wrote it, in an answered console call whose vtable slot does not mean on the console what it
means on the PC.** The object is the **`ID3D12Fence` proxy** the runtime creates for console device slot 36
(`CreateFence`). The 10^9 is the runtime's own `GetTimestampFrequency` answer (`1e9 / timestampPeriod`, 1 GHz on this
GPU). The title's deferred-submit routine calls the console queue's slot 16 with `(fence, value)` — the shape of
`Signal`/`Wait` on this title's console layout, which has **two methods inserted before `Signal`** — and the runtime's
queue proxy forwards console slot numbers to the PC vtable unchanged. PC slot 16 is `GetTimestampFrequency(UINT64*)`,
whose first argument is an out pointer: the PC method stored `0x3b9aca00` to the fence proxy, replacing its vtable
pointer, and the later fence wait dereferenced it. This is hypothesis (a) (a layout/slot mismatch in an answered call),
not (b) or (c); section 6 rules those out with the measurement.

## 1. The stop

`game+0x4fb0aec` is `call qword ptr [rax + 0x60]`; `rax` comes from `mov rax, qword ptr [rcx]` at `game+0x4fb0ae0`, so
the object in `rcx` has 10^9 as its first qword and the call target computes to `0x3b9aca60`. The chain is
`0x4fb0aec <- 0x4fb0ac4` (the glue wrapper `game+0x4fb0a90`) `<- 0x23cf363` (`game+0x23cf210`, the frame-end wait).
Measured registers at the fault (one boot, `PALMETTO_DUMP_REGS`): `rcx = 0x249ba60a850`, `rdx = 2`, `rsi = 2`,
`rax = 0x3b9aca00`; the earlier captured values were of the same shape with a different heap address.

The dispatcher above the wait is worth having in one place, because it says what the object is for. `game+0x4fb0a50`
takes a 64-bit token (`(type << 56) | value`), resolves `type` through a table at `image+0x68929D0`, and calls
`game+0x4fb0a90(rcx = table[type]->+0x10, rdx = token, r8d = flags)`. The wrapper builds a two-qword continuation
`{0, game+0x10a330}` on its stack and calls `game+0x4fb0ad0`, which does

```
mov rax, [rcx]          ; the object's vtable
call qword ptr [rax+0x60]   ; slot 12: returns a pointer to the value being waited on
```

and then loops on `monitorx`/`mwaitx` until `[rdi]` differs from the masked token value, invoking the continuation while
it waits. So slot 12 of that object is expected to answer "address of the watched value", and the object is the
waitable the title keeps for a channel.

## 2. The object in `rcx`: the `ID3D12Fence` proxy

The type-0 channel is the static singleton at `image+0x75E0E98`. The object in `rcx` is `[image+0x75E0EA8]`, i.e.
`channel+0x10`; this was verified at the fault (the dump of `image+0x75E0E98` shows `+0x10` equal to the faulting
`rcx`) and it is written once during boot:

| item | value |
|---|---|
| writer of `channel+0x10` | `game+0x5a8619a` (`mov [rsi+0x10], rax`, in `game+0x5a86180`, the channel constructor), found with a page write trap on `image+0x75E0E98..0xFFF` |
| what it stores | the return of `game+0x1d0b2b0`, a small COM creation helper |
| creation call | `game+0x1d0b2fa`: `call qword ptr [rax+0x120]` on the game's device object, with `r9 = &IID_ID3D12Fence` and the out pointer at `[rsp+0x20]` |
| console slot reached | `0x120 / 8 = 36` — console device slot 36, `CreateFence`; the runtime's own log for the same boot says `device slot 36 (CreateFence -> fence proxy) first call` and `fence slot 0 (QueryInterface)` / `slot 1 (AddRef)` right after it |

So `channel+0x10` is the **fence proxy** the runtime hands the title for that `CreateFence`. The same object is reached
from the game's other channel code (`game+0x248B4B0` releases it before `game+0x2472560` re-zeroes the field, and
`game+0x50BB280` reads the channel's token from `+0x20`). A fence interface is a COM object whose first qword is its
vtable pointer, and the proxy's is not the PC's: the runtime builds each child proxy's table in its own page
(`runtime/shims/d3d12_xs_device.cpp`, `get_child_vtable`), so the first qword is the table address inside that page,
placed at page end minus the slot count.

Measured at the moment the channel stored it (one boot, the write trap on `image+0x75E0EA8`): the new value was
`0x28540fd5300`, and the first 0x40 bytes at it began with `0x1c9f6fa0e00` — an address whose low 12 bits are
`0xE00` = page end − `kFenceSlots * 8` (`64 * 8 = 0x200`), exactly the fence proxy table layout, with the remaining
fields (heap pointers at `+0x08`/`+0x10`, a host code pointer at `+0x18`, `{0xC, 1}` at `+0x20`) that the same proxy
carries at the fault.

At the fault the same object reads, in its first 0x100 bytes (`PALMETTO_FAULT_MEM`):

| offset | qword | note |
|---|---|---|
| `+0x00` | `0x000000003b9aca00` | the corrupted vtable pointer |
| `+0x08` | `0x0000029ff9569d90` | heap pointer (as at creation) |
| `+0x10` | `0x00000292aeef8e00` | heap pointer (as at creation) |
| `+0x18` | `0x00007ff71b1cf090` | host code pointer (as at creation) |
| `+0x20` | `0x000000010000000c` | `{0xC, 1}` (as at creation) |
| `+0x28` | `0x90019e00a281933e` | unchanged |
| `+0x30`/`+0x38`/`+0x40`/`+0x48` | heap pointers and `{0, 2}` | unchanged |
| `+0x60`/`+0x68` | `object+0x60` | unchanged |

Only `+0x00` differs from the creation-time dump; every other field the proxy was built with is intact. That is the
signature of a single 8-byte store into offset 0, not of a garbage or freed object.

## 3. The write, measured

The runtime's PC `ID3D12CommandQueue::GetTimestampFrequency` computes `1e9 / timestampPeriod` and stores it through its
first argument; the backend's `timestamp_period` is 1.0 ns on this GPU, so the stored value is exactly `0x3b9aca00`.
A one-shot host probe (`PALMETTO_POKE_HOST`, an `int3` over the store instruction, re-armed on the single-step) caught
two hits in one boot on the final build:

| hit | `rax` (value stored) | `rdx` (out pointer) | `r8` | `r9` |
|---:|---|---|---:|---:|
| 1 | `0x3b9aca00` | `0x249ba60a850` | 1 | 0 |
| 2 | `0x3b9aca00` | `0x249ba60a850` | 2 | `0x7ffe6000` |

and the guest fault of the same boot, 0.19 s later, is `game+0x4fb0aec` with `rcx = 0x249ba60a850` and
`rax = 0x3b9aca00`. The out pointer the two frequency writes used **is** the object whose vtable slot the wait
dereferences. The probe also dumped the guest stack: the return address is `game+0x248260E`, and the instruction that
called into the proxy is `game+0x2482608`, `call qword ptr [rax + 0x80]` (vtable offset 0x80 = slot 16).

The guest call shape, from the submit routine `game+0x2481E90` (its records are 0x48 bytes at `this+0x10`, walked with
an index at `this+0x1b2c` under the lock at `this+0x1b10`): for each record it loads the object at `record+0x00` into
`r15`, and at `0x2481FCC..0x2481FF9` gathers the record's fields `+0x30` and `+0x40`; at `0x24825E0` it sets

```
mov rdx, [rsp+0x98]     ; record+0x30 -> first argument
mov rsi, [rsp+0xa0]     ; record+0x40 -> second argument
mov rax, [r15]          ; the queue proxy's vtable
mov rcx, r15            ; this
mov r8, rsi
call qword ptr [rax+0x80]
```

The first argument is the fence proxy (the object later clobbered) and the second is a small integer that took the
values 1 and 2 in the two calls. That is the calling shape of `Signal(fence, value)`/`Wait(fence, value)`, not of
`GetTimestampFrequency(out)`.

## 4. Why console slot 16 is not PC slot 16

The runtime's queue proxy is a child proxy over the PC `ID3D12CommandQueue`, and its table labels the console slots in
PC order (`runtime/shims/d3d12_xs_device.cpp`: `kQueueSignal = 14`, `kQueueWait = 15`, the queue names table gives
`GetTimestampFrequency` at 16, `GetDesc` at 18). For the generic slot it forwards to `self->real_vtable[Slot]` with the
same argument positions, so console slot 16 calls PC `GetTimestampFrequency` and PC slot 16's out-pointer write lands on
whatever the console call had in the first argument — here the fence.

This title's console queue chain is not the PC chain: it has **two methods inserted before `Signal`**. Two independent
facts put it in that layout:

* The same submit routine calls `call qword ptr [rax + 0x100]` (`game+0x24824C1`) = console queue slot 32, and the
  runtime's log for it is `queue slot 32 is beyond the PC queue vtable (19 slots); returning E_NOTIMPL (args 0x1 <ptr>
  <ptr> 0x2)` — the `(planeCount, planes, params)` shape of `PresentX`. `docs/d3d12_xs_surface.md` records the public
  observation (a public runtime project) that `PresentX` is at queue slot **30** in the older console layout and **32** in the newer
  one, "with the same SDK version but two methods inserted before `Signal`".
* Console slot 16 is called with `(fence, value)` (section 3). In the unshifted PC layout slot 16 is
  `GetTimestampFrequency(UINT64*)`, which a title does not call with a fence as its out pointer; `Signal` is the method
  with that shape.

So for this title the console queue's standard chain is `14`/`15` = the two inserted methods (never called in this boot),
`16 = Signal`, `17 = Wait`, `18 = GetTimestampFrequency`, `19 = GetClockCalibration`, `20 = GetDesc`, and the console
extensions follow (slot 32 `PresentX`, slot 35 another). The runtime never saw console slots 14/15 in this boot — the
trace has no `queue slot 14/15` line — while it did see slots 16, 18, 32 and 35. The slot-18 special case in the runtime
still answers a struct-return `GetDesc`; in this layout slot 18 is `GetTimestampFrequency`, which is the same
slot/interface mismatch one index over and should be re-measured before it bites (section 7).

The boot's other queue calls fit the same picture: slot 0/1 (`QueryInterface`/`AddRef`), slot 32 (PresentX shape),
slot 35 (the calibration pair shape the runtime already special-cases) and the two slot-16 calls.

## 5. Why the wait then faults

The frame-end routine (`game+0x23cf210`) reaches the wait through `game+0x9796d0`, which asks the type-0 channel for its
token and tail-calls `game+0x4fb0a50`. In the caught boot the token was `2` (type 0): the dispatcher resolves
`table[0] = image+0x75E0E98`, loads the waitable at `channel+0x10` — the fence proxy — and the wrapper reaches
`game+0x4fb0ad0` with `rcx = <fence proxy>`. `mov rax, [rcx]` now reads `0x3b9aca00`, and `call [rax+0x60]` reads at
`0x3b9aca60`, which is unmapped: the `0xc0000005` at `game+0x4fb0aec`.

Nothing on the wait path reads the fence as a fence; it only expects a virtual "get the watched address" at slot 12 of
whatever object the channel holds. That is why the corruption is fatal exactly here. Even with the vtable intact, the
fence proxy's slot 12 is outside the PC `ID3D12Fence` chain (0..11), so after the slot-number fix this path is the next
thing to measure (section 7).

## 6. Hypotheses ruled out

* **(b) a title sentinel.** The 10^9 is not a title value: it is the runtime's own answer, `1e9 / timestampPeriod`
  (1 GHz on this GPU), and the probe catches the runtime's own store instruction writing it. The guest made no 10^9
  store; the only guest activity at that address is the deferred-submit call handing the fence to the proxy.
* **(c) a legitimate object clobbered by a 32-bit write.** At creation the object's first qword was the fence child
  proxy table pointer (a host page address ending in `0xE00`), and the write that replaced it stored a full 64-bit value:
  `0x000000003b9aca00`. The high dword is zero because the value fits, not because a 32-bit store left the upper half
  intact. Both frequency stores wrote through the same 8-byte out pointer.
* **A garbage or freed object.** The channel field was written once (write trap) and the object's other fields are the
  ones it was created with; only offset 0 changed. The fault is the use of a real object whose vtable pointer was
  overwritten.

## 7. What changed (diagnostics)

`runtime/shims/startup_trace.h` gained three env-gated diagnostics, all off in a normal boot:

| variable | what it does |
|---|---|
| `PALMETTO_FAULT_MEM=1` | at a fault, dumps memory at `rcx`, `rdx`, `r8`, `r9`, `rdi`, `rsi`, `r14`, `r15`, `rax`, the 0x40 bytes below `rcx` and the stack, as hex in the exception event |
| `PALMETTO_FAULT_MEM_RVA=<hex rva>[,<hex rva>...]` | dumps 0x100 bytes at up to four image RVAs in the same event (this is how `channel+0x10` and `channel+0x20` were read at the fault) |
| `PALMETTO_WATCH_RVA_WRITE=<hex rva>` | makes the image page holding the rva read-only, logs every store into it with the store instruction and registers, single-steps and re-protects (this named `game+0x5a8619a` as the writer of `channel+0x10`) |
| `PALMETTO_POKE_HOST=<hex rva>[,<hex rva>...]` | one-shot `int3` over a runtime-image RVA, re-armed after a single-step; logs the registers and 192 words of the guest stack (this named the store inside `GetTimestampFrequency`, the guest return address `game+0x248260E` and the call site `game+0x2482608`) |

The host RVA of a given store moves with every rebuild; `tools`-side scratch used a pattern search for the
`GetTimestampFrequency` tail (`cvttsd2si / add / mov [rdx], rax / xor eax, eax / ret`) to find it per build. An earlier
attempt that also trapped writes into the object's own page (a host-heap page) re-entered the runtime's allocator from
the handler and changed the boot; it was removed rather than kept, and the trap stays on the image's own pages. Baseline
numbers in section 9 are the measurement that the diagnostics do not change a normal boot.

## 8. The smallest next change

In `runtime/shims/d3d12_xs_device.cpp`, stop forwarding console queue slot 16 to PC slot 16, because the two are
different methods. For this title's layout (section 4):

* console queue **16** → PC `Signal` (real-vtable slot 14), with the fence proxy argument unwrapped like the existing
  `kQueueSignal`/`kQueueWait` path (`unwrap_object(a1)`), value from `a2`;
* console queue **17** → PC `Wait` (real-vtable slot 15), same shape;
* console queue **18** → PC `GetTimestampFrequency` (real-vtable slot 16) and **19** → PC `GetClockCalibration`
  (slot 17); move the current slot-18 `GetDesc` struct-return case to console **20** (PC slot 18).

Because the shift is inferred from the two facts in section 4 rather than from a per-slot argument dump, the first
measured step should be one boot that logs the argument registers of console queue slots 16..20 (the existing
`note_child_slot` first-call line plus `a1..a4`), to confirm 16 = `(fence, value)` and 18 = `(out pointer)` before the
swap. If the dump agrees, make the swap and re-measure.

Expected signature of success: the two `XboxID3D12CommandQueue::GetTimestampFrequency called` lines for console slot 16
disappear (10^9 is no longer stored over the fence's vtable), the fence is signalled with values 1 and 2, and the stop
moves off `game+0x4fb0aec`. The likely next stop is the wait dispatcher's slot 12 on the same fence proxy: the PC
`ID3D12Fence` chain ends at slot 11, so `game+0x4fb0ad0`'s `call [rax+0x60]` will reach the proxy's refused tail
(`E_NOTIMPL`) and then read `[0x80004001]`. That stop would be the next measurement, not part of this fix.

## 9. The measurement

One boot at a time, same environment (`PALMETTO_SHADERS=<shader corpus work folder>`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`), same harness (`tools/boot_report.py`):

| run | instrumentation | max traced call | stop |
|---|---|---:|---|
| baseline, current tree | none | **#12085** | `game+0x4fb0aec`, read at `0x3b9aca60` |
| object page at fault | `PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1` | **#12085** | same |
| channel field + object at fault | the two above plus `PALMETTO_FAULT_MEM_RVA=75E0E98,75E5E90` | **#12085** | same |
| writer of `channel+0x10` | `PALMETTO_WATCH_RVA_WRITE=75E0EA8` | **#12085** | same |
| the frequency write | `PALMETTO_POKE_HOST=<store rva>` | **#12085** | same |
| final tree, diagnostics armed | `PALMETTO_DUMP_REGS=1`, `PALMETTO_POKE_HOST=<store rva>` | **#12085** | same |

Pipeline failures stayed 0 and the stop instruction and fault address were identical in every run. (The page-trap
attempt into the object's own heap page mentioned in section 7 is the one configuration that changed the outcome —
`0x80000004` in host code, early in boot — and is not part of the table; it was removed.)

## 10. Limits

* One machine, one copy of 1.0.2.1, one Vulkan backend build, one measured boot per configuration.
* The class and field names are inferred from code shape and from the runtime's own logs, not from symbols. "Fence
  proxy", "channel" and "deferred-submit routine" are descriptions of the measured behaviour.
* The two-methods-before-`Signal` layout is inferred from the console slot-16 call shape plus the project's own note on
  the public `PresentX` slot 30/32 split, not from a console header; section 8 keeps an argument-shape dump as the
  confirmation step before the swap.
* The exact identities of the inserted console slots 14/15 and of the console methods above 20 are not measured here.
* The host RVA printed for the `GetTimestampFrequency` store belongs to one build; it changes when the runtime changes.
