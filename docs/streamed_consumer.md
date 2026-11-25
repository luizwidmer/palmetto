# The streamed-block consumer: a title CPU deserializer, and the GPU-side step that never happens

**Status: measured, and the last open lead is closed.** Three 130 s harness boots name the consumer of the DirectStorage
blocks and answer the question the black frame raised: do the streamed bytes reach a GPU resource? They do not. All 4,096
decompressed 64 KiB blocks land in ordinary process-heap addresses, a title CPU deserializer (`game+0x248F680`, reached
as the virtual method `[obj+0x18]` through the job wrapper `game+0x4F65820`) reads the first batch's blocks straight
after they complete, and **no device entry point ever names any of the 4,096 destination ranges** - not as an argument,
and not inside a structure an argument points to. The earlier linear-layout A/B appeared to hand a block address to
console view-creation calls; this increment settles that call shape and shows those words were saved-register and
caller-stack carry-over, not arguments. The measured Xbox-ABI shapes are PC-shaped (3-argument
`CreateShaderResourceView`, 4-argument `CreateUnorderedAccessView` with a NULL counter resource, and a 3-argument
descriptor creation on console slot 70), each followed by a CPU cache-line writeback of its destination descriptor. The
sampled texture is black for a separate, already watched reason: its own title pages are never written. Counts, RVAs,
ranges, descriptor dwords and content-class labels only; no package bytes, paths, shader bytes or extracted tables.

## 1. What was added (diagnostic only, off by default)

`port/shims/dstorage.cpp` - `PALMETTO_STREAM_CONSUMER=1` (read once; with it unset every hook returns on one cached
boolean and no guard, handler or range is created):

* each completed request's destination is reported to the device proxy's range registry, and the first bytes of the
  first blocks are classified as a content class (a label, never a byte value);
* the pages **wholly inside** the tracked destinations get `PAGE_GUARD` on the worker thread right after the block is
  written (the block's first/last partial pages are left alone, so heap metadata and neighbours cannot fault). The first
  access after completion faults into a vectored handler that prints the accessing code as a title RVA, a
  host-runtime (`palmetto.exe`) RVA - a shim read, the XG fill being the one this looks for - or neither, plus the title
  frames on the stack, then clears the guard and lets the access retry. Bounded: 32 tracked destinations (the first
  sixteen requests, then one every 256th), 32 guard slots, 24 fault lines, fixed lines, no allocation or module lookup
  in the handler. A block written again by a later request is reported and re-armed;
* periodic counters every 1,024 completed requests (requests, MiB, distinct destinations, tracked blocks, fault count).

`port/shims/d3d12_xs_device.cpp`:

* the streamed-range registry (8,192 ranges, lock-free min/max prefilter) and the device-side correlation: every device
  call's first six argument words are tested against the ranges, and the first call of each entry point a consumer's
  GPU step would use is printed with its caller (bounded; a hit is reported as `STREAM CONSUMER: ... names streamed
  block #N`);
* `PALMETTO_XBOX_VIEW_SHAPE=1`: the Xbox-ABI call-shape probe for the three console view-creation sites. For slots 18
  (`CreateShaderResourceView`), 19 (`CreateUnorderedAccessView`) and 70 it prints, once per call site, the caller as
  `game+RVA` and all fourteen words the thunk receives with each word's origin (`arg`, `saved-rsi`, `retaddr`, `local`,
  `caller-stack`), the first 8 dwords of every readable pointer word, and it scans the first 256 bytes of every readable
  word for values inside a streamed block (the indirect case). Across the boot it counts streamed-range hits per slot and
  per word position, and scans pointed-to structures on every call for slots 18/19 and every 1024th call for slot 70;
  the histogram prints on the first call, on every new position hit and every 256 calls. Off by default, bounded, and it
  changes no argument;
* an exported range lookup (`palmetto_streamed_block_index`) for a future XG-shim correlation, unused by this increment.

Both files are also linked into GPU-free test binaries; the new code has no shim-registry dependency.

## 2. The stream, measured in the same boots

