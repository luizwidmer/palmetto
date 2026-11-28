# PE-loader rewrite

The runtime's existing `rt::Loader` maps console images in `port/src/pe_loader.cpp`, a 110 KiB translation unit that parses PE headers, maps sections, relocates, binds imports, installs static TLS, registers unwind data and drives module lifecycle. This page tracks a clean-room split of that work into a parser, mapper and resolver. The old loader remains the runtime path and parity oracle. Round 20 adds two strict opt-ins that do not change a mapped image by default: a shadow comparison of the parser against the old loader's validated header facts (`PALMETTO_PE_LOADER_SHADOW_PARSE`, `PALMETTO_PE_LOADER_SHADOW_STRICT`) and a named refusal for the TLS F1 fallback. `PALMETTO_PE_LOADER_V2` still selects no new path, and no parser, mapper or resolver component replaces `Loader`. Round 21 fixes the two open TLS-audit findings (F4 duplicate install, F7 thread-event callback self-deadlock), wires the loader census behind the exact `PALMETTO_LOADER_CENSUS=1` switch with assertions on the emitted lines, and makes `loader_tls_race` a strict gate; it still does not route a title image through the new components.

**Rules.** Each slice is independently written, uses synthetic fixtures rather than game bytes, and keeps the old loader as the reference until field-by-field parity is demonstrated. Peer ports are behavior evidence only; they are not an implementation source. The standard docs/build gates remain required before integration.

## Target layering and current state

| layer | responsibility | state |
|---|---|---|
| Phase 0 — `port/src/pe_image.{h,cpp}` | Decode a PE32+ file span into validated headers, sections, directories, exports, imports, relocation records, TLS metadata and exception-directory metadata. No mapping, code execution, IAT writes or host calls. | **Landed and CTest-registered.** CTest `pe_image` (29 refusal shapes, 6 positive edge shapes) and CTest `pe_image_fuzz` (48,659 deterministic mutation cases, 3,220 named PE errors) both pass from `port/CMakeLists.txt`. |
| Phase 1 — `pe_mapper.{h,cpp}` | Allocate at the preferred base (or permit a fallback when relocations exist), copy headers/section bytes, zero the rest, apply relocations and protect headers/sections through injectable interfaces. | **Mapper landed, CTest-registered and parity-measured.** CTest `pe_mapper` passes its six synthetic scenarios, and the round-20 `pe_mapper_parity` falsifier passes **309 checks, 0 failures** on byte-identical synthetic DLL/EXE fixtures (board task 253: mapped base/size, header/section bytes, relocations, protection flags and named refusals compared with the old loader). |
| Phase 2 — `pe_resolver.{h,cpp}` | Pure import/export binding planner behind `SymbolSource`; plan IAT writes and named refusals without loading modules or writing memory. | **Pure planner landed and CTest-registered; the Loader adapter remains open.** CTest `pe_resolver` passes 37 checks from `port/CMakeLists.txt`; no runtime module-load path uses the planner. |
| Loader adapter | Feed parsed/mapped descriptions into the existing module registry, shim/host/bundled-module binding, TLS, unwind, thread-event and lifecycle paths. | Not started as a switch. The round-20 shadow pass and TLS F1 refusal, and the round-21 F4/F7 TLS fixes and census wiring, are the only `Loader` changes; each keeps default mapping, binding, TLS and lifecycle behavior byte-identical when its switch is off. |
| Switch | Route through the new components only after mapping, binding, TLS/lifecycle and malformed-input parity gates pass. Keep the old path available for a measured transition. | Not wired; old path remains default. `PALMETTO_PE_LOADER_V2` selects no new path; the shadow switches only compare or refuse a mismatch, and the census only prints diagnostic lines behind its exact switch. |

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
- `pe_image_fuzz_test` is a separate deterministic CPU mutation executable, registered as CTest **`pe_image_fuzz`** in `port/CMakeLists.txt`. It exercises truncations around headers/table boundaries, every single-bit flip in the synthetic image and 2,048 fixed-seed mutations; invariants check that accepted section, directory, export/import, relocation, TLS and exception ranges remain valid. Final output: **48,659 cases**, 3,220 named PE errors, seed `0x5045494D`, no unexpected exception or invariant failure.
- A temporary VS18 parity harness (`pe_loader_parity_test.exe`, not registered in CTest) compares the parser with the old loader on synthetic DLL and EXE fixtures. It reported **0 discrepancies** for headers/DLL flags, three section rows, named aliases and forwarders, ordinal exports, imports/two nonzero IAT slots, relocations, TLS metadata and installed TLS. The ordinary `loader`, `pe_loader`, `pe_loader_bounds` and `pe_image` CTest subset passed **4/4**. The parity result is synthetic-only: actual host binding pointers and callback execution are outside parser metadata.

