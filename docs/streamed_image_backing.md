# Pooled and deferred device-local image memory

**Status: the mechanism is implemented behind two opt-in switches, unit- and GPU-tested, and measured; on this tree's
load stage the measured need is small, so the default path is unchanged and the slice is preparatory.** One 130 s boot
of the load stage creates 32 image allocations totalling 279.1 MiB, every one in device-local memory: a pool saves about
one driver allocation per small texture and deferred backing has no population to defer in this window. The mechanism
is the piece the streamed/placed image population needs once the title's streamed textures reach device creation (they
do not yet: [streamed_consumer.md](streamed_consumer.md) section 4), so it is landed complete and off, with its
measured numbers below rather than as an unmeasured claim.

Counts, sizes, offsets, switches and times only; no package bytes, paths, shader bytes or extracted tables.

## 1. The measured need on our tree

A census switch (`PALMETTO_IMAGE_MEM_TRACE=1`, section 6) records every image `create_texture` backs: its description,
the flags of the memory type it got, its size and its time since the first image, plus a summary line every eight
images with the running totals (including every other `VkDeviceMemory` the layer allocates). Standard environment (the
other runtime pages'), harness `tooling/boot_report.py`, 130 s, one boot:

| Measure | Value |
|---|---|
| Run | ALIVE 130.3 s, killed by the timeout as usual, 0 exceptions, 499,779 recorded d3d12 calls |
| Image allocations | **32**, total **279.1 MiB** (292,662,272 B) |
| Memory types | **32 device-local, 0 host-visible, 0 other** |
| Size spread | 512 B .. 63.75 MiB; 19 images at or under 1 MiB, 22 at or under 4 MiB |
| Flags | 15 render targets, 3 depth targets, 14 neither (the sampled/UAV-only set the pool can share) |
| Time | 2 at 0.0 s (the two 3840x2160 display targets, 33,423,360 B each), the rest at 17.6-22.3 s, none after |
| Load-stage image burst | 30 images in the load window, the pipeline-precompile window |
| Per-allocation pattern | one `vkAllocateMemory`/`vkFreeMemory` per image, all dedicated, no sharing |
| All `VkDeviceMemory`, all kinds | **38 allocations, 535.2 MiB** (the images plus the arena, heaps, staging and non-arena buffers) |

The census also prints the device's allocation total. In this boot the layer's *other* `VkDeviceMemory` allocations
(buffers outside the arena, heaps, staging, the GPU address arena) are counted in the same summary lines; the image
allocations above are the whole image population, and the boot makes no other device memory after the load burst.

This is the honest conclusion for the pool question on this tree: the title's streamed blocks are read by a CPU
deserializer and never named to the device (*streamed_consumer.md* section 4), so there is no 16,000-image streamed
population to pool here, and 32 dedicated allocations are far below both the driver cost and Vulkan's
`maxMemoryAllocationCount` (4096 on the measured driver). The measured case that justifies the mechanism is upstream's
(streamed and placed texture populations, hundreds to tens of thousands of images) and it is not reachable on this tree
until that population reaches device creation. The slice is therefore built for the measured population it *will* meet,
kept off by default, and its A/B result is a negative (section 8) - which is a result.

## 2. The upstream mechanism, as a reference

The public GPL-3.0 fork (ubiquitarianism/leonida) measured and landed this first, and is this page's reference for the
*shape* of the mechanism, never a code source:

* `2b72fb53d` "d3d12_vulkan: textures share blocks of device memory (VK-IMAGE-MEMORY-POOL)", by TheGeogeo: every
  committed and placed texture made its own `vkAllocateMemory` (about 100 us) and released it with its own
  `vkFreeMemory` (about 200 us, RTX 3080); small device-local textures take ranges of 64 MiB blocks, render targets,
  depth buffers, CPU-visible textures and textures over an eighth of a block keep their own allocation; a range goes
  back to its block through the deferred deletion queue; a texture in a reused range is zeroed before a shader reads
  it.
* `780a8770c` "log once when a texture or default-heap buffer spills out of device-local memory (D3D-299)", by
  0xTehuti: the first allocation that preferred device-local memory and got a type without it is logged once.
* `273b3bdf9` (story-load integration, Moriz82): "Deferred backing: placed and streamed images get private memory at
  their first write or GPU copy", plus "Images on plain device-local memory: host-copy usage excluded it on NVIDIA and
  4.8 GB of images spilled into host memory; streamed images are now filled through staging."

This layer already had the D3D-299 half (`palmetto::spilled_to_host` in `port/src/vulkan_memory.h`, the one-time line
in `DeviceState::allocate`, docs/d3d12_vulkan.md "Memory type policy"). This slice adds the other two, re-derived for
this layer's structures: our own block pool (`port/src/image_memory_pool.h`), our own deferred-binding rule on the
slot-133 title-placed texture path, and the census above, which upstream does not have.

## 3. The pool

`port/src/image_memory_pool.h` (a header, so no build-system change; included by the device and by its test):

* **Blocks and ranges.** `ImageMemoryPool<Memory>` keeps, per memory type, a list of blocks of `block_bytes`; each
  block is an `ImageMemoryRuns`: the unallocated bytes as runs sorted by offset, so `allocate` takes the smallest run
  that holds the request at its alignment (best fit), and `release` puts a run back and merges it with its
  neighbours. Across blocks the pool picks the block whose largest free run wastes the least, so a nearly empty block
  is preferred and a full block is passed over on one comparison. The API is device-agnostic (the caller passes the
  allocate/free callbacks), which is what lets its policy be checked without a GPU.
* **What shares a block.** Only the set upstream measured as safe: an optimal-tiled image that is neither a render
  target nor a depth target, not CPU-visible, no larger than an eighth of a block, with alignment a block can hold,
  and one Vulkan does not require a dedicated allocation for (`vkGetImageMemoryRequirements2` with
  `VkMemoryDedicatedRequirements`; when the driver cannot be asked, the image keeps its own allocation). Every range in
  a block is optimal-tiled, so `bufferImageGranularity` does not apply inside one.
* **Block size.** 64 MiB, capped at a sixteenth of the largest device-local heap, rounded to 64 KiB, and only when a
  block of at least 1 MiB fits; a heap too small for a sensible block turns the pool off with one line. This is the
  same measured policy upstream chose, re-derived: it keeps a 256 MiB BAR window (no Resizable BAR) usable.
* **Type policy.** A pooled image's candidate types are `palmetto::memory_type_order(..., required = 0, preferred =
  DEVICE_LOCAL, size = block)`; a type whose block does not exist yet is asked for one only when the driver has room.
  A pooled image that lands in a type without device-local memory goes through the same one-time D3D-299 spill line as
  a dedicated one.
* **Dedicated fallback.** No block fit, the image is too large, requires a dedicated allocation, or the driver had no
  room: `allocate` returns null and `Resource::bind_image_backing` makes the image's own `VkDeviceMemory`, exactly as
  every image did before this change. After a failed block allocation the pool stops asking that type for a block for
  64 requests (a full heap stays full until something is given back, and a failed `vkAllocateMemory` is slow); a
  released range clears that guard.
* **Release.** A pooled range goes back to its block through the deferred deletion queue
  (`DeferredDeletionQueue::Kind::pooled_image_memory`, a token that drops the range), so another image cannot reuse the
  bytes while a submission that read them is still running; the block is freed when its last range goes, except one
  empty block per type, so a free/remake cycle does not call the driver each time. `~Device` drains the queue and then
  frees the remaining blocks before its Vulkan device goes.
* **Zero-fill semantics.** A range may hold an earlier image's bytes, and D3D12's committed resources start zeroed.
  The replay already zero-fills a committed image the first time a command touches it
  (`vulkan_replay.h`, `initialize`), which is how a dedicated allocation was treated before this change (Vulkan does
  not guarantee zeroed fresh memory either), so a pooled image keeps exactly the same first-use semantics.

## 4. Deferred backing

`PALMETTO_IMAGE_DEFERRED=1` (only meaningful with the slot-133 path, the measured title-placed texture path): a
title-placed texture that is not an attachment, not CPU-visible and does not require a dedicated allocation is created
with its `VkImage` but **no memory** (`Resource::create_texture(..., defer_image_memory = true)` from
`create_at_host_address`). Its memory is allocated and bound at the first of:

* **its first host write** - `import_placed_pages`, the measured page-import path, which needs the image bound before its
  staging copy; or
* **before the first submission that could use it** - `VulkanQueue::execute_now` calls `VulkanContext::bind_deferred_images()`
  before it prepares or records the lists, so the first command that could name the image finds it bound.

If the driver cannot back a deferred image, the submission is **refused** (`ReplayResult::unsupported`, with one
line) instead of recording a command that uses an image with no memory. An image that is never written and never
submitted is destroyed without an allocation.

**The limit, measured and stated plainly:** the second rule is a *pre-submission* boundary, not a per-use one. It
defers the allocation from creation to the next submission, which is the safe half of upstream's "first write or GPU
copy" rule; binding at the exact first GPU reference needs the replay to name the images a submission resolves (the
bindless/descriptor path included), which lives in `vulkan_replay.h` and is out of this slice's files. On a tree whose
title writes its placed texture pages only after creation (the measured one), the first rule is the operative one; on a
tree whose streamed population is created and never used, the second rule still allocates at the next submission and a
later slice removes that.

Measured behaviour of the rule in boot B: the placed, non-attachment, non-CPU texture set (14 of the boot's 32 images,
18.4 MiB) is created without memory; the census sees at most 3 of them unbacked at the same moment (18.5 s) and 1 at
19.2 s and 24.0 s, because the title's next submission arrives within about a second. In other words the rule is
engaged and correct here, and its saving at this population is one to three driver allocations held for under a
second: small, and reported as such. A population that creates textures and leaves them unused for long (the streamed
case) is what makes the rule pay, and that is where it will be measured next.

## 5. The one-time spill line

D3D-299's behaviour is unchanged and now also covers the pooled path: the first allocation that preferred device-local
memory, needed no host access and got a type without it logs `[rt] d3d12: a <n>-byte allocation did not fit in
device-local memory and went to memory type <t> ...` once. Dedicated and pooled allocations share the same
`std::once_flag`.

## 6. Switches

| switch | effect |
|---|---|
| `PALMETTO_IMAGE_MEM_POOL=1` | opt-in. Small non-attachment, non-CPU-visible textures in device-local memory (at most an eighth of a block) share ranges of 64 MiB blocks. Unset: every image keeps its own `VkDeviceMemory`, the previous behaviour exactly. |
| `PALMETTO_IMAGE_DEFERRED=1` | opt-in. A title-placed texture (device slot 133) that is not an attachment is created without memory and backed at its first host write or before the next submission. Unset: backed at creation, the previous behaviour exactly. |
| `PALMETTO_IMAGE_MEM_TRACE=1` | diagnostic only. The per-image census lines and the every-eight-image summary (section 1), bounded to 256 image lines and 512 summaries, plus the pool's shutdown line. Unset: no line, no counter work. |

All three are read once per process. With all of them unset the default path is the tree before this slice: same
`vkAllocateMemory` calls, same log lines, same call counts (section 8, boot A).

## 7. Tests

`port/test/d3d12_vulkan_device_test.cpp` (the existing device test target, extended - no new target):

* `image_memory_checks()` - the pool without a GPU, with a fake block allocator: best fit across runs, alignment,
  split and coalesce, largest-free-run prefilter, a request larger than a block left to the caller, range reuse without
  a driver call (and the `reused` flag), a new block when the first is full, the second empty block freed while one is
  kept, the driver-full dedicated fallback (null range, no crash), the 64-request retry guard, zero/alignment argument
  checks, a release of a range the block never handed out (ignored), `shutdown()` freeing every block, and the stats.
* `image_memory_gpu_checks()` - on a real device created with `PALMETTO_IMAGE_MEM_POOL=1` and
  `PALMETTO_IMAGE_DEFERRED=1` set before creation: three rounds of seven small sampled textures (4x4..256x256) are
  created, viewed (SRV) and freed, so blocks and range reuse are exercised; one pooled 8x8 texture round-trips a
  known pattern through an upload buffer and a readback buffer; then a title-placed texture is created through device
  slot 133 (`create_buffer_at_host_address`) with deferred backing, copied to a readback buffer before its pages are
  written (it must be backed by the queue hook and read as zero), and copied again after the pages are filled with a
  known pattern (the first host write imports them). The switches are restored afterwards.

`build_tests.cmd`'s target list does not include the device test; it is built and run directly
(`cmake --build port/build --target d3d12_vulkan_device_test`). This slice also adds `PALMETTO_IMAGE_MEM_ONLY=1` to
the test binary: it runs only these two checks, which is how they were verified independently of the rest of the
suite. Measured on the RTX 4090: `PALMETTO_IMAGE_MEM_ONLY=1 d3d12_vulkan_device_test.exe` exits 0, prints the pool's
policy line and "21 small textures shared blocks and round-tripped, a title-placed texture was backed at its first
submission and imported its pages".

The rest of the device suite is not green on this machine and not because of this slice: a full run crashes with
`0xC0000094` after ~326 s inside an early host-comparison section, `d3d12_fence_timing_test` crashes at the proxy's
console queue `Wait` slot 17, and the repository's own `_setup/ctest.log` (23/11) already records
`d3d12_vulkan_device` as Failed. An attribution run with this slice's `execute_now` hook removed rebuilt
`d3d12_fence_timing_test` and it crashed at the same instruction, so those failures are in the queue/proxy surface this
slice does not touch; the slice's own checks pass, and both A/B boots below are clean at 130 s with 0 exceptions.

## 8. A/B boot

Same tree, same machine (RTX 4090, Vulkan 1.3 driver), harness `tooling/boot_report.py`, 130 s, one boot at a time,
standard environment plus `PALMETTO_PIPELINE_TIMELINE=5` and the census; A has both switches unset (the default path),
B has `PALMETTO_IMAGE_MEM_POOL=1 PALMETTO_IMAGE_DEFERRED=1`. Host private bytes and GPU memory used are sampled every
5 s from outside the process (PowerShell `PrivateMemorySize64`/`WorkingSet64`, `nvidia-smi memory.used`).

| Measure | A: default | B: pool + deferred |
|---|---|---|
| Verdict | ALIVE 130.3 s, 0 exceptions, 0 device-lost | ALIVE 130.2 s, 0 exceptions, 0 device-lost |
| d3d12 calls recorded | 499,779 | 502,031 (+0.45%) |
| Image allocations | 32, 279.1 MiB, 32 device-local | 32, 279.1 MiB, 32 device-local (same population, minted in a different thread order) |
| Images pooled | 0 | **14** (18.4 MiB of the 32), in **1 block** of 64 MiB |
| `VkDeviceMemory` allocations of all kinds | 38, 535.2 MiB | **25**, 580.8 MiB (+45.6 MiB: the 64 MiB block less the 13 dedicated allocations it replaced) |
| Deferred images | 0 | 14 (the placed, non-attachment, non-CPU texture set); at most 3 unbacked at once (18.5 s), 1 at 19.2 s and 24.0 s: the deferral window is under a second here because the title's next submissions come quickly |
| Submissions at 130 s | 23,219 | 23,326 (+0.5%) |
| Presents at 130 s | 11,609 | 11,662 (+0.5%) |
| Pipelines (timeline at 125 s) | slot57 10,241/10,241/0 refused; slot55 41,545; graphics 51,786; compute 95 | identical |
| Captured presented frames | 6 (frames 1..10,001), mean rgb 0.00 0.00 0.00 | 6, identical mean rgb |
| Host private bytes, average from 30 s (whole process, sampled outside) | 12,153 MB | 12,194 MB (+41 MB, matching the block's over-provisioning) |
| GPU memory used, average from 30 s (whole GPU, `nvidia-smi`, other processes included) | 3,638 MiB (3,330..4,004) | 3,851 MiB (3,814..3,906) |
| Anything advancing (loader after 13.5 s, population, non-black capture) | no | no |

Result, honestly: **the two mechanisms change the allocation pattern exactly as designed and the boot does not change.** The pool
replaces 13 driver allocations with one 64 MiB block; because this tree's population is 18.4 MiB of poolable images, that
block over-provisions by 45.6 MiB, so total device memory *rises* 8.5% and host private memory rises 41 MB at this
population. Deferred backing is engaged (14 placed textures) but the title reaches its next submission within a second, so
the deferral never holds more than three allocations at once and the number of allocations is the same as with the pool
alone. Nothing in the boot advances: same pipeline population, submissions/presents within 0.5%, 6 black captures in both.
The pool becomes a saving only when the population is large (upstream's case: thousands of streamed images), which is not
reachable here yet. A negative, measured.

## 9. Limits and what remains of upstream's mechanism

* **Not deferred to the exact first GPU use.** See section 4: the pre-submission boundary is this slice's safe half;
  upstream's per-use pending-binding map lives in the replay and needs a later slice that touches
  `port/src/vulkan_replay.h`.
* **Pooled set.** Attachments (render targets, depth targets) and CPU-visible textures keep their own allocation, as
  upstream measured them to need; a larger streaming population may justify revisiting that, with the census to decide.
* **No `VkMemoryPriority`/eviction.** The pool never hands memory back to the driver under pressure and has no notion
  of resident/non-resident; it only reuses what the title released.
* **The measured effect on this tree is a negative**, because the population that motivates the mechanism (streamed and
  placed images in the thousands) is not reached: the title's streamed bytes die in CPU deserialization
  ([streamed_consumer.md](streamed_consumer.md)). The switch exists so the mechanism can be measured and enabled when
  that path is unblocked, with the same tests still covering it.
* **Upstream parts not taken:** its `TitleImagePool`'s 2 MiB slot/8 MiB page size classes and its
  `DeferredImageBinding` in the replay (both measured against its own population and structures, not ours), and its
  "images on plain device-local memory / staging fill for streamed images" change, which belongs with the DirectStorage
  and streamed-image path.

## 10. Credit

The mechanism is described and measured in the public GPL-3.0 fork (ubiquitarianism/leonida) and this slice names it as
its reference: commits `2b72fb53d` (TheGeogeo, VK-IMAGE-MEMORY-POOL), `780a8770c` (0xTehuti, D3D-299 one-time spill
line) and `273b3bdf9` (Moriz82, story-load integration: deferred backing, images on plain device-local memory). The
implementation here is this tree's own: `port/src/image_memory_pool.h`, the `Resource`/`DeviceState` integration, the
deferred rule on the slot-133 path, the census and the tests were written for this code base's structures and style,
and the measured need above is our own measurement, not upstream's numbers.
