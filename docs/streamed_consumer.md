# The streamed-block consumer: a title CPU deserializer, and the GPU-side step that never happens

**Status: measured.** Two 130 s harness boots name the consumer of the DirectStorage blocks and answer the question the
black frame raised: do the streamed bytes reach a GPU resource? They do not in the black-frame configuration. All 4,096
decompressed 64 KiB blocks land in ordinary process-heap addresses, a title CPU deserializer
(`game+0x248F680`, reached as the virtual method `[obj+0x18]` through the job wrapper `game+0x4F65820`) reads the first
batch's blocks straight after they complete, and **no device entry point ever names any of the 4,096 destination
ranges**: no copy, no placed resource or heap at those addresses, no view. An A/B boot with the console texture layout
answered (`PALMETTO_XG_LAYOUT=linear`) shows the intended path does exist - the title then hands block-interior
addresses to console view-creation calls - and that is exactly the step the port drops (the console address words live
beyond the PC argument shape, and console slot 70 is refused). Counts, RVAs, ranges and content-class labels only; no
package bytes, paths, shader bytes or extracted tables.

## 1. What was added (diagnostic only, off by default)

`port/shims/dstorage.cpp` - `PALMETTO_STREAM_CONSUMER=1` (read once; with it unset every hook returns on one cached
boolean and no guard, handler or range is created):

* each completed request's destination is reported to the device proxy's range registry (below), and the first bytes
  of the first blocks are classified as a content class (a label, never a byte value);
