# The console fence's completed-value address (console fence slot 12)

**Status: measured on one machine, 2026-11-24**, against the 1.0.2.1 target console executable, one `--run` per
configuration of the Vulkan backend with the shader corpus. Nothing here quotes executable bytes, game data, package
paths or extracted tables: RVAs, structure field offsets, register values, counts and the project's own wording only.

**Answer: slot 12 of the `ID3D12Fence` proxy the runtime creates for console device slot 36 (`CreateFence`) is the
console extension that answers with the address of the fence's CPU-readable 64-bit completed value.** The title's
frame-end wait calls it as `(fence, target, &continuation, flags)`, keeps the returned pointer and polls `[pointer]`
with `monitorx`/`mwaitx` until the word there reaches the target, testing it with a 56-bit comparison and handling
`UINT64_MAX` (a removed device) before that test. The PC `ID3D12Fence`/`ID3D12Fence1` chain ends at slot 11/12, so the
console took a method the PC ABI does not have, and the fence is the waitable of the title's submit channel because the
console keeps the fence value where the CPU can watch it. The proxy now answers slot 12 with the address of a cell it
keeps at that value (section 5). Measured after the change: the stop at `game+0x4fb0b03` is gone, the boot runs to the
harness's 400 s limit with no exception and 6,887,539 traced D3D12 calls, and slot 12 itself is called **245,524** times
in that boot with its first answer `(target 0x2, completed 0x2)` (section 6).

## 1. The stop

The stop before this work was `game+0x4fb0b03`, `mov rdx, qword ptr [rdi]`, a `0xc0000005` read at `0x80004001`
(`E_NOTIMPL` used as an address). The call before it is `call qword ptr [rax + 0x60]` at `game+0x4fb0aec`: slot 12 of
the object in RCX. The proxy refused the slot as "beyond the PC fence vtable (12 slots); returning E_NOTIMPL (args
0x2 <stack> <flags> 0x0)", and the caller dereferenced the refusal. The object identity is measured in
`docs/frontier_4fb0aec.md` §2: the type-0 channel singleton at `image+0x75E0E98` has its `+0x10` written once, with the
return of a `CreateFence` through the game's device object; the runtime's own log for that object is "device slot 36
(`CreateFence -> fence proxy`) first call".

## 2. The caller: what slot 12 is asked for

The waiting function is `game+0x4fb0ad0` (`.pdata` `0x4FB0AD0..0x4FB0E84`, 948 bytes). Its entry keeps
`this = RCX`, `target = RDX`, `&continuation = R8`, `flags = R9D`, then calls the vtable slot:

```
mov rax, [rcx]              ; the waitable's vtable
call qword ptr [rax+0x60]   ; slot 12, with RCX/RDX/R8/R9 unchanged
mov rdi, rax                ; the answer is kept as a pointer
call game+0x10a2a0          ; arms the wait context on the stack frame
mov rdx, [rdi]              ; the word the call is about
```

and then polls that word in two loops (`game+0x4fb0bd0`, `game+0x4fb0cd4`): each iteration re-reads `[rdi]`, compares
it with `-1`, and tests `(value - target)` against bit 55 (`0x80000000000000`) — the title's 56-bit wrap comparison for
"has the value reached the target". The loops arm `monitorx` on the pointer (`rax = rdi`, ECX = EDX = 0), sleep with
`mwaitx` (ECX = 2, a bounded timeout in EBX) and invoke a continuation while waiting. A `-1` word takes the
`game+0x4fb0ca9` path, which calls `game+0x2474870` before the continuation; that function jumps to
`game+0x2474500`, a fatal-report routine (it sets a reporting flag, formats and hands a block to the crash machinery,
then `int3`). So `-1` is the title's "device removed" word, not a number it waits for.

The continuation the wrapper installs is `game+0x10a330`:

```
sub rcx, rdx        ; value - target
bt  rcx, 0x37       ; bit 55
setae al            ; true once the value has reached the target
ret
```

So slot 12's caller needs **the address of the 64-bit word whose value it waits for**; the pointer must stay valid
across the whole loop (no further call into the object happens while the loop spins).

## 3. Which object, and what its slot 12 must be