## Deliberate differences and compatibility risks

The new representation separates header/table decoding from mapping and resolution. It adds named, image-prefixed parse errors and explicit bounds/count limits. Two acceptance hardenings are intentional and remain compatibility gates: overlapping sections are refused, and every present data directory is checked against `SizeOfImage` before any supported directory is parsed. File-backed strings are terminated within their section/header range and capped at 4,096 characters; this cap and the parser's bounded table counts are also potential acceptance differences that must be tested against the reference corpus before switching the loader.

Alias preservation is **not** a behavioral difference: the legacy loader already exposes each named alias as a separate lookup key, and the synthetic parity fixture verifies two aliases to one RVA and two aliases to one forwarder. The named zero-EAT hole also matches legacy behavior after a parser fix. TLS template bytes are image-bounded, not required to be file-backed; the former page's file-backed-template claim was inaccurate and is removed. Relocation application, section protections, TLS installation, unwind registration and lifecycle remain mapper/adapter work, not phase-0 parser behavior.

## Phase 1: mapper plan and falsifier

The standalone mapper uses an injected `Allocator` and `Protector` (with `VirtualAlloc`/`VirtualProtect` defaults on Windows). It requests the preferred image base first and permits a fallback only when a relocation directory is present; explicitly zeroes the whole allocation; copies headers and file-backed section bytes; applies `DIR64` and `HIGHLOW` deltas, skips `ABSOLUTE`, and names unsupported relocation types; and rejects targets outside the image or in the headers. It returns a non-owning `MappedImage` with base, size, relocation state, mapped-section records and a protection plan. It does not run image code, initialize TLS, register unwind data or bind imports.

Protection follows section R/X/W bits: headers are read-only; X+R is `PAGE_EXECUTE_READ`; X-only is `PAGE_EXECUTE`; R-only is `PAGE_READONLY`; R+W is `PAGE_READWRITE`; no bits is `PAGE_NOACCESS`; X+W remains `PAGE_EXECUTE_READWRITE`. This is **not** strict W^X. The inherited loader now derives the same protections: the audit fixed its former `MEM_READ`-ignoring derivation, so a non-readable executable section is `PAGE_EXECUTE` (measured as `0x10` by `VirtualQuery` on a Windows image view) and an inaccessible section is `PAGE_NOACCESS`. The earlier X-only mapper-vs-legacy difference is therefore retired. The mapped image is private `VirtualAlloc` memory, so a writable section is `PAGE_READWRITE` where Windows would use copy-on-write `PAGE_WRITECOPY`; both are guest-writable, which is all the section asks for. CTest `pe_mapper_parity` now measures the agreement, protection flags included.

`pe_mapper_test` covers injected copy/zero-fill, `DIR64`/`HIGHLOW` relocation, protection planning, HIGHLOW overflow, unsupported relocation type, relocation into headers and allocation failure. CTest `pe_mapper` is registered in `port/CMakeLists.txt` and passed 1/1 in the shared `port/build` on 2026-11-27. The phase-1 falsifier is now the registered CTest `pe_mapper_parity` (**309 checks, 0 failures**): field-by-field comparison of the mapper and old `Loader` on the same synthetic DLL/EXE fixtures — chosen base/relocated state, header and section bytes, virtual zero-fill/gaps, relocation values/refusals, protected ranges/flags — plus the malformed corpus's named refusals. The old loader's protection derivation was fixed to the measured rule, so both paths agree; no unexplained byte, relocation, protection or acceptance mismatch remains in the synthetic matrix. A mismatch is still the rejection condition. Limits: the harness is synthetic-only (no title image and no executed image code), and no runtime path routes a title image through the mapper yet.

