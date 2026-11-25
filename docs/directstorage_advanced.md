# Advanced DirectStorage: pinned destinations, bounded staging, page lists, BCPack and unshuffle

**Status: implemented behind opt-in switches, measured negative on the boot gate.** This slice implements the parts of
the console DirectStorage request that the zlib-only shim refused by name: the bounded staging area for the transform
stages, the pinned destination write, the 64 KiB physical page list, the macro unshuffle of the swizzle modes, and the
locally calibrated BCPack predictor file. Everything is off by default; the measured boot asks for none of these
shapes today, so the A/B boot pair below reports **unchanged**, and the doc keeps the mechanisms that are ready apart
from the ones that are still refused by a named limitation. The porting policy is [upstream_port.md](upstream_port.md).

## 1. Why these calls are needed

`port/shims/dstorage.cpp` already serves the title's zlib requests, but the console ABI it implements
(`port/shims/dstorage_xs_abi.h`) carries more than zlib: `DSTORAGE_REQUEST_OPTIONS` has a `BcpackMode`, a
`SwizzleMode` and the two physical-page flags, and `DSTORAGE_REQUEST` has the page arrays and their page offsets plus
`IntermediateSize`. The shim refuses all of those whole, with one `E_NOTIMPL` line, before reading anything. So the
request half of the console contract that this title's package formats are built around (BCPack-compressed texture
data, [textures.md](textures.md)) has no runtime path at all, and a future request shape would fail rather than be
mis-decoded - or, worse, would need a guess at the point of failure.

Two measurements say which parts are worth building first and which are acceptance tests for later:

- The measured 130 s boot streams 256 MiB as 4,096 **zlib file-to-memory** requests (source ~17.8-24 KiB,
  destination 64 KiB), all through one EnqueueRequest caller, and never names a BCPack, swizzle or physical-page
  request ([wait_state.md](wait_state.md) section 3, [streamed_consumer.md](streamed_consumer.md) section 2). So a
  BCPack or page-list stage is not on the loading gate's critical path today; it is a correctness gap that a future
  request shape would hit, not a current blocker.
- Those 4,096 destinations are ordinary process-heap pages that a **CPU consumer reads** (the title's deserializer at
  ~2.3 s, [streamed_consumer.md](streamed_consumer.md) section 3). That makes the consistency of the destination write
  a measurable property of our layer, not only of the console's: the same pages are the ones the stream-consumer
  diagnostics put a first-touch watch on.

The reference port's story-load integration ([upstream_port.md](upstream_port.md)) carries the same four mechanisms
for the same reason; section 4 records what we took from it as evidence and where our answer differs.

## 2. Measured shapes

| Measure | Value | Evidence |
|---|---|---|
| Requests per 130 s boot | 4,096, all `file -> memory`, `zlib=1`, `bcpack=0`, `swizzle=0` | `PALMETTO_DSTORAGE_TRACE`, [wait_state.md](wait_state.md) section 3 |
| Request sizes | source ~17.8-24 KiB compressed, destination 64 KiB decompressed | same boot |
| Enqueue caller | `game+0x28C9A0E` for every request | same boot |
| Submits / status | seven submits (43, 7, 2, 475, 3,677, 2 entries and a later 2); every status entry `S_OK` | same boot |
| Fences / events | 0 `EnqueueSignal`, 0 `EnqueueSetEvent`; the title polls its status arrays | same boot |
| Throughput | 256.0 MiB in 1.7-2.0 s (121-157 MiB/s across repeats) | [streamed_consumer.md](streamed_consumer.md) section 2 |
| Destination shape | 4,096 distinct process-heap 64 KiB blocks; first families 1 MiB-strided (42 then 6, 1, 1, 2, 4, 14 blocks) | [streamed_consumer.md](streamed_consumer.md) section 2 |
| Destination pages our layer reads | the first sixteen blocks, read by the title's CPU deserializer at ~2.3 s; diagnostics watch the first 64 and sampled later blocks with `PAGE_GUARD` | [streamed_consumer.md](streamed_consumer.md) sections 1 and 3 |
| BCPack / swizzle / page-list requests | 0 measured; a request of either kind fails whole (`E_NOTIMPL`) before a read | [wait_state.md](wait_state.md) section 3, this slice's trace counters |
| Predictor file | not needed by any measured request; needed only by a BCPack decode, which the runtime does not have | [textures.md](textures.md) (BCPack calibration corpus) |

