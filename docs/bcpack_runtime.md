# Runtime BCPack: the block decoder behind the DirectStorage advanced slice

**Status: implemented behind the opt-in switch, measured negative on the boot gate.** This slice completes the
DirectStorage advanced work with the piece that was missing there: the runtime BCPack block decoder (the interleaved
rANS entropy layer plus the BC1/BC3/BC4/BC5/BC6H/BC7 block rebuilders), consuming the local predictor file instead of
only validating it. The measured 130 s boot still issues **zero BCPack requests**, so the A/B pair reports
**unchanged**: the zero is the result, and the slice's value is that a future BCPack request is decoded as the console
contract says instead of being refused. The porting policy is [upstream_port.md](upstream_port.md).

## 1. Why these calls are needed

[directstorage_advanced.md](directstorage_advanced.md) implemented the request half of the console DirectStorage stage
(bounded staging, pinned destination writes, the physical-page plan, the macro unshuffle and the locally calibrated
predictor file) and refused every BCPack request with one named limitation: there was no rANS/block decoder, so the
predictor's rows were loaded, validated and counted but never used. The title's package formats are built around
BCPack-compressed texture data ([textures.md](textures.md)): across the 57 measured pools BCPack is 11.6 GiB of the
57.7 GiB chunk payload (BC1 alone is 89.6% of it, BC7 10%, BC6H/BC3/BC4/BC5 the rest, BC2 does not occur). The
runtime therefore had a correctness gap, not a loading-gate blocker: the measured boot's 4,199 requests are all zlib
file-to-buffer requests ([wait_state.md](wait_state.md) section 3), and the zero-request A/B in section 5 re-measures
that. The gap is what this slice closes.

Two things bound the slice honestly:

- The measured boot never asks for a BCPack request, so nothing here is on the loading critical path today. The A/B
  pair cannot show a boot advance, and does not claim one.
- BCPack's initial endpoint/index history rows are unpublished codec constants recovered from the player's own
  package ([textures.md](textures.md), "Clean-room BCPack bitstream specification"); they stay in a local file outside
  the repository and are loaded through `PALMETTO_DS_BCPACK_PREDICTOR`. Nothing derived from them is compiled in or
  committed.

## 2. Measured shapes and the format contract

The contract below is re-derived from this project's clean-room specification ([textures.md](textures.md) section
"What BCPack is" and its bitstream subsection), cross-checked against the public GPL-3.0 reference port (section 4)
and re-validated by the differential vectors in section 7. The request shape is the console ABI already implemented in
`port/shims/dstorage_xs_abi.h`.

| Contract item | Value |
|---|---|
| Request options | `DSTORAGE_BCPACK_MODE` 1/3/4/5/6/7 = BC1/BC3/BC4/BC5/BC6H/BC7; 2 (BC2) has no known block decoder; 0 = none |
| Stage order | zlib inflate (if `zlib=1`), then BCPack decode, then the macro unshuffle (if a swizzle mode is set) |
| Sizes | a decompressing destination (`zlib` or BCPack) is larger than its source; a BCPack destination is whole 8-byte (BC1/BC4) or 16-byte blocks; `IntermediateSize` is present exactly when zlib feeds BCPack and is the BCPack-decoded input size |
| File source | `FileOffset` 16-byte aligned when a decompression stage runs |
| Destination write | the whole decoded stream is written once, after the stage succeeded |
| Entropy layer | 12 interleaved rANS lanes for BC1/BC3/BC4/BC5, 16 for BC6H/BC7; one synchronized element per cycle; 16-byte formats consume two elements per block (`decoded_bytes / 8` elements total) |
| Fragment header | `z:2 h3:1 flush:1 q:4` then a 1- or 3-byte payload length (`n = hdr1 + 1` or `hdr1 + 256*hdr2 + 257`) |
| Symbol width | `w = default_width[lane] - z`, `1 <= w <= 10`; `q` selects one of the 160 banded exponential probability models (total weight 65536, slot 0 reserved for the end marker) |
| rANS state | 32-bit, home state `L = 1<<24`; merge one byte while `state < L`, decode a symbol while `state > L`; flush fragments may omit up to 8 trailing zero bytes (virtual zeros) and must land exactly on `L`; a non-flush fragment hands its state to the next header |
| Scheduler | at most 8 lane bytes are served per cycle in lane order; a lane that requested but was not served freezes for that cycle; one element (one symbol per lane) is emitted when every active lane's output slot is full; all bytes after the last lane must be zero |
| BC1 | lane 0 = zig-zag backward distance; lane 1 = zero; lanes 2-7 = zig-zag endpoint deltas modulo 32/64/32/32/64/32; lanes 8-11 = 2x2 quadrant rank indices |
| BC3 | lane 0 = bit-interleaved distance; lane 1 = zero; lane 2 = alpha deltas, lanes 3-6 = alpha row-pair ranks, lanes 7-9 = colour deltas, lanes 10-11 = colour quadrant ranks |
| BC4/BC5 | lane 0 distance, lane 1 zero, endpoint deltas, 3-bit index-pair row ranks (BC5 has two 8-byte channel blocks per 16-byte block) |
| BC6H | 16 lanes: lanes 0/1 endpoint/index distances, lanes 2-10 endpoint fields, lanes 11-14 row index deltas, lane 15 mode symbol (0-13 maps to the 14 D3D11/Khronos modes) + partition (0-30) |
| BC7 | 16 lanes: lanes 0/1 distances, lanes 2-10 endpoint fields (rotation-aware), lanes 11-14 primary row indices, lanes 6/7/8/10 a second index set for modes 4/5, lane 15 a signature rank from the 286-entry table |
| History | `d = 0` uses the format's zero/midpoint state; `0 < d <= block index` predicts from a previous block in the same chunk; `d > block index` reads entry `256 + index - d` of a 256-entry built-in history table |
| Confidence | the local predictor records BC1-BC5 rows as confirmed (class 3) or fitted (class 1); BC6H/BC7 rows carry a class and per-field confirmed leading-bit depths. The runtime decoder is **strict**: only confirmed rows with enough confirmed bits are read |