## Phase 2: resolver plan and falsifier

The pure resolver defines a `SymbolSource` adapter boundary for shim, host and mapped/bundled module lookups. A response carries a final address, existing `Binding` (`Shim`, `Host`, `Module` or `Unimplemented`), owning module, or a forwarder string. `resolve_export` maps parsed name/ordinal exports to that response; `plan_imports` emits IAT-write records or named refusals without writing memory. It handles name and ordinal imports, aliases, `module.symbol` and `module.#ordinal` forwarders, case-folds module names and normalizes an omitted `.dll` for cycle detection, and limits forwarder traversal to eight edges by default. Results are stably sorted by IAT RVA. Duplicate imports are deliberately retained as separate IAT slots/resolutions, not deduplicated.

`pe_resolver_test` passed 37 checks: name/ordinal and alias targets, a two-hop forwarder ending at ordinal `#42`, a case/`.dll`-normalized cycle, a configured depth-two refusal, malformed forwarder, distinct missing-name/missing-ordinal refusals and repeated IAT slots. It is registered as CTest `pe_resolver` in `port/CMakeLists.txt`; no Loader adapter or runtime module-load path uses the planner yet.

The resolver falsifier is a synthetic old/new binding comparison: every accepted name/ordinal/forwarder must yield the same final pointer, `Binding` and owning module; a missing import must retain the current `PALMETTO_UNIMPLEMENTED` behavior when the later adapter translates the planner's named refusal; and cycles/depth errors must be bounded and deterministic. A lost duplicate slot, changed pointer/provenance, unbounded chain, or changed refusal is a failure. This pure planner test is readiness, not proof that imports are already routed through the new resolver.

## Adapter, lifecycle and default switch

The adapter remains the later integration slice. It must preserve the exported `rt::Loader` API and the order in which mapping, TLS setup, import binding, unwind registration, thread events, callbacks and `DllMain` currently occur. `load()` must remain non-executing; `initialise()` remains the point where TLS callbacks and `DllMain` run. First compare the full synthetic loader corpus, then run the existing loader/TLS/EH/fuzz CTests unchanged through an opt-in new path. A title boot is a later integration check, not a substitute for synthetic parity. Only after those gates pass may `PALMETTO_PE_LOADER_V2=1` route through the new components; keep the old path behind a temporary legacy switch for one measured round before removing it.

The round-20 `Loader` change is deliberately smaller than an adapter. `map_image` now optionally compares its already-read file buffer and the old loader's validated header, section and directory facts with `rt::pe::Image`: `PALMETTO_PE_LOADER_SHADOW_PARSE=1` enables the comparison, only the exact value `1` enables either switch, and a strict-only setting is a no-op. Non-strict mode prints bounded named diagnostics and continues; `PALMETTO_PE_LOADER_SHADOW_STRICT=1` throws a named refusal only when parsing is enabled and the comparison fails, before the image is allocated. With both switches off the shadow code is a single static guard: no file copy, comparison facts, parse or log. The synthetic end-to-end coverage is the new `pe_loader_shadow_wiring_test` target (parse off, strict-only, a truthy non-`1` value, parse-only diagnostic, strict match, strict mismatch).

## The inherited loader: measured audit (2026-11-27)

