# Console bottom-of-pipe writes, the 64-bit memory wait, and the allocator callback's invocation

**Status: measured, implemented behind opt-in switches, and A/B-booted, 2026-11-25.** The peer parity matrix
([cleanroom_peers.md](cleanroom_peers.md), slices 1 and 2) ranks two mechanisms this page carries: the console command
list's bottom-of-pipe writes and memory-value wait, and the title's allocator callback that slot 9 registers but nothing
invoked. Both are answered here: the four list slots are decoded from this title's own call sites and one probe boot, the
console list slot 70 named in the matrix turns out to be a different method (PC slot 62 `OMSetDepthBounds`), and the
allocator callback is invoked once per allocator under a switch with the callback's measured argument shape. RVAs,
argument words, counts and outcomes only: no executable bytes, game data, package paths or extracted tables.

## 1. Why these slots are needed

Five Star Recomp's published D3D12.X page and XCL's handoff document the console's cell handoff: a command list records
a value the GPU writes into title memory once its earlier work is done, and a later wait holds the queue until a cell
holds a comparison. Every peer that reached the in-world state implements some form of it, and the matrix calls it the
only peer mechanism in our measured loader/frame path that we do not carry: our list proxy answers slots 71, 72 and 74
with 0 and the title calls them 2, 4 and 1 time a frame. The allocator page's own limits section names the registered
callback as the next falsifier; this page answers it.

The measured value of the slice is not a stage move - the stage-2 stall is title-side job dispatch
([stage2_blocker.md](stage2_blocker.md) sections 11-14) - but the calls are no longer silent: the write model lands the
measured values in the title's cells, the wait model reports every unmet cell instead of dropping the wait, and the
allocator callback's lifecycle is exercised for the first time.

## 2. Measured shapes

Two sources: a linear disassembly of the retail image (kept outside the repository) and one 130.5 s probe boot of this
tree under the boot lock with the measurement switches below. The probe boot's environment was the tree's standard set
plus `PALMETTO_XS_CALLERS=1`, `PALMETTO_XS_CALLERS_PROFILE=1`, `PALMETTO_XS_CONSOLE_LIST_TRACE=1`,
`PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE=1`, `PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE=30` and
`PALMETTO_STATE_TRACE=2,130`; the default path answered every call exactly as before (no model switch), the boot was
`ALIVE 130.5 s`, 0 exceptions, 227,097 `[d3d12-trace]` calls, stage word `2` with `loading 1` to the kill.

### 2.1 The call table

The per-frame rate is the probe boot's caller-profile count divided by its slot-74 count (that slot runs approximately once a
frame: 4,386 calls against 4,640 presents in that boot). The caller is the return address of the call; slot numbers are console vtable
indices.

| slot | measured shape | cell | call site (return RVA) | calls in the probe boot | per frame |
|---:|---|---|---:|---:|---:|
| 70 | two floats in XMM1/XMM2, no integer argument | PC slot 62 `OMSetDepthBounds` | `game+0x33396` | 13,153 | 3.0 |
| 71 | (address, dword, 0, 0) | a per-frame marker ring in title memory | `game+0x4ED46AA` | 8,772 | 2.0 |
| 72 | (address, 0, 0, 1) | two 8-byte query-record cells | `game+0x2482B37` | 17,530 | 4.0 |
| 74 | (address, 7, qword, 1) | the marker ring's base cell | `game+0x2484B12` | 4,386 | 1.0 |

Slot 70's measured floats were `min 0.000000 max 1.000000` at every logged sample (the first eight calls and every
4,096th after). Slot 74's measured arguments were constant: address `0x1F97F3B0000`, comparison `7`, value `1`, flags
`1`. The dword-console arguments arrive in the low half of their stack slot; the upper 32 bits are caller residue (the
slot-71 flags printed as `0xAF00000000`, the slot-74 flags as `0x6500000001`), so the model compares the low dword only.

### 2.2 Console list slot 70 is `OMSetDepthBounds`, not an immediate write