The trace counters added in this slice print the same shape measurement on every `PALMETTO_DSTORAGE_TRACE` boot
(`[rt] dstorage trace: shape:` for the first six requests and a periodic `advanced:` counter line), so the negative
above is re-checkable without new instrumentation: if a future boot shows `bcpack`, `swizzle` or `dest-pages` non-zero,
that is the falsifier that promotes the next stage.

## 3. What we implemented

Two files carry the slice: `port/shims/dstorage_advanced.h` (header-only mechanisms, compiled into `palmetto` and the
GPU-free test) and the wiring in `port/shims/dstorage.cpp`. No new source file needs a build registration beyond the
test file.

| Switch | Default | Mechanism |
|---|---|---|
| `PALMETTO_DS_STAGING=<bytes>` (e.g. `1M`) | off | One reusable staging buffer for the compressed source and one for the inflated output, per queue worker, with a hard cap. A request above the cap falls back to an owned allocation and is counted; the peak, hits, fallbacks and bytes are in the periodic trace line. |
| `PALMETTO_DS_PIN=1` | off | A destination is **pinned** for the duration of its write: a `PAGE_GUARD` our stream-consumer diagnostics put on the page is released before the worker writes and re-armed after it, so the block is written in one go and the title's next access is still the first touch instead of a guard fault taken by our own store. Unguarded ranges pin nothing; reserved memory is refused. |
| `PALMETTO_DS_PIN_RESIDENT=1` | off | With pinning, `VirtualLock`/`VirtualUnlock` the destination range for the write, so a CPU consumer polling the block after the status completes cannot take a page fault on it while the write is in flight. The lock failures are counted, never fatal. |
| `PALMETTO_DS_PAGES=1` | off | A physical-page request reads the page numbers the transfer needs (bounded), plans the 64 KiB spans, then refuses by name: the plan and copy math are implemented and tested, but the runtime has no page resolver (xmem keeps its page table private, `port/shims/xmem.h` exposes only commit). |
| `PALMETTO_DS_UNSHUFFLE=1` | off | A swizzle request runs the macro-unshuffle stage (modes 1-4), with the optional zlib stage before it; the destination is written only after the whole stage succeeded, so a failed request leaves no partial unswizzled data. |
| `PALMETTO_DS_BCPACK=1` | off | A BCPack request loads and validates the local predictor file, then refuses with the named missing stage: this slice has no BCPack rANS/block decoder, so it will not guess one. BC2 is refused by its own name (no known block decoder). |
| `PALMETTO_DS_BCPACK_PREDICTOR=<path>` | unset | The local predictor file (outside the repository), named by policy below. |

**Default path.** Every switch reads once, on the first use; with none set the boot adds no line, no allocation and no
call beyond one cached boolean test per request in the zlib path. The request-shape counters are trace-only, so a boot
without `PALMETTO_DSTORAGE_TRACE` does not count them at all.

**The predictor file policy.** The BCPack initial endpoint/index history tables are recovered from the player's own
package by `tooling/bcpack_history_calib.py`; they are game-derived and must live **outside the repository**. The
calibration tool's `--out <dir outside the repository>` writes `<codec>.json` files (`bc6h.json`, `bc7.json`), and
`PALMETTO_DS_BCPACK_PREDICTOR` names that directory or a combined `.json`. The shim's loader is a bounded JSON reader
(8 MiB file cap, depth cap, 200,000 integers, 256-row tables) that validates every range the decoder relies on - BC6H
endpoint 16-bit components and per-channel confirmed-bit depths (16/10/6/6), index values 0..15, BC7 endpoint bytes
0..255 and the 24/32-entry bit-depth tables, the last-resort list - and reports rows/bytes. It keeps **no values**:
the block decoder that would consume them is the named gap. A missing, unreadable, oversized, malformed,
no-codec-section, out-of-range or bad-length file is a distinct refusal name, never a crash.