The user hypothesis was that the inherited loader explains why no image appears. The existing corpus falsifies that for
the black frame: 103/103 light arms across 37 builds map the title at its preferred `0x140000000` (`relocated:false`),
bind all 587 imports (`0 unimplemented`), initialise all five modules, install TLS, and show no loader-failure string;
100 sampled first frames are black (the only non-black frames are the synthetic presenter control). The one remaining
blind spot was the per-thread `/GA` TLS view: an arm with `PALMETTO_TRACE_TLS=1` and `PALMETTO_TEB_CHECK=1` shows the
title block and `TlsSlots[2]` view installed in every listed thread, runs 90.3 s with zero exceptions, and adds no new
stop. The black frame is the content/presentation frontier, not a loader state.

The audits still found real latent defects; round 20 fixes them and registers each audit as a CTest target (all synthetic, no game data):

| audit | measured faults fixed | test |
|---|---|---|
| core mapping | page-tail bytes past `VirtualSize` were zeroed instead of copied (Windows maps them; a relocation target there was fixed from zero); `PointerToRawData == 0` copied the DOS/NT headers into the section; no ImageBase/FileAlignment/SectionAlignment validation; two sections sharing a page with different protections silently lost one; protection derivation ignored `MEM_READ` (execute-only/no-access) | `loader_core_audit` (46 checks) |
| binding | `FirstThunk == 0` walked RVA 0 and overwrote the mapped headers; a bundled module missing an export silently bound a host System32 namesake; ordinal bits 16-62 were masked; a non-PE forwarder target aborted the importer; an extensionless package file shadowed the `.dll`; an export RVA into headers was accepted; bound-image diagnostics were generic | `loader_bind_audit` |
| TLS/thread/unwind | `DllMain`-triggered runtime load re-entered `initialise()` (stack overflow); one process-wide `TlsAlloc` slot for every mapped `/GA` EXE (cross-module TLS aliasing); module TLS index >=64 rejected though Windows supplies it; `DllMain` never received `DLL_THREAD_ATTACH/DETACH`; undersized TLS directory accepted; chained unwind entries unvalidated | `loader_tls_audit` (45 checks) |
| base reservation | `needed()` decided "relocatable" from the directory size alone, so a `/FIXED` image with `{RVA 0, Size N}` never reserved its base; weaker header validation than `map_image()`; silent relaunch | `pe_loader_test` (+20 checks), `loader_selfcheck` (203 checks, 0 faults) |

Round-20 registration and verification. Every audit and rewrite target is now in `port/CMakeLists.txt` and passes from
the shared `port/build` (run serially on 2026-11-27; no GPU and no game files): `loader_core_audit` 46 checks / 0
failures, `loader_bind_audit` 27 checks (20 contracts held, 0 faults still present, 7 already fixed), `loader_tls_audit`
45 checks / 0 reproduced faults, `loader_selfcheck` 203 checks / 0 faults, `pe_loader` 149 checks, `pe_loader_bounds` 54
checks, `loader_tls` 95 checks, and `pe_image`, `pe_image_fuzz`, `pe_mapper`, `pe_mapper_parity`, `pe_resolver`,
`pe_loader_shadow` and `pe_loader_shadow_wiring` pass.
The CPU loader subset ran **16/16** (24.44 s), including `module_names`, `unimplemented_policy` and `startup_trace`;
evidence is in `D:\leonida-work\scratch\docs260\ctest-loader-subset.txt`.

The independent mapped-image self-check (old loader vs `rt::pe::Image`) reports **203 checks, 0 faults**: every mapped
byte, IAT slot, relocated pointer, TLS copy and page protection matched on the synthetic matrix. Its one rewrite parity
finding was the page-tail rule: `Image::section_data()` still exposes only `min(SizeOfRawData, VirtualSize)`, so the new
mapper now derives the measured copy rule from the section headers instead of using `section_data()` for that shape.

## Phase 1/2 landing (2026-11-27)

- `port/src/pe_mapper.{h,cpp}` maps a parsed `Image` through injectable allocation/protection, with the measured section
  copy rule, DIR64/HIGHLOW/ABSOLUTE relocations and named refusals; `pe_mapper` CTest passes (copy/zero tail, page-tail
  bytes, relocations, protections, HIGHLOW overflow, unsupported type, header target, allocation failure).