The last row is the important policy one: a stream whose delta chains stay inside the chunk needs no predictor at all
(that is what the tests' synthetic chunks do); a stream that reads a built-in row needs the local file to confirm that
row, or the request is refused by name. Nothing is guessed.

## 3. What we implemented

Two files carry the slice: `port/shims/dstorage_advanced.h` (header-only decoder) and the wiring in
`port/shims/dstorage.cpp`. No new source file and no build registration change.

**Predictor retention.** The bounded JSON loader now also *retains* the validated rows in a process-local
`BcpackHistory` (BC1-BC5 confirmed rows as class 3 / fitted as class 1, BC6H endpoint and index rows with classes and
confirmed-bit depths, BC7 endpoint/index rows with their bit and confirmed-bit tables), and it reads `bc1`-`bc5`
sections in addition to `bc6h`/`bc7` (a directory scans `bc1.json`-`bc7.json`; a combined file carries the same keys;
a single-codec file is named by its `codec` string or by its table markers). The existing validation semantics, error
names and row counts are unchanged, and the old validate-only call is the same function with a null history. Every
walk is bounded; a bad row is still a named refusal (`malformed`, `out_of_range`, `bad_length`), never a crash.

**Decoder design.** `palmetto::dstorage::bcpack` is a streaming decoder:

- the lane scheduler is a direct state machine (one `LaneState` per lane, `p1` and `p0` kept separately), with a
  generous cycle bound so a corrupt stream fails instead of spinning;
- one element is emitted at a time, so a 32 MiB destination never needs a lane array: each rebuilder keeps a
  256-block ring of the reconstructed endpoint/index state (a distance is 8 bits, so only 255 blocks back are
  reachable) plus, for 16-byte formats, the first element of the current block;
- the rebuilders produce the standard BCn block bytes (BC6H/BC7 blocks are assembled bit by bit into two 64-bit
  words, because MSVC has no 128-bit integer) and validate the format's internal consistency (symbol width, BC7
  signature/position, anchor and p-bit rules, BC6H anchor index) instead of writing a guessed block;
- the codec constants (rANS probability models, BC1/BC3/BC4/BC5 rank tables, BPTC partition/anchor tables, the BC7
  signature permutation, the BC6H mode field plans) are codec-level format data generated from this repository's own
  `tooling/bcpack_tables*.py` modules; only the history rows are external;
- errors are named (`codec`, `destination_size`, `entropy`, `symbol`, `trailing`, `history_unknown`,
  `history_unconfirmed`, `history_depth`, `signature_unknown`, `signature_inferred`, `consistency`, `bounds`) and
  `DecodeStats` counts blocks, bytes and built-in history reads.

