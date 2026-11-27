# The title-memory model: our copies against the peers' page heaps and write watch

**Status: measured model; two reversible, opt-in steps are off by default.** The per-submission pull coalescer has one
boot A/B and is measured inert at today's frontier; the separate alias-expanded CPU page-watch has synthetic host tests
only and has not been tried in a title boot. The measurement says why the first step has no population. This is the
rank-3 slice of [cleanroom_peers.md](cleanroom_peers.md): the peers' title memory is aliased into GPU-visible
memory and kept current by a write signal, while our tree copies the title's bytes into an arena and has no enabled
change signal. This page
measures the whole model as it stands (every consumer, its bytes and CPU cost, from our own counters), states for each
peer mechanism what our Windows/Vulkan backend would need - measured in our tree wherever a measurement exists, and
marked unmeasured where it does not - and lands the safest step the measurement supports: a per-submission **pull
coalescer** that reuses a copy this same submission already made
([port/src/title_pull_windows.h](../port/src/title_pull_windows.h), [port/src/vulkan_backend.h](../port/src/vulkan_backend.h),
`PALMETTO_TITLE_PULL_TRACE` / `PALMETTO_TITLE_PULL_COALESCE`). The A/B then measured that the title's draws name a
**different byte range almost every time** (0 repeated ranges in 8,186 pulls), so the step has no population here: it
is recorded as a negative with no regression, and the model's measured conclusion is that the only title-memory copy
with reusable bytes at this frontier is the descriptor mirror, whose skip rule is already landed
([card_copies.md](card_copies.md)). No peer code and no game data are in it; section 6 gives the A/B and section 7 the
next falsifier.

Counts, sizes, RVAs and status codes only; no package path, shader byte, extracted table or peer code.

## 1. Why this slice, and what the peers claim