**Named limitations the code returns** (`E_NOTIMPL` with a one-time line naming the reason, one line per kind):

- BCPack block decode: no rANS/block decoder in the runtime; the predictor's state is logged first (missing, invalid,
  or loaded with its row counts). The offline decoder is `tooling/bcpack.py`.
- Physical-page list: the plan is computed and counted, but no page resolver exists (xmem's page table is private).
- BCPack mode 2 (BC2): no known BCPack block decoder, in the reference either.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 fork `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida>). The files read as evidence were `runtime/shims/dstorage.cpp` (request
validation, staging leases, `title_write`/`kernel_writes`, `page_transfer`, `bcpack_history()`),
`runtime/shims/dstorage_staging.h` (the 64 MiB shared staging pool and its paired-lease path),
`runtime/src/dstorage_swizzle.h` (the stripe table), `runtime/src/bcpack.{h,cpp}` and `runtime/src/bcpack_stream.h`
(the decoder and its rANS stream), and `tools/build_dstorage_history.py` / `tools/gen_bcpack_runtime_tables.py` (the
predictor sidecar); the relevant commits are `273b3bdf9` and `ba626820e` (Moriz82), `779f90108` (Tyrannys) and
`2d7be48de` (Mumogg), plus the story-load integration section that names all four mechanisms. No code was copied: the
header is our own structure, names, bounds and counting, and the upstream source was used to check semantics, not to
transcribe.

**Where it agrees.** The stage order is zlib, then BCPack, then the macro unshuffle; a compressed request's destination
must be larger than its source and a BCPack destination must be whole 8-byte (BC1/BC4) or 16-byte blocks; the
intermediate size is valid only with BCPack. A physical-page buffer is a list of 64 KiB page numbers plus a byte offset
into the first page, and a destination above the linear-buffer limit needs it. The predictor is a local, optional
sidecar outside the tree named by an environment variable; absent or invalid files keep the built-in codec policy
instead of failing the boot. The BC2 refusal is named. A staging area has a fixed cap, and a request over it falls
back rather than growing the area.

**Where it differs, and why.** (1) **Page lists**: the reference resolves page numbers through `xmem` and transfers
into them; our `port/shims/xmem.h` exposes only `commit_physical_range`/`commit_physical_pages`, not a read/write
resolver, so we implement and test the plan and copy math and refuse a live page-list request by name rather than
reach into another owner's file. The next increment is one resolver export in `port/shims/xmem.cpp`. (2) **BCPack**:
the reference carries a full C++ rANS/BCn decoder generated from its Python reference; we do not have a runtime block
decoder in this slice and refuse instead of porting a second one blind. The predictor loader is the part that can be
verified without vectors, and it is. (3) **Pinning**: the console path pins write-watched pages through kernel writes
and `xmem` committed ranges; the PC port has neither, so our pin is the one write-watch this tree actually has (the
stream consumer's `PAGE_GUARD` diagnostics) plus optional `VirtualLock` residency. (4) **Staging shape**: the
reference pools staging across queues and pairs two adjacent requests so a helper thread prepares the next one; our
queues have one worker and no helper, so ours is per-queue reusable scratch with the same cap-and-fallback rule, not
the cross-queue lease. (5) **Predictor format**: the reference reads a fixed-size binary calibration sidecar
(`BCPHIST1`); ours reads the JSON that `tooling/bcpack_history_calib.py` already writes, so the pipeline that
generates it is in this repository. (6) **Unshuffle provenance**: the stripe table is the feature-mode layout the
public Xbox-GDK-Samples `DirectStorageTextureShuffling` example documents; the NDA "DirectStorage Shuffle Formats" page
is not public, so this is re-derived from that reference table and cross-checked against the reference implementation
over synthetic buffers (20/20 cases equal: every mode, one to seven groups, and the partial-group refusals), not
measured from our own package.

## 5. A/B measurement

One boot pair, one at a time under the repository's boot lock, same executable (built from the shared working tree at
the time, so other slices in flight are in both arms; the differential below is only this slice's switches), 130 s
timeout, harness `tooling/boot_report.py`. Both arms ran the standard environment
(`PALMETTO_SHADERS=<shader corpus>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_PIPELINE_TIMELINE=5` and
`PALMETTO_STREAM_CONSUMER=1` - the stream consumer is in **both** arms, because its write-watch is the thing the pin
hands back, and a pair that differed in it would not be a controlled comparison. Arm B adds `PALMETTO_DS_STAGING=1M`,
`PALMETTO_DS_PIN=1`, `PALMETTO_DS_PIN_RESIDENT=1`, `PALMETTO_DS_PAGES=1`, `PALMETTO_DS_UNSHUFFLE=1`,
`PALMETTO_DS_BCPACK=1` and `PALMETTO_DS_BCPACK_PREDICTOR=<a synthetic 256-row predictor directory outside the
repository>` (the synthetic file exists to exercise the env-path plumbing end to end; the real file is generated from
the player's package by `tooling/bcpack_history_calib.py` and is never in the tree).

| Measure | A: default (no `PALMETTO_DS_*`) | B: advanced switches on |
|---|---|---|
| Verdict | ALIVE 130.1 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| Traced D3D12 calls | 529,649 (max #531,232) | 545,940 (max #547,498) |
| Requests and shape | 4,199 requests: file->buffer, `zlib=4199`, `bcpack=0`, `swizzle=0`, dest-pages=0, source-pages=0 | identical: 4,199, same shape counters |
| Decompressed | 256.0 MiB in 1.8 s (140.0 MiB/s) | 256.0 MiB in 1.9 s (137.7 MiB/s) |
| DirectStorage entry failures | 0 | 0 |
| Staging | hits 0, fallbacks 0, bytes 0 | hits 4,096, fallbacks 0, 133,990,447 source bytes served; the inflated buffer reuses one vector, no fallback |
| Pinned destinations | 0 | pins 4,096, pages 65,536 (all 4,096 destinations resident-locked), lock failures 0, refusals 0, **write-watched 0** |
| Predictor | `off` | `ok`: loaded BC6H 256/256 endpoints + 256/256 indices, BC7 256/256 + 256/256 from the env path |
| Unshuffle / BCPack / page-list refusals | 0 | 0 (no request of those kinds was issued) |
| Stream-consumer guards | 108 tracked, 0 first-touch faults, 108 still guarded | identical |
| Population at +125 s | slot57 10,241 converted 6,537 refused 3,704; graphics created 29,328; submits 25,710; presents 12,854 | slot57 10,241 converted 6,538 refused 3,703; graphics created 29,329; submits 26,551; presents 13,275 |
| Replay checkpoint | submission 24,576 at 104.7 s | submission 24,576 at 102.1 s |
| Loader / capture | loading loop unchanged (no GPU consume call, no capture switch on) | unchanged |

**Reading it honestly.** The counters that must move if the mechanisms run did move: staging served exactly the 4,096
compressed sources with no fallback, the predictor file loaded from the environment path with its 256-row tables
validated, and the pin accounted all 4,096 destinations. The **shape counters are identical** in both arms: this boot
issues no BCPack, swizzle or physical-page request, so the stages that would decode or transfer anything stayed on
their refusal paths (and were never reached). No `E_NOTIMPL` or request-failure line was added in either arm, and the
trace values are the same. The **write-watch half of the pin had no work in this boot**: the stream consumer armed its
guards on 108 tracked blocks and none of them was written again (`write-watched 0`, and `0 first-touch faults` in both
arms), so only the residency half of `PALMETTO_DS_PIN` was exercised. That is the measured shape, not a hidden failure.

The frame loop's counters differ by ~3% (presents 12,854 vs 13,275, D3D12 calls +3.1%) and the replay checkpoint sits
2.6 s earlier in B; the decompression rate is 1.6% lower in B (well inside the 133-153 MiB/s interval spread of both
runs, and B performs two extra work-set calls per request). The slice does not touch the frame loop, and the replay
timestamp moves in the same direction as the present count, so this is boot-to-boot scheduling variance rather than a
measured effect: one pair cannot resolve a 3% frame-rate difference, and the honest verdict is **unchanged** - the
loading gate does not advance in either arm and no regression is provable from this pair. The mechanisms are ready,
switch-gated and counted; the promotion falsifier is a future trace with `bcpack`, `swizzle` or `dest-pages` non-zero.

## 6. Limits

- **BCPack block decode is the largest gap.** The predictor file is loaded, validated and counted, but no request can
  be decoded because the rANS entropy layer and the six BCn rebuilders are not in the runtime. A BCPack request is
  `E_NOTIMPL` with that reason named. The offline path (`tooling/bcpack.py`) decodes the package's BCPack textures
  today, so the format is known; the runtime decoder is a later slice.
- **A live physical-page request cannot transfer.** The plan/copy math and its tests are in place, but resolving a
  64 KiB page number to a CPU address needs an xmem export that does not exist yet.
- **The unshuffle layout is reference-derived, not package-measured.** The NDA shuffle-format page is not public; the
  cross-check is an equivalence check against the reference implementation, not a decode of our own data.
- **Pinning is only observable with the stream-consumer diagnostics on.** Without `PALMETTO_STREAM_CONSUMER` there is
  no `PAGE_GUARD` to hand back, so only the residency part of the switch does anything.
- **Staging is per queue.** No cross-queue pool, no paired preparation: our queue has a single worker and nothing to
  pair with.
- **No boot advance is expected.** The measured boot issues zero advanced request shapes; the A/B pair above can only
  show that the mechanisms are inert on the default boot and that the switch-on boot moves the same counters. The
  falsifier for promoting any stage to default is a trace with `bcpack`, `swizzle` or `dest-pages` non-zero.

## 7. Tests

`port/test/dstorage_test.cpp`, target `dstorage_test` (registered in `port/CMakeLists.txt` in the host-test block
and built by `_setup/build_tests.cmd`; no GPU, no boot, no game data): **91 checks, 0 failed**. Sections: the staging
area (reuse, cap, fallback accounting, peak, the size parser), the pinner (a synthetic `PAGE_GUARD` released for the
write and re-armed after it, reserved memory refused, null/empty ranges, residency accounted), the page plan (offset,
boundary, short list, unmapped entry, too many spans) and the copy math both ways through a resolver, the unshuffle
(all four modes are permutations with the table's stripe positions, plus the refusals), and the predictor loader
(missing, unreadable directory, malformed JSON, no codec section, empty section, out-of-range value and class,
bad-length rows, over-size file, sparse index-keyed table, a directory of two codec files, a combined file). The test
runs under a per-process temp directory (`port/test/test_temp_dir.h`) and removes it.

## 8. Attribution

This slice was implemented from this project's own measurements (the DirectStorage trace in
[wait_state.md](wait_state.md) and [streamed_consumer.md](streamed_consumer.md), and the ABI in
`port/shims/dstorage_xs_abi.h`). The behaviour was cross-checked against the public GPL-3.0 project
`ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files
`runtime/shims/dstorage.cpp`, `runtime/shims/dstorage_staging.h`, `runtime/src/dstorage_swizzle.h`,
`runtime/src/bcpack.h`, `runtime/src/bcpack.cpp`, `runtime/src/bcpack_stream.h`, `tools/build_dstorage_history.py` and
`tools/gen_bcpack_runtime_tables.py`, commits `273b3bdf9`, `ba626820e` (Moriz82), `779f90108` (Tyrannys) and
`2d7be48de` (Mumogg). No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree. The `NOTICE.md` credit for this slice is added by the
policy slice that owns that file ([upstream_port.md](upstream_port.md) section 6).
