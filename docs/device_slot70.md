# Console device slot 70: the per-frame descriptor publication

Slot 70 is the largest console device entry point the proxy still refuses: **13,568 calls in a 130.2 s boot**, once per
frame, three arguments, refused by name with `E_NOTIMPL`. This page records the call's shape and ABI from evidence, what
the arguments hold in the boot, the PC effect that was tested, and what is still not modelled. The measurement also
falsifies the first conversion that was tried, which is recorded here rather than hidden.

No game data, package addresses, shader bytes or extracted tables appear here: ABI words, descriptor bytes, offsets and
counts only.

## 1. What was already measured

`docs/streamed_consumer.md` section 5 and `docs/boot_audit.md` section 5 measured, offline and in the boot:

* the wrapper `game+0x4ECC800` (function `0x4ECC800..0x4ECC830` per `.pdata`) forwards three arguments to
  `[device+0x230]` and then tail-calls `game+0x4ECC830`, a cache-line writeback of its third argument;
* the dominant site `game+0x4D9F363` (return address `game+0x4D9F369`, inside function `0x4D9F200..0x4D9F3B2`) calls
  the same slot directly, once per frame, 13,568 times;
* the second argument begins with a `D3D12_CONSTANT_BUFFER_VIEW_DESC`-shaped record and the third is a console
  descriptor destination;
* forwarding the slot by number to the PC device's slot 70 (`ID3D12Device8::CreatePlacedResource1`) was semantically
  wrong and crashed; the current tree refuses it with `E_NOTIMPL`, and the title does not test that `HRESULT`.

The console device vtable is the PC one only up to slot 43; from 44 on it is the console's own extension table
(`docs/device_slot55.md`). Slot 70 is therefore a **console extension**, not a PC method number.

## 2. Call shape and ABI (offline, RVAs only; confirmed in the boot)

**The wrapper** `0x4ECC800`, disassembled from the retail image:

```
0x4ECC800: push rsi
0x4ECC801: sub  rsp, 0x20
0x4ECC805: mov  rsi, r8            ; save the third argument
0x4ECC808: mov  r8, rdx            ; PC argument 2 = console argument 2
0x4ECC80B: mov  rdx, rcx           ; PC argument 1 = console argument 1
0x4ECC80E: mov  rcx, [device]      ; PC `this`
0x4ECC815: mov  r9, rsi            ; PC argument 3 = console argument 3
0x4ECC818: mov  rax, [rcx]
0x4ECC81B: call qword ptr [rax+0x230]   ; slot 70, returns to 0x4ECC821
0x4ECC821: mov  rcx, rsi           ; the writeback's pointer is the third argument
0x4ECC824: xor  edx, edx
0x4ECC826: add  rsp, 0x20
0x4ECC82A: pop  rsi
0x4ECC82B: jmp  0x4ECC830          ; cache-line writeback
0x4ECC830: sub  rsp, 0x28
0x4ECC834: mov  r9, rcx
0x4ECC837: test edx, edx
0x4ECC839: jne  0x4ECC85D          ; non-zero flag: flush a VA list instead
0x4ECC83B: mov  eax, [global]      ; the descriptor bytes to flush
0x4ECC841: lea  rax, [r9+rax-1]
0x4ECC846: and  r9, -0x40          ; round down to a 64-byte cache line
0x4ECC84A: clwb byte ptr [r9]
0x4ECC84F: add  r9, 0x40
0x4ECC853: cmp  r9, rax
0x4ECC856: jbe  0x4ECC84A
0x4ECC858: ret
```

So the slot takes **three arguments after the device** and the title then **publishes (flushes) the third one**.

**The dominant site** `0x4D9F363`, in the once-per-frame function `0x4D9F200..0x4D9F3B2` (a method of an engine object;
`r15 = this - 0x12780` is its pool base):

```
0x4D9F22F: call 0x4D9F3E0                  ; pool block for (size = 4th argument, 0x10 alignment)
0x4D9F234: mov  rbx, rax                   ; the block
0x4D9F237: cmp  r14, 0x80
0x4D9F23E: jne  0x4D9F37D                  ; not 128 bytes: the memmove path still publishes
0x4D9F244: vmovups ymm0, [r12]             ; 0x80 bytes of the caller's data into the block
...
0x4D9F28E: mov  cl, 0x27                   ; the block's element format code
0x4D9F290: movabs rax, 0x1000100000001
0x4D9F29A: mov  dword [rsp+0x60], 1        ; arg1: D3D12_RESOURCE_DESC, Dimension = BUFFER
0x4D9F2A2: mov  qword [rsp+0x68], 0        ;        Alignment = 0
0x4D9F2AB: mov  qword [rsp+0x70], r14      ;        Width = 0x80
0x4D9F2B0: mov  qword [rsp+0x78], rax      ;        Height = 1, DepthOrArraySize = 1, MipLevels = 1
0x4D9F2B5: vmovups [rsp+0x80], xmm0        ;        Format/SampleDesc/Layout
0x4D9F2BE: mov  dword [rsp+0x90], 0        ;        Flags = 0
0x4D9F2C9: call 0x4D9F3C0                  ; element size for format 0x27
0x4D9F2D0: movabs r8, 0x100000027
0x4D9F2E2: mov  qword [rsp+0x20], r8       ; arg2: the record's first word (offline value)
0x4D9F2E7: mov  dword [rsp+0x28], 0x1688   ;        second word  0x1688
0x4D9F2F8: div  rcx                        ;        elements = Width / element size
0x4D9F305: mov  dword [rsp+0x38], eax      ;        element count
0x4D9F310: mov  qword [rsp+0x3c], 0x100000000
0x4D9F320: mov  qword [rsp+0x48], rbx      ;        the CPU block it just filled
0x4D9F32D: call 0x4D9DEB0                  ; format 0x27's second descriptor word
0x4D9F339: mov  dword [rsp+0x50], eax
0x4D9F34E: lea  rdx, [rsp+0x60]            ; arg1 = &resource description
0x4D9F353: lea  r8,  [rsp+0x20]            ; arg2 = &record
0x4D9F358: mov  r9,  [rdi+rsi+0x1fa38]     ; arg3 = a 32-byte-aligned slot from the object's descriptor array
0x4D9F363: call qword ptr [rax+0x230]      ; slot 70, returns to 0x4D9F369
```