The peer evidence is feature-level and was read from their public documentation and repositories (reference only; the
programme's no-copy rule is [upstream_port.md](upstream_port.md)). All claims are from the clones named in
[cleanroom_peers.md](cleanroom_peers.md) section "Reference material consulted", re-read at those commits:

| peer | public claim (summarised) | the mechanism class it names |
|---|---|---|
| XCL (`6d12f58`, GPL-2.0-only) | README: "title graphics memory shared by CPU and GPU without copies, up to 16 GB (4 GiB windows)"; its handoff notes describe 4 GiB reserved windows whose 64 KiB tiles are mapped onto heaps, the CPU view staying the title's memory and a per-submit write-watch scan uploading only changed 64-byte chunks (measured there: ~0.8 ms a submit over ~2 GB of mapped tiles, then ~0.3 ms of uploads). | real GPU aliasing of title pages through a window address space plus a write watch |
| victory-fork (`9c184b3`, GPL-3.0-or-later) | README: small title buffers get a copy in card memory with shader writes found by hashing on the card (its title lists 68.0 -> 49.8 ms of card time a frame); "Title memory can be opened as a graphics heap so that buffers are used in place instead of copied"; a write through one view of a physical page is seen by copies taken through its other views; guarded memory is left out of a submission's write-watch; storage writes into physical pages are counted. | in-place card heaps, a guard-based write watch, and alias reporting |
| five-star (`0bfa755`, GPL-3.0-or-later) | "a page mapped at two addresses is one page and a remapped page keeps its contents"; GPU visibility by opening the page sections as heaps, 1 GiB windows. | pagefile-section page heaps with 64 KiB remappable pages |
| VIctory (`13f91ca`, GPL-3.0-or-later) | title memory at raw GPU addresses through a per-page host-copy raw-view table; writes found by read-only guards, alias reports from the runtime. | read-only-guard write detection |

Ours is the other end of the class: a **copy and mirror** model. [page_address_space.md](page_address_space.md) rebases
the title VAs into one arena and mirrors the console descriptor heap; [page_content.md](page_content.md) copies the
pages a draw names into the arena before the draw; [card_copies.md](card_copies.md) measured that mirror's refresh cost
and readied, but did not enable, a guard-page change signal. This page measures what the whole model costs and what
each peer mechanism would actually require here.

## 2. Our measured model

### 2.1 Every title-memory consumer, with its measured volume and cost

| consumer | code | when | measured volume | measured CPU cost |
|---|---|---|---|---|
| console descriptor-heap mirror | `sync_console_heaps` / `DescriptorHeap::sync_console_descriptors` (`port/shims/d3d12_vulkan_device.cpp`), switched measure in `port/src/vulkan_backend.h` | once per submission, before the lists | one range, **38.0 MiB** (39,845,888 B) of title bytes compared against a CPU-side mirror (76 MB read); only changed 32-byte slots copied | **3.4-4.4 ms** per refresh on its first boot ([page_address_space.md](page_address_space.md) section 4), **8.4-9.5 ms** in the later A/B ([card_copies.md](card_copies.md) section 6); 52.9% of that boot's wall clock before the skip rule, **27.3%** after it |
| per-draw page pull (vertex/index/root descriptor) | `console_pull_pages` (`d3d12_vulkan_device.cpp`), replay hook `pull_title_pages` (`port/src/vulkan_backend.h`, `port/src/vulkan_replay.h`) | while a draw is prepared, per title VA the draw names | **131,072 pulls / 273.0 MiB over a 300 s boot** on the tree that measured it ([page_content.md](page_content.md) section 8); **8,186 pulls / 31.9 MiB over the 130.4 s A/B boot** on this tree - about one pull a submission, ~4 KiB each (a vertex view of 0xFF0 and a root window of 0x60), every one a different VA | the hook's own copy is **7 us a pull, 54.4 ms over 130.4 s** in the A/B (measured with `PALMETTO_TITLE_PULL_TRACE`, section 6). The earlier pull-on vs pull-off boots attribute **~440 us a submission** (submission 8,192 at 38.6 s vs 35.0 s) to the *whole* pull path, so most of that is in the table pull and the mirror reads, not in this hook (section 7) |
| per-draw table pull (a descriptor table's V# data) | `console_pull_table` (`d3d12_vulkan_device.cpp`), replay hook `pull_title_table` | same, per descriptor table the draw names | each draw's table has one V# and it is title-named and pulled ([page_content.md](page_content.md) section 2); its copies run inside the device file and are **not** wrapped by this slice's trace | not timed by this slice; the measured whole-path figure above is the bound (section 7 names the counter as the next instrument) |
| copy-source pull | `console_pull_arena`, replay hook `pull_title_arena` | before a copy command reads a title range | **0 requests** in the measured window (no copy command at all) | 0 |
| page-mapping batch (queue slot 31) | `rt::xmem::copy_page_mappings_fn` (`port/shims/xmem.cpp`) | when the title calls `CopyPageMappingsBatchX` | implemented, **0 calls** at the frontier | 0 ([cpu_view_mapping.md](cpu_view_mapping.md)) |
| DirectStorage page-list resolver | `rt::xmem::resolve_physical_page_fn` (`port/shims/xmem.cpp`) | when a dstorage request names destination physical pages | implemented, **0 page-list requests**, `dest-pages=0` ([page_resolver.md](page_resolver.md)) | 0 |
| title-placed buffer import (slot 133) | `create_buffer_at_host_address` (`d3d12_vulkan_device.cpp`, `VK_EXT_external_memory_host`) | when the title places a resource on its own pages | the measured 182,452,224-byte placed range is **not importable** (`E_NOTIMPL`; xmem commits pages lazily) | 0 - the stand-in copy path is used instead |
| placed-texture import | `PALMETTO_TEXTURE_IMPORT` (`d3d12_vulkan_device.cpp`) | one bounded import per placed texture | opt-in; measured, the title's source addresses are not any device resource's address in the observed window | off by default ([texture_import.md](texture_import.md)) |

The backing stores the model copies *from* are the xmem physical pages: `XMemAllocatePhysicalPages` is measured at
3 calls / 114,096 pages (7,131 MB requested), **638 MB committed** at 110-140 s, one 64 KiB pagefile-section view per
page ([memory_growth.md](memory_growth.md) section 1, [xmem.cpp](../port/shims/xmem.cpp)). A kernel `ReadFile` of
**271,044,608 bytes** into those pages is repaired at boot by first-touching every destination page
([readfile_first_touch.md](readfile_first_touch.md)); section 3.4 shows why that measured fact decides the guard-page
option.

### 2.2 What this adds up to

At the measured frontier the title's CPU memory reaches the GPU through **two copies**, and only one of them has
reusable bytes:

- The **descriptor mirror** is a 38.0 MiB compare pass every submission, the largest single CPU block on the recording
  thread. Its bytes *are* reused: the title rewrites a few of 1,245,184 descriptors a frame, so a skip rule pays, and
  the landed one skips 50% of the refreshes ([card_copies.md](card_copies.md) section 6).
- The **per-draw pull** carries different bytes almost every time. This slice's A/B measured 8,186 pull requests in
  8,957 submissions, with **0 requests covered by an earlier pull in the same submission and 0 covered by the previous
  submission's pulls** (section 6). The title's vertex, index and constant views are per-frame rings and streamed
  ranges, so a copy of them is a fresh copy: the peers' "only changed pages are copied" win applies when pages *are*
  re-read, and at this frontier they are not.

Everything else the model carries (copy sources, page batches, resolver requests, the placed-buffer import) is measured
at **zero population** in the observed window. That is the honest shape of the gap: the peers' aliasing would remove a
38 MB compare and a per-draw copy; on this tree only the first has a reusable population today, and its skip rule is
already measured and landed. The per-draw copy's measured cost (54.4 ms over 130.4 s in the wrapped hook) is far
smaller than the mirror refresh, which is the other measured result this slice contributes to the ranking.

## 3. What each peer mechanism would need on our Windows/Vulkan backend

The instruction for this slice was to measure the feasible mechanisms before choosing. Each option below states what
it would change, what our backend already has, and the exact measured constraint - a measurement, a line of our code,
or an explicit "not measured".

### 3.1 External host memory import (`VK_EXT_external_memory_host`)

**Would do:** make a title range a Vulkan buffer over the title's own committed pages, so a GPU read is the CPU's
bytes with no copy.

**What our backend has:** the extension is enabled when the driver reports `minImportedHostPointerAlignment`
(`d3d12_vulkan_device.cpp`, the device-extension list) and is already exercised: `OpenExistingHeapFromAddress` imports
a caller's committed allocation, and slot 133 (`create_buffer_at_host_address`) imports the title pages behind a placed
buffer. So the mechanism itself is not speculative here.

**Measured constraints, all from our tree:**

- **The measured placed range cannot be imported.** `create_buffer_at_host_address` refuses when any page of the range
  is not committed, write-combined or non-cached, and when the address/size are not aligned to the driver's
  `minImportedHostPointerAlignment`. The 182,452,224-byte placement is reserved and committed page by page, so the
  import answers `E_NOTIMPL` and the stand-in copy path is what runs ([page_address_space.md](page_address_space.md)
  section 1, [replay_resolution.md](replay_resolution.md)).
- **An imported buffer's address does not satisfy the shader's address equation.** The translated shaders compute
  `host_address = heap_base + console_va` ([d3d12_vulkan.md](d3d12_vulkan.md) section 3.3); an imported buffer has a
  driver-chosen device address, and the only console VA that reaches it is `device_address - heap_base`. That bias is
  **measured to work for 64-bit root arguments but to fail for a V#**: the V# base field is 48 bits and the measured
  bias is negative (`0xffffffffffce0000` on one GPU, `0xffffffffffedc000` on another), so it truncates
  ([d3d12_vulkan.md](d3d12_vulkan.md) D3D-53, [remaining_mechanisms.md](remaining_mechanisms.md) section 3.3).
- **The peers measured the pinning cost of the in-place forms.** victory-fork reports that both ways of opening title
  memory to the card pin it, on a PC with 1-5 GB free, and that the buffer carrying the most traffic cannot be opened
  at all; XCL reports that tiles mapped onto heaps opened from file mappings removed its device (both README/handoff
  pages at the clones above, cited in [cleanroom_peers.md](cleanroom_peers.md)).

**Verdict:** real for a *defined* range used as a command argument (already used by slot 133), not a substitute for
the mirror or the pull. The address-equation blocker is the measured reason: the mirror's descriptor bytes are read by
shaders as an arena VA.

### 3.2 Host-visible aliasing (the page heap: the title's page is the GPU's page)

**Would do:** what the peers do - one memory, mapped at the title's console VA and visible to the GPU, so the title's
CPU write *is* the shader's read (the "page heap" of five-star/VIctory, the 4 GiB window of XCL).

**What it needs that we do not have:** a Vulkan memory object whose physical pages are the same pages xmem maps at the
title's VAs. In our backend the arena is one host-visible `VkBuffer` with a driver-chosen device address
([d3d12_vulkan.md](d3d12_vulkan.md) section 3.3), while the title's pages are pagefile-section views xmem maps into
reserved placeholder windows. Windows has no API to map arbitrary committed host memory at a second address, so the
aliasing would have to come from the other direction: a section-backed `VkDeviceMemory` imported by NT handle
(`VK_KHR_external_memory_win32`). Our device does not enable or probe that extension; whether a driver accepts a
pagefile-backed section, and whether it pins the whole section at import (xmem commits lazily), is **not measured in
this tree** - it is exactly the kind of prerequisite this programme's policy says not to assume.

**What blocks it even if the import worked:** the same measured V# 48-bit bias as 3.1. The shader's equation holds
only for arena ranges, so a title page visible at a non-arena device address still needs a different address-space
design (the peers' model: the console VA *is* the GPU address, which is a rewiring of the arena, not a slice beside
it). Two peer measurements stay as warnings: XCL's pagefile-mapping heaps removed the device; victory-fork found some
title buffers cannot be pinned at all.

**Verdict:** the only mechanism that removes the copies outright, and the largest and least measured step. Not this
slice's first step.

### 3.3 Sparse/placed emulation

**Would do:** page-granular demand copies into GPU memory: a sparse buffer/image whose tiles are bound as the title's
pages become live.

**What our backend has:** sparse and aliased buffers are already created for D3D12 reserved resources
(`sparse_buffer_info`: `SPARSE_BINDING | SPARSE_RESIDENCY | SPARSE_ALIASED`), and sparse-image tiles and reserved
buffer tile mappings have their own tests ([d3d12_vulkan.md](d3d12_vulkan.md) section 4.2). The mechanism exists; what
it does not do is remove a copy: XCL's measured sparse-window path still scans for CPU writes and uploads changed
64-byte chunks each submission (~0.8 ms scan + ~0.3 ms uploads over ~2 GB of mapped tiles at its content population),
and its GPU writes are not copied back. Its device address is again outside our arena, so a V# would hit the same
48-bit bias.

**Verdict:** a page-granular *copy* model - a natural companion to the streamed page path
([page_resolver.md](page_resolver.md)), not a win for the measured 2 KiB-per-draw pull population.

### 3.4 Copy-on-change with dirty tracking (the write watch)

**Would do:** copy only the pages that changed since the last copy. [card_copies.md](card_copies.md) measured and
landed the *signal half* (a guard-page write tracker in `port/shims/xmem.cpp`) and left it readied, not enabled. This
slice measured the next question - what the full mechanism would need on this tree - and found four measured
constraints that are not on the peers' structures:

1. **The OS write watch cannot see this memory.** `GetWriteWatch` answers `ERROR_INVALID_PARAMETER` for pagefile-section
   views (measured, [card_copies.md](card_copies.md) section 2.2). Guards are the only measured signal here.
2. **Our own pull path is incompatible with armed guards.** `console_pull_pages` skips a page whose protection carries
   `PAGE_GUARD` (it treats guard/no-access/write-combine/non-cached pages as "not the layer's to copy",
   `d3d12_vulkan_device.cpp`), so a guard armed on a title page silently refuses that page's copy. A guard-based cache
   therefore has to arm and disarm around every copy, or the device's changed-page loop has to learn to take a changed
   page directly - the same device-side hook [card_copies.md](card_copies.md) section 5 names for the mirror scan. The
   wrapper this slice landed covers only the replay's `pull_title_pages` hook; the table pull (`console_pull_table`)
   copies in the device file, which is outside this slice's scope.
3. **Read-only guards would break the loader's kernel writes.** The peers' alternative (VIctory: read-only pages +
   write faults) cannot be used here: the loader writes **271,044,608 bytes** into first-touch title pages with a
   kernel `ReadFile` ([readfile_first_touch.md](readfile_first_touch.md)), and a kernel write to a guarded or read-only
   page fails instead of faulting. XCL recorded the identical trade-off on its own tree: read-only protection of cold
   tiles would make the scan follow written pages, "but the kernel's own writes (ReadFile into title memory) then fail
   instead of faulting".
4. **Aliased views defeat a per-VA guard.** One physical page can be mapped at several title addresses (xmem_test
   checks that both mappings see one memory; five-star documents the same invariant). A write through another view does
   not trip a guard armed on ours: victory-fork measured 0.8-1.0 million such writes in a five-minute run and added a
   per-page alias report for them. The default tracker still publishes per-VA ranges. Section 10 adds a separate opt-in
   hook that enumerates the current xmem views of each source physical page and rebuilds the watched intervals after a
   relevant map/unmap; its eight-interval bound and rearm failures invalidate the watch and force an unavailable verdict.
   This is only a CPU-VirtualProtect view set, not a GPU alias or title slot-31 mapping. `write_physical_pages` also
   fans a physical-page delivery through each currently watched alias; that does not cover unmeasured kernel writers such
   as the loader's first-touch ReadFile case.

**Verdict:** the change signal and the new alias-list maintenance are measured feasible in synthetic xmem tests, but a
narrowed copy still needs a device-side changed-page consumer and a census of kernel writes into title pages. The new
alias hook does not make the title's pages GPU-visible; section 3.2's external-memory and shader-address blockers still
apply. The safest first step this measurement supports remains the pull coalescer below, whose measured population is
zero at this frontier.

## 4. What this slice implemented: a per-submission title-pull coalescer

**The hypothesis it was built on:** the frame records **two draws in one submission**
([page_address_space.md](page_address_space.md) section 4), both draws are full-screen triangles into
same-size targets, and every draw's vertex/index/root-descriptor pull goes through the replay's
`pull_title_pages` hook; if the two draws name the same vertex/index pages, the second draw's pull repeats the first's,
and reusing the first copy is safe without any claim about *what changed*, because the second copy would put exactly the
same bytes in the same arena range. **Section 6 measures the hypothesis false on this tree**: in 8,186 pulls over 8,957
submissions, no pull repeated an earlier range in its own submission and no pull repeated the previous submission's
ranges - the title gives each draw different per-frame ring bytes. The step is therefore landed as the safest measured
form of the peer mechanism, switched off, with its own trace; the measurement is the negative, and it is the reason
this page can say the cross-submission cache has no population at this frontier.

**The implementation, in our structure:**

- `port/src/title_pull_windows.h` - the pure policy: a bounded (64) interval set of 4 KiB-granular title ranges copied
  so far in a submission. `covers(va, bytes)` is true only when *every* byte is inside one window; `add` merges
  overlapping and adjacent ranges; past the bound the lowest window is dropped, which can only cause a copy that would
  have been skipped. No Windows, no Vulkan, no allocation (a fixed array), so it is tested without a GPU.
- `port/src/vulkan_backend.h` - the switches and the two hooks: `title_pull_begin_submission()` (called by
  `sync_console_heaps_measured`, once per submission, before the lists are recorded) resets the window and keeps the
  previous submission's windows for the trace; `title_pull_decide(va, bytes)` answers copy/skip and counts requests,
  bytes, copies, skipped bytes, the same-submission repeats and the previous-submission repeats. The replay's
  `pull_title_pages` lambda asks it first and, only when the answer is "copy", calls the device's pull and times it.
- Every switch is read once per process and **off by default**, so the default path is exactly the tree before this
  slice: one branch in a lambda, no clock read, no state change.

| switch | effect |
|---|---|
| `PALMETTO_TITLE_PULL_TRACE=1` | The measurement arm. Counts every request/byte/copy/skip/repeat and prints a line every 2,048 submissions into stderr (`[rt] replay: title pulls: ...`). The copy path is unchanged: TRACE counts what COALESCE would have saved. |
| `PALMETTO_TITLE_PULL_COALESCE=1` | The implemented step. A pull whose every byte this submission already copied is skipped; a later draw then reads the arena bytes the earlier draw's copy put there. Measured at this frontier: it skips nothing (section 6), so enabling it changes no submitted command. |

The coalescer deliberately does **not** cover the table pull (its copies are inside `console_pull_table` in the device
file, outside this slice's scope) and does **not** claim anything across submissions; the trace's
"previous-submission repeat" figure is the measurement that sizes a future guard-based cache, and it measured 0 here.

**Why this was the safe first step, and what the measurement now says:** it makes no decision about a page's contents,
so it cannot go stale in the way a write-watch gate can; the one assumption - that a second pull of a range would copy
the same bytes - is the existing pull's own assumption between preparation and execution. The A/B's job was to test the
population, and it measured none: the pulls are distinct per-frame ranges. Two conclusions follow, and the page keeps
both: (1) the coalescer is inert here and stays off, so the default path is unchanged; (2) a cross-submission
guard-based cache would also remove nothing at this frontier, which saves the device-side hook
[card_copies.md](card_copies.md) section 5 names from being written for a zero population.

## 5. Cross-check against the references

Reference-only, no code, comment text, identifier or table copied. The peer numbers and files are in section 1 and
[cleanroom_peers.md](cleanroom_peers.md).

- **Agreement on the mechanism class.** XCL's no-copy windows, victory-fork's in-place heaps and guard-based write
  watch, five-star's 64 KiB page heaps and VIctory's read-only guards all name the same two halves: *a memory visible
  to the CPU and the GPU without a copy*, and *a signal for what changed*. Our measurement agrees that the copy is the
  cost, and locates it: the mirror compare is the largest CPU block on the recording thread, while the per-draw pull's
  own copy measured only 54.4 ms over 130.4 s (0.04% of the boot) and its ranges do not repeat.
- **Measured disagreement on feasibility here.** Victory-fork can open some title memory as a card heap; our measured
  182 MB placed range cannot be imported at all (lazy commit), and an imported range cannot carry a V# base because of
  the measured 48-bit bias. XCL's window model gets its GPU address to equal the console VA by rebuilding the address
  space; our arena is one buffer with a driver-chosen base, so the same mechanism is a re-architecture here, not a
  switch.
- **Measured disagreement on the change signal.** The peers' write watch is the OS watch or read-only guards; our
  measured memory refuses `GetWriteWatch`, and the read-only form would break the loader's 271 MB kernel `ReadFile`
  (XCL independently recorded the same kernel-write trade-off). Our own pull path also skips guarded pages, which the
  peers' structures do not have to care about because their copies are device-side.
- **Where the numbers are not comparable.** The peers' 68.0 -> 49.8 ms of card time, 22,900 read-backs in six minutes
  and ~0.8 ms write-watch scans are their measurements on their structures and content population; this page's numbers
  are ours. A copy-side win here is bounded by our two consumers' measured costs, not by theirs.

## 6. A/B measurement

One 130 s A/B pair, same machine, same title, same binary (`port/build/palmetto.exe`, SHA-256 in the boot directory's
`meta.txt`), boot lock held, one boot at a time, harness `tooling/boot_report.py`. Standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL`, presenter capture on:
`PALMETTO_CAPTURE`, one every 2,000 presented, up to 6), external private-bytes sample every 5 s. Both arms set
`PALMETTO_TITLE_PULL_TRACE=1`; arm B adds `PALMETTO_TITLE_PULL_COALESCE=1`. Outputs are outside the repository
(`D:\leonida-work\memmodel\boot-a`, `boot-b`).

| measure | A: `PALMETTO_TITLE_PULL_TRACE=1` | B: + `PALMETTO_TITLE_PULL_COALESCE=1` |
|---|---|---|
| run | ALIVE 130.4 s, killed by the timeout as usual, **0 exceptions**, 0 `device was lost`, 0 validation errors | ALIVE 130.3 s, killed by the timeout as usual, **0 exceptions**, 0 `device was lost`, 0 validation errors |
| d3d12 calls recorded (max #) | 220,495 (#223,619) | 220,668 (#223,573) |
| `ExecuteCommandLists` ok / failed | **8,957 / 0** | **8,955 / 0** |
| `PresentX` ok / failed | **4,478 / 0** | **4,477 / 0** |
| `DrawInstanced` | 8,954 | 8,952 |
| `Close` ok / failed | 9,582 / 0 | 9,579 / 0 |
| submission 8,192 reached at | **90.1 s** | **89.3 s** |
| title pull requests / requested bytes | **8,186 / 31.9 MiB** | **8,186 / 31.9 MiB** |
| pulls copied / skipped | 8,186 / **0** | 8,186 / **0** |
| repeated in submission (the coalescer's population) | **0 / 0.0 MiB** | **0 / 0.0 MiB** |
| previous-submission repeat (a cross-submission cache's population) | **0 / 0.0 MiB** | **0 / 0.0 MiB** |
| pull copy time / per copy / max | 54.4 ms / 7 us / 4,248 us | 57.2 ms / 7 us / 6,643 us |
| captures | 3 frames (1, 2,001, 4,001) at 2564x1421, mean rgb **0 0 0** each | same 3 frames, same mean; all 6 PPMs **byte-identical** (SHA-256 `1B8380D477F7D161CCBC67BB25413CABCE3C6391C7E40AAA16BFA750F8F64207` in both arms) |
| host private bytes, average from 30 s (external sample) | 28.35 GiB (max 35.99) | 29.76 GiB (max 36.12) |
| host working set, average from 30 s | 21.67 GiB | 22.73 GiB |
| verdict | **no win, no regression**: the coalescer has **zero population** on this title - every one of the 8,186 pulls is a different byte range, so COALESCE changed nothing (0 skips). The two arms submit, present and draw the same work (within 0.02%), have the same 0 exceptions/device-lost/validation errors, and their captures are byte-identical. The 54.4 vs 57.2 ms pull time and the memory difference are boot-to-boot noise between two arms that executed identical copies; no effect is claimed for either. | |

The history this pair writes into the page: the pre-slice estimate of the pull population (two full-screen draws in one
submission, "about 2 KiB a draw") predicted a same-submission repeat; the counters say the title's draws name
*different* vertex, index and constant ranges (arm A's first pull lines: one draw's vertex VA `...bef30 + 0xFF0` and
root window `...ff20 + 0x60`, the next draw's `...df10 + 0xFF0`), and the same holds across submissions
(`previous-submission repeat 0` in all 4 milestone lines, at submissions 2,048/4,096/6,144/8,192). That is the measured
reason the peer copy-avoidance models have nothing to reuse here yet: streamed content, which creates repeated reads
of the same pages, is the missing population.

Reproduction: `boot-a` and `boot-b` under `D:\leonida-work\memmodel` (outside the repository), each with `boot.out`
(the harness summary above), `stderr.log` with the four `[rt] replay: title pulls:` milestone lines, `boot.jsonl`,
`series.csv`, `capture/index.txt` and the three PPMs, and `meta.txt` naming the binary (SHA-256
`198D69DF...72FD8`) and the exact environment. Harness `tooling/boot_report.py`; boot lock held through
`_setup\boot_lock.ps1 -Acquire -Who memmodel` and released after both arms.

## 7. Limits and the next falsifier

- **The step has no population at this frontier - measured, not assumed.** 8,186 pulls, 0 same-submission repeats and
  0 previous-submission repeats (section 6). The A/B's job was to test the population and it failed to find one; the
  step stays off. It becomes useful only when the same title range is pulled twice without a change between the pulls,
  which is what streamed content is expected to do.
- **The wrapped hook measured 54.4 ms over 130.4 s, but the older pull A/B attributes ~440 us a submission to the
  whole pull path.** The difference has to be in the parts this slice does not wrap: `console_pull_table` (64 slots
  scanned and the V# bytes copied, inside the device file) and the post-slice mirror reads. That counter is the next
  instrument, and it is a small device-file addition - the same follow-up that would make the table copy coalescable.
- **Table pulls are not coalesced.** `console_pull_table` makes its own copies inside the device file; the wrapper
  covers only the replay's `pull_title_pages` hook.
- **The trace counts requested bytes, not copied bytes.** The device caps a pull at the alias range and at the arena
  boundary, and leaves uncommitted pages alone; the counters report what was asked for, so arms compare like with like,
  not against the device's own byte counter.
- **The picture check is the capture, and at this frontier every capture is black.** The two arms' captures are
  byte-identical, which proves no drawn content changed - not that a black frame stayed black for the same reason.
- **No write watch, alias expansion or import was enabled in the A/B.** The guard tracker stays readied
  (`PALMETTO_CARD_COPY_WATCH`, off), and nothing about title memory is pinned. Section 10's
  `PALMETTO_XMEM_ALIAS_WATCH` was implemented later and has only synthetic host-test evidence; it was not part of these
  runs.
- **The cross-submission cache is measured to be worthless *here*, so a device-side changed-page hook is not worth
  writing for it at this frontier.** The `previous-submission repeat 0` figure is the population and it is zero. The
  CPU alias-view list/map-refresh primitive now exists behind the separate switch (section 10), but a changed-page
  consumer, a title population that repeats reads and a kernel-write census remain prerequisites before a useful
  guard-based cache could be considered.
- **The next falsifier:** the content path. The first streamed block or texture fill that makes a draw re-read the same
  title pages (or the title's page-mapping batches reaching the boot, [cpu_view_mapping.md](cpu_view_mapping.md)) is
  where this step gets a population and where the guard-based cache's prerequisites become worth their cost. A negative
  at that point would say the peers' copy model does not transfer to this title's access pattern.

## 8. Tests

- `port/test/pipeline_recipe_cache_test.cpp` - the existing GPU-free section, extended with **24 EXPECT assertions**
  (the test binary passes; its line `title pull coalescer: ok (policy and VulkanContext decision)`): the pure policy's
  coverage, partial-range refusal, merge of overlap and adjacency, the empty/overflowing range, the 64-window bound
  (the lowest window is dropped and its range is copied again), and the real `VulkanContext` decision path with fake
  Vulkan procedures - TRACE counts a repeat and never skips, COALESCE skips a covered pull and copies a range that is
  not fully covered, and a new submission starts with an empty window. No device, no title memory, no game data.
- Section 10 extends `port/test/xmem_test.cpp` with synthetic alias-watch controls (the default per-VA false negative,
  writes via another view, a new alias mapped after arm, source remap, eight-range overflow/fallback, and
  `write_physical_pages` coherence) and extends `port/test/buffer_va_index_test.cpp` to show two title VAs can rebase to
  one arena address only when an alias identity is already supplied. These tests do not establish physical-to-GPU page
  identity. The earlier coalescer A/B is unchanged and TRACE still only changes counters.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`; the gate
  binaries pass (`xs_proxy_mock_test` PASS, `unimplemented_policy_test` 98 checks/0 failed, `graphics_record_test`
  180/0, `graphics_pso_test` 312/0, `xbox_present_test` all cases, `buffer_va_index_test` pass, `dstorage_test` 224/0,
  `xapu_chad_test` 87/0, `deferred_compile_test` 18/0, `frame_dump_test` pass). `d3d12_fence_timing_test` crashes in
  `Fence::from` on queue console slot 17, the **documented pre-existing crash** that predates this slice and is
  unchanged by it ([issue1_baseline.md](issue1_baseline.md) section "gates" records the same binary built against HEAD's
  `d3d12_xs_device.cpp` crashing identically). The documentation checks (`tests/test_docs_*.py`) pass 42 tests; the
  environment-variable check is the one exception and only because it enumerates `git ls-files` and this page is
  untracked at slice end - with `docs/title_memory_model.md` added to that list the check passes (verified), so it goes
  green with the lead's commit. No git write was made by this slice.

## 9. Attribution

This slice was implemented from this project's own measurements: the consumer table, the A/B boots, the counter trace,
the coalescer policy and its tests are ours. The behaviour was cross-checked against these public GPL projects, which
are evidence sources here and not code sources - no code, comment text, identifier or table was copied from them:

- **XCL** (<https://github.com/xcl-dev/xcl>, GPL-2.0-only), clone `6d12f58` (ivall): README's "title graphics memory
  shared by CPU and GPU without copies, up to 16 GB (4 GiB windows)", and its `HANDOFF.md` write-watch/sparse-window
  notes and their measured costs and kernel-`ReadFile` trade-off.
- **victory-fork** (<https://github.com/fxceless/victory-fork>, GPL-3.0-or-later), clone `9c184b3` (fxceless):
  README's card-memory copies, guard-based write watch, alias-view report, in-place heap status and its measured pinning
  limit.
- **five-star** (<https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later), clone `0bfa755`
  (ThatRecompDev): the documented "a page mapped at two addresses is one page" / pagefile-section heap model.
- **VIctory** (<https://github.com/Marvisak/VIctory>, GPL-3.0-or-later), clone `13f91ca` (Marvis): the read-only-guard
  write-detection and per-page raw-view model.
- **ubiquitarianism/leonida** (<https://github.com/ubiquitarianism/leonida>, GPL-3.0): the reference the programme's
  [upstream_port.md](upstream_port.md) policy names, clone `ed94fbef9`; its page/title-memory pages are the prior art
  [cleanroom_peers.md](cleanroom_peers.md) ranks, and this page's measurement is our own.

The upstream projects remain the copyright of their contributors under their own licenses. `NOTICE.md`'s attribution
entry for this slice is owed by the lead if the mechanism (rather than only its measurement) is adopted; this page
carries the credit now.

## 10. Opt-in alias-expanded CPU page watch (not GPU-visible aliasing)

Section 3.4's false-negative control is now implemented as a separate, opt-in xmem page-watch hook. It closes one
narrow gap: when several CPU virtual addresses map the same xmem physical page, a PAGE_GUARD placed only on the source
VA does not see a store through another VA. It does **not** change the Vulkan memory object, arena layout, shader address
equation, or the bytes the GPU reads. The one-shot pull-coalescer A/B in section 6 remains the only title boot measured
for this page; the alias-aware watcher has synthetic host-test evidence and is not A/B measured in a title boot.

### Switch and selection

| switch | effect |
|---|---|
| `PALMETTO_XMEM_ALIAS_WATCH=1` | Requests the alias-expanding arm only when the existing `PALMETTO_PLANE_REFRESH=watch` path is active and a placed texture is imported within its 1 MiB watch cap. Unset, empty, or a value beginning with `0` is off; any other non-empty value is on. This switch alone does not turn plane refresh or texture import on. |
| `PALMETTO_XMEM_ALIAS_WATCH=0` (or unset) | Keeps the existing single-VA `page_watch_arm` behavior; no alias ranges are discovered. Default, unchanged. |

With the alias switch on, a missing/failed alias hook does not silently fall back to a single-VA guard: that resource is marked unarmed and its per-resource refresh verdict is `unavailable`, so it hashes/compares even if another range made the shared tracker answer `unchanged`. This closes the shared-tracker case where an unarmed texture could otherwise inherit another texture's skip. Alias mode is deliberately selected only for `watch`; hash/off behavior stays as before.

### Rebuild rule and bounds

`page_watch_arm_aliases` rounds the requested source span to 4 KiB guard pages, looks up each backing-section identity and 64 KiB offset in xmem's `g_mapped` table, and includes every currently mapped view of those exact bytes. It deliberately does not key by title page number alone: XMem recycles frame numbers, while an old mapped view can still own a different section. Map, remap and unmap operations call the alias-watch notifier while the mapping lock is held; a poll serializes with that notifier while an alias owner exists. Each successful rebuild forces one `changed` verdict before a future skip. The ordinary `page_watch_arm` entry point remains a per-VA watcher.

The published tracker is bounded to eight intervals and eight owners. A candidate that exceeds the interval cap, overlaps another owner's guard, or contains a page that cannot be made writable/guarded invalidates that alias owner and drops its partial intervals. A live rearm failure does the same. `page_watch_poll` then answers `unavailable` until the owner is forgotten; the device compares/hashes instead of trusting incomplete coverage. The separate existing 16,384-trip record is unchanged by this slice and is not what the alias-interval overflow test exercises. The bound is intentionally conservative; there is no claim that the title's map population fits it.

`write_physical_pages` still writes the shared backing page and repeats the bytes through each current armed view so its PAGE_GUARD trips reach the same poll the plane refresh consumes. That is tested separately from any title DirectStorage activity: the measured boot still has zero page-list requests.

### What the CPU-only tests establish

- `port/test/xmem_test.cpp` pins the default negative: two VAs map one page, a store through the second VA does not trip the ordinary single-VA arm. The opt-in arm sees that store, expands after a new alias is mapped, changes coverage after the source VA is remapped, stops watching aliases of the old page, and watches an existing view of the new page.
- The same test delivers a synthetic physical-page write through the active alias set and checks the bytes at both views, `watch_views`, and the `changed` poll. It maps a ninth disjoint view after an eight-view arm and checks that the partial watch is disarmed (`page_watch_pages()==0`) and repeated polls stay `unavailable`.
- Switch parsing checks unset/null, empty, zero, and non-empty-on values. `port/test/buffer_va_index_test.cpp` checks that two distinct title VAs can be rebased to the same arena address if a caller supplies that identity. `BufferVaIndex` and `ConsoleVaMap` still index VA ranges; neither derives an xmem physical-page identity, and this test does not create a GPU mapping.
- Device integration keeps a per-texture `placed_watch_armed` bit. When it is false, the texture sees `unavailable` regardless of the shared poll's `unchanged` result, which preserves the full-compare fallback for partial arm failure.
- Final CPU verification: `cmake --build port/build --target xmem_test buffer_va_index_test -j 8` completed; `xmem_test: OK (496 checks)` and `buffer_va_index_test` reported that its fixed cases and 200 random tables agree with the linear scan. The first test run exposed that XMem recycles page numbers while old section-backed mappings can still exist; the implementation and the `write_physical_pages` fanout now compare backing `Section` identity plus offset. The alias test also first-touches the remap and capacity-fixture pages before guarding them, because the watch correctly refuses uncommitted section pages. `python -I tests/test_docs_env_vars.py` also passes (4/4) with this switch named in the tracked page. The coordinating agent is running the latest `palmetto` full gate; these host-only checks are not a title boot or GPU-alias proof.

### Limits and next falsifier

This is **CPU alias watch**, not the peers' GPU-visible page heap: it observes user-mode writes through current xmem CPU views and repairs the watched view set after xmem mapping calls. It does not make those pages a `VkDeviceMemory` object, prove `VK_KHR_external_memory_win32` accepts the section, map slot 31, or make a shader address resolve to the same physical page. Slot-31 page-mapping calls and title page-list destinations remain zero at the measured frontier (section 2.1). Kernel writers beyond the tested `write_physical_pages` hook, including first-touch `ReadFile`, remain a separate measurement and limitation.

The falsifier for a future GPU-backed test is an actually populated, successfully imported title texture on a page with at least two xmem CPU views: arm alias mode; write through the non-source view; verify the poll cannot say `unchanged`; then verify the submitted GPU stand-in reflects the changed bytes after the compare/re-upload. Repeat after source remap and after an alias is removed. A stale GPU read or an `unchanged` verdict after a store through a live alias rejects the design. Until such a resource/population exists, no title GPU-alias or performance win is claimed.

## 11. P-30 title alias/watch boot deferred by the stop rule

**Status: not run; no new title alias or page-list evidence.** P-30 called for one light alias/watch arm after the P-31 submission-overlap pair. The first `PALMETTO_SUBMIT_OVERLAP=1` arm logged one Vulkan device loss and stopped in the title at 64.3 s; its single permitted retry logged a second device loss and stopped at 68.5 s. The task's two-consecutive-loss rule therefore ended the whole series before a P-30 arm could start. The boot lock was released, and no Palmetto process remained. The loss logs and the full P-31 comparison are in [peer_port_streaming.md](peer_port_streaming.md), section 16.

This is **not** a controlled negative. No P-30 environment was launched, so these runs say nothing about whether the title changes a GPU-view alias or delivers page-list destinations that the watch catches. The evidence already recorded above remains as it was: section 10's alias-expanded watcher has synthetic CPU-view tests only; section 2.1's zero slot-31/page-list counts are from its earlier measured frontier, not this follow-up. The prior GPU-view reservation measurement in [peer_port_movie.md](peer_port_movie.md), section 11, likewise observed no placed texture resolved through the paired view in its sampled window; it is not a P-30 result and is not proof that the title never uses such a resource later.

**Next falsifier, only after the host is cleared for a new arm.** Use one light, content-backed boot with the existing opt-ins: `PALMETTO_TEXTURE_IMPORT=1`, `PALMETTO_PLANE_REFRESH=watch`, `PALMETTO_PLANE_REFRESH_TRACE=1`, `PALMETTO_XMEM_ALIAS_WATCH=1`, and `PALMETTO_GPU_VIEW_PAIR=on` plus `PALMETTO_GPU_VIEW_PAIR_TRACE=1`; add `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_DS_PAGES=1`, and `PALMETTO_DS_PAGES_VIEW=1` to sample actual page-list destinations, without injecting a synthetic request. Record whether a placed texture resolves through a recorded pair, whether the watch arms/rebuilds current CPU views, and whether any page-list write is delivered to an armed view. A zero population would be a negative only for that bounded title window. Even a positive CPU-view or GPU-view-pair record would not alone prove shared GPU physical memory: the section-10 GPU-backed falsifier must still show the changed bytes in the submitted stand-in after a write through a live alias. No P-30 probe code or game data was added here.

## 12. Rank-4 native write faults and shadow invalidation (synthetic readiness, not title-armed)

**Status: current gap measured from our code and retained counters; a separate read-only write-fault retry is implemented behind an explicit xmem arm and has synthetic host evidence only. No title consumer calls the new arm.** This closes a narrowly testable fault-handling gap; it does not claim that the title is write-watched, that kernel writes are observable, or that a GPU shadow is now incremental.

### What the current tree and traces establish

`port/shims/xmem.cpp` has two distinct mechanisms that must not be conflated:

- `physical_page_fault` commits a `SEC_RESERVE` backing page when user code first touches a mapped view. Once `commit_mapped_page` reports `committed_before`, a write to a committed `PAGE_READONLY` view fails `access_allowed` and the handler returns `EXCEPTION_CONTINUE_SEARCH`. The existing xmem test deliberately verifies that a write to both a whole read-only view and a read-only 4 KiB part of a writable view still reaches the next handler. Thus first-touch is not repeat-write watch.
- The separate `PAGE_GUARD` tracker is one-shot per access: Windows clears the guard, the handler records read/write trips without taking the tracker mutex, and a poll/rearm is required before another access on that page is signalled. Its alias-expanded variant can track current xmem views, but it is not a `PAGE_READONLY` write-fault retry.

The retained copy traces in `cardcopies/boot-a/stderr.log` and `cardcopies/boot-b/stderr.log` report `write tracker 0 range(s)` and `page watch changed 0, read-trip 0`. Boot A's 8,192 refreshes show no watcher skips; boot B's skipped refreshes are explicitly `no-shader`, not unchanged-page verdicts. Those runs had the tracker disabled, so zero ranges/trips is **not** a title-negative for write activity. The retained first-touch trace in [readfile_first_touch.md](readfile_first_touch.md) records 4,136 pages / 271,044,608 bytes touched from user mode before one `ReadFile` retry succeeded. That is evidence that the kernel copy did not produce the user-mode first-touch faults; it is not a measurement of a committed read-only `ReadFile` destination, which this change still refuses to claim.

The existing remap stress test is synthetic: four writer threads race 256 map/unmap slices over 4,096 pages, and its acceptance bound is completion under 30 seconds. In the latest run, it passed with 76,047 writes landing, 486 writes correctly hitting an unmapped view, and 15 ms at millisecond resolution. Counts vary with scheduling and are not title timings. The map path takes `g_fault_lock` exclusively; ordinary first-touch takes it shared. The new write-fault path instead tries that mapping lock at most 32 times and never takes the watcher's `std::mutex` or a process-heap lock.

### Opt-in coverage and limits

`PALMETTO_XMEM_NATIVE_WRITE_WATCH=on` (also exact `1`, `yes`, or `true`, case-insensitive) permits an explicit caller of `rt::xmem::page_write_fault_arm`; unset, empty, `0`, `off`, and unknown values stay off. The switch alone neither changes mappings nor arms title memory, and no plane/descriptor consumer is wired to call this API. With it off, the new arm returns the named `disabled` refusal, does not change protection or the shadow epoch, and ordinary stores remain byte-identical.

The arm rounds to 64 KiB xmem views, accepts at most 16,384 pages (1 GiB) per owner and eight disjoint owners, and requires every page to be a committed, `MEM_MAPPED`, exactly `PAGE_READWRITE` xmem view. It refuses by name (`invalid-range`, `too-many-pages`, `not-xmem-mapped`, `uncommitted-page`, `unsupported-protection`, `alias-present`, `owner-limit`, `overlap`, `allocation-failed`, `protect-failed`, `already-armed`, `owner-unavailable`, or `disabled`). Existing backing aliases are refused rather than partially guarded. A mapping change that introduces an alias or remaps an owned view invalidates the tracker; `poll` returns `unavailable`, requiring the consumer's full compare.

For an armed, unaliased view, the first user-mode write raises an access violation. The handler verifies that the view is still committed/read-only, changes that 64 KiB view to read/write, records the page, advances a process-wide shadow epoch, and retries the original store. More stores before the next poll do not fault again; a changed poll re-protects the recorded page, so a later write can be detected in the next epoch. The epoch is an invalidation signal, not a linearizable snapshot: it advances before the faulting instruction retries, so a consumer that copies concurrently with title writes still needs synchronization or a periodic full compare. The `xmem::write_physical_pages` hook writes the section's shared backing view without touching the read-only CPU mapping, so it separately advances the epoch for matching backing pages. DStorage currently uses that hook only in its opt-in `PALMETTO_DS_PAGES_VIEW` destination branch; the default resolver scatter is not marked by this hook and must not be treated as covered.

The fault handler publishes only fixed-size address intervals; it does not allocate or take the tracker mutex. If the mapping SRW lock stays busy for all 32 try-acquisitions, if an unexpected protection is found, or if the bounded trip list overflows, the handler refuses to claim success and the next poll is `unavailable`; a caller must do a full compare. This bound is intentionally fail-closed, not a promise that a contended write succeeds. Kernel writes (including `ReadFile`), writes through an alias created outside the tracked view, and GPU writes cannot be intercepted by this native CPU fault path. This is synthetic readiness only until a consumer can prove that its watched pages are committed, unaliased, CPU-written, and shadowed.

### Synthetic validation and next falsifier

`port/test/xmem_test.cpp` exercises the actual vectored handler and xmem mappings: switch-off protection/epoch identity; named refusals for host, uncommitted and aliased pages; successful write retry and repeat-write behavior; stale-shadow epoch and byte refresh; physical-page delivery invalidation without a CPU fault; post-arm alias invalidation; and a held-lock fault that returns within the explicit test bound, increments the lock-refusal counter, and makes the poll unavailable. The xmem binary passes 545 checks total. No GPU test is needed for this CPU-side signal, and no game data is used.

No title arm was run: the host/boot slot was occupied by another agent's arm (`palmetto` was active and `D:\leonida-work\.boot-lock` existed), and the lead explicitly reserved that arm. The prior write-watch A/B counters above are not substituted for this missing title measurement. The next falsifier is to wire a small, explicitly selected committed xmem range with a known shadow into a consumer during a free, authorized host/boot window: a CPU store must increment the epoch, yield `changed`, and leave the shadow stale until refreshed; a second store after poll must fault again; a page-list write routed through `write_physical_pages` (DStorage's `PALMETTO_DS_PAGES_VIEW` branch) must change the epoch without a CPU fault; and a live alias/remap must yield `unavailable`, never `unchanged`. Separately, the default DStorage resolver-scatter path must remain on full compare until it is wired to an invalidation hook. Any stale shadow paired with `unchanged`, a lost retried store, or a mapping change that still permits a skip falsifies the implementation. A title result would still not cover `ReadFile`/other kernel writers without a separate mechanism.

Peer context is behavior-only and is recorded in the existing [cleanroom_peers.md](cleanroom_peers.md) audit: Five Star's physical-page alias/first-touch model, VIctory's per-page guard and alias-reporting description at its historical review pin, and XCL's write-watch scan cost plus the `ReadFile`/kernel-writer limitation (also present in XCL `HANDOFF.md` at `623d02e`). The current clone-head inventory there names Five Star Recomp (`34b4dfed`, GPL-3.0-or-later), VIctory (`63bcca4`, GPL-3.0-or-later), and XCL (`623d02e`, GPL-2.0-only); those current pins are not a claim that each current snapshot adds new write-watch evidence. The implementation and synthetic tests are this tree's own; no peer code, comments, identifiers, or tables were copied. A supplemental `NOTICE.md` entry is drafted in the task report but is not applied here.
