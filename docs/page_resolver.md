# The page-list resolver: a live physical-page transfer for DirectStorage

**Status: implemented behind `PALMETTO_DS_PAGES`, measured negative on the boot gate.** The console DirectStorage request
can name its destination (or its source) as a list of 64 KiB physical page numbers instead of a linear buffer. Until this
slice our `port/shims/dstorage.cpp` computed the page plan and then refused a live request by name, because `xmem` kept
its page table private. It now installs a page resolver in `port/shims/xmem.cpp`, and the request path transfers through
it with the same plan/copy math the tests already covered. The measured boot issues no page-list request
(`dest-pages=0 source-pages=0` over 8,272 requests, [story_load_probe.md](story_load_probe.md) section 5), so the A/B pair
is a latent negative: the mechanism is inert on today's frontier, and the counters that would prove it works are in
place for the boot that reaches it. The porting policy is [upstream_port.md](upstream_port.md); the preceding advanced
slice is [directstorage_advanced.md](directstorage_advanced.md).

Counts, page numbers, spans, RVAs and HRESULTs only; no package bytes, package paths, shader data or extracted tables.

## 1. Why this is needed

The console ABI in `port/shims/dstorage_xs_abi.h` has `DSTORAGE_REQUEST_OPTIONS.DestinationIsPhysicalPages` and
`.SourceIsPhysicalPages`, and `DSTORAGE_REQUEST` overlays the page array pointer at the linear `Destination`/`Source`
offset with the first-page byte offset just after it. A streamed texture library built for the console names its read
destinations that way, and a request of that shape fails whole before this slice: the plan is computed, then the request
returns `E_NOTIMPL` with "the xmem page table is not exposed, so there is no page resolver for a live request"
(`port/shims/dstorage.cpp`, the pre-slice `page_list_request`).

The measured need today is zero, and the page says so rather than dressing the readiness as progress:

| Measure | Value | Evidence |
|---|---|---|
| Requests in the committed 130 s / 300 s boots | 4,199 / 8,272, all `file -> buffer`, `zlib` on, `bcpack=0 swizzle=0` | [story_load_probe.md](story_load_probe.md) section 5, [streamed_consumer.md](streamed_consumer.md) section 2 |
| Page-list shape counters | `dest-pages=0 source-pages=0` in both boots, refusals 0 | same traces (`PALMETTO_DSTORAGE_TRACE=1`) |
| Slot-31 page-mapping calls | 0 in the 130 s boot (the call site and the page pools exist) | [cpu_view_mapping.md](cpu_view_mapping.md) section 1 |
| Page-pool registrations the title does | four per boot (`XMemAllocatePhysicalPages`, caller `game+0x1E6C2C1`) | [cpu_view_mapping.md](cpu_view_mapping.md) section 1 |

So the slice is proactive: the page pools are built every boot, the page-list ABI is part of the request shape that a
streamed texture library uses, and the refusal was the one named correctness gap left in the advanced request path. The
promotion falsifier is a trace with `dest-pages` or `source-pages` non-zero on an arm with `PALMETTO_DS_PAGES=1`; that
boot must show the transfer counters move and every status `S_OK` (section 6).

## 2. Measured shapes the resolver has to answer

| Measure | Value | Evidence |
|---|---|---|
| Page unit | 64 KiB; a page number is slot * 16, slot in [1, 2^21), 0 = an unmapped entry | `port/shims/xmem.cpp` (the page allocator), measured at the title's four `XMemAllocatePhysicalPages` sites |
| Page backing | one pagefile-backed section per `XMemAllocatePhysicalPages` call, committed on touch, mapped into reserve-only windows | `port/shims/xmem.cpp` |
| Page-list plan | a page array plus a byte offset into the first page; only the pages the transfer needs are read | `plan_page_transfer` in `port/shims/dstorage_advanced.h` |
| Source page list | a page list only makes sense for a memory source (a file read is linear) | ABI shape, and the upstream check (section 4) |
| Request shape in the boot | the page arrays are never named; `Destination` is a process-heap buffer | [streamed_consumer.md](streamed_consumer.md) section 2 |
| Title page windows | reserve-only 64 KiB windows; a mapped page is a `MapViewOfFile3` view of the page's section | `port/shims/xmem.cpp`, `copy_page_mappings` |
| Test shape | real pages and real windows without a GPU (`xmem_test`), synthetic page tables (`dstorage_test`) | section 7 |