The object field `+0x1FA20 + 0x20*index` is a per-frame 32-byte entry; the destination is its `+0x18` word. The image
has two instantiations of the template, the dominant one over a 128-byte block (`0x4D9F200`, the one the boot calls
13,568 times) and a second at `0x4EECB40` with the same constants and a different upload pool, which the measured 130 s
boot does not reach.

**ABI decision:** the console call is a device method with **three argument words** (`arg1 = D3D12_RESOURCE_DESC*`,
`arg2 = a 0x40-byte record`, `arg3 = destination descriptor`), followed by a cache-line writeback of `arg3`. There is no
hidden struct return and no by-value structure. The fifth word the thunk receives at the wrapper site is the wrapper's
own saved `rsi` (`push rsi`), measured as `0x1475df0e8`, not an argument - the boot confirms the three-argument split.
The PC device has no method with this arity for a descriptor publication, which is why forwarding by number crashed and
why the fix has to be a conversion, not a forward.

## 3. What the arguments hold in the boot

`PALMETTO_SLOT70_PROBE=1` dumps, per call site and then every 2048th call, the three argument words plus the fifth word,
both records, and the destination before/after. Measured in `boot-slot70-probe` (retail image, Vulkan backend, 140 s,
ALIVE, no exception, 594,356 D3D12 calls):

| measured | wrapper site `game+0x4ECC821` | dominant site `game+0x4D9F369` |
|---|---|---|
| calls in the window | 74 | 14,262 |
| `arg1` (0x80-byte dump) | `D3D12_RESOURCE_DESC` of a **BUFFER**: Dimension 1, Alignment 0, Width **0xAE00000**, Height 1, DepthOrArraySize 1, MipLevels 1, Format 0, Flags 0 | same shape over a **128-byte block**: Width 0x80, Height 1, Depth 1, MipLevels 1, Format 0, Flags 0 |
| `arg2` measured qwords | +0x00 `0x100000027`, +0x08 `0x1688`, +0x10 0, +0x18 `0x2B80000`, +0x20 `0xFFFF00000001`, +0x28 a title VA, +0x30 0 | +0x00 **the destination address** (equal to `arg3`), +0x08 `0x1688`, +0x10 0, +0x18 **0x20** (elements), +0x20 1, +0x28 **the CPU block the caller filled** (title VA), +0x30 **0x16** (format word) |
| `arg3` (destination) | 32-byte slot at a **console descriptor heap** page `+0x220`; first word `0x12` before the call | 32-byte slot at `+0x1680`/`0x8000` of an **ordinary shader-visible heap** (host descriptor slot); all zeros before the call |
| destination-before statistics | - | 12,214 of the first 12,288 calls found the slot zeroed; destination offsets covered `0x0..0x8000` (a 1024-slot, 32-byte ring) across at most two heap pages |
| published (conversion off) | 0 | 0 |

The dominant caller's instruction bytes were dumped from the running image and match the on-disk function (including the
`mov qword [rsp+0x20], r8` store and the `call [rax+0x230]`); the record's first word in the boot at that site is the
destination address, not the offline store's `0x100000027`. That difference is an open measurement (the field's writer is
not explained by the code path), and it is exactly why the first conversion below had to be tested rather than assumed.

## 4. The PC effect that was tested and what it measured

**The first conversion, falsified.** `arg2`'s first two words look like `D3D12_CONSTANT_BUFFER_VIEW_DESC`'s
`BufferLocation` and `SizeInBytes`, so the proxy was given an opt-in conversion: call the PC device's own
`CreateConstantBufferView((const D3D12_CONSTANT_BUFFER_VIEW_DESC*)arg2, D3D12_CPU_DESCRIPTOR_HANDLE{arg3})` when the call
matches the measured shape (a readable BUFFER description, a readable view record with both fields set, and a
32-byte-aligned writable destination). The A/B boot (`PALMETTO_SLOT70_CBV=1`, same environment, 140 s limit) measured:

