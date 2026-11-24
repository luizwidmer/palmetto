# Where the eight "untranslated" pipeline programs came from

**Status: measured, negative result.** The eight captured dumps are not programs from any other container. They are
zero and partially-zero reads of the title's own pipeline library: compute records 31 to 41 of the library in
`shaders.arc` ([shader_library.md](shader_library.md)), which is also the package's PSO/shader cache
([pso_cache.md](pso_cache.md)). The cause was the DirectStorage shim refusing a request's zlib stage, so the title was
handed zero pages; [boot_frontier.md](boot_frontier.md) records the boot measurement once that stage inflates. No game
bytes, hashes or package addresses are reproduced here, only counts, offsets, field names and structure names.

## The question

The boot asked the runtime for 42 precompiled compute pipelines. 31 of the 42 kind-6 program byte strings were
identical to extracted library programs, and 11 calls (8 distinct keys, sizes 320 to 1024 bytes) matched nothing: not
a library program, not a FidelityFX program, and not any raw or inflated package data. The hypotheses were a second
container, library records the extractor skipped, or a load-time patch of stored programs. All three are wrong.

## What the captured dumps are

The 11 calls match compute records 31 to 41 of the library's compute table, which name program table entries 31 to 41
(record `i` names program `i` for every record inspected). Their program sizes are
1024, 320, 384, 512, 576, 768, 768, 448, 640, 640, 320 bytes. The eight captured dumps have exactly the eight distinct
sizes among those (`lib_cs_00031` to `lib_cs_00041` in the work folder's library extraction), which is the whole of
the "distinct programs" observation:

* six dumps are **entirely zero bytes** (sizes 320, 448, 512, 576, 640 and 768); their `.state` sidecars are 28 zero
  bytes as well;
* the 384-byte dump is zero code plus a real 28-byte kind-0 state, which equals the state blob of compute record 33;
* the 1024-byte dump is the first **212 bytes** of program table entry 31 followed by 812 zero bytes, and its kind-0
  state equals record 31's state blob. 212 bytes is the partial output of a decompression that then failed.

The 11 calls collapsed to 8 distinct keys only because a zero read of a given size is the same key as every other
zero read of that size: records 32 and 41 share the zero 320-byte key, 36 and 37 the zero 768-byte key, 39 and 40 the
zero 640-byte key. The "closest library program of the same size differs in 269 to 622 bytes" figure is the same
artifact: zero pages and a truncated prefix compared against real programs of those sizes.

## What was ruled out

* **A second container.** A raw scan of `shaders.arc`, `data.arc`, `common.arc`, `packs.arc` and the title executable
  finds no second library header and no second copy of its tables (a scan of compressed archives can only find
  uncompressed bytes, but the title executable has the library's build ID in build metadata and no copy of the
  library). A re-extraction with `--stages all` writes all 21,522 program table entries, every one of them assigned a
  stage, and records 31 to 41 are ordinary compute records naming ordinary entries in that table: nothing the
  extractor skipped.
* **A load-time patch or relocation.** A patch would leave most of a program equal to the stored entry with a
  localized changed region. The 1024-byte dump is instead equal for a prefix and then zero, and the other dumps have
  no non-zero byte at all, so there is no patch region to describe.
* **A missing program in the package.** The library already holds records 31 to 41; the dump simply did not contain
  them, and a raw search could not find bytes that were never there.

## Why 31 worked and 11 did not

The same zeroed-read effect explains the split without a second source. The decompression that fed the title's
library bytes failed partway through: the kinds of data it had already delivered (31 complete programs, two of the
eleven states, one program's first 212 bytes) came through, and everything that would have come from the failed part
arrived as zeros. The library layout is contiguous, so a resource page can hold a program prefix that reads as real
code and a following page that reads as zeros, which is what the 1024-byte dump shows.

## Consequence for a fresh build

A fresh build does **not** need a source for the eight programs, because there are no eight programs: all 42 pipelines
are library compute records 0 to 41, and records 31 to 41 translate from the same extraction as records 0 to 30. What
a fresh build needs is the pipeline-library step itself: extract the library, write the kind-0 state sidecars, translate
its compute programs, and index them by kind-6 and kind-0 keys. With the zlib stage inflating, the same boot reports
11,894 D3D12 calls, **0 of 42 pipeline failures and 0 programs dumped**. The pipeline-library step is not yet a
`make_game.py` stage ([boot_frontier.md](boot_frontier.md)), so a fresh build still reproduces the older failure until
it is added.

## Checking

`tools/shader_library.py` extracts the library (all stages with `--stages all`) and writes the `.state` sidecars; the
record tables are in its `library.json`. The captured dumps stay in the work folder, outside the repository. The fix
is the zlib stage in the console DirectStorage shim (`runtime/shims/dstorage.cpp`). No GPU run was made in this
investigation.

Correction (later): the pipeline-library step is now a `make_game.py` stage (`library_shaders`), so the last paragraph
of "Consequence for a fresh build" no longer applies.

## The 1,266 graphics programs the next boot asks for

**Status: implemented and measured on the work folder.** One retail boot at the current stop deserializes 9,943
graphics pipeline records that reference only **1,266 distinct native programs (947 PS, 319 VS)**. Each is identified
by the SHA-256 of its bytes as passed, which is the index's `fspx_code_sha256` key - the same key
`create_compute` resolves compute programs with. The hash list is data: it stays outside the repository (a file named
by `PALMETTO_GRAPHICS_PROGRAMS`, else `<work>/graphics_programs.json`) and is never committed or quoted.

`tools/shader_library.py --wanted LIST.json` adds the programs whose stored bytes have those hashes to the
extraction (whatever `--stages` selects; the extraction records the outcome in `<out>/wanted.json`). The new
`make_game.py` step `graphics_shaders` extracts that selection into `<work>/shaders/pipeline_graphics`, translates it
into `<work>/shaders/spirv/pipeline_graphics`, and publishes the entries in the FSPX index beside the compute
library's (`merge_library_corpus`, entry names `lib_ps_*` and `lib_vs_*`): the runtime looks a program up by its
stage and `fspx_code_sha256`, with the module at `fspx/<name>.spv`.

Measured on the work folder: **1,266 of 1,266 hashes found** in the library's program table (947/947 PS, 319/319 VS),
all 1,266 have a carved source container, all 1,266 translated and pass `spirv-val`, and each published
`fspx_code_sha256` equals the wanted hash - so all 1,266 become resolvable. A build without the list does nothing:
the step logs that it has no list and leaves the corpus as it is. The extraction (`extraction.json`) and the
translation (`pipeline_graphics_build.json`) carry reuse markers, so a rerun is a publish-only no-op unless the
package, the tools, llvm-mc, the wave64/fallback flags or the list changed.

Publishing runs in the shaders stage, so the supported way to republish a work folder is (with `LLVM_MC`,
`spirv-as` and `spirv-val` on `PATH`, as for the step itself; `scripts\build_llvm_mc.ps1` builds llvm-mc):

```sh
python -I make_game.py --game "<package>" --work "<work>" --stages shaders
```

The list is read from `<work>/graphics_programs.json` in that case; set `PALMETTO_GRAPHICS_PROGRAMS` to name another
file. No hash or program byte of this selection is in the repository.
