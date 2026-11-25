# Card-side title-buffer copies and texture write-back: the measured copy path and what a write watch can do

**Status: measured path and a readied change signal; the avoiding copy is implemented behind switches and one small
device hook is pending.** This is the rank-4 slice of [cleanroom_peers.md](cleanroom_peers.md): the peer fork's newest
measured wins are keeping small title-buffer copies on the graphics card, hashing the changed ranges so only changed
chunks move, and copying shader-written textures back into title memory. This page measures what our tree actually
copies per submission and per draw, states the win threshold before any code, records that the OS write watch cannot
answer for this memory (measured), lands the guard-page write tracker and its switch-gated consumers, and names the one
device-side hook that makes the avoidance safe to enable.

Counts, bytes, times, addresses of our own allocator and switch names only; no game data, package path, shader byte or
extracted table.

## 1. Why this slice, and what the peers measured

The peer evidence is feature-level only ([cleanroom_peers.md](cleanroom_peers.md)): the victory-fork README's status
sections report that keeping copies under 1 MiB on the card cut its title lists from 68.0 to 49.8 ms of card time a
frame, that a 4K title texture's shader writes are found by on-card hashing so only changed 256-byte chunks are sent
back (22,900 read-backs in six minutes, which it says unblocked a mission), and that only changed title memory is
copied. XCL documents title graphics memory shared by CPU and GPU with no copies at all. Their structures are not
ours; what transfers is the mechanism class: **a change signal for title memory, and a copy narrowed to the pages that
changed**.

Our tree sits at the opposite end of that class. [page_address_space.md](page_address_space.md) section 4 measured the
per-submission mirror refresh of the title's shader-visible console descriptor heap at **3.4-4.4 ms**, "most of the
~3.8 ms between submissions". [remaining_mechanisms.md](remaining_mechanisms.md) ranks the missing write-watch consumer
fourth. This slice measures that path precisely, reproduces the number, and readies the change signal.

## 2. Measured shapes

### 2.1 What a submission copies, and what a draw copies

Two different copies leave title CPU memory for GPU-visible memory today:

| path | when | bytes | measured cost |
|---|---|---|---|
| console descriptor-heap mirror (`sync_console_heaps`, `port/src/vulkan_backend.h` -> `DescriptorHeap::sync_console_descriptors`, `port/shims/d3d12_vulkan_device.cpp`) | once per submission, before the lists are prepared | one pass over the whole heap: **0x2600000 = 39,845,888 B (38.0 MiB)** of title bytes compared against a CPU-side copy, changed 32-byte slots copied into the GPU-visible mirror | **3.4-4.4 ms** per submission (page_address_space.md section 4); **3.508 ms** reproduced by a standalone probe over the same shape (38.0 MB compared against a 38.0 MB shadow: 76 MB read); **8.4-9.5 ms** per refresh measured by this slice's A/B boot (section 6) |
| per-draw page pull (`console_pull_pages`, `docs/page_content.md`) | while a draw is prepared, per vertex/index/constant range the draw names | measured 131,072 pulls / 273.0 MiB over a 300 s boot, about 2 KiB per draw | below the submission path; it is demand-driven and not per submission |

The measured range population: the title places two console VAs in one xmem region - a **placed buffer**
(182,452,224 B, `ConsoleVaKind::buffer`) and the **shader-visible descriptor heap** (0x2600000 B,
`ConsoleVaKind::descriptors`). Only the heap is mirrored and refreshed per submission; the placed buffer is pulled per
draw (page_content.md). So the per-submission title-memory copy is **one buffer: the 38.0 MiB descriptor-heap mirror**,
and it is a *compare* of 76 MB per submission, not a copy of the whole heap: only changed slots are written to the
mirror. The A/B boot measured 8,192 refreshes and 69.0 s of refresh time in a 130.4 s boot - 52.9% of the boot's wall
clock on one recording thread - so the two documented figures differ by about 2x and the later, larger one is what this
page's threshold uses.

### 2.2 Can the OS write watch answer? No - measured

The title exports `XMemGetWriteWatch`/`XMemResetWriteWatch` (our shim forwards them to the OS) and the peers' mechanism
is a write watch, so the first question is whether Windows can watch this memory. It cannot:

