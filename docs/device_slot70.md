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
| `arg3` (destination) | descriptor slot at a **console descriptor heap** page `+0x220`; first word `0x12` before the call | descriptor slot at `+0x1680`/`0x8000`/`0x7890` of an **ordinary shader-visible heap** (host descriptor slot); all zeros before the call. Three earlier boots measured 32-byte-aligned offsets; the publication boots measured a 16-byte-aligned ring base (the object's array is 16-byte aligned), so the publication's rule is 16-byte alignment and a 16-byte V# write |
| destination-before statistics | - | 12,214 of the first 12,288 calls found the slot zeroed; destination offsets covered `0x0..0x8000` across at most two heap pages |
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

## 5. The matrix publication (`PALMETTO_SLOT70_PUBLISH=1`, new increment)

The measured record has the shape of a view publication whose data the console device turns into a descriptor: 0x20
elements (`+0x18`), a 128-byte BUFFER description (`arg1`), a declared byte size (`+0x08` = 0x1688), the CPU block the
caller just filled (`+0x28`), and the block's format word (`+0x30`). The title's own table slot 0 is empty every boot
(`docs/root_arguments.md` section 7), while these calls are the only measured producer of 32-byte descriptor slots and
their destinations are zeroed before the call - so the destination stays empty only because this layer refuses.

**Encoding decision.** The publication writes a MATRIX V# (raw constant buffer) over the record's block into the
destination slot:

| word | value | why |
|---|---|---|
| `w0` | block VA bits 31:0 | the module's matrix fetch builds `base = ((w1 & 0xFFFF) << 32) | (w0 & ~3)` (`docs/root_arguments.md` section 7) |
| `w1` | block VA bits 47:32, stride 0 in bits 29:16 | stride 0 means the declared size in `w2` is a byte count |
| `w2` | the record's declared size (>= 64) | the module reads 16 dwords (64 B) and gates each load on `declared >= 4*(k+1)`; the record's own size is used, so the view declares what the console declares |
| `w3` | `0x30000FAC` | raw out-of-bounds select + identity destination select: the module never reads `w3` for a matrix fetch, and `0x30000FAC` is this layer's raw-buffer convention for the other raw views (the title's constants V# uses `0x30014FAC`) |

The base is written as the block's **title VA**, not an arena VA. This is deliberate: the shim has no arena VA to write
(the title-VA map lives in the layer, and the shim links no layer symbols), while the layer's console-heap mirror and
in-place descriptor scan already rebase every 48-bit V# base that names a registered title buffer at submission - the
same path measured for the title's own constant-buffer views ("2 V# base(s) rebased"), which produces exactly the
arena-relative value the shader adds `heap_base` to. The replay's page pull then copies the V#'s declared range from the
title pages into the arena before the GPU reads it (`docs/page_content.md`: the draw's table V#s are pulled by their
declared byte range). A block inside a buffer the title placed through console slot 133 has such a range; the proxy
records those placements and refuses to publish a block outside them, so the publication can never produce a V# that
nothing rebases or pulls.

Refusals (all logged once, counted, and then the call keeps the measured refusal): the block is not readable from this
process; the block's first 64 bytes are all zero (the publication carries no data yet - nothing is fabricated); the block
is outside every slot-133-placed range; the record's element count is not 0x20 or its declared size is below 64.

**Measured boot (`boot-slot70-publish3`, 150 s, ALIVE 150.2 s, no exception, 665,437 D3D12 calls, 32,304 draws).**
Environment: standard + `PALMETTO_GRAPHICS_CONSOLE_ROOT=1` (rootargs' console feed, so the draw's `s8` names the table),
`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` (framecap's vertex-stream V# in the `s0`/`s1` driver block - without it there are no
vertices to transform), `PALMETTO_VERTEX_PATH=1`, `PALMETTO_READBACK_AT_DRAW=20001`, `PALMETTO_FRAME_DUMP=1`,
`PALMETTO_FRAME_DUMP_PS_CONTROL=1` (+ cull-none), `PALMETTO_FRAME_DUMP_CONTROL_PS=<build>/fs_solid.spv`,
`PALMETTO_SLOT70_PUBLISH=1`, `PALMETTO_SLOT70_PROBE=1`. framecap's matrix-experiment switch was **off**, so the publish is the only writer of the matrix slot. (`PALMETTO_VERTEX_STREAM_DESCRIPTOR` was framecap's uncommitted diagnostic during that working window - off by default and reverted after their measurement - so the acceptance numbers above are reproducible only with that switch present; the slot-70 publication itself is independent of it, as the boot with the switch off measured the same 12,214 V# words.)

| measured | value |
|---|---|
| matrix V# publications | **12,214 by the 12,288-call milestone** (~1/frame), 0 refused (no empty block, none outside a slot-133 range, none unreadable) |
| V# written | `w0 = 0x...73de20` (block title VA bits 31:0), `w1 = 0x...01a2` (bits 47:32, stride 0), `w2 = 0x1688`, `w3 = 0x30000FAC` |
| destination | one host-heap page, offsets `0x0..0x7890` (16-byte ring), 1 call left the destination zeroed (the wrapper site, whose record count is not 0x20) |
| armed draw's table slot `s8+0x00` | `0086de20 00000000 00001688 30000fac` -> **"V# buffer address 0x86de20 stride 0 records 5768 format 0"** - a matrix V# with a real arena address (the layer rebased the published title VA `0x...73de20` to arena `0x86de20`) |
| matrix data at that V# | `vertex path: constants at 0x86de20 + 0x1688, read 480: mean 19, non-black 74/480, peak 255` - the pages are **not empty**: the publication carries data and the replay's page pull brought it into the arena |
| table slot `s8+0x40` | `0086dc40 ... 000001e0 30014fac` (the title's 480-byte constants V#, untouched) |
| driver block `s0/s1` | `0086df10 00180000 000000aa 00016fac` -> vertex V# stride 24, records 170, format 22 (framecap's switch, needed for any vertex fetch) |
| **PS-control readback** | mean **23 115 172**, non-black **11,776/16,384**, peak 240 - **the control left black for the first time with real title data** |
| write control | mean 51 102 204, non-black 16,384/16,384 (probe path intact) |
| page content | 65,536 pull(s), 181.3 MiB copied - the published V#'s declared ranges are part of the pull set |
| plain display readback | not produced in this boot (the 150 s window ends before that one-shot line; framecap's equivalent boot measured the display still black), so the display axis stays open and is the sampled-texture/colour path |

How the descriptor reaches the table: the title copies the staging ring's descriptor into its console descriptor heap with
`CopyDescriptors` (not recorded by the layer's trace), and the heap's mirror sync then rebases the 48-bit V# base from the
title VA to the arena VA at submission - the table slot at the draw shows exactly that rebased value. The publication was
also measured the boot before with the vertex-stream switch off: **12,214 publications, same V# words**, but the PS
control stayed black (no vertices), i.e. the publication effect and the vertex effect are independent and both are needed.

A diagnostic detail worth keeping: the first acceptance attempt published **0 times** in a boot whose ring base was
16-byte but not 32-byte aligned, because the publication then required 32-byte alignment. The measurement fixed the rule
(16-byte alignment, 16-byte V# write), not the other way round.


## 6. The shape census: slot 70 is a generic publication, and it is not the sampled-T# producer

`PALMETTO_SLOT70_CENSUS=1` keeps a bounded census of the call shapes (distinct shape <= 64, one line per new shape, a
summary every 4096 calls): the resource description's dimension/width/height/format/mips/array, the record's element
count/declared size/format word, the kind of its first word, how many of the block's first 16 dwords are non-zero, and the
destination's alignment. Measured in two 150 s boots (`boot-slot70-census` with the PS control, `boot-slot70-census2`
without it), 12,288 calls each:

| measured | value |
|---|---|
| distinct shapes | **34-35** |
| per-frame dominant site (`game+0x4D9F369`) | **12,214 calls, exactly one shape**: desc dim 1 (BUFFER) width 0x80, record count 0x20, declared 0x1688, format word 0x16, first word 0x100000027 - the matrix publication |
| startup wrapper site (`game+0x4ECC821`) | **74 calls, 34-35 shapes**: buffers of width 0x80/0xa0/0xc0/0x140/0x21c/0x300/0x800/0x1800/0x4b000/0xae00000, and **texture descriptions**: dim 2 (TEXTURE1D) 0x80x1 fmt 0x41 and 0x100x1 fmt 0x1d; dim 3 (TEXTURE2D) 0x1x1 fmt 0x1c/0x1e/0x29 with arrays 1/2/6/12, 0x100x0x80 fmt 0x3d, 0x500x0x2d0 fmt 0x43, 0x80x0x48 fmt 0x29 mips 7; dim 4 (TEXTURE3D) 0x1x1 fmt 0x1c |
| non-buffer descriptions | **60 calls** (all at the wrapper site) |
| record format word (`+0x30`) | **0x0 for all 74 wrapper calls**, 0x16 only for the per-frame matrix shape |
| record declared size (`+0x08`) | **0x1688 for every shape** (buffer and texture), i.e. not the texture's byte size |
| destinations | wrapper site: the **console descriptor heap** page (offsets from its start, e.g. 0x220); dominant site: one host staging-heap page in a ring; all 32-byte aligned in these two boots (one earlier boot had a 16-byte-aligned ring base) |

**Decision.** The publication stays buffer-only. A texture-shaped record is refused by name, once logged, and counted (60
per boot): `a texture-shaped description (dimension=3 width=0x100 height=0x80 format=0x3d ...) is not pinned to a T#
encoding; refusing instead of writing a T#`. The reasons are the measured ones: the record carries no texture type,
format or dimensions (its format word is 0 for every texture call and its declared size is the same constant as the buffer
calls), so what the block contains - the texture's texels or a pre-encoded descriptor - is not pinned; the console's T#
encoding is not in any measured field; and writing a T# from that would be a guess. The offline call sites agree where
they can: both instantiations build a `D3D12_RESOURCE_DESC` with `Dimension = BUFFER` (the constant `1` at `[rsp+0x60]`)
and the texture shapes arrive through the wrapper, which only forwards its caller's three records - the type comes from
the caller, not from anything the device sees.

**Why this is the negative for the sampled-T# question.** The armed draw's sampled images are not among the census
shapes: sampled T# key 1 resolves to a **3840x2160 format-37** image over a title VA (a placed texture created through
console slot 133), and the census' widest texture is 1280x720; sampled key 5 is **unresolved** by the layer's image set.
So slot 70 did not publish the sampled descriptors measured in the window, and the sampled content gap is not a slot-70
descriptor gap.

**Colour-path numbers of this increment** (same binary, matrix publication on, 150 s each):

| boot | PS-control readback | display readback | colour path line |
|---|---|---|---|
| `boot-slot70-census` (PS control on) | mean 23 115 172, non-black **11,776/16,384**, peak 240 | not produced (the control takes the one-shot readback) | `ps present, 6 image sample(s) -> the PS reads sampled images; non-zero push constants s0/s1 (table pair), s8 (table VA) - 3 of 32` |
| `boot-slot70-census2` (plain + sample) | - | mean **0 0 0, non-black 0/16384**, peak 0; write control 16,384/16,384 | `ps present, 0 image sample(s) -> constant-output path` (this boot's armed draw); framecap's same-binary plain boot has the 6-sample PS: sampled key 1 reads back all-black, key 5 unresolved |

Acceptance on the colour axis is therefore **not met**: the sampled/display axis is still black, and this increment's
measurement says the reason is not a missing slot-70 T#. The next producer is the **page producer of the placed 4K
texture**: its title pages are never written in the window (framecap's page-writer watch), which is the same negative
`docs/streamed_consumer.md` and `docs/texture_source.md` reached from the other side, and sampled key 5 needs a layer
image in set 0 before it can be read at all.

## 7. The texture-shaped publications: the title does not write the T#

The 60 startup wrapper-site calls carry texture resource descriptions (dim 2/3/4; section 6). `PALMETTO_SLOT70_PROBE=1`
now dumps, for the first 8 distinct texture destinations, the destination descriptor's 32 bytes before the call, and
re-reads those destinations later in the boot (three times each, every 1024 calls), so a write that happens *after* the
call is caught. Measured in `boot-slot70-texprobe` (150 s, ALIVE, 659,910 D3D12 calls):

| measured | value |
|---|---|
| destination of a texture publication | a slot in the **console descriptor heap** (heap base + offset `0x340`/`0x380`/`0x3a0`/`0x3c0`/`0x3e0`/`0x400`/`0x420`/`0x440`, i.e. the heap's slots 26..34), 32-byte aligned |
| descriptor **before** the call | dword0 only, a 1-based slot index (`0x1b` at offset `0x340`, `0x1d` at `0x380`, `0x1e` at `0x3a0`, ... = slot + 1); dwords 1-7 zero |
| descriptor **after** the call (re-dump at call 1024, all 8) | **unchanged**: the same single index dword, nothing else ever written |
| record | declared size `0x1688` and format word `0x0` for every texture shape; block first 64 bytes non-zero for most shapes (16/16 or 1/16 dwords), counted not printed (no game data) |

**Result: the hypothesis is refuted.** The title does not write the console T# into the destination - it only stamps the
slot's 1-based index into dword 0 and leaves the rest zero, before and after the call. So the console T# encoding is *not*
observable in the title's memory: slot 70 at this site is the same kind of device-side publication as the matrix one (the
device is expected to write the descriptor), and what it would write is an encoding this layer has never seen.

**What the upscale pass' inputs actually are, as far as the measurement pins them.** The armed compose draw samples 6
images; the one that resolves is the 3840x2160 format-37 image over a title VA created through console slot 133
(`docs/root_arguments.md`'s sibling-pass target), and the sampled table the draw reads is the console heap region at heap
offset ~0x135efa0 - **not** the heap slots 26..34 that these 60 publications stamp. The publication descriptions also do
not match the texture resources the title created in the same boot (e.g. publication `dim3 0x100x0x80 fmt 0x3d`,
`dim3 1x1 fmt 0x1c`, `dim2 0x80x1 fmt 0x41`, `dim3 0x500x0x2d0 fmt 0x43`, `dim3 0x80x0x48 fmt 0x29 mips 7` against created
textures `1x1 fmt 28`, `3840x2160 fmt 65`, `2048x2048 fmt 87`, `2560x1440 fmt 10`, `1280x720 fmt 42/26`,
`640x360 fmt 62`, `256x256 fmt 10/26/34`, `64x2 fmt 26`, `127x128 fmt 35`): only formats 0x1c/0x41 and the 1280x720
dimension appear on both sides, not a single full desc. So the pairing "publication -> the resource it views" is not
proven, and without it a T# cannot be built honestly: the console T# needs a base VA the call does not carry, and its
encoding is not written by the title.

This boot's compose axis stayed where the colour-path increment left it: the armed draw had 0 image samples
(`constant-output path`), display readback mean 0 0 0 / non-black 0/16384, write control 51 102 204 (16,384/16,384); the
matrix publication was unchanged (12,214 V#s, 0 refused) and the 60 texture records were refused by name (0 T# written).

**Next falsifier.** The publication can be modelled only if the resource each texture publication views is identified:
the next measurement is to pair every texture publication with the slot-127/133 resource created immediately before it
(desc match on dimension/width/height/format/mips/array) and, where a pair is proven, create the PC shader-resource view
on that resource with the layer's own T# encoder at the same destination - the device-side console encoding then does not
have to be guessed. If no publication can be paired, the sampled T#s come from the title's own T# writes elsewhere and the
frontier is again the page producer of the placed 4K texture.

## 8. Pairing the texture publications with the resources the title creates

The texture publication carries a resource description but no resource handle, so the device behind the call can only
resolve the resource from the creations around it. `PALMETTO_SLOT70_PAIR=1` keeps a bounded ring (16) of console resource
creations - console slot 127 (create committed with a layout) and slot 133 (placed resources) - with each creation's Xbox
description, the real resource the layer made, the title VA (133) and the slot-70 call count at which it was seen. On a
texture-shaped publication the ring is searched for creations **before** the call within `kSlot70PairWindow = 24` slot-70
calls whose description matches on dimension/width/height/format/mips/array; the newest match wins. A proven pair calls
the **real device's `CreateShaderResourceView`** with a view description built from the publication (view dimension from
the desc, format, mip and array counts) and the title's destination slot as the CPU handle - so the layer writes its own
T#, no console encoding is guessed. No pair keeps the measured refusal.

Measured in `boot-slot70-pair` (150 s, ALIVE, 654,034 D3D12 calls; env: console feed + vertex-stream descriptor + matrix
publication + pairing + census + sample dump, readback armed at the compose draw 28000, matrix-experiment switch off):

| measured | value |
|---|---|
| texture-shaped publications | 60 (the measured census) |
| **paired** | **6** - all of one shape (`dim3 0x80 x 0x48 fmt 0x29 mips 0x7 array 0x1`), each at calls 71-76 with a slot-133 creation at **distance 1** (the immediately preceding creation, 1..6 candidates in the window, all full matches) |
| paired destinations | console heap page + offsets `0x1120`, `0x1140`, `0x1160`, `0x1180`, `0x11a0`, `0x11c0`; the layer's T# written there (`6 layer T# published`) |
| **not paired** | **54** - no creation within 24 slot-70 calls matched (e.g. calls 4-12: 2 candidates, 0 full matches; the rest: no candidate at all) - the measured refusal kept |
| compose draw 28000 | `ps present, 6 image sample(s)`; **render-target readback mean 0 0 0, non-black 0/16384, peak 0**; write control 51 102 204 (16,384/16,384) |

**Result: the pairing exists but is not the compose pass' input.** Six publications prove the rule end to end (the
layer's own T# at the title's destination), but their shape (`128x72`, format 0x29, 7 mips) and their destination slots
(heap offsets 0x1120..0x11c0) are not the compose pass' sampled table slots, and the compose draw's display readback is
unchanged at 0/16384 while its colour path still reports 6 image samples. The other 54 publications - including the
`0x100x0x80`, `1x1` and `0x500x0x2d0` shapes at the start of the boot - have no matching creation within the window, so
they cannot be paired by this rule.

**What this pins and what it leaves.** Pinned: a texture publication *can* be turned into the layer's T# when the
resource it views is one the title created in the same call neighbourhood, and the rule's pure part (description match +
window) is exercised by the GPU-free test. Left: 54 publications need their resource identified some other way (the
create call may be further away, or the publication may not be a view of a slot-127/133 resource at all), and the compose
pass' missing sampled inputs are not among the six paired slots - framecap's trace names compose table key 1 as the
sibling 4K image (resolved) and its other slots as float data or unresolved, so the next step is framecap's complete
T#-slot list for draws 28000/28001 compared against these destinations and against the creations the ring recorded.

## 9. The creation lifetime registry, and the identity of the compose pass' T# address

The bounded 16-entry pairing ring is now a **lifetime record** of every console resource creation the proxy handles:
slot 76 (the console's committed-resource creation), slot 127 (create-with-layout) and slot 133 (placed resources), up to
1024 entries (dropped creations are counted), each with the Xbox description, the real resource, its **GPU VA**
(`GetGPUVirtualAddress`: a buffer VA or, for a texture, the layer's image key), the title VA (133) and the **device-call
index**. Pairing searches every recorded creation before the publication (not a fixed window) and takes the newest
description match. `PALMETTO_SLOT70_IDENTITY_VA=<hex>` answers the identity question for one address: it is looked up as
a creation's title VA, as a creation's GPU VA (image key), and as an offset inside a created range, and reported with the
creation's description, resource and call index.

Measured in `boot-slot70-identity` (150 s, ALIVE 150.2 s, 653,204 D3D12 calls; env: console feed + vertex-stream
descriptor + matrix publication + pairing + identity 0x100001600 + sample dump, readback armed at 20001):

| measured | value |
|---|---|
| creation registry | **37 creation(s)** recorded, 0 dropped: slot 76/127/133 = **2/2/33**; only **3 report a GPU VA** (the buffers, arena VAs) - **every texture creation reports GPU VA 0** |
| 3840x2160 2D creations | 4 in the early set: slot 127/76 twice at format **0x1c**, slot 133 at format **0x41** |
| **identity 0x100001600** | **"NOT the title VA, GPU VA (image key) or range of any of the 5 creation(s) recorded (at that moment); 4 of them are 3840x2160 2D textures"** - and the full lifetime run (37 creations) reaches the same negative |
| pairing | **10 of 60** texture publications paired (up from 6 with the 24-call window), 10 layer T#s published; 50 unpaired |
| compose/armed draw | `ps present, 6 image sample(s)`; render-target readback **mean 0 0 0, non-black 0/16384** (the 4K sibling image, which is this boot's draw-20001 target); write control 16,384/16,384 |

**The negative, precisely.** No console creation - slot 76, 127 or 133 - was ever made at title VA `0x100001600`, with
that GPU VA, or with a range containing it. Two things make the answer sharper:
* the value is in the layer's documented **image-key range** (`0x100000000` + 256-byte steps; `0x100001600` is key 22)
  and it is **identical in five boots** while `heap_base` moves - so it is deterministic, not a title heap VA;
* this tree's `GetGPUVirtualAddress` returns **0 for every texture creation**, so the creation registry cannot resolve an
  image key at all: the layer assigns image keys but does not expose them here. The T# carries a gfx10 image format (56)
  that does not equal any creation's DXGI format field (0x1c/0x41), so no description match exists either.

So the compose pass' T# address is **not the title VA of any console creation**: either it is an image key the layer
stopped exposing through `GetGPUVirtualAddress` (a layer gap to fix before any bind-by-address can work), or it is the
title's own fixed GPU VA in the same 4 GiB space. In both cases the route to content is the one framecap proposed: build
a layer image from the placed 4K texture's title pages (the sibling target) and bind that image for the compose pass,
rather than matching a console creation by address. The paired publications (10) are still not the compose pass' inputs;
the compose readback is unchanged at 0/16384.

## 10. What is modelled now, and what is not

**Modelled:** the call's shape, arity and ABI (three words; third published with `clwb`); both call sites; the argument
records and destination kinds measured; the measured refusal as the default; an opt-in probe that dumps the arguments,
records, destination bytes and per-heap destination statistics; the falsified CBV conversion behind its own switch; the
matrix publication over the record's block behind `PALMETTO_SLOT70_PUBLISH=1`, with the placement/readability/emptiness
refusals; a bounded shape census (`PALMETTO_SLOT70_CENSUS=1`) that measured the 34-35 shapes above; the named refusal of texture-shaped records (no pinned T# encoding); the texture-destination probe that measured the title's non-write and the later re-dumps; the pairing rule (`PALMETTO_SLOT70_PAIR=1`) that turns a texture publication whose resource was created in the same window into the layer's own T# at the destination; non-write and the later re-dumps; and a shape test that refuses anything that is not the measured call instead of guessing.

**Not modelled / still open:** the record's other fields are measured but not interpreted (the first word differs between
the two sites - `0x100000027` at the wrapper site, the destination address at the dominant site - and its writer is not
explained; the resource description in `arg1` is not used by the publication, and `+0x10` is zero in both sites). The
publication writes the matrix view; whether the title then copies that descriptor into the table slot its draw names is
what the measured boot below decides.

## 11. Gates, files and the next falsifier

Gates on the final tree: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`;
`xs_proxy_mock_test` -> `PASS`, whose `check_slot70_console_view` covers the falsified conversion's hand-off, the matrix
publication (a block placed through console slot 133 publishes the V# words above into the destination slot), and the
refusals (empty block, block outside the placed ranges, element count other than 0x20); the `--env-slots-invalid` run
covers the default refusal (`E_NOTIMPL`, no PC method reached) with no slot-70 switch set. Boots under the single boot
lock, one at a time, `tooling/boot_report.py`, 100-150 s.

Files: `port/shims/d3d12_xs_device.cpp` (the slot-70 handler, its probe, the falsified conversion and the matrix
publication; the slot-133 placement registry), `port/test/xs_proxy_mock_test.cpp` (the mock's `CreateConstantBufferView`
record and `check_slot70_console_view`), this page. Evidence boots under `D:\leonida-work\`: `boot-slot70-probe` (probe,
140 s, ALIVE), `boot-slot70-cbv` (conversion, stopped at 29.5 s), `boot-slot70-final` (default path, 100 s, ALIVE), and
`boot-slot70-publish3` (publication + acceptance, 150 s, ALIVE), `boot-slot70-publish2` (publication with the vertex-stream switch off, 150 s, ALIVE) and `boot-slot70-publish` (first publication boot, 150 s, ALIVE). The shape census and colour-path measurements are `boot-slot70-census` (PS control, 150 s, ALIVE) and
`boot-slot70-census2` (plain + sampled readback, 150 s, ALIVE). The texture-destination probe is `boot-slot70-texprobe` (150 s, ALIVE): 60 texture-shaped publications,
their destinations' before/after bytes and the re-dumps. The pairing boot is `boot-slot70-pair` (150 s, ALIVE): 6 of 60 texture publications paired and published, 54 unpaired; compose draw 28000 readback unchanged at 0/16384. The lifetime-registry and identity boot is `boot-slot70-identity` (150 s, ALIVE): 37 creations
recorded, identity 0x100001600 not a creation, 10 of 60 publications paired, compose readback still
0/16384.

**Next falsifier.** The publication's acceptance is the PS-control readback changing from black: with the matrix A view
published, the armed draw's `s8+0` fetch slot must decode as a matrix V# and the solid-pixel control must produce colour.
If the fetch slot is a real matrix V# and the PS control stays black, the fetch-site reading is wrong or the 16 dwords
still carry no usable transform; if the slot stays zero, the title does not copy the staging descriptor into its table
and the next frontier is where it does. The measurement that would refute the whole mapping is a destination the title
itself writes before the call; at 12,214 of 12,288 dominant-site calls it did not.