The slot inventory page ([console_slots.md](console_slots.md) section 9) guessed slot 70 from its integer census words
as "the recorder's immediate-write fallback ... shape matches PC slot 66". That reading is wrong, and the census's
integer words were residue: the only call site is `game+0x33390` (return `0x33396`) inside the recorder
`game+0x32520..0x33CB5`, which compares two floats and stores them at `owner+0x123F0`/`0x123F4`, then calls
`call [rax+0x230]` with the list in RCX and the **floats in XMM1/XMM2**. `0x230/8 = 70`, and the frames are exactly
`OMSetDepthBounds(Minimum, Maximum)` - which Five Star Recomp's own vtable table places at console slot 70 / PC slot 62.
The proxy's integer thunk cannot carry the XMM words, so console slot 70 has never reached any PC method; the fix is a
float-shaped vtable entry (section 3). This is a correction of the programme's ledger, not a peer disagreement: the
peers' numbering puts the bottom-of-pipe writes at 71/72 and the waits at 73/74, which our measurements match (XCL's
handoff numbers the same methods two lower, #69/#70 and #71/#72, because its console list order differs).

### 2.3 Slot 71, `Write32BitValueBottomOfPipeX`

The caller `game+0x4ED4670` computes its address with `game+0x4ED46D0`:

```text
address = base[0x75DF268] + frame_counter[0x75DF25C] * 72 + index[owner+0x1F9C4] * 4
```

so one 18-dword record per submission, 72 bytes apart. The probe boot's addresses show what one boot writes:
`base + 0x08` and `base + 0x0C` of block 0, then `+0x50`/`+0x54` of block 1, then `+0x98`/`+0x9C`, then the block index
cycles 0, 1, 2 - a three-block ring of per-record cells. The value written at `+0x08` is the incrementing record id
(0, 1, 2, 3, ... in the logged calls); the value at `+0x0C` was the constant `0x29A5EE15`; flush and flags are 0. The
master's parity matrix quotes the peer's wording "a per-frame marker array of 18 dwords"; the measurement agrees on the
stride and adds the ring size and the two indices used by this title's call site.

### 2.4 Slot 72, `Write64BitValueBottomOfPipeX`

Caller `game+0x2482AE0` (return `0x2482B37`) passes `record->+0x58 + index*8` as the address, a zero value and flags 1.
The probe boot shows pairs of 8-byte cells 8 bytes apart (`0x1F7DA596210`/`0x218` and `0x1F7DA59AA30`/`0xAA38`): two
query records' result cells. Flags 1 or 2 ask the console for its 100 MHz clock instead of the value; this call site
always passed flags 1 and a zero value, which is why only the timestamp form appears in the boot.

### 2.5 Slot 74, `Wait64BitValueX`

Caller `game+0x2484AD0` (return `0x2484B12`) passes `(address, 7, qword, 1)`; comparison 7 is `GREATER_EQUAL` and the
whole 64 bits are compared. The waited cell is `0x1F97F3B0000`, the base of the slot-71 marker ring (section 2.3), and
the target is `1` - the first record id. The cell is not written by the slot-71 site (its two cells per record are at
`+0x08`/`+0x0C`), so in the console model it is the GPU's own progress cell: the writer is the title's GPU work and the
wait is the CPU half of the handoff, exactly the peer mechanism the matrix describes as "waits on a pending record's
cell reaching the frame number".

### 2.6 The registered allocator callback

The slot-9 registration itself is unchanged from [allocator_slot9.md](allocator_slot9.md): `(game+0x555DF10, context)`,
once per presented frame, `S_OK`, answer ignored. The addendum there carries this slice's measurement of the callback's
own body: the arguments it reads, the pool it allocates from, the cell it stores into, and the invocation this slice
performs. The probe boot confirms the registration's shape on this tree (callback `0x14555DF10`, the same context every
sample) and that no title code invokes it.

## 3. What we implemented

All of it is in `port/shims/d3d12_xs_device.cpp` and `.h`, behind switches that default off; nothing in the replay or
backend layers was touched.

- **Slot 70, a float-shaped proxy entry.** The command-list vtable's entry 70 is
  `list_slot_om_set_depth_bounds(ListProxy*, float, float)`, which reads the same XMM1/XMM2 registers the title's call
  wrote. With `PALMETTO_XS_CONSOLE_LIST_EXTRAS=1` it forwards to the real list's PC slot 62 `OMSetDepthBounds`; with the
  switch off it answers 0 as before (the console method is void). The caller probe still records the call site.
