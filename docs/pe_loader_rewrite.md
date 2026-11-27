# PE-loader rewrite

The runtime's existing `rt::Loader` maps console images in `port/src/pe_loader.cpp`, a 110 KiB translation unit that parses PE headers, maps sections, relocates, binds imports, installs static TLS, registers unwind data and drives module lifecycle. This page tracks a clean-room split of that work into a parser, mapper and resolver. The old loader remains the runtime path and parity oracle: none of the new components is wired into `Loader`, and `PALMETTO_PE_LOADER_V2` does not currently select a new path.

**Rules.** Each slice is independently written, uses synthetic fixtures rather than game bytes, and keeps the old loader as the reference until field-by-field parity is demonstrated. Peer ports are behavior evidence only; they are not an implementation source. The standard docs/build gates remain required before integration.

## Target layering and current state

| layer | responsibility | state |
|---|---|---|
| Phase 0 — `port/src/pe_image.{h,cpp}` | Decode a PE32+ file span into validated headers, sections, directories, exports, imports, relocation records, TLS metadata and exception-directory metadata. No mapping, code execution, IAT writes or host calls. | **Landed.** `pe_image_test` is registered as CTest `pe_image`; deterministic mutation coverage also passes, but its executable is not registered in the repository CMake file. |
| Phase 1 — `pe_mapper.{h,cpp}` | Allocate at the preferred base (or permit a fallback when relocations exist), copy headers/section bytes, zero the rest, apply relocations and protect headers/sections through injectable interfaces. | **Standalone mapper landed; old-loader mapping parity remains open.** Six synthetic mapper scenarios pass in an isolated VS18 CMake/CTest harness (`pe_mapper`); no target or test is registered in `port/CMakeLists.txt`. |
| Phase 2 — `pe_resolver.{h,cpp}` | Pure import/export binding planner behind `SymbolSource`; plan IAT writes and named refusals without loading modules or writing memory. | **Pure planner landed; Loader adapter and CMake/CTest integration remain open.** `pe_resolver_test` passes 37 checks in the VS18 harness. |
| Loader adapter | Feed parsed/mapped descriptions into the existing module registry, shim/host/bundled-module binding, TLS, unwind, thread-event and lifecycle paths. | Not started; existing `Loader` behavior remains authoritative. |
| Switch | Route through the new components only after mapping, binding, TLS/lifecycle and malformed-input parity gates pass. Keep the old path available for a measured transition. | Not wired; old path remains default. |

The parser, mapper and resolver are separate components, not a completed replacement loader. The mapper currently returns a non-owning `MappedImage`; the caller owns and must release its allocation even when mapping fails. The resolver only returns a plan; it does not apply IAT writes, load a module, execute a forwarder target or invoke Windows APIs.

## Phase 0: parser contract and measured coverage

`rt::pe::Image::parse` accepts a byte span and an image name and returns an `Image` description or an `rt::pe::Error` whose message begins with that name. It accepts AMD64 PE32+ only. Header parsing checks the DOS/NT headers, optional-header extent, section table, `SizeOfHeaders`, nonzero `SizeOfImage`/`ImageBase`, and entry-point bounds; section parsing checks virtual and raw-file ranges, the 96-section limit and section/header or section/section overlap. A section exposes its virtual extent and the file-backed byte count the existing mapper copies (`min(raw_size, virtual_size)`, or `raw_size` when virtual size is zero).

The parser reads at most the 16 standard data-directory entries. Each *present* directory (both RVA and size nonzero) must fit within `SizeOfImage`; unknown directory types are range-checked but left opaque. It currently interprets:

- **Exports:** names, ordinals and function RVAs; aliases remain separate named `ExportSymbol` entries, ordinal-only exports are retained, and forwarders inside the export-directory range are recorded as strings. Zero-RVA function slots—including a name-table entry that indexes a zero slot—are skipped like the legacy loader. Export and import strings must be NUL-terminated inside file-backed bytes and are capped at 4,096 characters.
- **Imports:** DLL descriptors through a null terminator inside the declared directory size, imports by name or ordinal, and the IAT slot RVA. `OriginalFirstThunk` falls back to `FirstThunk` when absent. Library/symbol counts and aggregate string data are bounded.
- **Base relocations:** validated relocation-block shape and entries as `(type, RVA)`, including width-aware target bounds. Parsing does not apply a delta or decide whether a relocation type is supported; the mapper owns that behavior.
- **TLS:** the directory's template VA/size, zero-fill, index RVA, callback-table RVA and callback RVAs, with image bounds checks. The template is **not** required to be file-backed: this matches the mapped-image model, where the unbacked portion is zero-filled. Parsing does not install TLS or run callbacks.
- **Exception directory:** directory bounds, a size divisible by the 12-byte `RUNTIME_FUNCTION` entry size, and the entry count. It does not validate each unwind record or register the table.

All table/string reads the parser performs are checked against the file-backed range as well as the image range; parsers do not follow arbitrary pointers. The component never maps memory, calls the host, patches relocations, writes the IAT or executes image code.

### Phase-0 tests and parity evidence

- `pe_image_test` is the executable name; the CTest name is **`pe_image`**. It is pure CPU and uses a synthetic PE fixture, including exports/aliases/forwarders, named and ordinal imports, relocation records, TLS and exception metadata. Final output: `pe_image_test: OK (valid synthetic image, 29 refusal shapes, 6 positive edge shapes)`.
- `pe_image_fuzz_test` is a separate deterministic CPU mutation executable, not yet registered by the repository CMake file. It exercises truncations around headers/table boundaries, every single-bit flip in the synthetic image and 2,048 fixed-seed mutations; invariants check that accepted section, directory, export/import, relocation, TLS and exception ranges remain valid. Final output: **48,659 cases**, 3,220 named PE errors, seed `0x5045494D`, no unexpected exception or invariant failure.
- A temporary VS18 parity harness (`pe_loader_parity_test.exe`, not registered in CTest) compares the parser with the old loader on synthetic DLL and EXE fixtures. It reported **0 discrepancies** for headers/DLL flags, three section rows, named aliases and forwarders, ordinal exports, imports/two nonzero IAT slots, relocations, TLS metadata and installed TLS. The ordinary `loader`, `pe_loader`, `pe_loader_bounds` and `pe_image` CTest subset passed **4/4**. The parity result is synthetic-only: actual host binding pointers and callback execution are outside parser metadata.

## Deliberate differences and compatibility risks

The new representation separates header/table decoding from mapping and resolution. It adds named, image-prefixed parse errors and explicit bounds/count limits. Two acceptance hardenings are intentional and remain compatibility gates: overlapping sections are refused, and every present data directory is checked against `SizeOfImage` before any supported directory is parsed. File-backed strings are terminated within their section/header range and capped at 4,096 characters; this cap and the parser's bounded table counts are also potential acceptance differences that must be tested against the reference corpus before switching the loader.

Alias preservation is **not** a behavioral difference: the legacy loader already exposes each named alias as a separate lookup key, and the synthetic parity fixture verifies two aliases to one RVA and two aliases to one forwarder. The named zero-EAT hole also matches legacy behavior after a parser fix. TLS template bytes are image-bounded, not required to be file-backed; the former page's file-backed-template claim was inaccurate and is removed. Relocation application, section protections, TLS installation, unwind registration and lifecycle remain mapper/adapter work, not phase-0 parser behavior.

## Phase 1: mapper plan and falsifier

The standalone mapper uses an injected `Allocator` and `Protector` (with `VirtualAlloc`/`VirtualProtect` defaults on Windows). It requests the preferred image base first and permits a fallback only when a relocation directory is present; explicitly zeroes the whole allocation; copies headers and file-backed section bytes; applies `DIR64` and `HIGHLOW` deltas, skips `ABSOLUTE`, and names unsupported relocation types; and rejects targets outside the image or in the headers. It returns a non-owning `MappedImage` with base, size, relocation state, mapped-section records and a protection plan. It does not run image code, initialize TLS, register unwind data or bind imports.