* the conversion works mechanically: each shaped call it reached (the probe logged the wrapper site and the dominant
  site) reached the PC method, and the 32-byte destination slot changed from zeros (dominant site) / a stale `0x12`
  first word (wrapper site) to the layer's 16-byte GFX10 V# (`... 0x1688 0x30014fac ...`) whose **base is the record's
  first word**;
* at the dominant site that base is the destination descriptor address itself, not a buffer VA, so the descriptor names
  the descriptor heap. The replay then refuses the frame: `ExecuteCommandLists failed: unsupported; nothing was
  submitted`, followed by `present failed (VkResult -3)` / `(VkResult -4)`;
* the title does not survive it: the boot stopped at **29.5 s** with an access violation reading `0xcdc61075b` in
  `game+0x4DA41C0`, after 51,724 recorded D3D12 calls (the ALIVE probe boot had 594,356 at 140 s).

So the CBV reading is **falsified** by measurement: writing a constant-buffer view from that record produces a descriptor
that cannot be honoured, and the frame path breaks. The conversion is left behind the off-by-default switch as a
measurement instrument, not as a mode this tree enables.

**What is modelled instead, and measured:** the default answer is the measured refusal (`E_NOTIMPL`, which the title does
not test), answered inline by the slot so the default path is byte-identical to the tree that measured the boot. The
destination descriptors therefore stay as they were (zeroed, measured above), and the boot is unchanged: the final
default-path validation boot was **ALIVE 100.1 s with no exception**. The probe also measured the frame readback in the
probe boot - 128x128 window, mean 0 0 0, non-black 0/16384 - i.e. the same black frame the tree already has, so nothing
in this increment claims to change the picture.

A note on boot stability, measured across this increment and reported by the parallel root-signature work: an
intermittent execute fault at `game+0x75DF0E8` (a title address in an unmapped image gap) appears right after the first
device-slot-70 call (~4.8 s). It was observed on default-path 150 s boots (3 of 3 in one series), not on 25 s boots (0 of
3), and re-running the same binary got past it; the default-path validation boot recorded here (`boot-slot70-final`,
100.1 s) and a second default boot (150.1 s) were ALIVE. The fault is therefore intermittent and not attributable to a
slot-70 conversion or probe: it is recorded here so a later increment can tell it apart from a slot-70 effect, and the
140 s ALIVE probe boot above is the data source for this page.

## 5. What is modelled now, and what is not

**Modelled:** the call's shape, arity and ABI (three words; third published with `clwb`); both call sites; the argument
records and destination kinds measured; the measured refusal as the default; an opt-in probe that dumps the arguments,
records, destination bytes and per-heap destination statistics; the falsified CBV conversion behind its own switch; and a
shape test that refuses anything that is not the measured call instead of guessing.

**Not modelled:** what the device is supposed to *write* into the destination. The record carries a CPU block
(`+0x28`), a byte size (`0x1688`), an element count (`+0x18`) and a format word (`+0x30`), and the destination slots are
zeroed before the call - so the console call publishes a device-encoded descriptor from that record, but the encoding is
not decoded. The record's first word differs between the two sites (`0x100000027` vs the destination address) and its
writer is not explained; the resource description in `arg1` is passed to the console and unused by the PC conversion.

## 6. Gates, files and the next falsifier

Gates on the final tree: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`;
`xs_proxy_mock_test` -> `PASS`, with `check_slot70_console_view` covering the conversion's hand-off (the measured view
record and the title's 32-byte destination reach the PC `CreateConstantBufferView`, the mock's descriptor write lands in
that slot, and the console slot is never forwarded by number) and the shape refusals; the `--env-slots-invalid` run
covers the default refusal (`E_NOTIMPL`, no PC method reached) with no slot-70 switch set. Boots under the single boot
lock, one at a time, `tooling/boot_report.py`, 100-140 s.

Files: `port/shims/d3d12_xs_device.cpp` (the slot-70 handler, its probe and the opt-in conversion),
`port/test/xs_proxy_mock_test.cpp` (the mock's `CreateConstantBufferView` record and `check_slot70_console_view`), this
page. Evidence boots under `D:\leonida-work\`: `boot-slot70-probe` (probe, 140 s, ALIVE), `boot-slot70-cbv` (conversion,
stopped at 29.5 s), `boot-slot70-final` (default path, 100 s, ALIVE).

**Next falsifier.** If slot 70 is a descriptor publication whose record is device-encoded, then on the console the
destination slot must be the encoding of the record's block, and a later draw that binds the table containing that slot
must read it. The measurement that would refute the whole mapping is a destination that the title itself writes before
the call (then there would be no device effect to model); at 12,214 of 12,288 dominant-site calls it did not. Failing
either, the next step is to decode the record's encoding by comparing the record bytes with a destination slot that a
console would produce - the tracked work is the record's `+0x18`/`+0x28`/`+0x30` fields against the 128-byte block and
the element format table at `game+0x4D9F3C0`, and then to publish that encoding into the console heap's GPU mirror,
which already rebases the V# bases the title writes.
