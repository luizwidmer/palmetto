# The constant pass's colour source: a title-owned 96-byte block with a zero colour window, and no writer in the stationary loop

**Status: measured negative.** The constant pass's whole data input is four dwords at `block + 0x20` of a 96-byte
buffer the draw's descriptor table names at slot 0. They are **zero** at the first frame and at two later frames, the
block's 96 bytes are identical at all three (a 12-byte tail the module never reads carries a per-boot value, so the
title writes the block), and the exact-content writer watch saw **no write** to any of the three sampled positions
across two 150 s boots. The pass's pixel shader has no image samples and no other data read, so zero inputs explain the
measured output: `0/8,294,400` non-black in both 4K targets, alpha zero, all eight presenter captures black. The frame
is **black by design while the title is in its stationary pre-content loop**; this is not a missing driver block, and no
runtime fix is made here. The next frontier is the title's state step ([stage2_blocker.md](stage2_blocker.md)) and the
first content pass ([frame_capture.md](frame_capture.md) section 10).

No game data, package path, shader byte or extracted table appears here: dword values, offsets, ids, counts and
readback statistics only.

## 1. Why this measurement

[slot70_publication.md](slot70_publication.md) section 5.1 left the constant pass's colour source unmeasured: the pass's
pixel shader `fspx/lib_ps_00820` has no image sampling, the data it reads is its own descriptor table (a 96-byte V# at
slot 0 plus raw float dwords at `+0x40`/`+0x60`), and no diagnostic had read that block's content on the shipping path.
"Black by design" and "empty because nobody wrote it" were indistinguishable, and the two candidate conclusions are
different slices: the title's state step, or a layer that must model driver-provided constants.

This page adds the two measurements that separate them: a bounded opt-in reader for the block (full 96 bytes, the
block's title-VA mapping, and the table's raw data slots at `+0x20`/`+0x40`/`+0x60`, at the boot's first draw and at
every readback-armed draw), and a byte-exact writer watch on the block's title pages across the boot - the same
machinery [texture_source.md](texture_source.md) uses for sampled placed textures, extended to compare a small span
byte for byte.

## 2. What ran