Protection follows section R/X/W bits: headers are read-only; X+R is `PAGE_EXECUTE_READ`; X-only is `PAGE_EXECUTE`; R-only is `PAGE_READONLY`; R+W is `PAGE_READWRITE`; no bits is `PAGE_NOACCESS`; X+W remains `PAGE_EXECUTE_READWRITE`. This is **not** strict W^X. In particular, X-only differs from the legacy loader, which maps non-writable executable sections as `PAGE_EXECUTE_READ`; that is an explicit compatibility risk, not an established parity result.

`pe_mapper_test` covers injected copy/zero-fill, `DIR64`/`HIGHLOW` relocation, protection planning, HIGHLOW overflow, unsupported relocation type, relocation into headers and allocation failure. The isolated `pe_mapper` CTest passed, but it is not yet part of the repository's normal CMake test inventory. The next falsifier is field-by-field comparison of the mapper and old `Loader` on the same synthetic DLL/EXE fixtures: chosen base/relocated state, header and section bytes, virtual zero-fill/gaps, relocation values/refusals and protected ranges/flags. Any unexplained byte, relocation, protection or acceptance mismatch rejects the mapper as a parity substitute; the execute-only protection difference must be resolved or explicitly waived before integration. The malformed corpus must also produce equivalent refusals where no intentional hardening is documented.

## Phase 2: resolver plan and falsifier

The pure resolver defines a `SymbolSource` adapter boundary for shim, host and mapped/bundled module lookups. A response carries a final address, existing `Binding` (`Shim`, `Host`, `Module` or `Unimplemented`), owning module, or a forwarder string. `resolve_export` maps parsed name/ordinal exports to that response; `plan_imports` emits IAT-write records or named refusals without writing memory. It handles name and ordinal imports, aliases, `module.symbol` and `module.#ordinal` forwarders, case-folds module names and normalizes an omitted `.dll` for cycle detection, and limits forwarder traversal to eight edges by default. Results are stably sorted by IAT RVA. Duplicate imports are deliberately retained as separate IAT slots/resolutions, not deduplicated.

`pe_resolver_test` passed 37 checks (VS18): name/ordinal and alias targets, a two-hop forwarder ending at ordinal `#42`, a case/`.dll`-normalized cycle, a configured depth-two refusal, malformed forwarder, distinct missing-name/missing-ordinal refusals and repeated IAT slots. The test is not yet registered in `port/CMakeLists.txt`; no Loader adapter or runtime module-load path uses the planner yet.

The resolver falsifier is a synthetic old/new binding comparison: every accepted name/ordinal/forwarder must yield the same final pointer, `Binding` and owning module; a missing import must retain the current `PALMETTO_UNIMPLEMENTED` behavior when the later adapter translates the planner's named refusal; and cycles/depth errors must be bounded and deterministic. A lost duplicate slot, changed pointer/provenance, unbounded chain, or changed refusal is a failure. This pure planner test is readiness, not proof that imports are already routed through the new resolver.

## Adapter, lifecycle and default switch

The adapter remains the later integration slice. It must preserve the exported `rt::Loader` API and the order in which mapping, TLS setup, import binding, unwind registration, thread events, callbacks and `DllMain` currently occur. `load()` must remain non-executing; `initialise()` remains the point where TLS callbacks and `DllMain` run. First compare the full synthetic loader corpus, then run the existing loader/TLS/EH/fuzz CTests unchanged through an opt-in new path. A title boot is a later integration check, not a substitute for synthetic parity. Only after those gates pass may `PALMETTO_PE_LOADER_V2=1` route through the new components; keep the old path behind a temporary legacy switch for one measured round before removing it.

## The inherited loader: measured audit (2026-11-27)

The user hypothesis was that the inherited loader explains why no image appears. The existing corpus falsifies that for
the black frame: 103/103 light arms across 37 builds map the title at its preferred `0x140000000` (`relocated:false`),
bind all 587 imports (`0 unimplemented`), initialise all five modules, install TLS, and show no loader-failure string;
100 sampled first frames are black (the only non-black frames are the synthetic presenter control). The one remaining
blind spot was the per-thread `/GA` TLS view: an arm with `PALMETTO_TRACE_TLS=1` and `PALMETTO_TEB_CHECK=1` shows the
title block and `TlsSlots[2]` view installed in every listed thread, runs 90.3 s with zero exceptions, and adds no new
stop. The black frame is the content/presentation frontier, not a loader state.

