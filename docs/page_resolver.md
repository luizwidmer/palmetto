# The page-list resolver: a live physical-page transfer for DirectStorage

**Status: the live resolver and transfer are implemented behind `PALMETTO_DS_PAGES`; the optional page-watch hand-off is
behind `PALMETTO_DS_PAGES_VIEW`. The title-request frontier remains a measured negative, while a synthetic request arm
covers the destination path.** The console DirectStorage request can name its destination (or its source) as a list of
64 KiB physical page numbers instead of a linear buffer. The resolver in `port/shims/xmem.cpp` backs the tested plan/copy
path in `port/shims/dstorage_advanced.h` and `port/shims/dstorage.cpp`. With `PALMETTO_DS_PAGES_VIEW=1` (and
`PALMETTO_DS_PAGES=1`), a physical-page destination instead goes through xmem's bounded page-write hook so armed mapped
views report the write to the page tracker. The title itself still issues no page-list request: its later 9,216-request
trace is `file -> buffer`, with `dest-pages=0 source-pages=0` ([peer_port_streaming.md](peer_port_streaming.md) section
13.1). A separate `PALMETTO_DS_PAGES_ARM` synthetic request is documented and measured in sections 14.3-14.4 there; its
two page-list requests are the arm's, not the title's. See sections 6, 7 and 9 here for current limits, test results and
the remaining falsifier. The porting policy is [upstream_port.md](upstream_port.md); the preceding advanced slice is
[directstorage_advanced.md](directstorage_advanced.md).

Counts, page numbers, spans, RVAs and HRESULTs only; no package bytes, package paths, shader data or extracted tables.

## 1. Why this is needed

The console ABI in `port/shims/dstorage_xs_abi.h` has `DSTORAGE_REQUEST_OPTIONS.DestinationIsPhysicalPages` and
`.SourceIsPhysicalPages`, and `DSTORAGE_REQUEST` overlays the page array pointer at the linear `Destination`/`Source`
offset with the first-page byte offset just after it. A streamed texture library built for the console names its read
destinations that way, and a request of that shape fails whole before this slice: the plan is computed, then the request
returns `E_NOTIMPL` with "the xmem page table is not exposed, so there is no page resolver for a live request"
(`port/shims/dstorage.cpp`, the pre-slice `page_list_request`).

The measured need in title-originated requests is still zero; the synthetic arm is reported separately rather than treated as title demand:

| Measure | Value | Evidence |
|---|---|---|
| Requests in the earlier committed 130 s / 300 s boots | 4,199 / 8,272, all `file -> buffer`, `zlib` on, `bcpack=0 swizzle=0` | [story_load_probe.md](story_load_probe.md) section 5, [streamed_consumer.md](streamed_consumer.md) section 2 |
| Latest follow-up title request shapes | 9,216 `file -> buffer`, `zlib=6,005`, `bcpack=0 swizzle=0`; title-originated page-list flags remain 0 | [peer_port_streaming.md](peer_port_streaming.md) section 13.1 |
| Synthetic arm requests, not title requests | Two memory-source/destination-page-list requests: one 100,000-byte transfer and one missing-page refusal; `pages transferred=1`, `pages refused=1` | [peer_port_streaming.md](peer_port_streaming.md) sections 14.2-14.4 |
| Page-list shape counters in title population | `dest-pages=0 source-pages=0`; the arm's two page-list requests are synthetic | [story_load_probe.md](story_load_probe.md) section 5, [peer_port_streaming.md](peer_port_streaming.md) sections 13.1 and 14.4 |
| Slot-31 page-mapping calls | 0 in the 130 s boot (the call site and the page pools exist) | [cpu_view_mapping.md](cpu_view_mapping.md) section 1 |
| Page-pool registrations the title does | four per boot (`XMemAllocatePhysicalPages`, caller `game+0x1E6C2C1`) | [cpu_view_mapping.md](cpu_view_mapping.md) section 1 |

So the resolver covers a page-list shape named by the console ABI, while the measured title population remains linear.
The original `E_NOTIMPL` page-list refusal is removed behind `PALMETTO_DS_PAGES=1`; its title-originated promotion
falsifier is a trace with `dest-pages` or `source-pages` non-zero, non-zero transfer counters and every status `S_OK`. To
validate the optional page-watch hand-off, the run must also set `PALMETTO_DS_PAGES_VIEW=1` and show a `changed` compare
for a watched texture whose pages were delivered (section 9).