| | |
|---|---|
| Tree | `D:\leonida` at HEAD `f714f9b51` plus this slice's uncommitted files |
| Binary | `port/build/palmetto.exe`, built 17:10:52, SHA-256 `7338EF7D8197BC97C37BFF01403950982E1EBE25870F749872B3DA89A097A965` |
| Boots | two 150 s boots under the shared boot lock, one at a time: `boot-colour1` (an earlier build of the same code) and `boot-colour2` (the binary above); both ALIVE, 0 exceptions, no device loss |
| Environment | the standard set (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_COLOUR_PATH=1`, `PALMETTO_READBACK_AT_DRAW=100,101,4000,4001`, `PALMETTO_READBACK_FULL=1`, `PALMETTO_PIPELINE_TIMELINE=10`, and `PALMETTO_CAPTURE` (every 300 presented frames, limit 8) |
| Counters at +140 s | 8,233 submissions / 4,116 presents; 220,190 D3D12 calls (max #222,862); slot 57 10,241 converted / 0 refused; graphics pipelines 51,786; compute 481 calls / 407 created |
| Evidence | `D:\leonida-work\colour\boot-colour1`, `boot-colour2` and `capture-colour2` outside the repository; the capture statistics are reproduced by `D:\leonida-work\colour\analyze_captures.py` |

The two boots differ only in the binary's build (the first is the same code before a lock-scope cleanup of the new
shim hook); both measure the same frame and the same block. Counter differences between them (8,823 submissions at
+140 s in the first) are the boot-to-boot spread [frame_capture.md](frame_capture.md) section 7 records, not a property
of this diagnostic.

## 3. Measured shapes

### 3.1 The table and the block

At every sampled draw the constant pass's single descriptor table holds 64 slots, **0 T# keys** and eight non-T#
slots. Slot 0 is a V# the title wrote; its base resolves through the layer to a title VA inside the slot-133 placed
buffer ([page_content.md](page_content.md) section 1 measures the same range), so the block is title memory and not a
layer or driver allocation:

```text
colour path: draw 0 table VA 0x2901675ef00: 64 slot(s), 0 T# key(s), 8 data/V# slot(s)
colour path: slot 0 (table + 0x00) words: 0006ff20 00000000 00000060 30014fac
colour path: slot 0 V# address 0x6ff20 stride 0 records 96 format 20, declared 96 byte(s)
colour path: block base 0x6ff20 is the arena stand-in of title VA 0x2901675ff20 (+0xada00e0 mapped)
```

The V# is a raw (stride 0) view of **96 bytes** with format 20 (`GFX10_FORMAT_32_UINT`), i.e. the block is 24 dwords.
The table is not a single per-frame object: the title rotates the descriptor table and the block through a ring, and
the three sampled draws (0, 100 and 4000) land on three positions:

| sampled draw | table VA | slot-0 V# (`w0 w1 w2 w3`) | block base | block title VA |
|---|---|---|---|---|
| 0 | `0x2901675ef00` | `0006ff20 00000000 00000060 30014fac` | `0x6ff20` | `0x2901675ff20` |
| 100 | `0x29016d9ef00` | `006aff20 00000000 00000060 30014fac` | `0x6aff20` | `0x29016d9ff20` |
| 4000 | `0x29016e5ef00` | `0076ff20 00000000 00000060 30014fac` | `0x76ff20` | `0x29016e5ff20` |

### 3.2 The block's content over frames

The same 96 bytes at all three positions, on both boots: dwords 0..19 are zero, then a 12-byte tail the module never
reads (dwords 20..22 = `ffffffff`, `0000ffff`, and a per-boot dword), and the last dword zero:

```text
colour path: block read 96 of 96 byte(s): 3 of 24 dword(s) non-zero
colour path: block dwords [ 0.. 7]: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
colour path: block dwords [ 8..15]: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
colour path: block dwords [16..23]: 00000000 00000000 00000000 00000000 ffffffff 0000ffff 2f992200 00000000
colour path: module constants (block + 0x20, dwords 8..11): 00000000 00000000 00000000 00000000 (floats 0 0 0 0)
```

The third tail dword is **runtime data, and per boot**: `0x2f992200` in boot-colour2, `0xd6592200` in boot-colour1
(same code), and `0x42992200`/`0xb2392200` in the earlier console-feed boots of
[frame_capture.md](frame_capture.md) sections 6-7 that read the same block type. Nothing in the layer writes title
memory - the page pull copies title pages into the arena, never back - so the tail is title-side data, and the title
therefore *does* write this block. The 4 dwords the module reads (`block + 0x20`, dwords 8..11) are zero in every
sample; the 12 non-zero tail bytes sit at `block + 0x50`, outside every address the module forms.

### 3.3 Why those four dwords are the whole colour input

A structural trace of the translated module (`<shader dir>/pipeline_graphics/lib_ps_00820.spv`, instruction indices and
addresses only, no shader bytes) finds exactly two physical-storage addresses in it:

1. `heap_base + (member0[9] << 32 | member0[8] & ~3) + 0`: the table, whose first four dwords are read as a V# (the
   console `s8`/`s9` pair the module reads, [vertex_constant_feed.md](vertex_constant_feed.md) section 2.2);
2. `heap_base +` that V#'s base `+ 0x20`: four dword loads at `block + 0x20/+0x24/+0x28/+0x2c`, each gated on the V#'s
   declared size being at least 36/40/44/48 bytes - 96 bytes declares all four.

Every value in the output colour chain is one of those four loaded dwords (bitcast to float, subnormal-flushed) or a
literal; the two words the final `OpStore` packs come from that chain, and the module contains no image instruction and
no `OpKill` for the measured state. With all four dwords zero the arithmetic yields zero, which is exactly the measured
output. The table slots at `+0x40`/`+0x60` are printed by the same diagnostic but the module **does not read them**:

| table slot | words | floats |
|---|---|---|
| `+0x20` (slot 1) | `00000000 00000000 00000000 00000000` | 0 0 0 0 |
| `+0x40` (slot 2) | `00000000 00000000 bf800000 bf800000` | 0 0 -1 -1 |
| `+0x60` (slot 3) | `3f800000 bf800000 00000000 ffffffff` | 1 -1 0 -nan |

The odd draw of the pair (the compose pass) prints its own table for comparison: slot 0 is empty, slot 1 is its one
sampled T#, slot 2 is the title's 480-byte constants V# (`... 000001e0 30014fac` - matrix B,
[vertex_constant_feed.md](vertex_constant_feed.md) section 2.3), slot 3 is empty.

Two distinct gaps exist, and this measurement separates them:

* **the data gap measured here**: the block's four colour dwords are zero, so even a correctly fed pixel stage computes
  black;
* **the feed gap already recorded** ([frame_capture.md](frame_capture.md), the graphics pack is vertex-stage only): on
  the shipping path the pixel stage's `s8`/`s9` pair arrives unfed, so the module would form `heap_base + 0` as its table
  address. That path could not produce the title's colour either. This page reads the table from the draw's recorded
  root arguments, so it measures the data the console pixel stage would find, independent of that feed gap.

### 3.4 The measured output

Full-4K readbacks of both targets at both arms, boot-colour2:

| armed draw | pass | target readback |
|---|---|---|
| 100 | constant (0 image samples) | `mean 0 0 0, non-black 0/8,294,400, peak 0, alpha 0/8,294,400` |
| 101 | compose (6 image samples) | same, `0/8,294,400`, alpha zero |
| 4000 | constant | same, `0/8,294,400`, alpha zero |
| 4001 | compose | same, `0/8,294,400`, alpha zero |

The 8x8 tile maps are all-zero on all four. The write control is skipped for both targets (each is an armed sampled
input), so the independent control is the presenter capture path: 8 of 8 captures (frames 1..2,101, 2564x1421) read
`mean 0.00 0.00 0.00, non-black 0/3,643,444`. The black target is the pixel shader's arithmetic on zero inputs, not a
raster, descriptor or capture gap ([raster_state.md](raster_state.md) measured the raster path; this page measures the
data it consumes).

## 4. The writer watch

`PALMETTO_COLOUR_PATH=1` registers the block each sampled table's slot 0 names with the shim's page watch, and the watch
now samples a small span's content **byte for byte** every 64th submission (the texture watch only counts non-zero
bytes). Across boot-colour2 it registered three spans and printed, for each, a first sample and its dwords:

| title VA watched | first sample | content |
|---|---|---|
| `0x2901675ff20 + 0x60` | sample 1 (submission ~1) | 96 B, 9 non-zero, dwords as section 3.2 |
| `0x29016d9ff20 + 0x60` | sample 3 (submission ~129) | identical |
| `0x29016e5ff20 + 0x60` | sample 64 (submission ~4033) | identical |

There were **no `CONTENT CHANGED` lines** in either boot: after its first sample each watched block kept every one of
its 96 bytes for the rest of the run. So within the measured window no per-frame or later write fills the colour
window; the only write that produced the tail happened before the first sample, and the block's content at the first
frame already carries it (the first sample is taken at submission 2, after the title's resource setup and before the
measured draws at 100/4000 exist).

## 5. The verdict: black by design in the stationary loop, not missing driver data

1. **The block is title memory and title data.** The V# is in the title's own descriptor table and its base resolves to
   a title VA inside the title's placed buffer; the layer never writes title memory. The 12-byte tail carries a
   per-boot value, so the block is written at runtime by the title, not loaded from the package.
2. **The colour window the module reads is zero, and stays zero.** Dwords 8..11 are zero at the boot's first frame and
   at two later ring positions; the byte-exact watch sees no write to any watched block afterwards.
3. **That fully explains the measured black.** The module's only data loads are those four dwords; zero inputs give the
   zero RGB and zero alpha the readbacks and captures show. The constant pass is a black cover/background pass in the
   title's pre-content loop: the frame is black by design for this loop, not because the layer failed to provide data.
4. **"The console driver was expected to fill it" is not supported by any measured mechanism.** The driver-authored
   data this project has measured is driver *memory*, not title buffers: the vertex-stream driver block is read through
   the `s0`/`s1` SGPR pair ([vertex_constant_feed.md](vertex_constant_feed.md) section 3), and slot 70 publishes a
   *descriptor* (a V#) into the title's table, not the contents of the block it names
   ([slot70_publication.md](slot70_publication.md) section 3). A driver fill of this block would have to write the
   title's own placed pages, and no measured console path does. The one driver-defined dword `root_arguments.md`
   section 9 left open is answered by the trace above: the module uses `s9` only as the high half of the table address,
   which is zero for a heap-relative table VA, not as a colour scalar.
5. **No runtime change.** Neither the data path nor a driver block is missing in the measured sense, so nothing is
   filled in, synthesised or defaulted on. The diagnostic switch stays off by default and the default path executes no
   new read.

What would change the verdict: a boot in which the title leaves the pre-content loop and a content pass runs while these
dwords stay zero (then the pixel-stage feed - the graphics pack gap - is the remaining suspect), or a console trace
showing the driver writing the block's title VA (then a measured driver fill becomes a real slice, with that trace as
the semantic). Neither exists in the tree today.

## 6. Limits and the next falsifiers

1. **Two 150 s boots of one title build on one machine.** The block ring rotates and the watch holds at most four
   spans, so three of its positions were sampled and watched; a write to an unwatched position is not excluded, though
   the three sampled positions held identical bytes.
2. **The watch cannot attribute the tail's writer.** It starts after the title's resource setup, so the write that put
   the 12-byte tail in place is inferred from ownership and the per-boot value, not observed. A `cdb` write watchpoint
   on the block's title VA from process start would name the writer's caller; it was not run here.
3. **The module's read chain is a structural trace.** The two address computations, the declared-size gates and the
   four loads are proven from the translated module; the exact arithmetic is not claimed beyond "zero inputs produce
   the measured zero output".
4. **The sampled draws are draws 0, 100 and 4000 of a stationary loop** (about the first frame, ~1.6 s and ~63 s of the
   150 s window); no post-load frame is claimed - the title never leaves the loop in the measured window.
5. **The dword values published are the measured 24-dword window of one constants block** and the table's data slots,
   under the project's id/count/dword convention ([frame_capture.md](frame_capture.md) limit 2); no shader bytes or
   package bytes are involved.
6. **No validation layer** was enabled in these arms (`PALMETTO_VK_VALIDATION` unset); 0 exceptions were recorded and
   no device-lost line appears, but a validation arm remains the honest form of that claim.
7. The next measurement is not another colour read: it is the title's state step
   ([stage2_blocker.md](stage2_blocker.md) sections 11.4-11.6: the loader job graph is queued but never started) or the
   first content pass ([frame_capture.md](frame_capture.md) section 10). When either appears, `PALMETTO_COLOUR_PATH=1`
   is the instrument: the four dwords at `block + 0x20` should become the first non-zero colour data, and the exact
   watch will say when and whether the title writes them.

## 7. Tests, gates, evidence and files

* **New tests** (`port/test/frame_dump_test.cpp`): the measured slot-0 V# shape (raw 96-byte, stride 0, records 96,
  format 20) and the declared-size rule (records when the stride is 0, stride x records otherwise); the measured
  `+0x40`/`+0x60` data slots decode as neither a V# nor a T#, so they cannot be mistaken for the block; and the
  byte-exact writer-watch comparison (equal content is not a change, a changed byte with the same non-zero count is).
  `frame_dump_test` passes (exit 0, "command kinds, ordered writers and table windows exact").
* **Other targets**: `xs_proxy_mock_test` PASS, `unimplemented_policy_test` 98 checks 0 failed,
  `graphics_record_test` 158, `graphics_pso_test` 272, `xbox_present_test` pass, `buffer_va_index_test` pass,
  `dstorage_test` 224, `xapu_chad_test` 65 - all exit 0.
* **Gates**: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`, `_setup\build_tests.cmd` -> `TESTS_BUILT`.
* **Documentation checks** (`tests/test_docs_*.py`): encoding, env vars, links, list items, repo paths, table ids,
  table rows, repeated lines, design rows and python deps pass; `test_docs_cli_flags.py` fails on an untracked
  `_setup/xcl/HANDOFF.md` (another slice's file, not this page).
* **Evidence**: `D:\leonida-work\colour\boot-colour1`, `boot-colour2` (each `stderr.log`, `boot.jsonl`, `summary.txt`),
  `capture-colour2\frame_*.ppm` (8 presenter captures, all black) and `analyze_captures.py`, all outside the repository.
* **Files**: `port/src/vulkan_replay.h` (the bounded reader, the declared-size/float/comparison helpers, the call at
  the boot's first draws and the readback-armed draws), `port/shims/d3d12_vulkan_device.cpp` (the V# base resolution and
  the exact-content writer watch), `port/test/frame_dump_test.cpp`, and this page. No git writes: the lead commits.
* **Attribution**: this slice is this project's own measurement; no upstream reference is used on this page, so there
  is no file or commit to credit. The policy of [upstream_port.md](upstream_port.md) is followed in the form "measure
  first, implement our own, state the limits".
