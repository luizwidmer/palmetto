# Queue slot 31: the page-mapping call, mapped through xmem

**Status: implemented, awaiting the loader stage that uses it.** The title's page-mapping call (`CopyPageMappingsBatchX`,
console queue slot 31) is now decoded and answered through `rt::xmem`, which gives each destination page a second view of
the physical page already mapped at its source page. On this tree the call is not reached in a boot yet, so the honest
result of the A/B measurement is "no difference, 0 calls" - the implementation is the forward-looking half, and the
static call site is re-derived here from our own copy of the retail executable.

Counts, RVAs, sizes and status codes only; no package bytes, paths, shader data or extracted tables.

## 1. The need, re-derived from our own copy of the executable

**Static.** Our copy of the retail executable calls a console queue method at `game+0xF2CC7F` through the queue's
`[+0xF8]` entry, i.e. **console queue slot 31** (the console queue is two slots longer than the PC one before
`Signal` - `docs/console_slots.md` section 4 - and our measured `PresentX` is slot 32, so 31 is the method between
`GetDesc`-shifted tail and `PresentX`). The call passes five arguments in the measured console order: the batch count,
the batch array, the physical-page pool's base VA, the pool's length in 64 KiB pages, and 0 as flags; the instruction
after the call does not test its result. The function that builds the call (`game+0xF2C460..0xF2CD18`) writes:

- a **batch** every 24 bytes: `{UINT64 destination; UINT32 range count; const range* ranges}`;
- a **range** every 12 bytes: `{UINT32 kind, start, count}`, with the batch cursor moving on by `count` pages per range.