* the pages **wholly inside** the first tracked destinations get `PAGE_GUARD` on the worker thread right after the
  block is written (the block's first/last partial pages are left alone, so heap metadata and neighbours cannot fault).
  The first access after completion faults into a vectored handler that prints the faulting RIP as `game+RVA`, the
  access kind, the offset inside the block, and the title frames on the faulting stack, then clears the guard and lets
  the access retry. Everything is bounded: 8 tracked destinations from the first requests plus one per 512th request
  (12 tracked), 32 guard slots, 16 fault lines, fixed 768-byte lines, no allocation in the handler, no module lookup in
  the handler (the title image base is captured once from the enqueue caller). A block written again by a later request
  is reported and re-armed, so a queue-side rewrite is never mistaken for a title read;
* periodic counters every 1,024 completed requests: requests, MiB, distinct destinations, tracked blocks, fault count,
  still-guarded count.

`port/shims/d3d12_xs_device.cpp` - the streamed-range registry (8,192 ranges, lock-free min/max prefilter) and the
device-side correlation, active when the shim switch filled the registry:

* every device call's first six argument words are tested against the recorded ranges, so a heap created from a
  block's address, a placed resource at it, or **any** entry point whose number this proxy has not identified yet is
  reported as `STREAM CONSUMER: ... names streamed block #N` with the caller as `game+RVA` (bounded: 32 lines);
* the first call of each entry point a consumer's GPU step would use is printed with its arguments and caller:
  device slots 8/12/27/28/29/48/49/53/54/69/70/73/75/76/77/127/133 (queue/list creation prints the command type, so a
  COPY queue would show up) and command-list console slots 15-19/85/146 (copy family and the enhanced-barrier group);
* the match test is independent of the console's slot numbering, which is the important part: a title call that passes
  one of the block addresses is caught whatever slot it uses.

Both switches change no path: with them unset the boot is byte-for-byte the one before this increment, and with them set
the guard handler only clears the page protection it set. Gated measurements: `PALMETTO_OK`, `TESTS_BUILT` (section 8).

## 2. The stream, measured in the same boot

`PALMETTO_DSTORAGE_TRACE=1` plus the consumer counters, one 130.3 s boot (`PALMETTO_DSTORAGE_TRACE` is the pre-existing
queue trace; the boot ran with the standard environment: shader corpus, Vulkan device, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`):

| Measure | Value |
|---|---|
| Run | ALIVE 130.3 s (killed by the 130 s timeout, as usual), 0 exceptions, no device-removed line in the log |
| Requests | 4,096, all zlib file requests, every status entry `S_OK` |
| Decompressed | 256.0 MiB in 2.3 s (109.7-121.0 MiB/s) |
| Enqueue caller | `game+0x28C9A0E` for every request (unchanged from `docs/wait_state.md`) |
| Destination ranges | 4,096 **distinct** process-heap addresses, 64 KiB each |
| First eight destinations | `0x2A5127FAE20`, `0x2A51280AE20`, ... `0x2A51286AE20` - 64 KiB blocks spaced 1 MiB apart in one allocation |
| Later sampled destinations | different allocations (`0x2A51482AE68`, `0x2A55914F6D4`, `0x2A55B14F6D4`, ...) |
| Content class (first 8 blocks) | structured binary 7/8; 16-byte repeating pattern 1/8; no known container tag, all-zero or still-compressed block |

So the stream is exactly as `docs/wait_state.md` and `docs/texture_import.md` measured; the new fact is what happens to
the blocks after the worker writes them.

## 3. The consumer chain (first-touch evidence)

Twelve blocks were guarded (the first eight requests, then one destination every 512 requests). The first eight were
read **in order, at ~2.3 s, immediately after the last block completed** - the first access is a read at the block's
first guarded page, i.e. the consumer starts at the block head and walks forward:

| Block | First access | Faulting RIP | Title frames on the faulting stack (top to bottom) |
|---|---|---|---|
| #0 | block + 0xF20 (read) | `game+0x248FEF9` | `game+0x248F79D` <- `game+0x4F6584A` |
| #1 | block + 0xF20 (read) | `game+0x248FEF9` | `game+0x248F7F3` <- `game+0x4F6584A` |
| #2 | block + 0xF20 (read) | `game+0x248FEF9` | `game+0x248F849` <- `game+0x4F6584A` |
| #3 | block + 0xF28 (read) | `game+0x248F6EF` | `game+0x4F6584A` |
| #4 | block + 0xF48 (read) | `game+0x248FEF9` | `game+0x248F772` <- `game+0x4F6584A` |
| #5 | block + 0xF38 (read) | `game+0x248FEF9` | `game+0x248F772` <- `game+0x4F6584A` |
| #6 | block + 0xF28 (read) | `game+0x248FEF9` | `game+0x248F772` <- `game+0x4F6584A` |
| #7 | block + 0xF20 (read) | `game+0x248FEF9` | `game+0x248F79D` <- `game+0x4F6584A` |

(The offset is the first page boundary inside the 64 KiB block, so the fault says where the walk crossed the block's
first partial page, not where the read starts. The stack lines above are the frames that resolve into the title image;
the deep-scan produced further words that resolve to no function in `.pdata` and are not quoted.)

Disassembly of the two RIP owners (offline, RVAs only) fixes the chain:

* `game+0x248F680..0x248FEA1` is a deserialization routine: it takes a pointer at `[object+0x150]`, walks a
  begin/end pair at `[object+0x150]`/`[object+0x158]`, feeds two dwords from the walked record (`+0x8`, `+0xC`) to a
  helper together with pool/buffer pointers at `[object+0x180]`/`[object+0x1B0]`, then resolves a run of fields at
  `+0x38..+0x68` of the record through the small thunks `game+0x248FEE0`/`game+0x248F6EF`, and compares a header word
  against a repeated 16-bit constant. That is loader/deserializer shape, not a GPU call: there is no D3D12 entry point,
  no copy and no resource creation in it;
* the routine is reached as a **virtual method `[object+0x18]`** from `game+0x4F65820..0x4F658BB` - a job-dispatch
  wrapper (it checks job state words at `+0x78`/`+0x64`, calls `[object+0x30]->vtable[3]`, then the virtual method) -
  and the frame `game+0x4F6584A` is the wrapper's return address after that virtual call. The wrapper itself is reached
  through function-pointer tables (its own address appears in `.rdata` dispatch tables), so the consumer is a loader
  job, not a render-thread call.

So the chain is: **DirectStorage worker writes a 64 KiB decompressed block -> the title's loader job
(`game+0x4F65820`) runs the block's deserializer virtual method (`game+0x248F680`) -> the deserializer reads the block
on the CPU at 2.3 s.** No title write to a guarded block was observed in the window (all first-touch faults were reads).

**The sampled later blocks are not touched in the window.** The four destinations sampled from requests 512, 1024, 1536
and 2048 (different allocations than the first batch) have 0 first-touch faults after 130 s - so the consumer is not a
uniform pass over every streamed block; the later batches are not deserialized in the measured window. That is one more
observable of the "pre-content loop": content is streamed, only the first batch's blocks are consumed, and the rest stay
untouched.

## 4. The GPU side in the black-frame configuration: nothing names the ranges

The device-side correlation ran for the whole boot, from the first completed block on (before that there was nothing to
name). Measured:

| Question | Result |
|---|---|
| Any device call argument pointing into a streamed range | **0** (32-line cap never reached; no `STREAM CONSUMER` line at all) |
| Copy-family command-list slots (15 CopyBufferRegion, 16 CopyTextureRegion, 17 CopyResource, 18 CopyTiles, 19 ResolveSubresource, 85 CopyTextureRegionX, 146 barriers) | **never called** in the window (no first-call line for any of them) |
| COPY command queue or COPY command list created | **no** (queue/list creation first-call lines show command type 0 and no type 3) |
| Heap/placed-resource entry points naming a block address (slots 27/28/29/48/49/53/54/69/70/76/77/127/133) | **none** |
| Console `CreatePlacedResourceX` (slot 133) | runs (first call, caller `game+0x193E2DB`) but the `va` it names is a **title page** (e.g. `0x2C460600000`), not a streamed block |
| Console committed-resource/resource creation | first call before streaming began (nothing to name at that time); no call afterwards names a block |

This is consistent with the earlier replay-wide measurements (`docs/page_content.md` section 8: 0 copies, 0 dispatches,
0 `WriteToSubresource` in a 300 s boot, 0 copy-source pulls) and it sharpens them: it is not that a copy is recorded
with the wrong source - **no device call at all carries one of the streamed block addresses in the black-frame
configuration**. The blocks are consumed by the CPU loader and never handed to the GPU.

## 5. A/B: the console texture-layout answer lets a streamed buffer be named - and the port drops it

One more 130.3 s boot with the same environment plus `PALMETTO_XG_LAYOUT=linear` (the shim answers the console texture
computer's linear layout instead of refusing; this is **not** the black-frame configuration, it is the A/B probe):

* the stream itself is identical (4,096 requests / 256.0 MiB / 2.3 s; the same first-touch consumer chain on the same
  deserializer, blocks #0-#7 read at the block head at +0x1E0..+0x208 of their first guarded page);
* the XG texture-computer fill does run, but its sources are not the guarded streamed blocks: the first copy is the
  initial 256x128 atlas from static title CPU memory (`src 0x2A5126D4DE0`, `0x8000` bytes, caller `game+0x16F71FF`),
  and the following copies are 4-byte 1x1 defaults from stack addresses. So CopyIntoSubresource is not what carries
  the 256 MiB;
* **the title then names a streamed block to the device.** During the resource-creation phase (~100 s in the boot),
  console view-creation calls receive addresses inside streamed block #33 (`block + 0x3260 .. block + 0x3700`):
  `CreateShaderResourceView` (wrapper `game+0x164EE0`, site `game+0x164F01`), `CreateUnorderedAccessView` (wrapper
  `game+0x16475F0`, site `game+0x1647616`) and console slot 70 (site `game+0x4ECC821`, still refused `E_NOTIMPL` by
  the proxy). The block-internal addresses appear in the calls' higher argument words (a4/a6 of the Xbox-ABI thunk);
  the placed textures in the same boot are at title page VAs, not at streamed addresses, so no `CreatePlacedResourceX`
  names the buffer either.

Interpretation, with its limit stated: the title's console GPU addresses are its own process addresses (UMA), so the
intended GPU-side step for this data is a console-addressed resource/view creation over the CPU-resident buffer - the
kind of call whose PC counterparts take an `ID3D12Resource*` and cannot carry the address. Our proxy forwards slots
18/19 to the PC methods (which read only pDesc/pResource/dest and drop the rest of the Xbox argument words) and refuses
slot 70, so that address reaches nothing in this layer. **Caveat:** the two wrappers pass only three (SRV) and four
(UAV) arguments after the device, so some of the higher argument words examined at the thunk are saved registers or
caller-frame carry-over rather than true arguments; the doc treats "which word is a real argument" as the next
falsifier below, not as settled. What is settled is that (a) block #33's range is live and is handed to view-creation
code paths at ~100 s, and (b) no device call in the baseline boot names any block range.

## 6. The answer: where the GPU-side step is missing

* **Do the streamed bytes reach a texture?** No, not in the black-frame configuration. They reach a title CPU
  deserializer (`game+0x248F680`) and are never named by a device call: no upload, no copy, no placed resource or heap
  at their addresses, no view. This closes the "streamed bytes -> sampled texture" hypothesis for this window: the
  sampled T# image is black because the streamed content never enters a GPU resource, not because a copy landed in the
  wrong place.
* **What is the missing GPU-side step?** The step that would bind the data is a console-addressed resource binding
  (a placed resource / buffer view over the CPU-resident block range, or a copy from it), and it never happens in the
  black-frame configuration. In the linear-XG A/B the title does start issuing exactly that shape of call at ~100 s,
  but the port cannot carry it: the addresses sit in Xbox-ABI argument words that PC forwarding does not consume, and
  console slot 70 is refused by name. The step is therefore missing between "the loader has deserialized the block" and
  "a D3D12 resource names the block's memory" - not at the pixel shader, the raster path or the present path.
* **Timing/wait:** no wait is missing (`docs/wait_state.md` disproved a DirectStorage signal/event gap), and the
  consumer does read the first batch immediately; the later batches staying untouched is the observable of the same
  gap, not a stall on a waitable object.

## 7. Limits of this measurement

1. Only 12 of 4,096 destinations are guarded (first 8 requests + every 512th). The eight first-batch blocks are proven
   read; the later four samples are proven untouched; blocks 9-511 and the ones not sampled are unobserved.
2. `PAGE_GUARD` covers only pages wholly inside a block; a consumer touching only the block's first or last partial
   page would not fault. The consumer's own walk crosses the first guarded page, so this did not hide it here.
3. The window is 130 s. The 300 s boots of `docs/page_content.md` show no uploads either, but the later-batch consumer
   was only sampled in 130 s.
4. The match test covers each device call's first six argument words and the copy-family list slots' six; an address
   passed in a deeper stack argument or inside a pointed-to structure is not seen. The registry holds 8,192 ranges; a
   boot with more requests would stop recording (4,096 here, so complete).
5. Content classes are first-256-bytes statistics and container-tag matches, not decoding; no byte value is printed.
6. The linear-XG A/B changes the title's answers and is a diagnostic configuration; the numeric addresses differ from
   boot to boot (the ranges and the RVAs are the stable facts).
7. Whether the higher Xbox-ABI argument words are true call arguments is not settled (section 5 caveat).

## 8. Gates, files and the next falsifier

Gates: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`. Both switches are off by
default and alter no answer; the boot A/B pairs above are the only cost. Files touched: `port/shims/dstorage.cpp`,
`port/shims/d3d12_xs_device.cpp`, this page.

Next falsifier, smallest honest first:

1. **Settle the Xbox view/placed call shape.** Log all 14 argument words of the console slots at the call site
   (18/19/70/76/127) with their per-site callers, or read the wrappers' callers to count the true arity. If a block
   address is a real argument, the smallest fix that would let the streamed bytes reach a resource is to model that
   console-addressed view/placed-resource creation (a buffer over the title's address at that VA, or the console view
   form carrying the VA) instead of forwarding it to the PC method that drops the words - then re-run the frame capture
   (`PALMETTO_FRAME_DUMP_SAMPLE=1`) in the same A/B and see whether the sampled image stops being black.
2. If the address is caller-frame carry-over, the next place to look moves one step earlier: instrument the XG texture
   computer (slots 2/8/10) with the source and destination addresses and correlate them with the streamed block ranges,
   i.e. finish the `docs/texture_import.md` section 5 address-mismatch question with the streamed ranges in the table.
3. Widen the consumer observation: guard the first 8 destinations of **each** submit batch (and a longer window), so
   "later batches are never deserialized in 130 s" becomes a per-batch statement rather than four samples.