The wait dispatcher is `game+0x4fb0a50`. A 64-bit token carries its type in the top byte (`type = RCX >> 56`, `value =
RCX & 0x00FFFFFFFFFFFFFF`), the dispatcher resolves `table[type]` at `image+0x68929D0` (in `.rdata`; only entry 0 is
non-null in the file, `image+0x75E0E98`), takes `table[type]+0x10` and enters the wait function through the wrapper
`game+0x4fb0a90`, which masks the token to 56 bits with `bzhi`, builds the continuation struct `{0, game+0x10a330}` and
passes it in R8. The value part of the token comes from the channel object: `game+0x50BB280` builds
`(type << 56) | ([channel+0x20] & 0x00FFFFFFFFFFFFFF)`.

The type-0 channel's `+0x10` is the `ID3D12Fence` proxy. The fence is created by `game+0x1d0b2b0`:

```
mov  rcx, [image+0x6a4ea1d]     ; the game's device object
lea  r9,  IID_ID3D12Fence
xor  edx, edx                   ; InitialValue = 0
mov  r8d, 1                     ; D3D12_FENCE_FLAG_SHARED
mov  rax, [rcx]
mov  [rsp+0x20], &out
call qword ptr [rax+0x120]      ; device slot 36 = CreateFence
```

and `game+0x5a8619a` stores its result at `channel+0x10` (measured with the page write trap in
`docs/frontier_4fb0aec.md` §2). At the old stop the wait was on that same object, and the runtime log named it "fence
slot 12 is beyond the PC fence vtable" — so slot 12 is called on the fence proxy.

The fence interface's PC chain ends at `GetCreationFlags` (slot 11, ID3D12Fence1 = 12 slots); slot 12 is past it. The
caller's shape says what the console method does: it returns a pointer to a 64-bit value that is compared with the
target using the title's 56-bit wrap rule, whose `UINT64_MAX` is the removed-device answer. That is the **fence's
completed value in memory**, which is why the title uses `monitorx`/`mwaitx` instead of polling
`ID3D12Fence::GetCompletedValue` for this wait: before the first slot-12 call the boot's only fence calls are slots 0/1
(`QueryInterface`/`AddRef`); the first fence slot 8 (`GetCompletedValue`) call is later in the boot, at `#17856`.

## 4. What proves it (the measurement, not plausibility)

* The target the wait passed is the value the deferred submit had signalled through console queue slot 16 (`Signal`):
  `docs/frontier_4fb0aec.md` §3 measured the two `Signal(fence, value)` calls with values 1 and 2, and the wait's
  RDX at the stop was 2.
* The first answer of the new implementation was logged by the proxy in the measured boot:
  `fence slot 12 answered as the address of the completed-value cell (target 0x2, completed 0x2, flags 0x1)`: the
  target equals the fence's completed value at the moment of the call, so the word the caller polls is the value it
  waits for.
* `-1` is the removed-device answer: the runtime's `Fence::GetCompletedValue` returns `UINT64_MAX` when the device is
  lost (and the PC runtime treats a removal as signalling monitored fences with it), and the caller sends `-1` into its
  fatal-report path instead of comparing it with the target.
* The wait's use of `monitorx`/`mwaitx` on the returned address only makes sense for a memory cell something writes
  asynchronously (the GPU/driver), not for a kernel handle or an object field the caller would read once.

Hypothesis (b) from the task ("an opaque token whose first qword is -1 until the wait fires") is ruled out: the word is
read before the wait as well, and the caller's comparison is against the submitted fence value, not a validity flag.
Hypothesis (c) (the object belongs to a title channel type, not the fence) is ruled out: the object at
`channel+0x10` is the proxy the runtime built for the title's own `CreateFence`, and the proxy's log line names it a
fence.

## 5. The implementation

`port/shims/d3d12_xs_device.cpp` (the fence proxy) keeps a 64-bit cell per fence proxy and answers console fence
slot 12 with its address. The cell is:

* created with the proxy and seeded from the real fence's `GetCompletedValue` (PC slot 8) — the fence is created with
  `InitialValue` 0 here, but the code uses the real value, not an assumption;