## 2. Measured shapes the resolver has to answer

| Measure | Value | Evidence |
|---|---|---|
| Page unit | 64 KiB; a page number is slot * 16, slot in [1, 2^21), 0 = an unmapped entry | `port/shims/xmem.cpp` (the page allocator), measured at the title's four `XMemAllocatePhysicalPages` sites |
| Page backing | one pagefile-backed section per `XMemAllocatePhysicalPages` call, committed on touch, mapped into reserve-only windows | `port/shims/xmem.cpp` |
| Page-list plan | a page array plus a byte offset into the first page; only the pages the transfer needs are read | `plan_page_transfer` in `port/shims/dstorage_advanced.h` |
| Source page list | a page list only makes sense for a memory source (a file read is linear) | ABI shape, and the upstream check (section 4) |
| Title-originated request shape | no page arrays on the latest 9,216 title requests; `Destination` is a linear process-heap buffer | [peer_port_streaming.md](peer_port_streaming.md) sections 13.1 and 14.4 |
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
page number, and all page bytes and the buffer remain untouched. The named results are `no_resolver`, `null_buffer`,
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
5. publishes: by default, scatters into a destination page list through the resolver; when `PALMETTO_DS_PAGES_VIEW=1`
   (which requires `PALMETTO_DS_PAGES=1`), sends that destination through xmem's bounded page-write hook so armed
   mappings are recorded by the page-watch tracker; for a source page list, copies into the linear destination. A
   successful transfer counts `pages transferred` and `pages bytes` and logs one bounded line.

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
   one backing per physical page, so no additional CPU alias is needed for visibility. The later `PALMETTO_DS_PAGES_VIEW`
   route uses existing armed title mappings when it needs a page-watch signal; it does not create a paired view or a second
   mapping (sections 6 and 9). This is the #968 comparison applied to page lists: the paired view was needed for the
   reference's separate reservations, not for the console semantics.
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

- **No measured title-originated page-list request.** The section-5 A/B pair and the later load population contain none;
  the diagnostic arm injects two synthetic requests (one transfer, one named refusal), which proves the request branch but
  not title demand. The promotion falsifier is a title-originated trace with `dest-pages` or `source-pages` non-zero; it
  must show `pages transferred` non-zero, every status `S_OK` and no new refusal.
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
- **The page-watch hand-off depends on the destination switch.** With `PALMETTO_DS_PAGES_VIEW` unset,
  `PALMETTO_DS_PAGES=1` keeps the resolver scatter into the physical-page backing; a `PAGE_GUARD` armed on a mapped title
  view does not see that store. With both switches on, a page-list destination goes through xmem's bounded
  `write_physical_pages` hook, which resolves and commits every planned page before writing and repeats the bytes through
  every armed mapped view so the tracker records the delivery. The view switch does not affect source-page gathers. A
  missing write hook or unresolved page is a named refusal, not a fallback. This is an opt-in extension of the original
  limit, not evidence that the title requests page lists: title-originated `dest-pages` and `source-pages` remain zero in
  the measured frontier (section 9).
- **No additional CPU/GPU alias or mirror is created** by this resolver (section 3). The optional page-watch hook uses
  existing mapped title views only to record writes; it does not create another mapping. If a future alias/mirror layer
  keeps a separate copy of title pages, this resolver is where its invalidation has to be added.
- **The reference's post-#968 gaps are unchanged here.** The mapping is applied when the copy runs, not in queue order,
  and a page list the title remaps after `Submit` is used as it is at execution time (the page numbers are snapshotted on
  the worker, as the reference also snapshots); neither remap-timing case is exercised by the synthetic arm or a
  title-originated page-list request yet.

## 7. Tests

**Current host-test result from the existing binaries (this reconciliation did not build or relink `palmetto`):**
`port/build/dstorage_test.exe` passed **261 checks, 0 failed**; `port/build/xmem_test.exe` passed **496 checks**. The
older 224/258 totals from the first resolver slice were snapshots, not the current test inventory.

- `dstorage_test` keeps the pure plan shapes and copy math for both directions, and tests `transfer_page_list` against a
  synthetic resolver: unresolved pages are named and leave payload/page bytes unchanged; null resolver, zero page,
  too-many-pages, empty and null-buffer cases refuse. Its page-write request-shape checks validate the 4,321-byte offset,
  two planned spans, span-to-hook conversion, refusal shape and `PALMETTO_DS_PAGES_ARM` switch parsing.