The plan's fixed table holds 32 spans, so a page-list transfer moves at most about 2 MiB (32 x 64 KiB, minus the first
page offset). That bounds the scratch buffer the request path materialises and the number of pages one request can name;
a request needing more spans is refused as `too_many_pages` rather than truncated.

## 3. What we implemented

Two layers, split where the ownership already was:

**The resolver (xmem).** `port/shims/xmem.h` declares `rt::xmem::resolve_physical_page_fn`, a hook in the same shape as
the slot-31 `copy_page_mappings_fn`: `page number -> the writable CPU address of that page's 64 KiB backing`, or null for
a number that is not a live page (0, never allocated, freed). `port/shims/xmem.cpp` installs it. The resolver commits the
page's backing before returning it (`SEC_RESERVE` pages are committed on touch; a page allocated by the title but not yet
touched is still reserved, and the fault handler knows mapped views, not the backing), with `PAGE_READWRITE` as
`commit_physical_pages` already does. A null hook keeps the request path's named refusal, so a GPU-free build cannot
silently read a page table it does not have.

**The transfer seam (dstorage).** `port/shims/dstorage_advanced.h` gains `transfer_page_list`: it runs the tested plan,
then resolves every page the plan names before the first byte moves, and only then copies the bytes in or out of the
caller's scratch buffer. Resolving first is the difference that matters for a destination page list: the pages are real
title memory, so a half-applied scatter would be visible to the title. A resolver miss returns `unresolved_page` with the
page number, and every page and the buffer are untouched. The named results are `no_resolver`, `null_buffer`,
`unresolved_page`, `empty`, `zero_page`, `too_many_pages`, `too_short` and `offset_out_of_range`.

**The request path (`port/shims/dstorage.cpp`, `page_list_request`).** Behind `PALMETTO_DS_PAGES=1` (off by default, one
cached boolean when unset) the request path now:

1. validates the shape: not both page lists, a source page list only on a memory queue, a non-null page array, no
   BCPack/swizzle over a page list, no intermediate size, the 1 GiB source+destination cap, the decompression size rule
   (`DestinationSize > SourceSize` with zlib, equal without), and the 32 MiB rule only for a linear destination;
2. snapshots only the page numbers the plan needs (bounded to the 32-span table + 1) with the existing guarded copy, so
   an inaccessible page array is a named `E_DSTORAGE_ACCESS_VIOLATION`;
3. plans and counts (`pages planned`), refusing by the plan's own error names for a 0 entry, a short list or too many
   spans;
4. materialises the source bytes in the queue worker's own vector: a file read, a memory copy, or a gather out of a
   source page list; then the zlib stage inflates to exactly `DestinationSize` (a malformed stream leaves every page as
   it was);
5. publishes: scatters into the destination page list through the resolver, or copies into a linear destination for a
   source page list; a transfer success counts `pages transferred` and `pages bytes` and logs one bounded line.

**Two corrections the live path needed.** `process()` reported every completed request's `Destination` as a streamed
block to the device-side registry ([streamed_consumer.md](streamed_consumer.md) section 1). For a page-list request that
field is the *page array*, not a data range, so the registry now skips a request with `DestinationIsPhysicalPages`; a
future page-list transfer can therefore not be mistaken for 64 KiB of streamed content. The trace's `advanced:` line now
prints the page counters as `pages planned=... transferred=... bytes=... refused=...`.