- The `pe_mapper_parity` source (CTest `pe_mapper_parity`) compares the old loader's mapping with the new mapper
  on byte-identical synthetic DLL/EXE fixtures, including protections and the malformed-shape named refusals: 309 checks,
  0 failures on 2026-11-27.
- `port/src/pe_resolver.{h,cpp}` is the pure `SymbolSource`/forwarder/ordinal/alias planner for imports and exports
  (`pe_resolver`, 37 checks); it performs no Windows calls or IAT writes.
- `port/test/pe_loader_parity_test.cpp` compares the old loader with the parser on synthetic DLL/EXE fixtures:
  base/size/entry/DLL flag, sections, aliases and forwarder aliases, ordinal-only exports, imports/IAT, relocations and
  TLS all match; **0 discrepancies**. A named zero-EAT-slot hole was found and the parser now skips it like the legacy
  loader. The harness is not CTest-registered yet (it links the runtime loader and shims).
- `port/test/pe_loader_shadow_test.cpp` is the pure shadow-comparison layer and switch parser, registered as CTest
  `pe_loader_shadow`; round 20 wires it into `map_image` as described in the adapter section above.
- `pe_image` grew to 29 refusal shapes + 6 positive edge shapes; `pe_image_fuzz` covers 48,659 deterministic
  truncate/mutation cases with no crash and 3,220 named refusals.

The remaining falsifiers are: the opt-in loader adapter behind `PALMETTO_PE_LOADER_V2`, TLS audit F4/F7 (duplicate-install
window and thread-event-callback deadlock, contract-tested in board task 264 and fixed in round 21, where the gate is now
strict), and
the forced-rebase `--dry-run` for the 598,282 title DIR64 relocations (the title is relocatable but every measured Windows
boot maps it at its preferred base). The mapper mapping/protection parity falsifier has landed and passes (board task 253),
and the shadow wiring and the TLS F1 fallback are wired: round 20 makes F1 a named refusal and adds the parser comparison,
both described below.

## Round-20 loader state (2026-11-27)

- **TLS F1 is a named refusal, and the audit verifies it.** When a mapped `/GA` EXE has no free `TlsAlloc` slot below 64
  for its index-0 TLS reads, `initialize_tls` now frees a slot it cannot use and throws a load refusal naming
  `GA_TLS_SLOT_UNAVAILABLE` (before this round the loader printed the same condition and continued with the title's
  index-0 reads pointing at the wrong cell). The extended `loader_tls_audit` fails a synthetic `/GA` image after exhausting
  every low `TlsAlloc` slot, and the rebuilt CTest passed **45 checks, 0 reproduced faults**, printing
  `FIXED AUDIT-F1 ... refused by GA_TLS_SLOT_UNAVAILABLE` (measured 2026-11-27). Mapping, import binding, the per-module
  TLS slot and the default TLS path are unchanged. This is the hardening captured from board task 254.
- **Shadow parser comparison is wired behind two exact switches.** `PALMETTO_PE_LOADER_SHADOW_PARSE=1` compares the old
  loader's validated header/section/directory facts with `rt::pe::Image` inside `map_image`; non-strict diagnostics are
  bounded (at most 16 entries, 240-byte messages) and named, and `PALMETTO_PE_LOADER_SHADOW_STRICT=1` turns a mismatch into
  a named refusal before allocation. Only the exact value `1` enables either switch, and a strict-only setting is a no-op.
  Default mapping/import/TLS behaviour is byte-identical, and the registered CTest `pe_loader_shadow_wiring` covers the
  semantics end to end: unset switches, strict-only as a no-op, a truthy non-`1` value, parse-only diagnostics, a strict
  mismatch refused by name and a strict match accepted (6 checks, 0 failures, measured 2026-11-27).