`PALMETTO_DSTORAGE_TRACE=1` plus the consumer counters, standard environment (shader corpus, Vulkan device,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`; linear texture layout is the default now, so
`PALMETTO_XG_LAYOUT` is omitted), final binary, 130.2 s boot:

| Measure | Value |
|---|---|
| Run | ALIVE 130.2 s (killed by the timeout as usual), 0 exceptions, no device-removed line, 572,218 recorded d3d12 calls |
| Requests | 4,096, all zlib file requests, every status entry `S_OK` |
| Decompressed | 256.0 MiB in 1.7 s (152.2 MiB/s; 121-157 MiB/s across the repeats) |
| Enqueue caller | `game+0x28C9A0E` for every request |
| Destination ranges | 4,096 **distinct** process-heap addresses, 64 KiB each |
| First sixteen destinations | `0x...5FAE20`, `0x...60AE20`, ... `0x...6EAE20` - sixteen 64 KiB blocks spaced 1 MiB apart in one allocation (two 8-block resources' worth) |
| Later sampled destinations | different allocations (`0x...162AE68`, `0x...555F4F6D4`, ...) |
| Content class (first 8 blocks) | structured binary 7/8; 16-byte repeating pattern 1/8; no known container tag, all-zero or still-compressed block |

## 3. The consumer chain (first-touch evidence)

Thirty-two destinations were guarded (the first sixteen requests, then one destination every 256th request). The first
sixteen were read **in order, at ~2.3 s, immediately after the last block completed**, each starting at the block head:

| Blocks | First access | Accessor |
|---|---|---|
| #0-#15 (all sixteen) | first guarded page, +0x1E0..+0x208 into the block; all reads, no writes | 14 at `game+0x248FEF9`, 2 (#5, #12) at `game+0x248F6EF`; the same five-frame stack each time (`game+0x248F772/79D/7F3/849` <- `game+0x4F6584A`) |
| #16-#31 (requests 256, 512, ... 4096) | **never touched** in the 130 s window (0 faults, still guarded at the end) | - |

So the consumer reads the first two streamed resources and then stops: sampled blocks from every later request are never
deserialized in the window. The consumer is the loader job wrapper `game+0x4F65820` invoking the deserializer virtual
method `game+0x248F680` (disassembly in section 5 of the previous revision of this page: it walks a record pointer at
`[object+0x150]`, feeds dwords to a pool helper and resolves a run of `+0x38..+0x68` fields through the two tiny thunks
that own the faulting RIPs). **No host-runtime first touch was observed on any of the 32 tracked blocks** - the XG fill
path does not read them (section 5).

## 4. The GPU side: no device call names the ranges

The device-side correlation ran for the whole boot, from the first completed block on:

| Question | Result |
|---|---|
| Any device call argument (first six words, every slot) pointing into a streamed range | **0** - no `STREAM CONSUMER` line at all |
| Any value inside a structure an argument points to (256-byte window, first call per site) | **0** in the boot-shape1 probe; the wider probe in section 5 scans across the boot and finds only caller-stack structures, never an argument's |
| Copy-family command-list slots (15/16/17/18/19/85/146) | **never called** in the window |
| COPY command queue or COPY command list created | **no** (queue/list creation lines show command type 0, no type 3) |
| Heap/placed-resource entry points naming a block address (slots 27/28/29/48/49/53/54/69/70/76/77/127/133) | **none** |
| Console `CreatePlacedResourceX` (slot 133) | runs (first call caller `game+0x193E2DB`), names **title page** VAs, not streamed blocks |

This agrees with the replay-wide measurements (`docs/page_content.md` section 8: 0 copies, 0 dispatches, 0
`WriteToSubresource` in a 300 s boot) and sharpens them: no device call carries a streamed block address at all. The
blocks are consumed by the CPU loader and never handed to the GPU in this window.

## 5. The console view-creation sites: the block address is carry-over, not an argument

The earlier linear-layout A/B saw addresses inside a streamed block in the *higher* words of the console
`CreateShaderResourceView` (slot 18) / `CreateUnorderedAccessView` (slot 19) calls and of console slot 70. This increment
settles the call shape offline (disassembly of the wrappers, RVAs only) and measures it in the boot with
`PALMETTO_XBOX_VIEW_SHAPE=1`.

**Offline: the three wrappers and the writeback.**

* slot 18, wrapper `game+0x164EE0` (the call returns to `game+0x164F01`): `push rsi; sub rsp,0x20; rsi=r8; r8=rdx;
  rdx=rcx; rcx=<device global>; r9=rsi; call [rax+0x90]`, then `jmp 0x4ECC830` with `(rsi=r8, 0)`. It forwards
  **three** arguments after the device - (descriptor, resource, destination descriptor) - exactly the PC
  `CreateShaderResourceView` shape. Callee words: 1-3 arguments, 4 = the caller's `rsi` saved by `push` (a register save),
  5 = the wrapper's return address, 6+ = the wrapper caller's stack;
* slot 19, wrapper `game+0x16475F0` (returns to `game+0x1647616`): `push rsi; sub rsp,0x30; r9=rdx; rdx=rcx;
  rcx=<device global>; rsi=r8; [rsp+0x20]=r8; r8d=0; call [rax+0x98]`, then the same writeback on `rsi=r8`. It forwards
  **four** arguments - (descriptor, resource, **NULL** counter resource, destination descriptor) - exactly the PC
  `CreateUnorderedAccessView` shape, and the explicit NULL for the counter resource is the giveaway that this is the PC
  prototype. Callee words: 1-4 arguments (4 is stored to the stack), 5 = an unwritten local slot, 6 = the caller's `rsi`
  saved by `push`, 7 = the wrapper's return address, 8+ = the caller's stack;
* slot 70, wrapper `game+0x4ECC800` (returns to `game+0x4ECC821`): byte-identical prologue to the slot-18 wrapper, calling
  `[rax+0x230]`, then the same writeback on its third argument. It forwards **three** arguments. Observed at the site:
  arg1 = pointer to a stack struct, arg2 = pointer to a `D3D12_CONSTANT_BUFFER_VIEW_DESC`-shaped struct (first dwords
  `BufferLocation 0x100000027`, `SizeInBytes 0x1688`), arg3 = a destination descriptor in the console descriptor heap
  (`0x...510000220`). Its other (and dominant) call site is `game+0x4D9F369`, where the same 3-argument call carries a
  CBV-shaped second argument and a destination descriptor loaded from an engine object at `+0x1FA38`; it runs once in the
  per-frame constant-buffer path and accounts for the 13,568 calls;
* all three wrappers tail-call `game+0x4ECC830`, a cache-line writeback loop (`clwb` over `[pointer, pointer+global)`,
  pointer rounded to a cache line). This is the title's UMA publication step for CPU-written descriptors: write the
  descriptor on the CPU, then flush its cache lines so the GPU sees it.

**In the boot (final binary, 130.2 s).** The probe monitored 13,629 calls: 13,568 on slot 70, 39 on slot 18 and 22 on
slot 19 (one site each for 18/19; two sites for 70). Measured:

| Question | Result |
|---|---|
| Streamed-range hit in any of the 14 words, any call, any of the three slots | **0** (histogram: "no streamed-range hit in any word" at every print through slot 70's 13,568 calls; no position-hit line for slots 18/19) |
| Streamed-range hit inside an argument-pointed structure (all scans: every 18/19 call, every 1024th slot-70 call) | **0** |
| Streamed-range hit inside a **caller-stack**-pointed structure (not an argument) | **10**: slot 19, calls 12-16, caller `game+0x1647616`, words 9 and 10 (both `caller-stack`), structures at `+0x80` and `+0x38` holding pointers inside streamed blocks #44 and #45 |
| Observed argument words | slot 18: word 3 = console descriptor address `0x...510000000`; slot 19: word 4 = `0x...5100000A0`; slot 70: word 3 = `0x...510000220` - all console descriptor destinations, never a block address |

So the earlier A/B matches sat exactly in the positions the offline analysis predicts for carry-over: word 4 of the
slot-18/70 thunk (saved caller `rsi`), word 5 of the slot-19 thunk (unwritten local) and words 6+/8+ (caller stack). The
A/B happened to catch a boot in which the caller's saved register held a block pointer; this boot's 13,629 calls produced
no word hit at all, and the only block addresses near these calls are in the caller's own frame structures, which the
device call never receives.

**What that means for the upload lead:** the direct-argument and indirect-argument (structure) cases are both negative.
The streamed data is present in the title's CPU frames next to the descriptor-creation path (blocks #44/#45 in two caller
locals during five UAV creations) but it is not handed to any device call. The port does not drop a streamed address at
these sites - there is no such address to drop.

## 6. The answer: where the GPU-side step is missing

* **Do the streamed bytes reach a texture?** No. They reach a title CPU deserializer and, in the whole 130 s window, no
  device call names them - not directly, and not through a descriptor an argument points to. The first two resources
  (16 blocks) are deserialized at 2.3 s; every sampled later block is untouched. The "streamed bytes -> sampled texture"
  hypothesis is closed: the sampled T# image is black because the streamed content never enters a GPU resource.
* **What is the missing GPU-side step?** There is no GPU-side step for the streamed 256 MiB in the measured window. The
  data ends on the CPU: no copy is recorded, no placed resource or heap is created at a block address, no view names one.
  The Xbox call shape at the three console sites is PC-shaped and carries descriptor destinations, so the A/B's
  "console-addressed view" reading is withdrawn. The GPU publication step that does exist for CPU-written memory is the
  cache-line writeback (`clwb`) after each descriptor creation - a UMA flush, not a copy, and it is not applied to the
  streamed blocks.
* **The sampled texture's own gap is separate and already watched.** `framecap`'s page-writer watch (linear layout
  default) reports the sampled T# placed texture's title pages all-zero and never written over 150 s, and this boot's
  readback agrees: T# image format 37 3840x2160 (title VA `0x...22D0000`), 128x128 window mean 0 0 0, non-black
  0/16384; target readback 0/16384; write control 51/102/204; the V# constants are plausible. So the black sampled
  image is "the page fill never happened", not "a fill landed elsewhere".
* **XG is not the carrier either.** In this boot the texture computer did: suggest 44, create 24, `GetResourceLayout`
  (slot 2) 2, `CopyIntoSubresource` (slot 8) 65 calls copying 34,044 bytes, slot 10 never called. The first copy is the
  256x128 atlas from static title CPU memory; the rest are 4-byte 1x1 defaults from stack addresses. None of them reads
  a tracked streamed block, and no host-runtime first touch was observed on any of the 32.
* **Timing/wait:** no waitable object is missing (`docs/wait_state.md`), and the consumer does read the first resources
  immediately; the later batches staying untouched is the observable of the pre-content loop, not a stall.

## 7. Limits of this measurement

1. Guard sampling: the first sixteen destinations are proven read; the sixteen sampled later destinations (requests 256,
   512, ... 4096) are proven untouched; blocks 17-255 and the unsampled ones are unobserved. The consumer might stop
   somewhere between request 16 and 256; that range is not measured.
2. `PAGE_GUARD` covers only pages wholly inside a block; a consumer touching only a block's first or last partial page
   would not fault. The observed consumer walk crosses the first guarded page, so this did not hide it here.
3. The window is 130 s (three boots: 130.1 s, 130.2 s, 130.2 s; 575,142 / 577,671 / 572,218 recorded d3d12 calls). The
   300 s boots of `docs/page_content.md` show no uploads either.
4. The structure scan covers 256 bytes from each readable word pointer - enough for the measured descriptor structs, not
   a proof for arbitrary pointer graphs. The slot-70 structure scan runs every 1024th call (14 samples); slots 18/19 on
   every call (61 calls total).
5. Content classes are first-256-bytes statistics and container-tag matches, not decoding; no byte value is printed.
6. The address values change from boot to boot (ASLR/runtime allocation); the RVAs, counts, shapes and the word-origin
   analysis are the stable facts.
7. Console slot 70's *semantics* are not decoded: it is measured as a 3-argument console descriptor creation whose second
   argument is a CBV-shaped struct, refused by name, 13,568 calls per boot. Nothing in this increment depends on its
   meaning; it is named as the next device-side increment.

## 8. Gates, files and the next falsifier

Gates: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` (re-run on the final
binary). Both switches are off by default and alter no answer. Files touched: `port/shims/dstorage.cpp`,
`port/shims/d3d12_xs_device.cpp`, this page. Evidence boots on disk (outside the repository): `boot-shape1`,
`boot-shape2`, `boot-shape3` (the final binary's numbers above), each with `stderr.log` and `summary.txt`.

Next, smallest honest first - the stream itself is closed, so these are device/coverage follow-ups:

1. **Console slot 70.** 13,568 calls per boot, 3 arguments, second argument a CBV-shaped struct, third a console
   descriptor destination, then a cache-line writeback; refused `E_NOTIMPL` by name. Decode it (its two stack structs and
   the engine object at `+0x1FA38`), because it is the largest unmodelled console device entry point in the boot and the
   only remaining place a console-addressed descriptor/view could be dropped. This does not reopen the stream lead: no
   block address appears in its words or pointed-to structures.
2. **The caller-stack structures at the five slot-19 calls.** They hold block #44/#45 pointers while the title creates
   UAVs; identify the struct fields and owning object (log the two `caller-stack` words' structures and the caller's
   object) to finish the CPU-side picture of what the deserialized data becomes. If it is only a CPU-side cache, the
   stream has no GPU step by design in this build.
3. **Coverage of the consumer stop.** Guard the first 32 destinations (all of requests 1-32) and extend the window past
   130 s, to bound where between request 16 and 256 the consumer stops; that is the last open number in the stream
   chain.
4. The sampled-texture content frontier stays with `framecap`/`xgslots`: the T# pages are never written (page-writer
   watch), while the XG fill writes only the 32 KB atlas and 1x1 defaults. The next step there is to find which title
   path was supposed to write the sampled pages, not the streamed blocks.
