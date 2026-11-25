# Title-placed texture page import, and naming the consumer of the streamed blocks

**Status: measured increment, negative on content; a later measurement names the streamed consumer and corrects this
page's next step (sections 4-5).** The surface is now
`port/shims/d3d12_vulkan_device.cpp` (the placed-texture stand-in) and the device entry points a consumer of the
title's memory would use. Two pieces were built and measured: a bounded page import for title-placed textures
(`PALMETTO_TEXTURE_IMPORT=1`, off by default), and off-by-default caller-RVA logging on the device creation/view/Map
paths (`PALMETTO_DEVICE_TRACE=1`). The A/B is still black, and the measurement says why: **the bytes the title's
texture-computer fill writes do not land in any title-placed resource's pages in the observed window**, so there is
nothing for the import to read where the sampled images live. The streamed-block consumer this page tried to name was
named later: a title CPU deserializer reads the blocks and never names them to the device, and the sampled 4K placed
texture's pages are never written (`docs/streamed_consumer.md`, `docs/texture_source.md`).

Counts, RVAs, formats and addresses only; no package bytes, paths or extracted tables.

## 1. What was implemented

`create_at_host_address` (device slot 133) already gave a title-placed **texture** an ordinary committed image of the
same description; its memory is ours, not the title's pages, so what the title writes at the placement address never
reached the sampled image (`docs/texture_layout.md`). With `PALMETTO_TEXTURE_IMPORT=1` the placement is also registered
in `DeviceState::placed_textures_pending` and the image imports the title's pages:

* the queue calls `VulkanContext::import_placed_textures()` at the top of `VulkanQueue::execute_now`, next to the
  console-heap refresh, so an import completes before the submission that could sample the image is recorded;
* `Resource::import_placed_pages()` copies subresource 0's tightly row-major bytes from the placement address into the
  committed image through the existing CPU-copy/staging path (`Resource::cpu_copy`), so the layout bookkeeping is the
  one every `WriteToSubresource` uses;
* it is retried at later submissions while the title's fill has not run yet (the fill happens on the title's thread
  after creation; a fixed attempt budget bounds it), and it is bounded and gated: at most 16 MiB per texture, pages must
  be committed and readable, and an all-zero head means "not filled yet", so a title-placed render target or shadow is
  never imported as uninitialised memory; more than one subresource imports subresource 0 only (the title's linear
  subresource order is not measured yet; the measured CPU-built textures are 1-mip);
* every import and every give-up is logged, and `texture_imports`/`texture_import_bytes` count them.

Default behaviour is bit-identical: with the variable unset the placement path is exactly the previous stand-in, and the
XG family's slots 2 and 10 still refuse, so the linear layout remains opt-in.

For the consumer side, `PALMETTO_DEVICE_TRACE=1` prints, once per site (eight lines), the caller frame that lies in the
title image for `CreateCommittedResource`, `CreatePlacedResource*`, `CreateRenderTargetView`,
`CreateShaderResourceView`, `CreateUnorderedAccessView` and `Map`/`Unmap`, with the description/format/size. It is
bounded, off by default, and does not change the path.

## 2. The A/B: does any pixel appear?

Same tree, harness boots only, standard environment plus `PALMETTO_XG_LAYOUT=linear`; import off in A, on in B. Readback
armed at the first draw (`PALMETTO_READBACK_AT_DRAW=1`), capture on, write control as always.

| | A: linear, import off (`docs/texture_layout.md` boot 2) | B: linear + `PALMETTO_TEXTURE_IMPORT=1` |
|---|---:|---:|
| Run | ALIVE 150.2 s, 0 exceptions, 0 device-lost | **ALIVE 150.3 s, 0 exceptions**, 0 device-lost, 0 refused submits, 0 failed `Close` |
| Draws / presents | 29,116 / 14,559 | 26,186-29,701 / 13,093-14,850 |
| Import lines | - | one unbounded run imported 8 textures (512 B .. 35 MB range); the bounded/gated runs imported **no** texture and skipped two 29.5/33.2 MB descriptions |
| Import retries | - | the imported-size gate or the "pages all zero" gate fired; one texture was retried 512 submissions and gave up |
| Target readback | mean 0 0 0, non-black 0/16,384 | **mean 0 0 0, non-black 0/16,384**; write control 51/102/204 |
| Captured presented frames | black, 24/24 | **black, 24/24** |

An early, deliberately unbounded version did import real title data into eight images (including 8294400 B, 16777216 B
and 29491200 B descriptions), but it also slowed submissions enough to expose a title-side timing fault and the boot
stopped at 28.9 s with a host C++ exception; it is not a usable scheme, and the bounded version above replaced it. The
bounded version is healthy on every gate but imports nothing, because the fill is not writing where the sampled image
lives (section 3).

## 3. Why the import has nothing to read there

The XG linear fill's destination addresses and the placements the device sees do not overlap. One boot
(`PALMETTO_XG_TRACE=1`, `PALMETTO_ARENA_LOG=1`):

| What | Addresses measured |
|---|---|
| XG slot 8 destinations (the fill) | `0x1E34C402000` (256x128 atlas, 32,768 B, row pitch 256), then consecutive 4 KiB pages `0x1E35A9C7000`, `...C8000`, `...C9000`, ... (1x1 defaults, 4 B each) |
| Title-placed textures (device slot 133) | `0x1E35AC00000` (1x1), `0x1E35B000000` (1x1), `0x1E35B400000` (3840x2160 format 65), `0x1E35BC00000` / `0x1E35CC00000` (2048x2048 format 87), `0x1E35DC00000` (2560x1440 format 10), `0x1E35FA00000` / `0x1E35FDC0000` (1280x720), ... |