- **Loader census instrumentation is registered and now wired.** Board task 263 added the pure `loader_census.h`
  formatter and `loader_census_test` (47 checks, CTest `loader_census`; exact switch `PALMETTO_LOADER_CENSUS`, only value
  `1` enables it). Round 21 applies the `pe_loader.cpp` insertion hunks: the loader emits the bounded mapped/bound/TLS
  lines per module, the `tls-counters` line on destruction and `protect-failed`/`protect-omitted` lines for failed
  `VirtualProtect` calls, and `loader_core_audit` now captures the loader's real stderr and asserts four emitted line
  shapes. With the switch off every emission is guarded, so no line is formatted (round-21 section below).
- **The V2 switch still selects nothing.** `PALMETTO_PE_LOADER_V2` remains unwired; the adapter and the switch are the next
  integration slice, gated on the F4/F7 contract tests.

### Round-20 CTest registrations (2026-11-27)

| target | scope | measured result |
|---|---|---|
| `pe_mapper_parity` | phase-1 falsifier (old loader vs mapper) | passed, 309 checks / 0 failures |
| `pe_loader_shadow_wiring` | P-56 shadow wiring, end to end | passed, 6 checks / 0 failures |
| `loader_tls_race` | F4/F7 gate (board task 264) | round 20 measured 9 checks, 2 reproduced faults under `--expect-faults`; round 21 fixes both, and the strict form (no flag) passes 9 checks, 0 reproduced faults (below) |
| `loader_census` | P-57 formatter layer | passed, 47 checks in round 20; round 21 wires the switch into `pe_loader.cpp` and the emitted lines are asserted by `loader_core_audit` (below) |

Adjacent pure contracts registered in the same round, none of them in the loader path: `ds_publication` (85 checks),
`query_heap_readiness` (7 synthetic cases), `ui_input_script` (5 cases) and `d3d12_residency_budget` (60 checks), all
passing; the GPU CTest `d3d12_title_va` (P-48) is also registered and passed 1/1. The 16/16 CPU loader subset was
recorded in the shared `port/build` on 2026-11-27 before these targets were registered, and the round-20 additions then
ran serially as 9/9 pass; the evidence paths are under the audit section above.

The F4/F7 gate is deliberately an expected-fault test (board task 264). F4 (duplicate install in
`install_on_existing_threads`) reproduces deterministically through a test-only IAT seam: a thread created inside the
loader's thread snapshot after the helper's `DLL_THREAD_ATTACH` already gave it a block receives `DLL_THREAD_DETACH`
twice; a marker-entry fix on a scratch copy of the loader flips it to exactly one detach. F7 (a thread-attach TLS
callback that runtime-loads a TLS module) self-deadlocks between the shared TLS lock and the exclusive lock taken by
`initialize_tls`; the fault child is killed after 4 s while the two controls complete. The target runs with
`--expect-faults` in round 20 while both findings were open (a reproduced fault was REPRO and exit 0). Round 21 fixes
both (below); the strict form is now the gate. Evidence: `D:\leonida-work\tlsrace\report.md` and the round-21 section.

## Round-21 loader state (2026-11-28)

Round 21 closes the loader's open TLS-audit findings and its observability gap. Default mapping, binding, TLS,
unwind and lifecycle behaviour is unchanged; `PALMETTO_PE_LOADER_V2` still selects no path.

- **AUDIT-F4 (duplicate install) is fixed.** `install_on_existing_threads` now records an `event_owned` marker for
  every TLS block the helper's `DLL_THREAD_ATTACH` installed, and its thread-snapshot scan skips a thread already
  served by such a marker. The marker owns no allocation: the thread-local slot still releases the real block and
  receives its callbacks exactly once, and the detach-removal loop skips markers. Measured on the synthetic seam:
  the seam thread and the post-load control each receive exactly one `DLL_THREAD_DETACH`, and the hard precondition
  still proves the helper-event block was installed before the snapshot.