- The descriptor heap's title bytes are **pagefile-section views**: `console_heap_back` maps one 64 KiB pagefile
  section per page over the console placeholder (`port/shims/d3d12_vulkan_device.cpp`).
- A standalone probe (no game data, `GetWriteWatch` on exactly that shape - a placeholder replaced by a pagefile-section
  view - and on a plain `VirtualAlloc` commit) measured `ERROR_INVALID_PARAMETER` for both; the OS write watch only
  works on private memory allocated with `MEM_WRITE_WATCH`. The title's own imports never call `XMemGetWriteWatch`
  ([xmem_exe_calls.md](xmem_exe_calls.md): it imports 12 functions, and the watch pair is not among them), so there has
  never been a consumer to answer.

### 2.3 The guard-page alternative, measured

The mechanism the peers describe for write tracking is a **guard page**, so that is what this slice measures and lands:
arm `PAGE_GUARD` on the watched pages; the first access raises `EXCEPTION_GUARD_PAGE`, the system clears that page's
guard, and a handler records the page. A standalone probe over the measured shape (608 x 64 KiB page views = 38.0 MiB,
one `VirtualProtect` per view) measured:

| measure | value | consequence |
|---|---|---|
| arm 608 x 64 KiB page views | **0.604 ms** total (0.99 us per view) | one-time at heap creation, acceptable |
| one `VirtualProtect` over the whole multi-view range | **fails** | arming must be one call per view |
| a guarded write | **10.3 us** (one exception, the write lands, the guard is cleared by the system) | a changed page costs one exception, not a scan |
| a guarded read | **5.1 us**, and the system clears the guard | the handler must tell reads from writes: a read is not a change |
| a full scan of an armed heap | **35.1 ms** | the guard costs one exception per 4 KiB page when *our own* refresh scan reads the heap |

The last row is the slice's central measurement: the change signal is cheap, but **our own full-heap scan is not
compatible with it**. A gate on the signal therefore has to skip the scan when nothing changed and pay the scan
(`suspend` the guards, scan, `resume`) only when something did. That split needs to know whether *any* console heap
needs the refresh for reasons the tracker cannot see (section 5).

## 3. The win threshold, stated before implementing

The measured refresh is 8.4-9.5 ms per submission in this slice's A/B boot (3.4-4.4 ms on the earlier page's boot) and
the frame loop submits twice per presented frame, so the refresh is the largest single CPU block on the recording
thread. Against that:

- **A copy that still compares all 38.0 MiB per submission cannot win.** Any change that leaves the full compare in
  place saves noise.
- **The threshold adopted: skip at least 25% of the refreshes**, which at 8.4-9.5 ms each is >= 2.1 ms per submission
  on average, with any guard maintenance bounded to the probe's 0.6 ms arm plus one exception per changed page. Below
  that, the mechanism is recorded as readied for a later, narrower copy rather than enabled.