- `xmem_test` exercises real allocated physical pages and mapped windows: resolver aliasing/commit behavior, scatter and
  gather through the transfer seam, missing/freed-page refusal, and the bounded physical-page write hook. Its mapped
  destination fixture checks armed aliases, recorded watch pages, `changed`/drained polls, and all-or-nothing refusal for
  an unallocated page.
- The real `page_list_request` branch is covered separately by the opt-in synthetic `PALMETTO_DS_PAGES_ARM`: the existing
  fixture is 21/21 PASS, and section 14.4 records one prior 130.2 s light boot with 0 exceptions and 0 device losses.
  Those arm requests are synthetic; they do not demonstrate a title-originated page-list request.

```powershell
cmake --build port\build --target xmem_test dstorage_test -j 24
port\build\xmem_test.exe          # xmem_test: OK (496 checks)
port\build\dstorage_test.exe      # 261 checks, 0 failed
```

The transfer seam and xmem hook have host coverage, and the actual request branch has the synthetic arm coverage above.
The remaining promotion falsifier is a title-originated request with `dest-pages` non-zero, successful transfer and status,
and—when testing the watch path—a `changed` compare for the delivered watched texture (section 9).

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

## 9. Reconciliation: current request coverage and remaining falsifier

This addendum reconciles this resolver page with the later mapped-destination work in
[peer_port_streaming.md](peer_port_streaming.md) sections 13-14. It records the current code and existing test results; it
adds no runtime code.

| Request shape | Current coverage and limit |
|---|---|
| Title's measured load requests | The latest light population is 9,216 `file -> buffer` requests (`zlib=6,005`, BCPack/swizzle off); title-originated `dest-pages=0 source-pages=0` ([peer_port_streaming.md](peer_port_streaming.md) section 13.1). |
| Memory source page list -> linear destination | Accepted only on a memory-source queue; page bytes are gathered through xmem before optional zlib and linear publish. A read-side page list does not notify the write-watch tracker. |
| File or memory source -> destination page list | Behind `PALMETTO_DS_PAGES=1`; page numbers are bounded to the plan (32 spans, about 2 MiB), all needed pages are checked before publishing, and an optional zlib result is scattered. `PALMETTO_DS_PAGES_VIEW=1` selects xmem's page-write hook to record writes through armed mappings; with it unset the resolver scatter remains the default page-list path. |
| Unsupported or invalid shapes | Both page-list flags, a null list, or an unresolved page are named refusals; a source page list on a file queue returns `E_DSTORAGE_INVALID_SOURCE_TYPE`; BCPack/swizzle over a list or a missing resolver/write hook returns `E_NOTIMPL`; an inaccessible array returns `E_DSTORAGE_ACCESS_VIOLATION`. Page-plan failures are logged by name; the existing 1 GiB request cap (source plus destination for memory-source requests), 32 MiB linear-destination cap and 32-span page-plan cap bound the transfer. |

The checked current source is `page_list_request` in `port/shims/dstorage.cpp` and the resolver/write hooks in
`port/shims/xmem.cpp`. Existing binaries passed: `xmem_test` **496 checks, OK** and `dstorage_test` **261 checks, 0
failed**. The synthetic `PALMETTO_DS_PAGES_ARM` exercises the real request branch only when both page switches are on;
[peer_port_streaming.md](peer_port_streaming.md) section 14 records its 21/21 GPU-free fixture, and one prior 130.2 s
light arm with 0 exceptions and 0 device losses. In that arm, the two page-list requests were synthetic: one delivered
100,000 bytes across two spans and one failed all-or-nothing on an unmapped page. This reconciliation did not build or
relink `palmetto` and did not repeat the arm.

**Remaining falsifier:** a title-originated destination-page request must make `dest-pages` non-zero and show a completed
transfer/status; to establish the optional page-watch hand-off, the same run must also show a `changed` compare for the
watched texture whose pages were delivered. The synthetic arm validates the request path, but does not change the measured
title population or justify enabling either switch by default. The page-write follow-up and its peer cross-check are
already attributed in [peer_port_streaming.md](peer_port_streaming.md) section 13.6 and the existing `NOTICE.md` row; this
reconciliation introduces no new peer mechanism.