The pools those batches name come from the page-pool setup at `game+0x1E6C110`: `XMemAllocatePhysicalPages(0x380,
&count, pageArray)`, an `XMemVirtualAlloc` reserve-only 64 KiB window (`0x20402000`, attributes `0x23`, protect
`0x2004`), `XMemMapPhysicalPages(window, count, pageArray)`, and a console device call at slot 74 (`[device+0x250]`) that
registers the pool. The call census of a 130 s boot sees four of those pool registrations (device slot 74, caller
`game+0x1E6C2C1`), so the title builds its page pools in every boot. The kinds the batch builder emits in our copy are
1 (one pool page at the run's destination pages) and 2 (unmap the destination pages); the others are covered by the
decode below.

**Runtime.** In the same 130 s boot the title calls the queue only at slots 0, 1, 16, 18, 32 and 35: **no slot 31
call is made** (`boot-slot31`, and the earlier call census in `census.md`). The loading stage is where the upstream
measurement sees the call (below), and in our tree that stage stops before it dispatches a mapping batch: the streamed
loader's last small batch is armed at about +13.6 s and is never armed again (`docs/streamed_consumer.md` section 9).
So the need in our own measurement is **conditional and forward-looking**: the call site and the pools exist in our copy
of the title, the mapping path is not executed at this frontier, and answering the slot with `E_NOTIMPL` today is
harmless only until the loading stage advances. The static evidence is why the slot is worth implementing now.

**Cross-check.** The public upstream fork (GPL-3.0; see section 7) measured the same call on the same title and reports
about 300 calls per boot from the load stage (its `linux_proton.md` page), with the same argument order and the same
24-byte/12-byte layouts. Its figure is a measurement from a tree that reaches further into the load than ours; our copy
confirms the call site, RVA, argument shape and layouts, not the count.

## 2. What the call means, and what this tree does with it

`CopyPageMappingsBatchX` is a console command-queue method: it installs page-table mappings from a pool of 64 KiB
physical pages to the destination pages the title names, so the title's own addresses for those pages become readable
(the pool's pages were mapped by the title itself before the call). Microsoft's public Learn reference does not list the
method (the console ABI names come from public console layers that dispatch it), so the branch is written from the
measured call site above, not from a documented signature.

`port/shims/xmem.cpp` owns the physical pages (`g_pages`) and the per-address views (`g_mapped`), so the queue proxy
answers the call by decoding the batch and calling `rt::xmem::copy_page_mappings_fn` - a header-declared hook in
`port/shims/xmem.h` that this file's implementation fills in and a GPU-free test binary replaces. For each range:

| kind | meaning | hook call |
|---:|---|---|
| 0 | map pool pages `start, start + 1, ...` at the destination pages one after another | `(destination, pool + start * 64 KiB, count, false)` |
| 1 | map the one pool page `start` at every destination page | `(destination, pool + start * 64 KiB, count, true)` |
| 2 | leave the destination pages unmapped | `(destination, nullptr, count, false)` |
| 3 | leave the destination pages as they are | none |

The hook gives every destination page a second view of the physical page found at its source page (the same memory, as
two `XMemMapPhysicalPages` calls with one page number make it), using the destination window's protection; a source page
that is not mapped leaves its destination page unmapped. Every source page is looked up before any destination page
changes, so a source range that overlaps the destination maps what was there before the call.

**The paired CPU view.** Upstream's second commit exists because its layer reads the title's GPU-only reservation at a
separate paired CPU reservation, so it maps the copy at both addresses. This tree has no such pair: `xmem` maps host
memory at the console VA itself and the D3D12 layer commits and reads those same pages by that VA
(`docs/page_address_space.md`, `port/shims/xmem.cpp`'s `commit_physical_range`). The destination window is therefore
already the CPU view the layer reads, and making the destination pages readable is what makes the title-visible memory
readable here; there is no second address to fill. If a future layer reads the title's pages through a separate alias,
this hook is where the extra mapping would go.

## 3. Refusals

- **No mapper** (a GPU-free test binary that links no `xmem`): the hook is null, the branch is not taken and the call
  keeps the answer every queue slot past the PC chain gets - the "beyond the PC queue vtable" warning once and
  `E_NOTIMPL`, recorded as `unsupported` in the D3D12 trace. The new branch therefore changes nothing for those builds.
- **With a mapper**: a batch count with a null batch array, a batch whose range count has a null range array, a range
  whose pooled pages run past the pool's page count, a kind other than 0-3, or a range `xmem` refuses (unaligned
  address, destination outside a physical-page window) is logged once by name and the call returns `E_INVALIDARG`. The
  ranges that could be applied before and after the refused one still are: the title does not read the result and reads
  the destinations, and a partly applied batch is closer to the console than dropping all of it.
- A `count` of zero and a kind-3 range are answered `S_OK` and map nothing.

The first call and the first refusal of each boot are printed (`queue slot 31 (CopyPageMappingsBatchX) first call`,
`queue slot 31: ... refusing that range with E_INVALIDARG (logged once)`), so a future boot that reaches the stage names
the pool and the refusal without any new switch.

## 4. Tests

`xs_proxy_mock_test` (the GPU-free proxy test, no xmem linked) now covers the console decode with a recorder in place of
the hook: the no-mapper `E_NOTIMPL` and never reaching the real queue; a two-batch call with all four kinds and the
destination cursor (including that a kind-3 range calls nothing); the batch/range values unchanged; a kind-3-only call;
and the refusals (run past the pool with the following range still applied, unknown kind, null pool, hook returning
false, null batch array, null range array, zero count), then the `E_NOTIMPL` again after the hook is cleared.

`xmem_test` (real physical pages, no GPU) covers the mapping itself: two source pages copied one after another, one
source page repeated over two destinations, a destination write visible at the source address (one memory), a source page
unmapped before the copy leaving its destination reserved, a null source unmapping, and the refusals (unaligned
destination/source, a page count that overflows the address space, a destination outside any window).

```powershell
cmake --build port\build --target xs_proxy_mock_test xmem_test -j 24
port\build\xs_proxy_mock_test.exe     # xs_proxy_mock_test: PASS
port\build\xmem_test.exe              # xmem_test: OK (231 checks)
```

## 5. The A/B boot measurement

One 130 s boot with the change (`boot-slot31`; standard environment, `PALMETTO_XS_CALLERS=1`,
`PALMETTO_PIPELINE_TIMELINE=5`, harness `tooling/boot_report.py`, boot lock held), against the tree's preceding standard
130 s boot of the same harness (`boot-timeline1`, built before this change).

| Measure | A (`boot-timeline1`, before) | B (`boot-slot31`, with slot 31) |
|---|---:|---:|
| Run | ALIVE 130.2 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| D3D12 calls recorded / max# | 535,283 / 537,569 | 549,034 / 551,123 |
| Queue slot-31 calls | **0** | **0** |
| `CopyPageMappingsBatchX` lines / mapped entries / refusals | 0 / 0 / 0 | 0 / 0 / 0 |
| DrawInstanced recorded / submissions | 25,477 / 25,478 | 26,190 / 26,190 |
| `PresentX` ok | 12,739 | 13,095 |
| Loader / DirectStorage `[rt]` lines | 2 / 5 | 2 / 5 |
| Last submission milestone | 24,576 at 104.6 s | 24,576 at 105.2 s |
| Device lost / device removed | none | none |

**The result is a negative one and is reported as such:** the title never calls slot 31 in this boot, so the new branch
is not executed, and nothing about the boot can change. The two runs' counts differ by about 2-3 %, the boot-to-boot
host variance seen between all recent 130 s boots; the loader and DirectStorage activity after the load window, the
pipeline population, the replay registrations and the frame loop's shape are identical, and neither run lost the
device. What the measurement does establish is that the change is inert on today's frontier and that the answer path is
instrumented for the boot that does reach it.

## 6. Limits

- **The mapping is applied when the call is made**, not when the queue reaches the batch, as on the console. Whether
  earlier queued work still reads a destination page at the old mapping is not measured here; the upstream implementation
  has the same gap.
- **The upstream call count is not reproduced in our boot.** It is a load-stage effect on a tree that reaches further;
  our loader stops before any mapping batch is dispatched. Until a boot reaches it, the mapping's effect on streamed
  views is unmeasured in this tree.
- **A source page must be mapped at the time of the call.** A pool page the title has not mapped yet maps as "nothing
  here", leaving the destination page unmapped; a later map of that pool page does not revisit the destination.
- **The destination must be inside a window** `XMemVirtualAlloc` recorded; a destination outside every window is a
  named `E_INVALIDARG` refusal. The pool base is used as a VA exactly as the title passed it; no page-number decode is
  assumed.
- **No paired CPU view** exists in this tree (section 2); the one mapping is at the console destination VA.

## 7. Upstream credit

Facts cross-checked against the public upstream fork (GPL-3.0), which is a reference and evidence source here, not a
code source; all code above is written in this tree's own structure and comments, and none was copied:

- `f2268abea` "d3d12_xs: queue slot 31 is CopyPageMappingsBatchX, mapped through xmem (RT-QUEUE31)" (CLHelper) - first
  identification of the call and the batch/range layouts, and the xmem mapping primitive.
- `255f17001` "xmem: map slot-31 copies at the GPU-only range's paired CPU view too" (Jan Trencansky) - the paired
  CPU-view half this tree does not need (section 2).
- `7fcbf1fde` docs (Jan Trencansky); merged in `0aecc5efa` (#968, aslan2142); later refinements `eedc7c8db`
  ("slot 31 without a mapper keeps the unmodelled answer", Jan Trencansky) and `682a557f8` (test recorder), whose
  behaviour - the no-mapper answer and the recorded call shape - this implementation reproduces.
- Upstream documentation: its `d3d12_vulkan.md` section 4.9, its `linux_proton.md` "Where it stops", its console-ABI
  page section 4.

What this page adds over those references: the call is re-derived here from our own copy of the executable (RVA, the
argument order, the two record strides, the four pool registrations in a boot), the answer is adapted to this tree's
single-VA page design instead of the fork's paired view, and the measured need is recorded honestly as zero calls at this
frontier with the A/B numbers above.