- **AUDIT-F7 (lock-order self-deadlock) is fixed by not holding the TLS lock across mapped callbacks.**
  `on_host_thread_event` and `release_existing_blocks_of_this_thread` collect the initialised modules under the
  shared TLS lock, register a callback flight, release the lock and only then run the `DLL_THREAD_ATTACH`/`DETACH`
  callbacks and `DllMain` notifications; `Loader::~Loader` retires modules under the exclusive lock and drains an
  in-flight callback before process detach and unmapping, so a captured `Module` stays alive. A thread-attach
  callback that runtime-loads a static-TLS module therefore reaches `initialize_tls` without deadlocking. The two
  F7 controls (process-attach load, already-mapped dependency) still exit 0. Ordering is preserved: attach still
  installs and calls in ascending slot order, detach still calls the existing-thread copies first and then the per-slot
  callbacks in reverse, and a callback-free final sweep releases a TLS block loaded into an already-visited slot during
  a detach callback.
- **The census is wired behind `PALMETTO_LOADER_CENSUS=1`.** The exact switch (only `1`; unset, empty and `0` stay
  off) now drives `pe_loader.cpp` itself: one `[rt] loader: census v1 mapped ...`, `... bound ...` and `... tls ...`
  line per mapped module, a `tls-counters attach=.. detach=.. failures=..` line when a `Loader` is destroyed, and
  `protect-failed`/`protect-omitted` lines for failed `VirtualProtect` calls (bounded and named). Asserted, not just
  formatted: `loader_core_audit` redirects its own stderr, sets the switch and checks the mapped, binding, no-TLS and
  TLS-counter line shapes - **52 checks, 0 failures** with the switch on. With the switch off every emission is
  guarded by `loader_census::enabled()`, so nothing is formatted and no counter moves beyond the cached switch check:
  the recorded switch-off comparison on the 46-check synthetic audit measured identical stdout
  (`D99A70C1...D52FD`) and stderr (`E577520F...F230`) for the baseline and the census-patched loader
  (`D:\leonida-work\census\run-baseline.{out,err}` vs `run-off.{out,err}`).
- **`loader_tls_race` is the strict gate.** The test's no-argument strict form now passes: **9 checks, 0 reproduced
  faults**, with `FIXED AUDIT-F4` and `FIXED AUDIT-F7`; a reintroduced fault fails it. The lead-owned registration
  change drops the old `--expect-faults` flag (applied; stanza below).
- **Measured gate set (2026-11-28, shared `port/build`, no GPU and no game files):** `loader_tls_race_test.exe`
  (strict) 9 checks / 0 faults; the registered `loader_core_audit`, `loader_tls_audit`, `loader_selfcheck`,
  `pe_loader`, `loader_tls` and `loader_census` targets **6/6 passed** in 3.75 s; `loader_core_audit` with the census
  switch on **52 checks / 0 failures**. Evidence: `D:\leonida-work\scratch\r21\loader\validation_verified.log` and
  `core_audit_switch_on.log`; the two final-binary runs above are also re-measured in
  `D:\leonida-work\scratch\r21\docs-r21d3\` (`loader_tls_race_strict_final.log`,
  `loader_core_audit_census_final.log`).
- **Boot falsifier (`boot_smoke.md`, 2026-11-28).** On the round-21 binary `5C30DE03...` the census+shadow light arm
  emitted **15 census lines** for the five mapped images (title `base=0x140000000`,
  `total=587 shim=95 host=420 module=72 unimplemented=0`, `tls index=6 slot=0 ga_slot=2 ga_sites=53246`; the four helper
  DLLs each mapped/bound/tls) with **0 shadow divergences**; the baseline arm had the identical loader surface and no
  census lines, and the third arm reached stage 3 and stayed ALIVE 88.2 s with the class-0xa mitigation installed. One
  arm per condition: the census arm's single device loss preceded a title-side stop at `game+0x86F97E`, which is not
  attributed to the loader or to either diagnostic switch. This is the first valid-arm real-title observation of the census line
  shapes; the synthetic assertions above remain the gate, and the shadow comparator's positive control stays the
  synthetic `pe_loader_shadow_wiring` test.
- **Limits carried.** Mapped DLLs without static TLS still get no thread-event delivery, the manual static-TLS slots
  stay capped at 16 and the ended-thread-id history at 1,024; and no real `VirtualProtect` failure was forced, so the
  `protect-failed`/`protect-omitted` output is formatter- and wiring-covered rather than observed end to end. The F7
  flight/drain path is exercised by the thread-event child, but no deterministic arm holds a callback open while
  another thread destroys its `Loader`, and destruction re-entered from that same thread-event callback is not
  supported (the flight would self-wait); that case is not claimed as covered.

### Registration change (lead-owned file, applied)

```cmake
    add_test(NAME loader_tls_race COMMAND loader_tls_race_test)
    set_tests_properties(loader_tls_race PROPERTIES TIMEOUT 180)