* refreshed on every slot 12 call, so the caller that asks again after a wait sees the current value;
* published when the value moves through a proxy: console queue slot 16 (`Signal`) refreshes the fence proxy it was
  given, and the fence's own `Signal`/`GetCompletedValue` forwarding refreshes it too;
* handed to the Vulkan layer's fence dispatcher (`d3d12_com.cpp`, via the registered
  `palmetto_register_fence_value_watcher` hook) when the target has not been reached yet. The dispatcher already wakes on
  a timeline value, so the watch is one enqueued callback that writes the target into the cell when the GPU reaches it.
  This covers a target signalled later by another thread and a console `Signal` that the runtime holds behind pending
  GPU work. The cell only increases (an Xbox fence value is monotonic; `UINT64_MAX` is its top).

The proxy's cell is shared, so a callback dispatched after the proxy is released still writes a valid cell. On a
removed device the dispatcher drains the callback, so the cell reports the target rather than `UINT64_MAX`; the
runtime's own removal path is what reports a removed device. That is the one known difference from the console memory
the method would hand out, and it is honest in the log (the first slot-12 line prints target, completed value and
flags).

`port/shims/d3d12_xs_device.h` documents the slot, `kConsoleFenceValueAddress = 12`, the watcher hook, and
`port/test/xs_proxy_mock_test.cpp` covers it on the GPU-free mock: slot 12 is answered by the proxy and not
forwarded, the address is fixed for the fence, the cell carries the mock's completed value, a target the value has not
reached asks the registered watcher once (and not for a reached target), and a `Signal` through a console queue slot 16
moves the real value into the same cell.

## 6. The measurement

Same environment as the previous fronts (`PALMETTO_SHADERS=<shader corpus work folder>`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`), same harness (`tooling/boot_report.py`), one boot at a time:

| run | max traced call | outcome |
|---|---:|---|
| baseline, tree before this change | **#12087** | stop `game+0x4fb0b03`, `0xc0000005` read at `0x80004001` |
| slot 12 implemented, diagnostics off | **#6,887,539** | **no exception**; harness verdict `ALIVE 400.2 s`, killed by its 400 s limit |

In the changed boot: `PALMETTO_OK` and `TESTS_BUILT` are green, the proxy mock test passes, and the only failed trace
result in the boot is the command-list `Close` counted below (491,037) — no pipeline-creation failure occurs. The
proxy's own log gives `fence slot 12 (past the public chain) first call` followed by
`fence slot 12 answered as the address of the completed-value cell (target 0x2, completed 0x2, flags 0x1)`.
`XboxID3D12Fence::Slot12` is called **245,524** times in that boot (the frame-end wait of each iteration), all
answered; `no fence value watcher is registered` never appears.

What the title did after the wait is the next frontier, not this one. The boot entered a repeating frame-shaped
sequence (245,519 iterations): present attempt (console queue slot 32, still refused with `E_NOTIMPL`), two `Signal`s,
`GetTimestampFrequency`, command-allocator `Reset`, its console extension slot 9 (still refused), two
`ID3D12GraphicsCommandList::Close` calls, a fence wait and clock calibration, over and over. In that loop every
`Close` returns failed (`already closed`: 625 closes succeeded during startup, then 491,037 failed), and the whole boot
contains no command-list `Reset` and no `ExecuteCommandLists` — so the title renders nothing yet and spins on its frame
path. The likely next question is that path (why the lists the frame closes are already closed, and queue slot 32
`PresentX`, still unanswered), not the fence.

## 7. Limits

* One machine, one boot after the change; the baseline row is the previous front's boot on the same tree.
* The cell is the proxy's copy of the completed value, not GPU-written memory: it is exact at every proxy call and at
  every dispatched fence completion, and monotonic, but a title reading it between those points sees the last
  published value. No observed wait in this boot needed a dispatch (the target was already reached).
* The console method's name and declared signature are inferred from the caller (no console header is available):
  the proxy reads the wait target from RDX and the flags byte from R9, and uses the target only to decide whether a
  watch is needed. The 56-bit rule is the caller's, measured, not a console ABI statement.
* A removed device is reported as the target rather than `UINT64_MAX` (section 5); this was not exercised by the boot.
* The old stop's fault address `0x80004001` is `E_NOTIMPL`; the new boot never returns it from slot 12.