- **Slots 71 and 72, the bottom-of-pipe writes.** With the model on, the measured value is written into the mapped title
  address when the recording call is made: slot 71 writes one dword, slot 72 writes all 64 bits (flags 1/2 take the
  console's 100 MHz clock from the CPU's counter at record time). The destination is CPU-visible process memory in this
  layer, so the writes go through the kernel (`WriteProcessMemory`) and a destination the title has unmapped reports a
  refusal instead of faulting the recording thread. Shapes outside the measured one - a null or misaligned address, a
  non-zero flush, flags other than 0 for slot 71 or 0/1/2 for slot 72 - are refused by name and counted.
- **Slot 74, the memory wait.** With the model on, the cell is read at record time and the comparison evaluated: a cell
  that holds counts as met, a cell that does not is counted and the first eight are logged (the measured unmet case);
  an unreadable cell or an undecoded comparison/flags shape is refused by name. **The recording thread never blocks**,
  so a wait whose producer is gone cannot hang it. When the same list has already had its bottom-of-pipe write (the
  model writes it at record time), the recorded order meets the wait, which is the model's answer to a same-list
  producer.
- **The allocator callback.** With `PALMETTO_XS_ALLOCATOR_CALLBACK=1` the proxy invokes the registered pair once per
  allocator proxy on its first registration, with the callback's measured shape: `(0, 0, 0, class 0x18, size 0x10000,
  0, context)`. Class `0x18` is measured from the title's own use of the same pool (it allocates 8 bytes of class 0x18
  at `game+0x18845E`, immediately before it registers), size `0x10000` is one console recording chunk, and the context
  is the registration's own context, made readable before the call. The returned block and the `context+0x12798` cell
  before and after are logged; a null return, an unreadable context or a pair without both words is refused by name.
  The two switch inputs the probe may move are `PALMETTO_XS_ALLOCATOR_CALLBACK_SIZE` and
  `PALMETTO_XS_ALLOCATOR_CALLBACK_CLASS`; `PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE=1` logs the registrations and
  `PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE=<seconds>` invokes the pair once after that second as a measurement.
- **Counters.** `console_list_extra_counts()` and `allocator_callback_counts()` are the same numbers the boot logs:
  a summary line is printed on the first extra call and every 16,384 extra calls (a boot that is killed by the harness
  timeout never runs an exit summary), so the last line is the boot's table.

## 4. Cross-check against the reference

The behaviour was cross-checked against the public projects the parity matrix names; no code, comment text or table was
copied, and this implementation is this repository's own structure, naming and tests.

| topic | reference reading | this slice | why |
|---|---|---|---|
| slot 70 | Five Star Recomp: console slot 70 -> PC slot 62 `OMSetDepthBounds`, floats in XMM1/XMM2 | same, from our own call-site disassembly and probe boot | independent agreement, and a correction of our own slot inventory page |
| slots 71/72 | Five Star Recomp: `Write32/64BitValueBottomOfPipeX`, `(address, value, flush, flags)`, `WriteBufferImmediate` in `MARKER_OUT` mode at the translated address; XCL: same two methods, UMA window buffer | measured the same argument shape and the same cell layout; the model writes the mapped title address at record time rather than a desktop marker-out command | our replay's `WriteBufferImmediate` needs a VA-resolvable buffer; the measured destinations resolve as process memory but not as registered buffers, so the GPU-ordered form would refuse whole submissions. Recorded as a limit (section 6) |
| slot 72 timestamp | Five Star Recomp and XCL: flags 1/2/3 write the GPU/system timestamp; Five Star uses `Stopwatch` x 100 MHz | same reading; our model computes the console's 100 MHz clock from `QueryPerformanceCounter` at record time | measurements agree on the flags; the clock source is our own |
| slot 74 | Five Star Recomp: `Wait64BitValueX(address, comparison, value, flags)`, comparison `GREATER_EQUAL`, whole 64 bits, queue held until the cell holds, list split at the wait, watcher thread releases a fence, 5 s timeout; XCL: 2 s timeout, lists split; PercyScarlett credits the same handling to XCL | measured the same shape and comparison; our model evaluates at record time and counts met/unmet, and the recording thread never blocks. The cross-list queue hold is **not carried** (limit 1 of section 6) | our queue-hold machinery lives in `port/src/vulkan_backend.h`, which another slice owns this round; the shim-side model is the honest subset |
| slot 73 | Five Star Recomp: `Wait32BitValueX`; no call site identified in its tree | our boot never called console slot 73 (0 in the census) | nothing to implement; the shape stays the peer's inference, not ours |
| allocator slot 9 callback | the reference fork behind [allocator_slot9.md](allocator_slot9.md) records the pair and never invokes it | we invoke it once per allocator under a switch; the reference's recorded answer is unchanged | this slice goes one measured step past the reference; the limits say exactly what was and was not proven |

Reference files and commits read: Five Star Recomp (`docs/d3d12x-shim.md`, `src/FiveStar.Loader/Shims/D3D12X/
BottomOfPipeWrites.cs`, `MemoryWaits.cs`, `tests/FiveStar.Loader.Tests/D3D12XBottomOfPipeTests.cs`,
`D3D12XMemoryWaitTests.cs`), XCL (`crates/xcl/src/hle/d3d12x/list_x.rs`, `cpu_writes.rs`, `HANDOFF.md`), VIctory
(`crates/victory-gpu/src/list.rs`, `hold.rs`), PercyScarlett (`crates/d3d12_xs/src/cpu_writes.rs`) - clones at the
commits the parity audit records: five-star `0bfa755`, XCL `6d12f58`, VIctory `13f91ca`, victory-fork `9c184b3`,
PercyScarlett `e44c465`. Licences: GPL-3.0-or-later (five-star, VIctory, victory-fork), GPL-2.0-only (XCL,
PercyScarlett).

## 5. A/B measurement

One boot pair under the shared boot lock, one boot at a time, the tree's standard environment plus the measurement
switches of section 2. Arm A is the probe arm: the default path with the trace switches only (the model off), which
measures the call tables above. Arm B is the same binary with the model on
(`PALMETTO_XS_CONSOLE_LIST_EXTRAS=1`, `PALMETTO_XS_ALLOCATOR_CALLBACK=1`) and the stage-2 state probe and the job-graph
probe (`PALMETTO_STAGE2_READY=1`). Both arms ran `tooling/boot_report.py --timeout 130`.

| measurement | A: probe arm, model off | B: model arm |
|---|---|---|
| binary | SHA-256 `EF7ABD3E...` (built before the final two-line flag-mask fix, whose change is inside arm B's model-only comparisons and cannot reach the default path) | SHA-256 `C23086A8...` (gate build `PALMETTO_OK`) |
| verdict | ALIVE 130.5 s, 0 exceptions, 227,097 D3D12 calls | ALIVE 130.4 s, 0 exceptions, 224,482 D3D12 calls |
| stage word / requested stage | `2` / `2`; `loading 1` from 28.0 s to the kill; stage-3 request never written | `2` / `2`; `loading 1` from 24.0 s to the kill; stage-3 request never written |
| slot 70 calls / forwarded | 13,153 / 0 | 12,261 / 12,261 |
| slot 71 calls / writes / refused | 8,772 / 0 / 0 | 8,176 / 8,176 / 0 |
| slot 72 calls / writes / timestamps / refused | 17,530 / 0 / 0 / 0 | 16,340 / 16,340 / 16,340 / 0 |
| slot 74 calls / met / unmet / unreadable / refused | 4,386 / not evaluated (model off) | 4,088 / **4,088** / 0 / 0 / 0 |
| allocator callback | 8 registrations logged, never invoked (probe arm; the one-shot probe used there predates its firing-order fix, so it did not fire) | invoked once per allocator proxy on its first registration: the first eight are logged, each returning a class-0x18/64 KiB pool block (`0x1BB75400000` at `0x20000` stride) with `context+0x12798` 0 -> block; 0 null returns, 0 refusals |
| job graph (pending / ready ids / aggregate holder) | not probed | pending `0x7C` (124) at the wait (+25.356 s), `0x69` (105) at +31.434 s and +78.053 s; ready bit count 1 (id `152`); aggregate's three nonzero qwords frozen at `0x00000033590A0000`/`0x001010040581C020`/`0x0000800000000000` between the two steady samples |
| presented frames (PresentX ok) / refused submissions | 4,640 presents, stationary black loop, 0 refused submissions | 4,236 presents, stationary black loop, 0 refused submissions, 0 `ExecuteCommandLists failed`/`nothing was submitted` lines |

Per frame both arms hold the measured ratios to the slot-74 count exactly: 3.0 slot-70, 2.0 slot-71, 4.0 slot-72 and 1 slot-74. The slot-74 count is not the present count (it trails it by 5.5% in arm A and 3.5% in arm B), so the per-frame figures are the ratios and the two present counts are given directly.

### 5.1 What the model does in the boot

- **Slot 70 is carried instead of dropped:** all 12,261 calls reached the real list's PC slot 62 with the measured
  floats; arm A forwarded none of its 13,153. The layer's command list does not model depth bounds yet (the method is
  a generated stub that answers "unimplemented" by name), so the forward's effect at this tree is that a real recording
  call is now named at the layer instead of being invisible in the shim; no depth-bounds state reaches the GPU. That
  gap is limit 7 of section 6.
- **The bottom-of-pipe writes land:** 8,176 slot-71 dwords and 16,340 slot-72 64-bit timestamps were written into the
  measured title cells, none refused - the destinations are committed process memory, so the kernel write succeeds.
- **The wait is already met at record time in every measured call.** 4,088 of 4,088 slot-74 waits found the cell at or
  above the target `1` when the list was recorded (0 unmet, 0 unreadable, 0 refused). The recorded order therefore
  meets the wait at this content population, and the cross-list queue hold the peer mechanism adds is **not needed for
  the cell this title waits on** in the measured window. That is the measured result of the slice: the negative
  (a cell that does not hold yet) did not occur, so the missing consumer (limit 1 of section 6) is unobservable here rather than
  proven necessary.
- **The allocator callback runs and returns memory:** every first registration on an allocator proxy invoked the pair;
  the bounded log records the first eight, each allocating a 64 KiB class-0x18 block from the recording pool and
  storing it at `context+0x12798`, with no null return and no refusal. The title never reads the block back
  (limit 5 of section 6), so the effect is the lifecycle itself, not a title-visible change.
- **The stage-2 job graph is the same title-side stall:** pending drops from 124 to 105 in the seconds around the wait
  and then freezes with one ready id (`152`) and the frozen aggregate of the stage-2 page's section 13-14 holder. No
  runtime call is outstanding; the model's calls do not move it.
- **Nothing regressed:** no exception, no device loss, no refused submission, no `[d3d12-trace]` submission failure in
  either arm; the presented image stays the stationary black loop. The 1.2% D3D12-call difference between the arms and
  the stage-2 transition moving 28.0 -> 24.0 s are the boot-to-boot variance the tree's other A/B pages record.

## 6. Limits and the next falsifier

1. **The cross-list memory wait is not carried.** The model evaluates slot 74 at record time; it does not hold a queue's
   submission when a cell the list did not write itself does not hold yet. The peer mechanism is a list split at the
   wait plus a queue `Wait` released by a watcher thread; in our tree that consumer belongs to the submission-hold
   machinery in `port/src/vulkan_backend.h` (`QueueHolds`/`Deferred`), which this slice did not touch. **Next
   falsifier:** an arm-B boot in which the slot-74 cell is still short of its target while the frame keeps presenting
   would name the cell and the writer to hold for; if the cell always holds at record time, the missing consumer is
   unobservable at this content population and the negative is recorded.
2. **The bottom-of-pipe write is a record-time write, not a bottom-of-pipe one.** The console writes the value after the
   list's earlier GPU work; our model writes it when the recording call is made, because the measured destinations are
   process memory that is not a registered replay buffer. If a title read of one of these cells needs the GPU ordering,
   the difference is visible as a too-early value. **Next falsifier:** a boot in which a cell the title reads changes
   between the record-time value and the value the GPU would have produced.
3. **The slot-72 timestamp is the CPU's clock.** Flags 1/2 ask the console for its GPU/system clock; the model computes
   100 MHz ticks from `QueryPerformanceCounter` at record time, so the value is the right unit and epoch family but not
   a GPU clock.
4. **The allocator callback's invocation point and size are models, not measurements.** The registration and the
   callback's argument shape are measured; the console's invocation condition (first use, reset, or exhaustion) and the
   size it would ask for are not. The model chooses the first registration on an allocator (where a console runtime
   would set the recording memory up) and one 64 KiB chunk with the class the title's own pool allocation uses.
   **Next falsifier:** an allocator whose `Reset` or recording fails only when the callback was invoked (or only when it
   was not) would make the real contract measurable.
5. **The invoked block is not read by title code.** A static scan of the retail image finds `context+0x12798` read only
   inside the callback itself and the callback address referenced only at the registration site, so the invocation is a
   parity and lifecycle step with no measured title-visible effect. The page records that rather than claiming one.
6. **Slot 73 `Wait32BitValueX` is unmeasured here** (0 calls in both arms) and stays the peer's inference, not ours.
7. **Slot 70's forward reaches a generated stub.** The layer's `ID3D12GraphicsCommandList1::OMSetDepthBounds` is not
   modelled: the command list's IR carries no depth-bounds command, so the call is answered with the layer's
   "unimplemented" stub and 12,706 trace entries in arm B say so (per call; the layer's own bounded diagnostic prints
   once). The forward is still a named real call, not a silent shim answer. **Next falsifier:** a frame that needs
   depth-bounds state (a depth-bounds test in a pipeline record with a changed bounds pair) would show whether the
   title's per-frame pair is load-bearing.
8. **One machine, one title build.** Each arm is one 130 s boot; the counts move with the boot's frame rate (the
   per-frame ratios are the stable figures). The arms' binaries are named in section 5: arm A's shim predates the
   final two-line flag-mask fix, whose comparisons live only on arm B's model path, and both arms carry the tree's
   other slices' uncommitted files, as the tree's other pages note.

## 7. Tests

`port/test/xs_proxy_mock_test.cpp` (GPU-free, registered with ctest as `xs_proxy_mock` and its environment variants)
extends its console command-list check with the slice's assertions:

- with the model off, the float-shaped slot-70 entry answers 0 and forwards nothing; with it on, the same floats reach
  the real list's PC slot 62 (`0.25`/`0.75` recorded by the mock) and the counters move;
- slot 71 writes the measured dword into the destination and refuses a flags shape outside the measured one;
- slot 72 writes all 64 bits, the flags-1 form writes the console clock instead of the argument value, and a 4-byte
  aligned address is refused;
- slot 74 counts met, unmet, unreadable and shape-refused calls;
- a new `check_allocator_callback_invocation` check: the synthetic callback is invoked once per allocator proxy on its
  first registration with `(class 0x18, size 0x10000, context)`, later registrations only replace the pair, a null
  return is refused by name, an unreadable context is refused before title code is called, and the off arm invokes
  nothing.

The test target builds `TESTS_BUILT` and `xs_proxy_mock_test.exe` prints its PASS line with the new checks.

## 8. Attribution

This slice was implemented from this project's own measurements: the call-site disassembly, the probe boot's call table
and the A/B boot pair. The mechanism was cross-checked against the public GPL-3.0-or-later project Five Star Recomp
(<https://github.com/ThatRecompDev/five-star-recomp>, clone `0bfa755`: `docs/d3d12x-shim.md`, its command-list
bottom-of-pipe and memory-wait shims and tests), the GPL-2.0-only project XCL (<https://github.com/xcl-dev/xcl>, clone
`6d12f58`: `crates/xcl/src/hle/d3d12x/list_x.rs`, `cpu_writes.rs`, `HANDOFF.md`), the GPL-3.0-or-later projects VIctory
(<https://github.com/Marvisak/VIctory>, clone `13f91ca`: `crates/victory-gpu/src/list.rs`, `hold.rs`) and PercyScarlett
(<https://github.com/rpatty-dev/PercyScarlett>, clone `e44c465`: `crates/d3d12_xs/src/cpu_writes.rs`). No code from
those projects was copied; the shapes were re-derived from our own image and boots, and the one place our measurement
disagrees with the ledger (slot 70) is recorded in section 2.2. The upstream projects remain the copyright of their
contributors under their own licences; the lead carries the `NOTICE.md` row at commit time, which this slice does not
edit. No git writes were made by this slice.