The audits still found real latent defects, now fixed and covered by new CTest targets (all synthetic, no game data):

| audit | measured faults fixed | test |
|---|---|---|
| core mapping | page-tail bytes past `VirtualSize` were zeroed instead of copied (Windows maps them; a relocation target there was fixed from zero); `PointerToRawData == 0` copied the DOS/NT headers into the section; no ImageBase/FileAlignment/SectionAlignment validation; two sections sharing a page with different protections silently lost one; protection derivation ignored `MEM_READ` (execute-only/no-access) | `loader_core_audit` (46 checks) |
| binding | `FirstThunk == 0` walked RVA 0 and overwrote the mapped headers; a bundled module missing an export silently bound a host System32 namesake; ordinal bits 16-62 were masked; a non-PE forwarder target aborted the importer; an extensionless package file shadowed the `.dll`; an export RVA into headers was accepted; bound-image diagnostics were generic | `loader_bind_audit` |
| TLS/thread/unwind | `DllMain`-triggered runtime load re-entered `initialise()` (stack overflow); one process-wide `TlsAlloc` slot for every mapped `/GA` EXE (cross-module TLS aliasing); module TLS index >=64 rejected though Windows supplies it; `DllMain` never received `DLL_THREAD_ATTACH/DETACH`; undersized TLS directory accepted; chained unwind entries unvalidated | `loader_tls_audit` (43 checks) |
| base reservation | `needed()` decided "relocatable" from the directory size alone, so a `/FIXED` image with `{RVA 0, Size N}` never reserved its base; weaker header validation than `map_image()`; silent relaunch | `pe_loader_test` (+20 checks), `loader_selfcheck` (203 checks, 0 faults) |

The independent mapped-image self-check (old loader vs `rt::pe::Image`) reports **203 checks, 0 faults**: every mapped
byte, IAT slot, relocated pointer, TLS copy and page protection matched on the synthetic matrix. Its one rewrite parity
finding was the page-tail rule: `Image::section_data()` still exposes only `min(SizeOfRawData, VirtualSize)`, so the new
mapper now derives the measured copy rule from the section headers instead of using `section_data()` for that shape.

## Phase 1/2 landing (2026-11-27)

- `port/src/pe_mapper.{h,cpp}` maps a parsed `Image` through injectable allocation/protection, with the measured section
  copy rule, DIR64/HIGHLOW/ABSOLUTE relocations and named refusals; `pe_mapper` CTest passes (copy/zero tail, page-tail
  bytes, relocations, protections, HIGHLOW overflow, unsupported type, header target, allocation failure).
- `port/src/pe_resolver.{h,cpp}` is the pure `SymbolSource`/forwarder/ordinal/alias planner for imports and exports
  (`pe_resolver`, 37 checks); it performs no Windows calls or IAT writes.
- `port/test/pe_loader_parity_test.cpp` compares the old loader with the parser on synthetic DLL/EXE fixtures:
  base/size/entry/DLL flag, sections, aliases and forwarder aliases, ordinal-only exports, imports/IAT, relocations and
  TLS all match; **0 discrepancies**. A named zero-EAT-slot hole was found and the parser now skips it like the legacy
  loader. The harness is not CTest-registered yet (it links the runtime loader and shims).
- `port/test/pe_loader_shadow_test.cpp` is the pure shadow-comparison layer and switch parser; the minimal `pe_loader.cpp`
  wiring is documented above for a later opt-in integration.
- `pe_image` grew to 29 refusal shapes + 6 positive edge shapes; `pe_image_fuzz` covers 48,659 deterministic
  truncate/mutation cases with no crash and 3,220 named refusals.

The remaining falsifiers are: old-loader-vs-new-mapper parity for mapping/protections, the opt-in loader adapter behind
`PALMETTO_PE_LOADER_V2`, the `PALMETTO_PE_LOADER_SHADOW_PARSE` wiring, TLS audit F4/F7 (race and deadlock, analysis-only),
and the forced-rebase `--dry-run` for the 598,282 title DIR64 relocations (the title is relocatable but every measured
Windows boot maps it at its preferred base).
