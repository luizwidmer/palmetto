# PE-loader rewrite

The runtime maps the console image itself (`port/src/pe_loader.cpp`: a 110 KiB translation unit that parses, maps,
relocates, binds imports, installs static TLS, registers unwind data and drives module lifecycle in one file). The
observable behaviour is well tested (`loader_test`, `pe_loader_test`, `test_pe_loader_bounds`, `pe_loader_fuzz`,
`loader_tls_test`, the tiny-game CTests), but the implementation grew by accretion: header parsing is interleaved with
mapping, the import/export tables are read through the same ad-hoc pointer arithmetic as the loader, and several
compatibility rules live only in the ordering of statements. This page tracks replacing it with a layered, bounds-safe
implementation without changing what the loader does.

**Rules.** Every slice is our own implementation, keeps the old loader as the reference until parity is demonstrated, adds
synthetic tests, keeps `PALMETTO_OK`/`TESTS_BUILT` and the documentation checks green, and never uses game bytes as test
data. The peer ports are evidence only (their loader notes are credited in `NOTICE.md` through the campaign entries);
this rewrite is driven by the local behaviour and tests.

## Target layering

| layer | responsibility | state |
|---|---|---|
| `port/src/pe_image.{h,cpp}` | decode a PE32+ file into a validated description: headers, sections, directories, exports (including aliases and forwarders), imports (name/ordinal + IAT slots), base relocations, TLS template/index/callbacks, exception table. No mapping, no code, no host calls. | **phase 0 landed** (`pe_image_test`) |
| `port/src/pe_mapper.{h,cpp}` | reserve/commit the image, copy headers and file-backed section data, apply base relocations with the existing type rules, protect sections (W^X), make the unwind table. | next |
| `port/src/pe_resolver.{h,cpp}` | bind imports and exports through a resolver interface: shim registry, host modules, bundled modules, forwarders, ordinals, runtime loads. Keeps the current `Binding` vocabulary. | after mapper |
| `Loader` adapter | static TLS, thread events, callbacks, module registry, lifecycle and the console-type hooks stay on the proven implementation; the adapter feeds them the parsed description instead of re-parsing. | after resolver |
| switch | `PALMETTO_PE_LOADER_V2=1` routes through the new layers; default stays the old path until the parity gates pass. | last |

## Invariants the rewrite must preserve

- Mapping never runs image code; `load()` maps and installs TLS, `initialise()` runs TLS callbacks and DllMain.
- The old loader's acceptance rules are the baseline, with these deliberate hardenings (each listed as a possible
  compatibility risk and covered by a fixture): overlapping sections are refused, every data directory must lie inside
  the image, export aliases (several names for one function) are represented instead of dropped, and TLS template data
  must be file-backed. Any real module that trips one of these is evidence for an explicit exception, not a silent
  relaxation.
- Every RVA read is bounds-checked against the mapped file and the image size; errors name the image and the shape
  (`synthetic.dll: section .rdata reads past the end of the file`).
- The exported `rt::Loader` API stays source-compatible; the shim registry, device slots and console-type hooks keep their
  current behaviour.
- No test data from the game: fixtures are built by the tests (`port/test/pe_image_test.cpp` builds a complete synthetic
  image, including export aliases/forwarders, imports by name and ordinal, relocations, TLS callbacks and exceptions).

## Slices and falsifiers

1. **Parsing core (landed in this slice).** `pe_image_test` covers the valid synthetic image and refusal shapes: bad DOS/NT
   signatures, wrong machine, PE32 (no `+`), zero image size, entry point outside, headers/sections past the file or image,
   section overlap, export ordinal outside the function table, function/import RVA outside the image, malformed
   relocation block, TLS end-before-start and callback table outside the image, odd exception directory, and the
   empty-export/absent-import shapes.
2. **Mapper.** Falsifier: map the same synthetic image with the old `Loader` and the new mapper, compare the module base,
   size, entry point, imported IAT values and protected section ranges; the old `loader_test`/`pe_loader_test` fixtures
   become the oracle. Success criterion: identical results and identical refusals for the malformed corpus.
3. **Resolver/importer.** Falsifier: a synthetic module importing by name, ordinal and forwarder resolves through both
   paths to the same function pointers; an unimplemented import keeps the current `PALMETTO_UNIMPLEMENTED` naming.
4. **TLS/lifecycle adapter.** Falsifier: `loader_tls_test`, the tiny-game TLS/EH CTests and the fuzz target pass unchanged
   with `PALMETTO_PE_LOADER_V2=1` on synthetic images; then one title boot arm checks the mapped module population and the
   stage-3 frontier are unchanged.
5. **Default switch.** Only after 1-4 pass in CI: make the new path the default, keep the old path behind
   `PALMETTO_PE_LOADER_LEGACY=1` for one round, then remove it.

## What differs from the old loader, deliberately

- Parsing, mapping and resolution are separate functions with a validated description in between; the module code no
  longer walks raw headers while it maps.
- Export aliases are emitted as separate symbols with the same RVA/ordinal; the old table could only name a function once.
- Every directory is checked to lie inside the image, and overlapping sections are refused.
- Errors carry the failing structure and always start with the image name; the current loader emits several variants.
- The rewrite does not change TLS, thread-event, unwind registration or console-type behaviour; those are kept and fed
  from the parsed description.

## Current status and the next measurable step

Phase 0 is the only landed slice. `pe_image_test` is registered as `pe_image` and is pure CPU (no GPU, no game image). The
next slice is the mapper with the old loader as the oracle; its first falsifier is a field-by-field comparison on the
synthetic fixtures already used by `loader_test`/`pe_loader_test`, followed by the malformed-image corpus to prove the
refusals match.