**The single-VA design decision: no second view.** [cpu_view_mapping.md](cpu_view_mapping.md) section 2 records why this
tree needed no paired CPU view for queue slot 31: upstream's layer reads a title GPU-only reservation at a separate,
paired CPU reservation, so #968 maps the slot-31 copy at both addresses; our `xmem` maps host memory at the console VA
itself and the D3D12 layer reads those same pages by that VA
([page_address_space.md](page_address_space.md)), so the one mapping *is* the CPU view. The page list is the same
argument one level down: a page number names a physical page, the page has exactly one backing, and every title mapping
of it is a view of that backing. Writing through the resolver's view is therefore visible at the title's mapping with no
second address to fill, and the tests prove that direction (section 7). If a future layer ever reads title memory through
a separate alias or keeps a mirror copy of title pages, this resolver is where its invalidation has to be added - the
single view is a property of today's design, not a law.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida>), used as an evidence source, not a code source. The page-list behaviour was
read at commit `273b3bdf9` (Moriz82), files `runtime/shims/dstorage.cpp` (the request validation, `page_transfer`,
`prepare`/`publish` and its staging pool) and `runtime/shims/xmem.cpp` / `runtime/shims/xmem.h`
(`rt::xmem::transfer_physical`), and the slot-31 mapping at `0aecc5efa` (#968, CLHelper; the paired CPU-view half is
`255f17001`, Jan Trencansky). The reference's own docs (its `directstorage.md` and the "Where it stops" section of its
`linux_proton.md`) state the page-list scatter/gather requirement and the two gaps it still lists after #968 (mappings
change at call time rather than in queue order; ranges no mapping call reached).