**Wiring.** `bcpack_request` in `port/shims/dstorage.cpp` now performs the reference's request validation (the size and
alignment rules in section 2), refuses a BCPack request by name when the predictor file is not usable, reads the
compressed source exactly as the zlib stage does (staging when `PALMETTO_DS_STAGING` is on), runs the optional zlib
stage to `IntermediateSize`, decodes into scratch, runs the optional macro unshuffle into a second scratch buffer, and
writes the destination once through the pinned-write path. A refused request therefore leaves no partial BCPack data
in title memory. The periodic trace line gains `bcpack refused= decoded= blocks= bytes= history=` counters and the
one-time predictor line reports every codec's row counts. The switch remains opt-in (`PALMETTO_DS_BCPACK`, default
off), so the default boot adds nothing.

| Switch | Default | Effect |
|---|---|---|
| `PALMETTO_DS_BCPACK=1` | off | enable the BCPack stage (predictor load plus block decode) |
| `PALMETTO_DS_BCPACK_PREDICTOR=<path>` | unset | the local predictor directory (`bc1.json`-`bc7.json`) or combined JSON, outside the repository |
| `PALMETTO_DS_UNSHUFFLE=1` | off | allows a request that sets both a BCPack mode and a swizzle mode to run the unshuffle stage after the decode |
| `PALMETTO_DS_STAGING=<bytes>` | off | the source/intermediate scratch reuse the decoder's stages already had |

## 4. Cross-check against the reference