The fill destinations are in the title's own allocation area, a few MiB to tens of MiB away from every placement; no
placed-texture address contains them. So the title builds its CPU texture data in memory that, in the observed window,
is never named by a device resource - the same pattern as the streamed 64 KiB blocks (section 4). The import machinery
works (the unbounded run copied title bytes into images); it has no correctly-addressed source for the sampled
textures yet.

## 4. Naming the consumer of the streamed blocks (`PALMETTO_DEVICE_TRACE=1`)

Boot 120 s, linear layout, import on, device trace on, ALIVE with 0 refused/0 device-lost; the first callers of every
instrumented entry point are title RVAs:

| Entry point | Title-side caller | First calls measured |
|---|---|---|
| `CreateCommittedResource` | `game+0x1E86853` | 1x DEFAULT buffer, 182,452,224 B (the 174 MiB pool) |
| `CreateCommittedResource` | `game+0x193E1BA` | 2x 3840x2160 format 28 textures (the display-sized targets) |
| `CreateCommittedResource` | `game+0x2481BD5` | 5x 19,680 B readback buffers |
| `CreateRenderTargetView` | `game+0xCC29EA` | rtv on the 3840x2160 textures |
| `CreateShaderResourceView` | `game+0x164F01` | srv on the streamed/placed images |
| `CreateUnorderedAccessView` | `game+0x1647616` | uav on the same |
| `Map` | `game+0xBA37B4` | the 19,680 B readback buffers |
| `Map` | `game+0x5121894` | two small title-placed textures (2,048 / 1,024 B) |
| `CreatePlacedResource*` | none | 0 calls in the window (title-placed resources arrive through device slot 133 instead) |

Nothing here maps or views the DirectStorage destination ranges: the queue's streamed blocks are ordinary process-heap
pointers (measured after `game+0x28C9A0E` enqueues them, `docs/wait_state.md` section 3), and the only large DEFAULT
allocation (`game+0x1E86853`, 174 MiB) is created once at load. So in 120-150 s the streamed data is never handed to
the GPU through any device entry point we instrumented.

**Correction, measured later - the consumer is named.** The inference that followed from this table ("either a title
function that has not run yet or one that copies memory without a D3D12 call") is resolved: the consumer *is* a title
function, and it never hands the blocks to the device. A `PAGE_GUARD` first-touch diagnostic named the reader at
`game+0x248F680`, reached as `[obj+0x18]` through the job wrapper `game+0x4F65820`: it reads the first batch's blocks
at ~2.3 s, and blocks sampled from later batches stay untouched. Over 13,629 device calls (13,568 of them console slot
70) no call names any of the 4,096 destination ranges - 0 hits in the 14 argument words and 0 inside every
argument-pointed structure; the only block pointers near these calls are in the caller's own frame structures. The
linear-layout A/B's apparent "block address handed to a console view call" is withdrawn: those words are saved-register
and caller-stack carry-over, not arguments. The streamed bytes die in CPU deserialization:
[streamed_consumer.md](streamed_consumer.md) sections 3-6.

## 5. What still refuses, and the next increment

**Still refused / unmodelled:** XG slot 10 (`GetResourceTiling`); slot 2 (`GetResourceLayout`) is decoded and the linear
layout is the default now - a 0x1B40-byte record, not the 0x6C8 this page assumed ([texture_layout.md](texture_layout.md)).
Placed resources in existing heaps are answered `E_NOTIMPL` at the documented branch (and the imported heap now
registers, so `CreatePlacedResource` on it reaches the address branch); page import for more than one subresource; and
any import whose pages were never filled.

**Correction, measured later - this page's next increment is superseded in both of its address families.** The streamed
64 KiB blocks are measured never to be named to the device (section 4), so there is no block address to place. The
CPU-built fill destinations are measured as a **different population** from the sampled texture: the XG
texture-computer fill writes only a 32 KB atlas and 1x1 defaults, while the sampled 4K placed texture has committed,
readable pages that are **never written** over 150 s (0 of 129,792 scanned bytes), and the import with its cap lifted to
64 MiB had nothing to copy. So the import scheme still has no correctly-addressed source, and the next named place is
the title-side producer that should write the sampled 4K texture's pages, not the import path:
[texture_source.md](texture_source.md) sections 3-4 and 6. The smaller open items from this page stand:
multi-subresource import and placed resources in existing heaps.

Baseline defended on every bounded boot: ALIVE to timeout, 0 exceptions, 0 device-lost, 0 refused submits, 0 failed
`Close`. Gates: `the local build script` -> `PALMETTO_OK`; `the local test build script` -> `TESTS_BUILT`; existing tests pass.

## 6. Note on repository identity

Mid-task the repository was rewritten for identity/sanitisation: the executable is now `palmetto.exe`, build targets are
`PALMETTO_*`, and every environment variable moved from the old prefix to `PALMETTO_*` (this page uses the new names).
The measured boots in this page ran with the new binary and names; earlier evidence pages (`docs/page_content.md`,
`docs/texture_layout.md`, `docs/wait_state.md`) predate the rename and name the same variables under the old prefix.
