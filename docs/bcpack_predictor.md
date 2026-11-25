# BCPack predictor: the local BC6H/BC7 initial history

**Status: implemented tool, format and policy.** `tooling/bcpack_predictor.py` builds, validates and self-tests the
local BCPack predictor - the 256-entry BC6H and BC7 initial endpoint and index history recovered from the player's own
package. The runtime loader named by `PALMETTO_DS_BCPACK_PREDICTOR` already reads and validates that JSON
([directstorage_advanced.md](directstorage_advanced.md)); the runtime block decoder that would consume the values is the
named gap there, while the offline decoder `tooling/bcpack.py` consumes the same file today. The predictor is
game-derived, so the tool refuses to write it inside this repository and this page is the policy that keeps it out.
The porting policy is [upstream_port.md](upstream_port.md).

## 1. What the predictor is

BCPack codes the first block of a chunk as a difference against a **fixed 256-entry window that starts before the
chunk**. The stream carries two backward distances per block, one for the block's endpoints and one for its indices
([textures.md](textures.md#clean-room-bcpack-bitstream-specification-and-decoder)):

- distance `d = 0` predicts from a fixed zero/midpoint state;
- `0 < d <= i` references block `i - d` of the same chunk, which the decoder has already rebuilt;
- `d > i` references entry `256 + i - d` of the codec's initial history - a block the decoder never sees, because it
  belongs to the stream before this chunk. The distance is 8 bits wide, so a direct pre-chunk reference reaches only
  entries `1..255`; row 0 is part of the 256-row window and is kept for the window's shape, not for a direct read.

So a chunk is only decodable when the 256-entry initial history for its codec is known. That history is not a public
BCn constant: it is a property of the title's own BCPack encoder, recovered by fitting the player's texture pools
read-only. The table that holds it is the predictor.

| Codec | History tables | Entries per table | What an entry supplies |
|---|---|---|---|
| BC6H (codec 6) | endpoint history, index history | 256 each | base channel endpoints `[3][4]`, and the 16 per-row indices |
| BC7 (codec 7) | endpoint history, index history | 256 each | 24 per-subset RGBA endpoint values, and the 32 per-group indices |

BC1, BC3, BC4 and BC5 keep their initial history in public codec tables (`tooling/bcpack_tables.py`) and are not part
of this file. The predictor covers exactly the two codecs whose tables the package cannot supply from a specification.

## 2. Generating it from your own copy

The generator scans the player's own `.ttpf` pools read-only through the committed calibration engine
(`tooling/bcpack_history_calib.py`), fits the two histories and writes one JSON file. It needs no network and writes
nothing except the requested output path; the pools are opened for reading only.

```text
python -I tooling/bcpack_predictor.py generate <package-root> --out <work>/bcpack-predictor.json
python -I tooling/bcpack_predictor.py generate <pool.ttpf> --out <work>/bcpack-predictor-smoke.json --limit 8
python -I tooling/bcpack_predictor.py generate <package-root> --out <work>/bcpack-predictor.json --dry-run
python -I tooling/bcpack_predictor.py validate <work>/bcpack-predictor.json
python -I tooling/bcpack_predictor.py self-test
```

| Subcommand | What it does | Writes |
|---|---|---|
| `generate` | resolves the package's pools, fits both codecs, validates the result and writes the predictor | the `--out` file only (nothing with `--dry-run`) |
| `validate` | reads an existing predictor or a `bc6h.json`/`bc7.json` directory and reports its coverage | nothing |
| `self-test` | builds a small synthetic sparse fixture from a fixed seed, writes it, reads it back and validates it | the fixture, in the temp directory unless `--out` names a path |

`<package-root>` is a game root (`data/ttdb/...` is discovered below it), a `data/ttdb` folder, or a single `.ttpf`
pool. `<work>` is a folder of your choice **outside this repository**: the predictor is game-derived and must never be
committed, so it has no fixed in-repo location and no example file in the tree.

The self-test fixture is synthetic and deliberately sparse: unlisted rows are the decoder's unknown state, so it
exercises the container, the ranges, the checksum and the loader path, not a real decode.

A full scan visits every mixed BC6H/BC7 texture in every pool and takes minutes; `--limit N` bounds it to `N` mixed
textures per codec, which is what a smoke run or a plumbing check uses (a bounded run is **not** a usable predictor,
section 6). `--max-blocks` bounds the decoded prefix per chunk (default 256). The scan prints its start line and then
one fit line per codec when it finishes - the calibration engine has no per-pool callback - so `--limit` is also what
keeps the wait short. Progress goes to stdout unless `--json` (one machine-readable report) or `--quiet` (the final
summary line only) is given. Exit status is `0` on success and `2` on a named failure.

The runtime reads the file through the environment variable, either directly or as the directory that holds
`bc6h.json` and `bc7.json`:

```text
set PALMETTO_DS_BCPACK_PREDICTOR=<work>\bcpack-predictor.json
```

Re-running `generate` over the same package overwrites the file atomically (a temp file in the same folder is
replaced, so a failed run never leaves a half-written predictor). The same package bytes give the same file bytes;
there is no timestamp, machine name, input path or tool version inside the file (section 3.5).

## 3. The format

### 3.1 Container and header

The predictor is one UTF-8 JSON object (no BOM). A file may carry both codec sections (what `generate` writes), or a
single codec section at the root; a directory is read as `<codec>.json` for the codecs it contains. `generate` writes
the `bc6h` and `bc7` sections only; any other top-level member is left to a consumer that knows that codec, and the
checksum covers it as a table member if it is present.

| Member | Type | Rule | Meaning |
|---|---|---|---|
| `format` | string | exactly `palmetto-bcpack-predictor` | format identifier; a different value is refused |
| `formatVersion` | integer | exactly `1` | the only version this tree reads; any other value is refused with `unsupported_version` |
| `checksum` | object | `{"algorithm": "sha256", "value": <64 lowercase hex>}` | integrity tag over the table members (below) |
| `bc6h` | object | a BC6H section (3.2) | required for a BC6H predictor, optional otherwise |
| `bc7` | object | a BC7 section (3.3) | required for a BC7 predictor, optional otherwise |

The checksum is the SHA-256 of the 29-byte prefix `palmetto-bcpack-predictor\0v1\0` followed by the canonical JSON of
every top-level member **except** `format`, `formatVersion` and `checksum`. The canonical JSON is the same encoding the
writer produces: keys sorted, `,` and `:` separators with no spaces, ASCII-escaped, no trailing whitespace. The
checksum therefore covers the tables and any other table-like member, not the header itself, so a regenerated file
with the same tables has the same predictor checksum while the **file** SHA-256 also depends on the header bytes. A
file over 8 MiB is refused before it is parsed: that is the runtime loader's cap, and a larger file is a wrong file
rather than a larger table.

Every table below is either a 256-entry list or an index-keyed object whose keys are the decimal indices it names
(`"0"`..`"255"`). A row may be written flat or nested up to three array levels; the consumers flatten it and require
the exact length the format lists. This is the same shape rule `tooling/bcpack_tables_bptc.py` already accepts, and a
sparse object leaves unlisted rows at the decoder's built-in "unknown" state.

### 3.2 BC6H section

BC6H has two independent histories, one for endpoints (`de`) and one for indices (`di`). The endpoint history is per
mode: a mode takes its base endpoint width from the mode, and the history value is the base the stream's zig-zag delta
is added to. Values are stored with the **most significant bits meaningful**: the confirmed-bit depth says how many
leading bits of each component an exact fit established.

| Member | Per-row shape | Value range | Meaning |
|---|---|---|---|
| `endpointHistory` | `[3][4]` integers: channel, then `W`, `X`, `Y`, `Z` | `W` 0..65535, `X` 0..1023, `Y`/`Z` 0..63 | base endpoint components for entry `k`; the decoder keeps the top `base` bits of `W` and the top `dc` bits of `X`/`Y`/`Z` |
| `indexHistory` | 16 integers | 0..15 | per-pixel (4x4) indices for entry `k`; the decoder keeps the top 3 (two-region modes) or 4 bits of each |
| `endpointClass` | 1 integer | 0..3 | `0` unknown, `1` fitted/provisional, `2` last-resort, `3` reference-confirmed |
| `indexClass` | 1 integer | 0..3 | the same four states for the index history |
| `endpointConfirmedBits` | `[3][4]` integers | `W` 0..16, `X` 0..10, `Y`/`Z` 0..6 | leading bits of each component an exact reference established |
| `indexConfirmedBits` | 16 integers | 0..4 | leading bits of each pixel index an exact reference established |

A strict consumer requires class `3` and enough confirmed bits for every component the block's mode reads
(`base` for `W`, `dc` for `X`/`Y`/`Z`, 3 or 4 for the indices). A provisional consumer may use classes `1` and `2`
and label the result as fitted; class `0` rows are unknown and the block is refused or marked. A document without the
depth tables is the legacy form: class-3 rows then keep full precision.

### 3.3 BC7 section

BC7 has up to three subsets per block, each with two RGBA endpoints, and two anchor groups of 16 indices. A slot is
addressed as `subset * 8 + endpoint * 4 + channel`, so the 24 endpoint values are three subsets of eight, and the 32
index values are two groups of sixteen.

| Member | Per-row shape | Value range | Meaning |
|---|---|---|---|
| `historyEndpoints` | 24 integers | 0..255 | 8-bit endpoint component values; the p-bit, when the mode has one, is the LSB of channel 0 |
| `historyEndpointBits` | 24 integers | 0..8 | leading endpoint bits the fit knows |
| `historyEndpointConfirmedBits` | 24 integers | 0..8, never above `historyEndpointBits` at the same slot | leading endpoint bits an exact reference confirmed |
| `historyIndices` | 32 integers | 0..15 | per-pixel indices for the two anchor groups; the decoder keeps the leading bits its mode uses (2, 3 or 4 for the primary set, 2 for the secondary set of modes 4-5) |
| `historyIndexBits` | 32 integers | 0..4 | leading index bits the fit knows |
| `historyIndexConfirmedBits` | 32 integers | 0..4, never above `historyIndexBits` at the same slot | leading index bits an exact reference confirmed |
| `historyEndpointLastResort` | list of `[k, subset, endpoint, channel]` or legacy `[k, slot]` | 0..255 for `k`; subset 0..2, endpoint 0..1, channel 0..3; slot 0..23 | endpoint slots the fit could not support from enough observations; a provisional consumer may refuse them |

The legacy `[k, slot]` form is slot `subset * 8 + endpoint * 4 + channel` of the 4-integer form. `generate` writes the
4-integer form; the validator accepts both. The last-resort list is local to the JSON format: it tells a consumer
which fitted endpoint slots are weak, and the runtime binary profile of the upstream reference does not serialize it
(3.6).

### 3.4 What a consumer does with it

The exact lane arithmetic is the clean-room bitstream specification in
[textures.md](textures.md#clean-room-bcpack-bitstream-specification-and-decoder); this is the history part only.

1. Load the section for the chunk's codec; keep the four or six tables and the class/depth rows.
2. For block `i` of the chunk, de-interleave the endpoint distance `de` and the index distance `di` from lanes 0 and 1
   (BC7 also zig-zags the endpoint distance; BC6H derives `de` from `di` plus the unzig-zagged lane-0 value).
3. `d = 0`: use the codec's zero/midpoint state. `d <= i`: copy the already rebuilt block `i - d`'s endpoint or index
   state. `d > i`: read entry `k = 256 + i - d` of the matching history table, and only its leading `confirmedBits`
   bits are trustworthy at that precision.
4. Add the stream's zig-zag deltas to the referenced base with the mode's per-channel widths, then rebuild the block.
   BC6H endpoints use the top `base` bits of `W` and, in the modes that read them, the top `dc` bits of `X`/`Y`/`Z`
   (`base` and `dc` are the mode's base-endpoint and component-delta widths). BC7 endpoints truncate each component to
   the channel width and p-bit position of the block's mode. Both index histories are truncated to the leading bits the
   mode's index precision uses before the delta.
5. Strict mode: every referenced entry must be class `3` and deep enough for the mode, otherwise refuse with the entry
   named. Provisional mode may use fitted entries and must report which ones it used.

### 3.5 Determinism

The writer is deterministic by construction: one canonical JSON encoding (sorted keys, compact separators, ASCII), no
timestamps, no input paths, no machine facts, no floating point, and the generator materializes every table to 256 rows
in a fixed order. The fit is deterministic too: the scanner visits the resolved pool list in sorted order, and the
accumulators are index-keyed or order-independent, so the same package and the same tool give the same bytes. The
checksum domain string carries the format version, so a `v1` checksum cannot be replayed as another version's file.
A changed format is a `formatVersion` bump; a changed fit changes the table bytes, and therefore the checksum, while
the header stays readable.

`--dry-run` proves the write is the only side effect: it runs the scan, the fit, the format validation and the checksum,
then reports the byte count and checksum and writes nothing (it does not even create the output folder).

### 3.6 Relation to the upstream binary profile

The public GPL-3.0 reference had reached this format before this tree: its generator writes a fixed-size binary
sidecar and its runtime reads it back. Derived from `tools/build_dstorage_history.py` and
`tools/gen_bcpack_runtime_tables.py` (evidence only, no code copied):

| Offset | Size | Content |
|---|---:|---|
| 0 | 8 | ASCII magic `BCPHIST1` |
| 8 | 17,920 | BC6H: 256 rows of 12 little-endian `uint16` endpoint components, 16 index bytes, 12 endpoint-depth bytes, 16 index-depth bytes, 2 class bytes |
| 25,928 | 43,008 | BC7: 256 rows of six tables in the order endpoints, endpoint bits, endpoint confirmed bits (24 bytes each), indices, index bits, index confirmed bits (32 bytes each) |
| 60,936 | - | end of table data; the runtime reads the magic, then one fixed `Calibration` structure, then end-of-file |

What agrees: the tables, their order, their value ranges and their meaning are the same; a full profile is
constructed from a local, optional sidecar outside the tree; a missing or invalid sidecar keeps the built-in policy
instead of failing the boot. What differs: (1) the container is JSON with a named format, a version and an integrity
checksum, where the binary file has only the magic and relies on a sibling stamp for reuse; (2) the JSON carries the
BC7 endpoint last-resort list, which the binary layout does not serialize; (3) the naming, the CLI, the validator and
the policy guard are this tree's own, in this tree's style; (4) the upstream binary is a fixed `sizeof(Calibration)`
read, so a row count or field order change is a new magic, while this format keeps every table self-describing and
refuses unknown versions explicitly.

## 4. Validation

`validate` never rewrites the file. It reads the path with the bounded reader (UTF-8, JSON with integer table values
only, no `NaN` or `Infinity`, 8 MiB cap), parses it, checks the header and every table range, verifies the checksum
when the header is present, and prints the row and slot coverage. It accepts a header-less document (the calibration
tool's own combined or per-codec JSON) and reports it as `legacy`, so an existing local predictor can be inspected
before it is replaced.

| Refusal reason | When it is reported |
|---|---|
| `missing` | the path does not exist, or a directory has neither `bc6h.json` nor `bc7.json` |
| `unreadable` | the file exists but cannot be opened or statted |
| `too_large` | the file is over the 8 MiB cap the runtime loader also applies |
| `malformed` | not UTF-8, not JSON, wrong member types, a header member without its partner, a wrong checksum shape |
| `no_codec_section` | a root object with no `bc6h`/`bc7` member and no single-codec markers |
| `out_of_range` | a value, class, depth, slot or last-resort entry outside the ranges of 3.2/3.3, or a confirmed depth above the recorded depth |
| `bad_length` | a row, list, table or last-resort entry with the wrong number of integers |
| `unsupported_format` | `format` is not `palmetto-bcpack-predictor` |
| `unsupported_version` | `formatVersion` is not the supported version |
| `checksum_mismatch` | the tables no longer hash to the header's checksum |

Each failure is one `ERROR: <reason>: <explanation>` line on stderr and exit status `2`, never a traceback. The tool is
stricter than the runtime loader in two places where the loader only bounds what the decoder touches: it enforces the
BC6H per-component widths (`16/10/6/6`) instead of one 16-bit bound, and it requires confirmed depth not to exceed the
recorded depth in BC7. The generated tables are additionally cross-checked against the offline decoder's JSON readers
(`tooling/bcpack_tables_bptc.py` `load_bc6h`/`load_bc7`) before anything is written, so a predictor this tool accepts is
one both committed consumers can load.

## 5. Policy: the predictor is game-derived and never committed

The tables are recovered from the player's own package; they are game-derived data, not source. The policy is the
hard rule of [AGENTS.md](../AGENTS.md) applied to one more artifact:

- The tool refuses an `--out` inside this repository; the guard runs before the scan, and the error names this page.
- The output path is an argument and a placeholder everywhere: no fixed in-repo location, no committed example file,
  no content of a generated table in a doc, test, log or report. Only the file size and its checksum are reportable.
- Tests are synthetic only: `tests/test_bcpack_predictor.py` builds its pools from the committed rANS helper and its
  fixture from a fixed seed. It never reads a package.
- The generated file contains no input path, host or package name, so it cannot identify a machine by itself; that is
  not a reason to commit it. It is regenerable - deleting it and running `generate` again is the normal cleanup.
- An unset `PALMETTO_DS_BCPACK_PREDICTOR` is the default path: the runtime loads no predictor, and a request that
  needs one is refused by name instead of guessing a table ([directstorage_advanced.md](directstorage_advanced.md)).

## 6. Limits

- **The fit is provisional.** A pool-only scan yields fitted (class `1`), last-resort (class `2`) and a few
  reference-confirmed (class `3`) rows; strict decoding of chunks that reference weak rows is refused, exactly as the
  calibration tool's own guides describe. Exact reference blocks (`bcpack_history_calib.py --ref-dds` /
  `--ref-pairs`) produce confirmed rows, but that needs reference DDS pairs and is outside this generator's CLI;
  merging a local confirmed calibration into the generator is the next increment. The upstream reference's native
  trainer refinement pass beyond the pool fit is not implemented here - this tree fits from its own pools only.
- **A bounded run is not a predictor.** `--limit N` stops after `N` mixed textures per codec; the resulting file is
  for plumbing checks. A useful predictor is a full scan.
- **One package copy, one predictor.** The tables are a property of the encoder that produced the package. A different
  patch, build or content set can need its own fit, so regenerate after the package changes rather than reusing a
  stale file; there is no cross-version fallback, and a wrong fit shows up as refused or marked blocks, never as a
  silent success.
- **Empty sections are valid.** A package without mixed BC6H textures yields a BC6H section with unknown rows and a
  valid checksum; the file is well-formed but cannot decode a BC6H chunk that references them. This is reported as
  coverage, not hidden.
- **No encoder.** The tool fits a decoder input; it cannot re-encode a chunk or verify a fit against a plaintext block
  that does not exist in the package. The consumers' class and depth rules are the verification available locally.
- **The format is this project's, not the console's.** The upstream binary profile and this JSON describe the same
  tables, but there is no public console specification of them; the cross-check in 3.6 is the evidence.
- **The runtime block decoder is a later slice.** This tool's file is consumed by `tooling/bcpack.py` and
  `tooling/ttpf_mips.py` today; the runtime loader validates and counts it but a request that needs a block decode is
  still a named refusal ([directstorage_advanced.md](directstorage_advanced.md)).

## 7. Tests

`tests/test_bcpack_predictor.py`, run with `python -I tests/test_bcpack_predictor.py` (24 checks, 0 failed): the
synthetic self-test fixture is byte-identical between runs and round-trips through the validator; the CLI writes it
outside the repository and reads it back; a synthetic two-pool package (a mixed BC6H texture and a mixed BC7 texture,
built from the committed rANS encoder helpers) generates a full 256-row predictor that is identical across two runs,
validates, loads through the offline decoder's readers, carries non-zero fitted coverage and leaks no input path; a
dry run writes nothing and creates no folder; the repository guard, the missing-input and the output-path guards fail
with one `ERROR` line and status `2`; a legacy combined document, a single-codec file and a two-file directory
validate; and the validator refuses a wrong format id, a future version, a tampered table (checksum), a truncated,
non-JSON or non-UTF-8 file, an over-cap file, an out-of-range value/class/depth/last-resort entry, a short row, a
confirmed depth above its recorded depth, and a missing path, without rewriting the file it checked. No test reads a
game file.

## 8. Cross-check and attribution

This slice was implemented from this project's own measurements: the BCPack bitstream and history semantics recorded in
[textures.md](textures.md#clean-room-bcpack-bitstream-specification-and-decoder), the predictor loader already in
`port/shims/dstorage_advanced.h`, and the calibration engine in `tooling/bcpack_history_calib.py`. The format was
cross-checked against the public GPL-3.0 project `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/shims/dstorage.cpp` (the
`BCPHIST1` reader), `runtime/src/bcpack.h` (its `Calibration` structure), `tools/build_dstorage_history.py`,
`tools/gen_bcpack_runtime_tables.py` and `tools/bcpack_history_train.cpp`, commits `273b3bdf9` (Moriz82, the
integration that added the sidecar) and `100fed1b3` (Jan Trencansky, the text-write and stdout-guard cleanup). No code
from that project was copied: the generator, the JSON container, the validator, the policy guard and the tests are
this tree's own, and section 3.6 records where the two formats agree and differ. The upstream project remains the
copyright of its contributors and its license continues to apply to its own tree. The `NOTICE.md` credit for this
slice is added by the policy slice that owns that file ([upstream_port.md](upstream_port.md#6-attribution-template)).