```

The round-21 change drops the round-20 `--expect-faults` flag from the registered command, so a reintroduced F4/F7
fault fails CTest. `loader_census`, `loader_core_audit` and the other targets need no stanza change (the census
formatter is header-only and the assertions live in the already-registered audit target).

## Remaining V2 adapter plan (round 21)

The round-20/21 loader work is hardening and observability, not the adapter: `PALMETTO_PE_LOADER_V2` still
selects no path. With F4/F7 fixed and the census wired, the adapter is the next loader slice and its gates are
now all measurable. The plan below is the slice to claim, in order; it is a plan, not a measured result.

1. **Switch and plumbing.** Parse `PALMETTO_PE_LOADER_V2` once (exact `1`, cached like the shadow switches) and
   keep the old `Loader` mapping path as the default and as the fallback for one measured round. No call site
   changes until steps 2-5 pass on the synthetic corpus.
2. **Map through the new components.** In `map_image`, route a `PALMETTO_PE_LOADER_V2=1` image through
   `rt::pe::Image::parse` + `rt::pe::mapper::map` with the production `Allocator`/`Protector` equivalents
   (`VirtualAlloc`/`VirtualProtect` behaviour measured by `pe_mapper_parity`), then feed the file-backed section
   bytes, relocations and protection plan back into the `Module` record the rest of the loader consumes. The
   mapper's non-owning `MappedImage` stays owned by the loader for the module's lifetime.
3. **Binding through the resolver.** Replace the hand-rolled import loop with `rt::pe::resolver::plan_imports`
   behind a `SymbolSource` adapter that answers from the existing shim/host/bundled-module registry, then apply
   the planned IAT writes. The parity gate is the existing synthetic corpus: every accepted name/ordinal/forwarder
   must produce the same final pointer, `Binding` and owning module, and every missing import must keep the current
   `PALMETTO_UNIMPLEMENTED` policy rather than a new refusal.
4. **TLS, unwind and lifecycle keep their measured order.** Install TLS from the parsed directory through the
   existing per-thread block/GA path, keep the F1 named refusal, keep the round-21 F4 marker fix and F7 callback
   drain, register the exception directory through the existing path, and preserve the current order of mapping,
   TLS setup, binding, unwind registration, thread events, callbacks and `DllMain` (`load()` still does not execute
   image code; `initialise()` still does).
5. **Gates before any boot.** Under V2, the loader/TLS/audit/fuzz CTests must pass unmodified, the strict shadow
   comparison should be required (a parser/mapper mismatch is a failure, not a diagnostic), and the census lines
   should match the old path's for the same images. The falsifier is any changed pointer, binding, TLS view,
   protection or refusal on the synthetic corpus.
6. **One measured boot, then retire the old path.** A boot with `PALMETTO_PE_LOADER_V2=1` +
   `PALMETTO_LOADER_CENSUS=1` against the baseline arm must show the same loader surface (preferred base,
   relocated state, bind counts, five modules, TLS view) and no new stop; only then remove the legacy mapping path
   after one measured round.

Known limits to carry into the adapter: the mapper may fall back from the preferred base while every measured
title boot maps at `0x140000000`; the forced-rebase `--dry-run` over the 598,282 title DIR64 relocations is still
unmeasured; and the parity evidence is synthetic, so a title image is the adapter's real acceptance test, not a
substitute for the synthetic gates.