- **The real fix (the peers' shape) is a copy narrowed to the changed pages.** It needs the changed-page list plus the
  device-side copy loop (section 5); the tracker landed here is the signal half, and the page states the follow-up
  exactly.
- **The measured skip rule reaches the threshold**: half the frame's two submissions carry no shader-launching command
  and cannot read the mirror, so the implemented switch skips 50.0% of refreshes (section 6).

## 4. What this slice implemented

### 4.1 The guard-page write tracker (`port/shims/xmem.cpp`, `port/shims/xmem.h`)

Our own implementation, filled in through the header's existing hook pattern so a GPU-free test binary that links no
xmem simply sees null hooks and keeps today's path:

- `page_watch_arm(range)` guards every 4 KiB page of a range (one `VirtualProtect` per 64 KiB view); a range with an
  uncommitted, non-writable, non-cached or already-guarded page is refused, and the caller keeps the full work for it.
- One vectored handler records guard exceptions: a **write** sets the change flag and records the page, a **read or
  execute** records only "guard this page again". A range the tracker does not watch goes back to
  `EXCEPTION_CONTINUE_SEARCH`, so a title's own guard pages keep their own handling.
- `page_watch_poll()` drains the record, re-guards the pages and returns `unchanged`, `changed` or `unavailable`
  (nothing armed, a range refused, or the bounded trip record overflowed - the caller then does the full work).
  Re-arming in the poll is what keeps the watch continuous between submissions.
- `page_watch_suspend()`/`page_watch_resume()` take the guards off around a full refresh scan (the measured 35.1 ms
  alternative) and restore them afterwards; a write inside that window is not recorded, which is the documented limit
  in section 7.

### 4.2 The consumers (`port/src/vulkan_backend.h`)

`VulkanContext` registers every console descriptor-heap mirror's title range with the tracker when
`PALMETTO_CARD_COPY_WATCH=1`, counts the mirror's ranges and bytes, and `VulkanQueue::execute_now` calls
`sync_console_heaps_measured(shader_submission)` instead of the bare refresh. Every switch is read once per process and
every one is off by default, so the default path is exactly the tree before the slice: no clock read, no tracker call,
no skip.

| switch | effect |
|---|---|
| `PALMETTO_CARD_COPY_TRACE=1` | The measurement arm. Counts each refresh (calls, skips, microseconds, max, ranges/bytes) and prints the totals at the runtime's submission milestones. Behaviour unchanged. |
| `PALMETTO_CARD_COPY_SKIP_NO_SHADER=1` | The implemented avoidance. A submission that records no command a shader can read the mirror from (barriers, clears, copies, queries, markers - `Draw`, `Dispatch`, `ExecuteIndirect` and a raytracing build are the shader-launching set) cannot read the descriptor mirror, so its refresh is skipped; the next draw-bearing submission refreshes before it is recorded. |
| `PALMETTO_CARD_COPY_WATCH=1` | Arms the guard-page tracker on the mirrored title ranges and polls it once per submission; counts only (no skip). |
| `PALMETTO_CARD_COPY_GATE=1` | With `WATCH`, skips the refresh when no watched page was written since the last one. **Readied, not enabled** (section 5). |
| `PALMETTO_CARD_COPY_GATE_REFRESH=<n>` | With `GATE`, forces one refresh every n submissions: the bound on how long a write that landed in the suspended window can stay unmirrored. 0 (default) means only a changed page refreshes. |

The milestone line is `[rt] replay: card copies: mirror <n> range(s) <MB>, tracker <n> range(s)/<MB> | refreshed
<n>, skipped <n> (unchanged <n>, no-shader <n>) | <us>/refresh, <ms> total, max <us> | page watch changed <n>,
read-trip <n>`; the boot JSONL trace and stderr under the harness's output directory carry it.

## 5. Cross-check against the reference

Reference-only, no code copied. The victory-fork README (night-of-25-November status section, clone `9c184b3`, per
[cleanroom_peers.md](cleanroom_peers.md)) reports the same class of mechanism with different structures: device-local
copies of title buffers with on-card hashing for changes, and a copy-back driven by the console list bitmap. Our
measurement agrees that **the change signal is the enabling piece** and disagrees about **where it lives**: their
device-local copy lets the GPU hash the changed ranges, while our mirror is host-visible and the comparison is the CPU
cost, so the signal has to come from the title's CPU pages - which is why this slice measures the write watch and lands
a guard-page tracker instead of an on-card hash. The peers' copy-back half has no population here: measured, the title
never uploads and never writes the sampled placed texture's pages in the window
([page_content.md](page_content.md) sections 7-8, [texture_source.md](texture_source.md) section 6), so a write-back
path would have nothing to copy - it is named as the next falsifier (section 7), not implemented as dead code.

**Pending for enablement - the one device-side hook.** The refresh also patches title-written V# bases of *in-arena*
shader-visible descriptor heaps (`scan_in_place_` in `port/shims/d3d12_vulkan_device.cpp`); those heaps are not
console-addressed, so they are not in `console_va_map` and the tracker cannot watch them. A gate that skipped their
refresh would miss a patch. The safe condition needs the device's heap census, which is five small lines in
`port/shims/d3d12_vulkan_device.cpp` (owed as a follow-up once the deferred-compile slice freezes that file):

```text
1. class DescriptorHeap, public part near `const D3D12_DESCRIPTOR_HEAP_DESC& raw_desc()`:
       bool mirrored() const { return mirrored_; }
2. DeviceState::add_console_heap, after the push_back:
       context->note_console_heap_added(heap->mirrored(), !heap->mirrored() && heap->in_arena());
3. DeviceState::remove_console_heap, after the erase:
       context->note_console_heap_removed(...);   // the same pair captured before the erase
```

(the two-line variant passes only `mirrored()` and treats "console heaps > mirrored heaps" as unwatchable, which is the
conservative rule the context already implements: `watch_covers_mirrors()` requires every mirrored range armed and
`console_watch_ranges() == mirrors`). With the census in place, `sync_console_heaps_measured` may gate on
`watch_covers_mirrors()` alone; the changed-page copy loop itself still belongs to the device's
`sync_console_descriptors`, which is what turns a changed page into a bounded copy instead of a full scan.

## 6. A/B measurement

One 130 s A/B pair, same machine, same title, boot lock held, one boot at a time, harness `tooling/boot_report.py`.
Standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=...`), presenter capture on (`PALMETTO_CAPTURE`, up to 6 frames, one every 2,000 presented -
3 were reached), external private-bytes sample
every 5 s. Both arms set `PALMETTO_CARD_COPY_TRACE=1`; arm B adds `PALMETTO_CARD_COPY_SKIP_NO_SHADER=1`. Outputs are
outside the repository (the harness's boot directories).

| measure | A: default path + counters | B: + `PALMETTO_CARD_COPY_SKIP_NO_SHADER=1` |
|---|---|---|
| run | ALIVE 130.4 s, killed by the timeout as usual, **0 exceptions**, 0 `device was lost`, 0 validation errors | ALIVE 130.3 s, killed by the timeout as usual, **0 exceptions**, 0 `device was lost`, 0 validation errors |
| d3d12 calls recorded | 232,947 (max #235,515) | **241,144** (max #244,028, +3.5%) |
| `ExecuteCommandLists` ok (submissions) | **9,583** | **10,031** (+4.7%) |
| presented frames (`PresentX` ok) | **4,791** | **5,015** (+4.7%) |
| `DrawInstanced` | **9,580** | **10,028** (+4.7%) |
| mirror population | 1 range, 38.0 MB (39,845,888 B) | identical |
| mirror refreshes / skips at the 8,192-submission mark | 8,192 / 0 | **4,094 / 4,098 (50.0% skipped)** |
| mirror time at that mark | **69.0 s** (8,427 us per refresh) | **35.7 s** (8,709 us per refresh) |
| mirror share of the boot's wall clock | 52.9% | **27.3%** |
| first (whole-heap) refresh | 914.5 ms | 785.9 ms |
| time to submission 8,192 | 84.9 s | **82.7 s** (-2.2 s) |
| captured presented frames | 3 frames (1, 2,001, 4,001) at 2564x1421, mean rgb **0 0 0**, non-black 0 | 3 frames, same ids, same mean rgb **0 0 0**, same extent |
| host private bytes, average from 30 s (external sample) | 29.73 GB (13.3-35.4 GB range) | 29.59 GB (13.3-36.0 GB) |
| host working set, average from 30 s | 23.51 GB | 23.40 GB |
| verdict | **measured win on the mirror path**: the same submitted work costs 48.4% less mirror CPU (8.43 -> 4.35 ms per submission), 52.9% -> 27.3% of boot wall clock, and the arm that skips finishes 4.7% more submitted/presented/drawn work in the same 130 s with the same black captured frames. Nothing advances in content: the presented picture is the same uniformly black frame both pages measured. | |

The row that matters for the threshold: **the implemented rule skips 50.0% of the refreshes**, twice the 25% the
threshold asked for, and the skipped submission is structurally the frame's clear-only submission - measured: 2
submissions and 2 draws per presented frame, with both draws in one of the two submissions (9,583 submissions, 9,580
draws, 4,791 presents in arm A). The draw-bearing submission always refreshes before it is recorded, so no shader ever
reads a stale mirror; the captured frames are byte-for-byte the same black frame in both arms, which is the picture
check this window can give (page_content.md found the content gap elsewhere).

The memory curve does not move beyond noise (29.73 -> 29.59 GB private average): this slice removes CPU work, not
memory, and the guard-page tracker is not armed by the shipped switch (it is `PALMETTO_CARD_COPY_WATCH`, section 4.2).
The tracker's would-skip rate is therefore **not** claimed from this pair; the page's guard-page numbers (section 2.3)
are the measured basis for its cost, and it stays readied for the narrow-copy follow-up. Reproduction: `boot-a` and
`boot-b` under `D:\leonida-work\cardcopies` (outside the repository), each `summary.txt`, `stderr.log` with the
`[rt] replay: card copies:` milestone lines, `series.csv`, `boot.jsonl`, and `capture/index.txt`.

## 7. Limits and the next falsifier

- **The OS write watch cannot see the title's memory.** Measured: `GetWriteWatch` returns `ERROR_INVALID_PARAMETER`
  for the pagefile-section views the descriptor heap uses (section 2.2). The guard-page tracker in this slice replaces
  it; it is the only signal measured to work on this memory.
- **Our own refresh scan is incompatible with armed guards** (35.1 ms for 38.0 MiB). A gate can only skip, or suspend
  the guards around the scan; a narrowed copy needs the device-side change (section 5).
- **The suspended window does not record writes.** A write that lands while the guards are off (the scan) is not
  reported: the same class of race the refresh already has with a title writing descriptors for work already in flight
  (page_address_space.md section 5). `PALMETTO_CARD_COPY_GATE_REFRESH` bounds how long such a write can stay
  unmirrored; it is a bound, not a proof.
- **The gate is readied, not enabled.** The in-arena heap patch path is not watchable, so the gate needs the device
  census hook (section 5) before it can be on. With the hook absent the code requires the conservative
  `watch_covers_mirrors()` condition and is off by default.
- **Texture write-back has no population here.** The measured window has no copies, no dispatches and no writes to the
  sampled placed texture's pages (page_content.md sections 7-8, texture_source.md section 6). The next falsifier is the
  content path: the first streamed block or texture-computer fill that writes title pages makes a write-back worth
  implementing, and the tracker's changed-page list is what would drive it.
- **One boot per arm, one machine, timing-sensitive.** The title's boot progress varies run to run; the copy counters
  are per submission and compare the same window, but the arm's absolute submission/present counts are not a
  benchmark.
- **The peers' numbers are not ours.** 68.0 -> 49.8 ms of card time and 22,900 read-backs are their measurements on
  their structures; this page's table is this tree's own.

## 8. Tests

`port/test/xmem_test.cpp` - the existing xmem ABI test, extended with `test_page_write_watch` (**28 new checks** of
286; the binary passes). It covers, against real memory and the real handler:

- the tracker hooks are installed by `xmem.cpp`, and the tracker answers `unavailable` before any range;
- a committed writable range arms (twice, two ranges) and reports the guarded page count; a freshly armed range is
  `unchanged`;
- **coherence**: a write through a guarded page lands unchanged and the next poll reports `changed` exactly once;
- a guarded read is not a change, a read-trip page does not lose detection for the other pages, and a write after a
  read trip is still reported (the dirty-range cases);
- two ranges: a write in one is reported, the verdict clears, a write in the other is reported again;
- the suspended window does not report a write, and `resume` restores detection (the documented limit's test);
- **fallback**: a reserved (uncommitted) range is refused, a page another owner already guards is not adopted, and a
  refused arm leaves the armed page count unchanged;
- `forget` releases the guarded pages, restores ordinary writable memory, and allows re-arming the same address.

## 9. Attribution

This slice was implemented from this project's own measurements: the copy path's counters, the standalone write-watch
and guard-page probes, the guard-page tracker and its tests, and the A/B boot are ours. The behaviour was cross-checked
against the public GPL-3.0-or-later project victory-fork (<https://github.com/fxceless/victory-fork>), README status
sections (the night-of-25-November section at clone `9c184b3`, as cited by [cleanroom_peers.md](cleanroom_peers.md)),
and XCL (<https://github.com/xcl-dev/xcl>, GPL-2.0-only, clone `6d12f58`) for its "title graphics memory shared without
copies" claim; both are evidence of the mechanism class, not code sources. No code from either project was copied.
The upstream projects remain the copyright of their contributors under their own licenses.