The reference is the public GPL-3.0 fork `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida>). Files read as evidence: `runtime/src/bcpack.h`, `runtime/src/bcpack.cpp`
and `runtime/src/bcpack_stream.h` (the decoder and its lane stream), `runtime/shims/dstorage.cpp` (request validation,
`decode_stages`, the `bcpack_history()` profile loader), `tools/gen_bcpack_runtime_tables.py` (their C++ table emitter)
and `tools/build_dstorage_history.py` (their profile format); commits `273b3bdf9`, `ba626820e` (Moriz82),
`779f90108` (Tyrannys) and `2d7be48de` (Mumogg). The commit-level attribution is the same set this repository already
credits for the advanced slice ([directstorage_advanced.md](directstorage_advanced.md) section 4 and section 8).

**Where it agrees.** The stage order and the size/alignment validation; the 12/16-lane interleaved scheduler with its
8-byte cycle budget; the fragment header, virtual-zero flush and trailing-zero rules; the BC1/BC3/BC4/BC5 distance
semantics and rank tables; the BC6H mode code mapping, 2-region partition set (the first 32 BC7 2-subset partitions),
the mode 13/14 endpoint-bit remap and the anchor index rules; the BC7 286-entry lane-15 signature permutation with its
inferred-only ranks; history entries addressed as `256 + index - d`; the strict policy that a built-in row the local
file does not confirm is refused rather than fitted. Both decoders fail closed and publish only a complete stream.

**Where it differs, and why.** (1) **History source**: the reference compiles generated BC1-BC5 history tables into its
runtime and loads only BC6H/BC7 from a fixed-size binary profile (`BCPHIST1`); ours compiles no history at all and
loads every codec's confirmed rows from the JSON that `tooling/bcpack_history_calib.py` writes, which is what keeps
the repository free of derived data (the policy in [directstorage_advanced.md](directstorage_advanced.md) and
[upstream_port.md](upstream_port.md) section 3). The reconciliation is one extra step for the player: calibrate with
`--codec bc1,bc3,bc4,bc5,bc6h,bc7`. (2) **Confidence gate**: the reference has a non-strict "allow fitted" mode
(`LEONIDA_DSTORAGE_FITTED`) that accepts provisional rows; we have no such path - strict confirmed-only is the only
mode, so a fitted row is a named refusal. The measurement cannot tell whether fitted decoding helps this title, so a
permissive mode would be an unearned guess. (3) **Refusal order inside a chunk**: the reference pre-scans BC7
signatures and history coverage for the whole chunk before rebuilding; the streaming decoder validates as it goes, so a
chunk that has both a structural error in an earlier block and an inferred signature later refuses by the earlier
error. Both are strict refusals with a named reason; the differential run in section 7 shows this as the only
difference over 503 synthetic vectors. (4) **Design**, as in the advanced slice: our C++ streams and counts instead of
carrying the offline decoder's lane arrays, and our decoder is a single header so the GPU-free test compiles it
unchanged.

## 5. A/B measurement

One boot pair, one at a time under the repository's boot lock, same executable (built from the shared working tree at
the time, so other slices in flight are in both arms; the differential below is only this slice's switches), 130 s
timeout, harness `tooling/boot_report.py`. Both arms ran the standard environment (`PALMETTO_SHADERS=<shader corpus>`,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_DSTORAGE_TRACE=1`,
`PALMETTO_PIPELINE_TIMELINE=5` and `PALMETTO_STREAM_CONSUMER=1`. Arm B adds `PALMETTO_DS_STAGING=1M`,
`PALMETTO_DS_UNSHUFFLE=1`, `PALMETTO_DS_BCPACK=1` and `PALMETTO_DS_BCPACK_PREDICTOR=<a synthetic 2-file predictor
directory outside the repository>` (the synthetic file exists only to exercise the env-path plumbing end to end; the
real file is the calibration tool's output and is never in the tree).

| Measure | A: advanced switches, BCPack off | B: the same switches plus `PALMETTO_DS_BCPACK=1` |
|---|---|---|
| Verdict | ALIVE 130.1 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| Traced D3D12 calls | 483,237 (max #484,830) | 523,305 (max #524,773) |
| Requests and shape | 4,199 requests, all `file -> buffer`, `zlib=4199`, `bcpack=0`, `swizzle=0`, dest-pages 0, source-pages 0 | identical: 4,199, same shape counters |
| Decompressed | 256.0 MiB in 2.2 s (115.7 MiB/s) | 256.0 MiB in 2.2 s (114.0 MiB/s) |
| Staging | hits 4,096, fallbacks 0, 133,990,447 source bytes served | identical |
| Predictor | `off` | `ok`: BC6H endpoints 256/256, indices 256/256; BC7 endpoints 256/256, indices 256/256; BC1/BC3/BC4/BC5 rows 0 |
| BCPack stage | refused 0, decoded 0, blocks 0, bytes 0, history 0 | identical (no request reached the stage) |
| Unshuffle | ok 0, refused 0 | identical |
| Stream-consumer guards | 108 tracked, 0 first-touch faults, 108 still guarded | identical |
| Loader state after 16.5 s | the 4,199-request burst completed at +2.2 s; at +15 s the loader is still pre-frame (presents 0, submits 0) | same burst and pre-frame state (presents 0, submits 1 at +15 s) |
| Frame loop start | between +15 s and +20 s (at +20 s: 2 submits, 1 present) | between +15 s and +20 s (at +20 s: 432 submits, 215 presents) |
| Population at +125 s | slot57 10,241 converted 6,539 refused 3,702; graphics created 29,334; submits 23,315; presents 11,657 | slot57 10,241 converted 6,539 refused 3,702; graphics created 29,334; submits 25,405; presents 12,702 |
| Replay checkpoint | submission 20,480 at 94.4 s | submission 24,576 at 106.0 s |
| Capture | no capture switch on, none taken | same |


**Reading it honestly.** The counters that must move if the stage ran did not: every request is still
`bcpack=0, swizzle=0`, `decoded=0`, `blocks=0`, `bytes=0`, and no BCPack refusal line is printed because no request
reached the stage. That is a measured negative, and it is the expected shape for this boot: the loading path is 4,199
zlib file-to-buffer requests, and the switch-on arm differs only in the predictor line (the file loads and its 256-row
BC6H/BC7 tables are retained) and in the inert counters. The loading gate does not advance in either arm. The
falsifier that would make this stage matter is a trace with `bcpack` non-zero - the same promotion rule as the
advanced slice.

**The pair needed a control fix, and it is recorded rather than hidden.** The first pair ran arm B with the advanced
switches (staging, unshuffle, BCPack) and arm A with none, and reported 139.4 vs 114.0 MiB/s. That difference was not
this slice: the staging switch was in B only. Re-running the control with staging and unshuffle on and BCPack off gave
115.7 MiB/s within 1.5% of B, which is why the table above uses that controlled pair. What remains between the arms is
boot-to-boot variance: the switched-on arm traced 8.3% more D3D12 calls and 9% more presents, while its loading
counters (requests, shapes, decompressed bytes, staging hits, guards) are byte-identical and its `+20 s` timeline is
further along than the control's. The slice does not touch the zlib request path (the `bcpack=0` branch is one cached
boolean test), so this is scheduling/load variance between two boots, not a measured effect - one pair cannot resolve
an 8% frame-rate difference, and the honest verdict is **unchanged**.

## 6. Limits

- **No measured BCPack request.** The decoder is exercised only by synthetic chunks (section 7) and by the
  cross-check against the offline decoder. Its correctness on the player's own BCPack textures is therefore inferred
  from the format, not measured on the title's data; the first real request is the acceptance test.
- **Every history row is external.** Until the player runs `tooling/bcpack_history_calib.py --codec
  bc1,bc3,bc4,bc5,bc6h,bc7` into a directory outside the repository and names it with
  `PALMETTO_DS_BCPACK_PREDICTOR`, chunks that read built-in rows are refused by name. Self-contained chunks decode
  with no file at all, but a whole texture is unlikely to be self-contained.
- **Strict only.** There is no fitted/provisional mode: a row the local file marks class 1, a BC6H/BC7 row whose
  confirmed bits do not cover every field the block reads, and an inferred-only BC7 signature are refusals.
- **BC2 is refused by name.** No public or reference block decoder for BCPack BC2 exists.
- **One-shot decode.** The decoder needs the whole chunk in memory (destination up to the 32 MiB linear-buffer limit the
  shim enforces) and writes into a scratch buffer; it does not stream straight into title pages or cooperate with the
  physical-page path. A page-list BCPack destination is still refused at the page-list check.
- **Performance is unmeasured on real data.** The scheduler is a straightforward state machine, not the reference's
  table-accelerated symbol lookup; with no measured requests there is nothing honest to report about throughput.
- **The pairing with a swizzle stage is implemented but unexercised** beyond the unit-level unshuffle tests, because
  no request sets both modes.

## 7. Tests

`port/test/dstorage_test.cpp`, target `dstorage_test` (GPU-free, no boot, no game data): **213 checks, 0 failed**.
Sections added by this slice:

- **rANS encoder + reference chunks**: the test carries a small rANS encoder (the inverse of the lane scheduler) and
  a set of 31 embedded chunks generated by this project's Python implementation from synthetic lane symbols
  (3 BC1, 2 BC3, 2 BC4, 2 BC5, 14 BC6H one per mode, 8 BC7 one per mode). For each: the encoder must reproduce the
  Python chunk **byte for byte**, the encoder output must decode to the Python block stream **byte for byte**, and the
  Python chunk must decode to the same blocks (three independent comparisons per vector). This is the rANS round-trip
  and the cross-implementation check.
- **The documented BC7 example**: the fixed stream published in [textures.md](textures.md) decodes to its published
  block, tying the test to the page.
- **Refusals and bounds**: BC2 (`codec`), a destination that is not whole blocks (`destination_size`), a null
  destination (`bounds`), a source with an empty destination, nothing to decode (ok), a truncated chunk, non-zero
  trailing bytes, and a destination smaller than the stream.
- **Built-in history through the predictor**: a synthetic `bc1.json` loads into `BcpackHistory` (confirmed row class
  3, provisional row class 1, values retained, row counts reported); a block whose distance reads confirmed row 255
  decodes from the predictor and reports a history read, while the same chunk with no predictor is `history_unknown`
  and with a fitted-only row is `history_unconfirmed`; a BC6H mode-11 block reading row 255 is `history_depth` until
  the predictor confirms the mode's bit depths and then decodes; a BC7 mode-6 block reading row 255 is
  `history_unknown`, then `history_unconfirmed`, then decodes once the row's depth and confirmed depth are recorded; a
  BC1 row outside a channel's range is `out_of_range`; a non-zero BC1 lane 1 is `consistency`.

In development the decoder was additionally run as a differential harness against `tooling/bcpack.py` over **503**
synthetic chunks (351 decoded byte-identically, 152 refused by both, 0 disagreements; the one ordering difference of
section 4 is the only caveat). That harness is a developer tool outside the repository; the embedded vectors above are
the reproducible part of it.

The repository gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`,
`dstorage_test` passes, and the documentation checks pass.

## 8. Attribution

This slice was implemented from this project's own measurements (the request shapes in [wait_state.md](wait_state.md)
section 3 and [streamed_consumer.md](streamed_consumer.md) section 2, the request ABI in
`port/shims/dstorage_xs_abi.h`, and the clean-room BCPack specification in [textures.md](textures.md)). The behaviour
was cross-checked against the public GPL-3.0 project `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/src/bcpack.h`,
`runtime/src/bcpack.cpp`, `runtime/src/bcpack_stream.h`, `runtime/shims/dstorage.cpp`,
`tools/gen_bcpack_runtime_tables.py` and `tools/build_dstorage_history.py`, commits `273b3bdf9`, `ba626820e`
(Moriz82), `779f90108` (Tyrannys) and `2d7be48de` (Mumogg). No code from that project was copied. The upstream project
remains the copyright of its contributors and its license continues to apply to its own tree. The `NOTICE.md` credit
for this slice is added by the policy slice that owns that file ([upstream_port.md](upstream_port.md) section 6).