**Where it agrees.** A page list is 4 KiB frame numbers of 64 KiB pages plus a byte offset into the first page; only the
pages the transfer needs are used; every page is resolved/committed before the destination write starts; a page number
the page table does not know refuses the transfer; a source page list is a memory-source shape; and the decode stages run
between the source read and the destination publish (the reference's zlib/BCPack/unshuffle pipeline, our zlib stage).

**Where it differs, and why.**

1. **One function versus two layers.** The reference's `transfer_physical(pages, offset, staging, write)` resolves the
   pages, commits them, copies under its page-table lock and notifies its write watches, all in xmem. Our page plan and
   copy math were already the tested seam in `port/shims/dstorage_advanced.h` (the advanced slice shipped them as pure
   arithmetic), so the resolver is an xmem export and the request path owns the transfer ordering, staging and refusals.
   The page-table lookup stays in xmem, the request semantics stay in dstorage.
2. **No paired CPU view.** Section 3: the reference fills its GPU-only/CPU-paired pair at both addresses; this tree has
   one address, so the resolver returns the page's single backing and there is no second mapping or watch notification to
   make. This is the #968 comparison applied to page lists: the paired view was needed for the reference's separate
   reservations, not for the console semantics.
3. **The refusal is by page number before any write.** The reference commits the pages and fails the transfer on a page
   it cannot commit; ours resolves the whole plan first and reports `unresolved_page` with the offending page number, so
   the request fails with the destination provably unchanged. Both preserve the no-partial-destination rule; ours names
   the page.
4. **Switch default.** The reference's extended DirectStorage path is on unless `LEONIDA_DSTORAGE_EXTENDED=0`; our slice
   starts behind `PALMETTO_DS_PAGES=1` because the measured boot asks for nothing (policy: a new mechanism starts off and
   states what would make it default).
5. **Page list + BCPack/swizzle refused by name.** The reference composes both decode stages with the page-list publish.
   Ours supports zlib over a page list, and refuses BCPack/swizzle over one by name: those stages in this tree write a
   linear destination, and composing them with the scatter is a follow-up seam, not a guess.

## 5. A/B measurement

One 130 s boot pair, one boot at a time under the repository's boot lock, harness `tooling/boot_report.py`, same
executable built from the shared working tree (other slices in flight are in both arms; the differential is only this
switch). Both arms: the standard environment (`PALMETTO_SHADERS=<shader corpus>`, `PALMETTO_D3D12=vulkan`,
`PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_DSTORAGE_TRACE=1` and
`PALMETTO_PIPELINE_TIMELINE=5`. Arm A is the default (no `PALMETTO_DS_*`); arm B adds `PALMETTO_DS_PAGES=1`.

| Measure | A: default | B: `PALMETTO_DS_PAGES=1` |
|---|---|---|
| Verdict | ALIVE 130.3 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| Traced D3D12 calls | 52,332 recorded | 52,405 recorded |
| Requests and shape | 5,123 requests at the trace's last shape print: file sources, `zlib=5,002`, `bcpack=0 swizzle=0`, dest-pages=0, source-pages=0 | identical: 5,123 requests, same shape counters |
| Decompressed | 354.3 MiB; the first 4,096 (256.0 MiB) in 1.8 s (140.9 MiB/s) | 354.3 MiB; the first 4,096 (256.0 MiB) in 1.7 s (147.2 MiB/s) |
| DirectStorage entry failures / refusals | 0 / 0 | 0 / 0 |
| Pages planned / transferred / bytes / refused | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Population at +125 s | slot57 10,241 converted, 0 refused; slot55 41,545; graphics created 51,786; compute 134 calls / 115 created; submits 80; presents 39; threads 95; console calls 107,240 | same slot57/slot55/graphics counts; compute 110 calls / 96 created; submits 76; presents 37; threads 96; console calls 107,159 |
| Replay checkpoint | submission 16 at 2.4 s | submission 16 at 2.4 s |

**Reading it honestly.** The counters that must move if a page-list request ran did not move, because no page-list request
exists in either arm: `dest-pages=0` and `source-pages=0` at every shape print, `pages planned=0`, `pages
transferred=0`, `pages bytes=0`, `pages refused=0` on the last advanced line of both arms, no refusal line and no
success line. The request shapes, the byte count, the failure count, the loading state (`replay: submission 16` at
2.4 s in both) and the pipeline population are equal; the differences that remain are boot-to-boot scheduling variance
on small counters (compute creations 115 vs 96, presents 39 vs 37, submits 80 vs 76, threads 95 vs 96, traced calls
52,332 vs 52,405) and one pair cannot resolve them. Verdict: **unchanged, a latent negative** - the mechanism is ready
and counted, and the boot today never asks for it.

Two honesty notes on the numbers. (1) The absolute frontier of this pair is not the committed pages' frontier (the
committed 300 s boot measured 8,272 requests / 498.9 MiB, [story_load_probe.md](story_load_probe.md) section 5, and the
committed 130 s boot measured ~25,700 submits and ~12,850 presents at +125 s,
[directstorage_advanced.md](directstorage_advanced.md) section 5). The working tree at boot time carried other slices'
uncommitted changes (`port/shims/d3d12_com.cpp`, `port/shims/d3d12_vulkan_device.cpp`, `port/src/graphics_pso.*`,
`port/src/main.cpp`, `port/src/vulkan_*.h` are dirty), so the absolute load and frame counts belong to that hybrid tree.
Both arms ran the same executable, which is what makes the differential this slice's; the page says so rather than
comparing across trees. (2) The switch adds one cached boolean read in `execute_request` for a request whose options name
no page list (the branch already existed for the refusal before this slice), so a no-page-list boot cannot be slowed by
it; that is consistent with the two arms' counters.

## 6. Limits

- **No measured request.** The boot does not ask for a page list, so the A/B pair can only show the mechanism is inert
  on the default boot and that the switch moves no counter. The falsifier that promotes it is a trace with `dest-pages`
  or `source-pages` non-zero; that boot must show `pages transferred` non-zero, every status `S_OK` and no new refusal.
- **BCPack and swizzle over a page list are refused by name** (zlib is supported). Composing the decode stages with the
  scatter is the next increment; `port/shims/dstorage_advanced.h`'s staging and predictor paths already own those stages.
- **The page-list stage does not use the queue staging area** (`PALMETTO_DS_STAGING`): the transfer materialises its
  source in an owned vector, bounded by the 32-span plan to about 2 MiB. Wiring the page-list stage into the staging
  switch is a smaller follow-up than the decode composition.
- **A linear destination keeps the PC 32 MiB rule.** A larger destination must be a page list; this matches the existing
  linear paths.
- **Page liveness is the console contract's.** The resolver returns the backing address and does not pin the page's
  section; a title that frees a page number while its own request is transferring is out of contract (the console keeps
  an enqueued request's page list live until its status completes). A concurrent free would invalidate the address; the
  request path does not defend against a contract violation.
- **PAGE_GUARD/write-watch hand-off is linear-destination only.** `PALMETTO_DS_PIN` releases the stream consumer's
  `PAGE_GUARD` on a linear destination before writing and re-arms it after. A page-list write goes to the page backing,
  not through the title's mapped view, so a guard armed on that view is not handed back. There is no measured page-list
  request and no guard on one, but a future streamed page-list shape that also wants the hand-off must extend the pinner
  with the resolver's views.
- **No second view**, by design (section 3). If a future alias/mirror layer reads title pages through another address or
  keeps a copy, this resolver is where its invalidation has to be added.
- **The reference's post-#968 gaps are unchanged here.** The mapping is applied when the copy runs, not in queue order,
  and a page list the title remaps after `Submit` is used as it is at execution time (the page numbers are snapshotted on
  the worker, as the reference also snapshots); neither is exercised by a measured request yet.

## 7. Tests

**`port/test/dstorage_test.cpp`, target `dstorage_test` (GPU-free, no game data): 224 checks, 0 failed** (213 before
this slice; the page-list section gained 11). New checks, on synthetic page tables through the request path's own seam:

- a live write transfer resolves two spans and reports the plan, and the bytes are visible at the planned page offsets;
- a live read transfer gathers the same bytes back;
- a page number with no live backing is `unresolved_page` with that page number, and **every page is left untouched**;
- a null resolver (the hook-off build) is `no_resolver`, and every page is left untouched;
- the plan's own refusals surface through the transfer: `zero_page`, `too_many_pages`, `empty`, `null_buffer`.

The plan shapes, the copy math both ways and the existing refusals stay as they were.

**`port/test/xmem_test.cpp`, target `xmem_test` (real physical pages and windows, no GPU): 258 checks, OK.** New checks:

- the resolver hook is installed;
- a live page number resolves to a CPU view that holds what the title's mapping holds (one memory);
- a write through the resolved view is visible at the title's mapped view;
- an allocated but never-touched page resolves and is committed on the way out, and mapping it afterwards shows the
  resolver's bytes;
- the request path's own `transfer_page_list` over real pages scatters a 100,000-byte payload and reads it back through
  the title's mappings, and a page number that was never allocated is `unresolved_page` with the mapped pages unchanged;
- page 0, a never-allocated number and a freed page number do not resolve.

```powershell
cmake --build port\build --target xmem_test dstorage_test -j 24
port\build\xmem_test.exe          # xmem_test: OK (258 checks)
port\build\dstorage_test.exe      # 224 checks, 0 failed
```

The full-request path is covered end to end at the unit level; a boot cannot cover it until the title issues a page-list
request.

## 8. Attribution

This slice was implemented from this project's own measurements: the DirectStorage request trace in
[story_load_probe.md](story_load_probe.md) and [streamed_consumer.md](streamed_consumer.md), the page table, placeholder
and mapping machinery already in `port/shims/xmem.cpp`, and the ABI in `port/shims/dstorage_xs_abi.h`. The behaviour was
cross-checked against the public GPL-3.0 project `ubiquitarianism/leonida`
(<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/shims/dstorage.cpp`,
`runtime/shims/xmem.cpp`, `runtime/shims/xmem.h` and the slot-31 mapping files, commits `273b3bdf9` (Moriz82) and
`0aecc5efa` (#968, CLHelper; `255f17001`, Jan Trencansky, for the paired CPU view this tree does not need), and the
reference's own docs (its `directstorage.md` and its `linux_proton.md`). No code from that project was copied. The
upstream project remains the copyright of its contributors and its license continues to apply to its own tree. The
`NOTICE.md` credit for this slice is added by the policy slice that owns that file ([upstream_port.md](upstream_port.md)
section 6).
