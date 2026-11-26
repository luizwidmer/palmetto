# Vertex buffers the command names in the title's own pages

**Status: measured and fixed as a class of refusals; the boot's stop is not removed by the fix and the earlier causal
claim for it is contradicted by this page's A/B.** `docs/3d_textures.md` section 11.6 left the stage-3 renderer's first
submitted draw lists refused: a draw's vertex-buffer view names a title VA the replay's `buffer_at` lookup has no
arena mapping for, the whole list is dropped, and the boot then stops at `game+0x13360A3` (a read of
`0xFFFFFFFFFFFFFFFF`). This page measures every bound resource of those lists, proves what the failing VA is (committed
host memory the title wrote, with no D3D12 resource behind it), imports those pages at first use so the lists resolve,
and reports what the boot then does: **the refusals go to zero and the content submissions execute, but the boot still
stops, so the two are not the same event.** Counts, VAs, RVAs and outcomes only; no package bytes, paths, shader data
or extracted tables.

## 1. Why this slice

The previous slice's 300 s arm (`boot-stage3fix300`, binary `96593942...A762C5`, same environment as section 11.1)
entered stage 3 at 69.6 s and stopped at `game+0x13360A3` at trace t 73.6 s, 4.0 s later. In between, three
`ExecuteCommandLists` calls returned failure with the replay's one line, `unknown resource; nothing was submitted`:
`list 2 of 4 refused in prepare: result 2, 11 command(s)`, once per submission, and `buffer_at miss for VA
0x15c08800900 size 0x120000` / `... 0x11ff80` named the resource the replay could not resolve. That is the renderer
submitting its first real content draws; the capture stayed black.

The question this slice had to answer is not "which check refused" (section 11.6 answered that) but "what is that VA,
which creation path should have registered it, and what rule makes it resolvable without executing an unrebased title
address". The answer turned out to be a fourth kind of title memory, not the slot-133 placements the earlier pages
described.

## 2. The measured refusal

One measurement boot on the same environment, with the replay's miss diagnostics at their measurement setting
(`PALMETTO_REPLAY_MISS=2`) and the placement trace (`PALMETTO_ARENA_LOG`) on: `boot-vmeas4`, one boot under the boot
lock, stage 3 at 58.0 s, crash at t 61.19 s at `game+0x515acf5` (read of `0x18`). It reproduced the three refusals.

### 2.1 Every command of the refused list

The refused list is list 2 of 4: 11 commands, and the miss is at command 8, after the pipeline, render-target and
depth checks resolved (that order is `prepare_draw`'s: the vertex-buffer lookup is the first check command 8 fails).

| # | command | bound resources it names | mapping at the time |
|---:|---|---|---|
| 0 | barrier | - | - |
| 1 | dispatch 5x5x33 | pipeline `0x18f6dce6bc0` | resolved |
| 2 | barrier | - | - |
| 3 | clear | render target view from the list state | resolved |
| 4 | barrier | - | - |
| 5 | barrier | - | - |
| 6 | barrier | - | - |
| 7 | draw 3 vertices, 1 instance | pipeline `0x18f60c29800`; vb0 VA `0x18f2f13e220` size 4080 stride 24 | resolved: inside the registered slot-133 range `0x18f2e3a0000 + 0xae00000` |
| 8 | draw indexed 30 indices, 1 instance | pipeline `0x18f66186080`; vb0 VA `0x18d58c00300` size `0x120000` stride 16; index VA `0x18d58d20380` size `0x60000` format 57 | **vb0 unresolved**; the index VA is in the same title allocation and was never reached (the vertex check refuses first) |
| 9 | barrier | - | - |
| 10 | draw 3 vertices, 1 instance | pipeline `0x18f56f07820`; vb0 VA `0x18f2f15d1b0` size 4080 stride 24 | resolved, same slot-133 range as command 7 |

The next two refusals carried the same allocation's views: vb0 VA `0x18d58c00380` size `0x11ff80`. The two spans are
the same block at the same end: `0x18d58c00300 + 0x120000` and `0x18d58c00380 + 0x11ff80` both stop at
`0x18d58d20300`, so the second is a view 0x80 into the first, and the index view's VA `0x18d58d20380` starts 0x80 after
the vertex block ends. That is a ring layout - a 1.125 MiB vertex block, 0x80 of padding, a 384 KiB index block - the
shape a streamed content draw names.

The draws' constant data is not in the failing range: each draw's user data `s0` is `0x18f2f13e200` or
`0x18f2f15e1a0`, inside the registered slot-133 buffer and the registered console descriptor heap, which the replay
already rebases.

### 2.2 What the failing VA is

The miss diagnostic's host probe ran at the refusal (`PALMETTO_REPLAY_MISS=2`), so this is a measurement, not an
inference:

```
[rt] replay: miss probe: VA 0x18d58c00300 size 0x120000 is host state commit protect 0x4 allocation base
     0x18d54200000 offset 0x4a00300 region 0x44a00000; of the first 0x120000 bytes 288 page(s) committed/288
     readable, first unreadable at 0x0, 52938 non-zero byte(s) in the first 64 KiB
[rt] replay: miss probe: 3282 title range(s) aliased, none holds the VA: 0; nearest above:
     0x18d5b5cf0f0 + 0x30 (buffer)
```

Read plainly: the VA is committed, writable host memory (`MEM_COMMIT`, `PAGE_READWRITE`) inside a 1.03 GiB committed
title region at base `0x18d54200000`, 0x4a00300 bytes into it. Every one of the 288 pages the 0x120000-byte view
covers is readable, and the first 64 KiB holds 52,938 non-zero bytes: these are real vertex bytes the title wrote. The
same region holds the index buffer at offset 0x4b20380 (1.1 MiB after the vertex block), and further up it also holds
slot-133 placements, which is why the nearest aliased range above the VA is a small placed buffer at
`0x18d5b5cf0f0 + 0x30`.

What it is **not**: no successful slot-133 placement covers that VA. The 3,282 aliased title ranges (one per
successful placed buffer or console heap) do not hold it, and the 36 MiB refusal below is a placement in a *different*
title region. The final arm's placement trace named all 79 of its failed buffer placements - the trace did not reach
its 160-line bound, so nothing was dropped - and every one of them is in that other region
(`0x2302xxxxxxx` / `0x2303xxxxxxx`); none is at the imported VA. The nearest placements in the same allocation are
the 0x30-byte buffers above. The title range the VA sits in therefore has no D3D12 resource and no arena stand-in, so
the earlier title-VA rule - "a slot-133 placement registers its title range when it is created" - never sees it.

### 2.3 The arena at the same moment

The same boot measures the arena pressure the import rule has to work under: a 36 MiB slot-133 buffer (request
`0x2400000`, VA `0x23036b70000`) is refused with `arena buffer: no room for 0x2400000 bytes` and
`GPU address arena full (raise PALMETTO_ARENA_MB)`, and 82 slot-133 creations fail in the final arm: 79 are buffer
placements refused with `E_OUTOFMEMORY` (the traced list runs down to 0x20000-byte blocks) and 3 are the pre-existing
placed-texture refusal with `E_INVALIDARG`; the log has 79 `no room` lines and the default arena is 1024 MiB. The
measured peak-end in the boot that reached the refusal is exactly the arena's 1024 MiB. A stand-in for the failing view
would need 1.125 MiB of arena the full arena may not have, and a per-draw pull of 1.1 MiB is a copy the console would
not make: on the console the GPU reads the pages the CPU wrote.

## 3. The measured rule

The span must become a GPU-visible buffer **at the title VA the command names**, from the title's own pages, with no
arena range: import the host pages with `VK_EXT_external_memory_host` at first use, exactly the mechanism the layer
already uses for the pages of a slot-133 placement when they can be imported (`create_host_buffer`) and for
`OpenExistingHeapFromAddress`. That is measured to work on this machine: the earlier stage-3 boot imported 26
title-placed buffers this way, and every import this slice made succeeded on the first attempt. It needs no arena room,
and it keeps the console's semantics: the CPU's writes at the VA are the GPU's reads, with no copy and no staleness
window.

The rule is bounded and named, in `port/src/vulkan_replay.h` (`title_buffers`):

- a span is offered only when no mapping resolves it: not a registered title range (`console_va_map.h`), not an
  ordinary buffer VA (`buffer_va_index.h`), not an earlier import;
- the VA must be at or above 4 GiB, outside the 32-bit arena window (`port/src/arena_size.h`);
- at most 32 spans per boot, 32 MiB each and 64 MiB in total (the measured span is 1.125 MiB plus its 384 KiB index
  view, covered by the first import's range);
- the device must be able to import every page (committed, not write-combined, not no-cache); a pagefile-backed,
  lazily committed or unreadable page is refused **by name**;
- a title descriptor (a V# base in the console heap's mirror) naming an imported span switches the layer back to
  refusing every draw rather than let a shader add `heap_base` to a title VA.

### 3.1 What was implemented

| file | change |
|---|---|
| `port/src/vulkan_replay.h` | pure `title_buffers::collect_requests` (a draw's vertex and index views and a WriteBufferImmediate address, in command order) and `title_buffers::refusal` (the bounds above); the refused-list dump and host probe behind `PALMETTO_REPLAY_MISS=2` |
| `port/src/vulkan_backend.h` | `ImportedTitleBuffer`, `set_title_page_importer`, `register_title_command_buffers` (called at submission time, before the lists are prepared), `buffer_at`'s first-use fallback, `imported_title_range`, the descriptor-conflict refusal, and the boot report's import counters |
| `port/src/console_va_map.h` | `snapshot()`, so the miss probe can say whether any aliased title range holds the VA and which is nearest |
| `port/shims/d3d12_vulkan_device.cpp` | `import_title_pages`: the `VK_EXT_external_memory_host` import (buffer, memory, bind at the page offset), its lifetime to device teardown, the hook registration, the teardown destroy of the buffers and memory |
| `port/shims/d3d12_xs_device.cpp` | measurement: every failed (or over-4-MiB) slot-133 buffer placement with its title VA under `PALMETTO_ARENA_LOG`; the view contents of console list slots 47/48 (index/vertex buffers) under `PALMETTO_XS_LIST_DUMP=47,48` |

The refusal path is unchanged: a span the device cannot import keeps the replay's named refusal (`title pages a
command names stay refused: VA ... (...)`, then `unresolved resource at draw.vertex_buffer`), so a list is never
submitted with a guess and no command object is left for the title to dereference.

## 4. Cross-check against the reference

The public prior-art project (`ubiquitarianism/leonida`, GPL-3.0, the fork of this tree named in
[upstream_port.md](upstream_port.md)) carries an independently developed answer to the same class of memory. Checked at
commit `ed94fbef9888` (PR #1723 merge by Moriz82):

- `runtime/shims/d3d12_vulkan_device.cpp` has the same finding in its own words: "title memory named by a shader
  address", windows charged to `LEONIDA_TITLE_WINDOWS_MB`, a mirrored window refreshed before every submission, and the
  same failure mode this page measures: `GPU address arena full (raise LEONIDA_ARENA_MB): title memory has no window`.
  `runtime/src/title_window_plan.h` is their bounded-window budget (one page-aligned window per request, capped and
  charged to a budget; their default is 64 MiB, the same number this slice caps its imported bytes at).
- Agreement: title memory a command names must be made GPU-addressable, the mapping has to be bounded and named when it
  cannot be made, and the arena can be full while this work runs.
- Difference, resolved by our measurement: the reference mirrors the pages into arena or out-of-arena windows and
  refreshes them per submission; our measurement shows the stage-3 spans are the title's committed, readable pages and
  are named only by command arguments (vertex/index views and immediate writes), so this slice imports them in place
  onto those arguments, with no copy and no per-submission refresh, and refuses by name when the host pages are not
  importable. The reference's shadow-copy mechanism would also serve a *V#* naming title pages; for us that case sets
  the conflict flag instead, because executing it needs a rebase this slice does not do.

No code from that project was used.

## 5. Boots and the numbers

Same environment as `docs/3d_textures.md` section 11.1 (deferred compile 4,096 bound / four workers, `PALMETTO_SHADERS`,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, state trace, arena default 1024 MiB)
plus the stage-3 pump probe and captures, one boot at a time under `_setup/boot_lock.ps1`, logs and captures outside the
repository. The after arm is `boot-vfix5`, binary SHA-256 `A33CA9DF...79C9696E`; the measurement arm is `boot-vmeas4`,
binary `311D044E...5904B37`; the before numbers are section 11.5's `boot-stage3fix300`, binary
`96593942...A762C5`.

| measurement | before, `boot-stage3fix300` (section 11.5) | measurement, `boot-vmeas4` | after, `boot-vfix5` |
|---|---:|---:|---:|
| verdict | STOP `game+0x13360A3`, t 73.6 s | STOP `game+0x515acf5`, t 61.2 s | **STOP `game+0x13360A3`, t 59.1 s** |
| stage 3 (machine word 3/3) | 69.6 s | 58.0 s | 56.0 s |
| `ExecuteCommandLists` refused as unknown resource | **3** | **3** | **0** |
| title spans imported at first use | 0 | 0 | **2** (1.125 MiB vertex view; 384 KiB index view; the 0x11ff80 sub-view falls inside the first) |
| draw-carrying submissions accepted | 0 of the 3 | 0 of the 3 | the 3, and every later one (`ok` 4,459) |
| captures | 1 frame, mean rgb 0.00 | 1 frame, mean rgb 0.00 | 12 frames (1..2201, every 200), **all mean rgb 0.00** |
| presents | 3,411 | 3,351 | 2,229 |
| slot-133 creations failed | 83 (80 `no room` + 3 named texture refusals) | 84 (81 `no room` + 3 texture) | 82 (79 `no room` + 3 texture) |
| device lost / submit failures | 0 | 0 | 0 |

The after arm's two imports name the views exactly:

```
[rt] replay: title pages imported at first use: VA 0x22309800300 + 0x120000 (a command names the title's own memory;
     the buffer is those pages, no arena stand-in)
[rt] replay: title pages imported at first use: VA 0x22309920380 + 0x60000
```

A repeat arm without the pump (`boot-vfix1`, the same source before a bookkeeping-only refactor of the import list)
measured the same result: stage 3 at 62.0 s, 0 refusals, 2 imports, STOP `game+0x13360A3` at t 65.5 s. A third arm
(`boot-vfix3`) measured 0 refusals and the same stop at t 62.6 s.

### 5.1 What the stop does, and the correction to section 11.6

The refusal and the stop are **not** one event. With the three lists accepted and executed, the boot stops at the
**same** `game+0x13360A3`, on the same `0xFFFFFFFFFFFFFFFF` read, 3.1 s after stage 3 entry: the same title-side
fault family section 11.6 described, unchanged by removing the refusals. The title stack is the same renderer path
(`0x182AB2 .. 0x186424`). So section 11.6's sentence "the title faults on its own failed submission" is not supported:
the failed submission was removed as a variable and the fault remained, at the same site in three arms.

The arena's role was measured separately (`boot-vfix4`): with `PALMETTO_ARENA_MB=2047` (the device's cap), the same
source has **no** `no room` lines and no failed buffer placements (only 3 placed-texture refusals with
`E_INVALIDARG`, the pre-existing format-118 refusal), and the stop moves to `game+0x515acf5` (read of `0x18`) at
t 53.3 s, i.e. to the site the measurement arm also reached. So the stop is a timing-dependent title-side crash family
after stage 3, not the arena alone and not the refusals; naming it is a later slice's job, and the falsifier is named
in section 7.

## 6. The picture

All twelve captured presents in the after arm read mean rgb 0.00: black. This does not say the content pass drew
black - the imports hold the title's real bytes, unlike the zeroed arena stand-ins - because the captures stop at frame
2201, and the three content submissions happen in the last moments before the t 59.1 s stop (the title presents again
after them in no arm). A capture every 50 frames or an attachment readback armed at the first imported-view draw is the
measurement that would answer it; this slice does not claim a picture.

## 7. Limits and the next falsifier

- **The stop is not fixed.** Removing the refusals does not move `game+0x13360A3`; the arena arm moves it. The next
  falsifier is a crash-site probe on the renderer's lookups (a `PALMETTO_POKE_LOG` arm at the two read sites) or a
  thread-stack sample one second after stage 3, which would turn "a wild -1 read in the renderer" into a named lookup.
- **The import is for command arguments.** A V# naming an imported span is detected and refuses every draw instead of
  faulting; that case was not measured, and the class of shader-visible copies of a non-slot-133 title range remains
  unsupported. The reference's window mechanism is the shape that case would need.
- **The budget is small on purpose.** 32 spans, 32 MiB each, 64 MiB total. The measured content stream advances its
  ring by a few MiB per block; a boot that streams many distinct blocks would spend the budget, log the named refusal
  and keep the lists refused. A later slice that measures the ring's reuse (the pages are imported once and the ring
  comes round to them) would decide whether the cap can stay.
- **One boot per arm, and the arms' timings vary**: stage 3 at 52.0-69.6 s and stop sites differing across arms
  (`game+0x13360A3`, `game+0x515acf5`, `game+0x4e59880`, `game+0x247a05f`, `game+0x86f97e`, `game+0x4d9f636`). Every
  counter above is one boot; the refusals-to-zero and the imports are the one-variable A/B this slice claims.
- **The import needs `VK_EXT_external_memory_host` and importable pages.** Without the extension the spans stay refused
  by name, as before; no fallback stand-in was added, because the arena is measurably full.

## 8. Tests and gates

- `port/test/frame_dump_test.cpp` gains a group for the pure rule: the scan collects the measured stage-3 vertex,
  sub-view, index and WriteBufferImmediate VAs in command order; a list with no buffer VA asks for nothing; the
  measured span is accepted; a VA inside the 32-bit arena window, an over-bound span, a spent range budget and a spent
  byte budget are each refused; and the scan does not append past its bound.
- Gate subset run on the changed tree: `frame_dump_test`, `unimplemented_policy_test` (98 checks),
  `graphics_record_test` (180), `graphics_pso_test` (312), `xbox_present_test`, `buffer_va_index_test`, `dstorage_test`
  (224 checks), `xapu_chad_test` (87), `xs_proxy_mock_test`, `arena_size_test` and `deferred_compile_test` (18) all
  exit 0; `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes.

## 9. Attribution

This slice was implemented from this project's own measurements: the refused list's commands, the host probe, the
placement trace and the A/B boots above. The behaviour was cross-checked against the public GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), files `runtime/shims/d3d12_vulkan_device.cpp`
(title-memory windows) and `runtime/src/title_window_plan.h`, commit `ed94fbef9888` (PR #1723, Moriz82). No code from
that project was copied; the upstream project remains the copyright of its contributors and its license continues to
apply to its own tree. No game data is added: RVAs, VAs, counts, shapes and outcomes only.

## 10. The content frame is black on the presented resource, and the stop is a title renderer object reused

**Status: measured; the picture question is answered and the renderer stop is attributed as title-side.** Section 6
left the content pixels unmeasured: the twelve presenter captures were black because no captured present lands after
the content submits. This section arms the existing draw readback on the submissions that imported title pages at first
use (the content submits), copies the draw targets and writes them out as pixels, and also measures the stop with the
in-process registers the trace already records. The answer is plain: the title's first content frames are black on the
compose targets, and those targets are the resources the presenter then presents; the stop is a title renderer/command
object whose memory is overwritten or released while the renderer still uses it, with no layer frame in the chain. The
arena default stays 1024 MiB: raising it to 2047 removes the buffer-placement refusals but does not advance the boot or
change the picture. Counts, RVAs, VAs and outcomes only; no package bytes, paths or extracted tables.

### 10.1 The capture schedule this slice adds

| file | change |
|---|---|
| `port/src/vulkan_replay.h` | `ReplayFunctions::title_import_capture`: true while the submission being prepared is one whose lists named title memory at first use; the pure `frame_dump::draw_readback_arms(listed, title_import)` rule that makes that submission's every draw arm the colour-target readback |
| `port/src/vulkan_backend.h` | `VulkanContext::title_import_capture_tick(imported)`: the submission whose registration imported title pages plus the next two are armed; the PPM writer that dumps each copied window (`PALMETTO_READBACK_PPM=<dir>`, at most `PALMETTO_READBACK_PPM_LIMIT` files, default 4) |
| `port/test/frame_dump_test.cpp` | four checks pinning the arming truth table: a listed draw arms, a title-import submission arms every draw it holds, both together arm, neither stays off |

The trigger is a measurement, not a guess: the first import happens in the content submission itself, and the
`title pages imported at first use` lines are the same ones section 5 measures. The arm is bounded to three
submissions because the content came in two in every boot measured here. `PALMETTO_READBACK_FULL=1` copies the whole
target; without it the existing 128x128-window readback runs with its write control (the same target cleared by the
layer to `51 102 204` and copied into a second window), which is how the copy path is proven. The new dump only writes
the bytes the existing readback has already copied; no recorded command changes.

### 10.2 The captured content pixels

Every content draw the trigger armed is a 3-draw submission: two draws into a 4K intermediate and one compose draw
into a 4K display buffer, all `format 37`, viewport `3840x2160`. The compose draw's target resource id is the id the
present trace prints for the presented plane, which is the identity that matters for the picture:

| arm | presented plane ids (present calls 1, 2, 1000) | content compose-draw target | content pixels |
|---|---|---|---|
| `contentr1`, full target, 1024 MiB | `0x1BC9958C550`, `0x1BD28020490`, `0x1BD28020490` | draw 3676 -> `0x1BD28020490`; draw 3679 -> `0x1BC9958C550` | all-zero: 8,294,400/8,294,400 texels of the full 3840x2160 target |
| `contentv1`, window, 1024 MiB | `0x1A9033599C0`, `0x1A903359C30`, `0x1A903359C30` | draw 4547 -> `0x1A9033599C0`; draw 4550 -> `0x1A903359C30` | 0 0 0 at the centre window (1856,1016, 128x128), non-black 0/16,384 |
| `contentb1`, window, 2047 MiB | `0x21033B98CB0`, `0x2103391D230`, `0x2103391D230` | draw 3644 -> `0x2103391D230`; draw 3647 -> `0x21033B98CB0` | 0 0 0 at the centre window, non-black 0/16,384 |

The full-target arm writes the copied 4K targets out as PPM files: four files of 24,883,217 bytes each, every one
all-zero (mean 0 0 0, maximum 0, non-zero byte 0). The two 3-draw submissions hold six draws in both arms; the
intermediate-target draws read back zero as well, so the frame's own passes leave the intermediate black too.

The copy path is proven on the same 4K `format 37` targets in the window arms: every capture's write control reads
`mean 51 102 204, non-black 16,384/16,384, peak 204`, so an all-zero content window is the target's content and not a
failed copy, mapping or format. One diagnostic artefact is named so a later reader reads the numbers in order: within
one submission, a second capture of the same target copies after the first capture's write-control clear, so its
"content" window is `51 102 204`; the first capture of a target after the title's last write is the measurement. The
`contentv1` first captures are draws 4545, 4547 and 4550 (intermediate and both display buffers), all zero. A
known-good raster control was also drawn in that boot (`vs_tri` + `fs_solid`); armed by draw index it landed on a
repeating title-page frame, so it proves the replay's raster/readback path for a 4K format-37 target but not inside the
content submission; the falsifier for a same-frame control is in section 10.6.

Plain answer: the title's first content frames draw black. The display resources the presenter names read 0 over the
whole 3840x2160 target immediately before the presents that follow them, so the black is not a capture-scheduling
artefact and not a readback artefact. The presenter's own 12 captures stay black because they stop before the
post-content presents; the draw readback covers exactly those frames.

### 10.3 The stop, with registers and the decompiled chain

`contentv1` (measured binary `80D9EEB7`) stops at `game+0x13360A3`, `read` of `0xFFFFFFFFFFFFFFFF`, t 60.27 s, 2.3 s
after the machine word first reads 3 (58.0 s); `contentr1` stops at the same site (t 92.08 s after a 54.7 s single
deferred build); `contentb1` (2047 MiB) stops at `game+0x515B22D` reading `0x3F800000`, t 53.41 s, 1.4 s after stage 3.

The fault instruction at `0x13360A3` is `CALL qword ptr [RAX+0xB0]` with `RAX = 0x3F8000003F800000`. That operand
address is non-canonical, and Windows reports a non-canonical access with address `0xFFFFFFFFFFFFFFFF`; the stop is
therefore not a read at -1. `RAX` is the object's first qword, so the object's vtable pointer holds two `1.0f` floats.
The object is `RSI = RCX = 0x1B60C1394E0` in `contentv1`, and the trace's 0x80-byte fault dump at it is
`float4(1,1,1,0)` repeated eight times - a constant/matrix block, not a renderer object. `RBX = 0x1B60B47F0A8` was
computed earlier in the same call as `[object+0x28]+0x18` (at `0x1335B1F`), so that field was a valid pointer when
read and was replaced before the call; the object's memory is reused while its executor runs.

The 2047 MiB arm is the same family at another point: `0x515B22D` reads at address `0x3F800000` - again a `1.0f`
constant used as a pointer - and its stack is `0x515B22D <- 0x538D651 <- 0x538D36C <- 0x538D96C <- 0x183CEA <-
0x1837F3 <- 0x182EB4`, i.e. the same outer renderer graph. The earlier 2047 arm's `0x515ACF5` stop (section 5) is the
same shape in the title's own allocator: the decompiled free path at `0x515ABB0` reaches `LAB_14515acf5`
(`plVar10 = *(uVar4+0x18) & ~3`) with a null tree node, so the block being released is not in the allocator's tree.

Decompiled with this project's Ghidra tooling, the chain is the title's render-command/job path. `game+0x1335A20` is
the per-command executor: it makes virtual calls on one object at vtable slots `0x28`, `0xA8`, `0x30`, `0x98`, `0xB0`,
`0xB8` and `0xC8`, and releases a second object (slot `0x10`) on its way out. `game+0x186170` is the job executor
around it: it checks a state dword at `job+0x40` (the code compares it with 3), runs the per-command executor, and
stores 2 in that dword when it returns. Its caller `game+0x1856D0` is the submit/wait: it waits while `job+0x40` is 0,
and when the dword reads 1 it clears `job+0x30` and releases the job (`func_0x14503e5c0`). Above that, `0x184EB0`
flushes, `0x184770` creates or finds a job, `0x1836D0` dispatches on a command opcode (1..6), `0x182D90`, `0x182B60`
and `0x1829C0` build the command, and `0x183A20`/`0x184250` walk the command graph's repeated list at `+0x90`. The
repeating `0x183AAF`/`0x18435F` frames in the stop stack are that walk.

Verdict: this is a title-side lifetime race in the renderer's job/command graph across the stage-3 transition, not a
runtime answer. No frame of the 12-16 frame chain is in the layer; the layer's submissions in the window were all
accepted (0 `ExecuteCommandLists` refusals, 0 unknown-resource refusals); the draws whose targets are read back were
resolved, executed and copied. The object that faults is title heap memory filled by the title's own constant data. The
honest statement of the remaining gap: the writer (the thread and the store that frees/reuses the object) is not named
yet, and the two stop sites are two windows on one race rather than a proven single store.

The scope question "is it ours" is answered by the isolation the slice already has: with the three refusals present
(section 5's before arm) the same site stopped; with the imports and zero refusals it stops at the same site
(`contentv1`, `contentr1`); and the arena arm moves it. Nothing this layer answers appears in the fault path.

### 10.4 The arena A/B

Both arms are the same binary, SHA-256 `80D9EEB7...5A237F3A`, the same environment and capture schedule, one boot
under the boot lock, `PALMETTO_ARENA_MB` unset (1024 MiB) against 2047 MiB (the device cap). The full-target arm of
section 10.2 is the pre-refactor build `1446B707...5733764C`; the only difference between the three is the pure
`draw_readback_arms` helper and its test (which record no command) and, after the measurements, comment text. The
tree's gate build is `E6131819...08213B17`; its PE timestamp differs from the measured arms and its code is the same:

| measurement | 1024 MiB, `contentv1` | 2047 MiB, `contentb1` |
|---|---:|---:|
| stage 3 (machine word current 3) | 58.0 s | 52.0 s |
| verdict | STOP `game+0x13360A3` read `0xFFFFFFFFFFFFFFFF`, t 60.27 s | STOP `game+0x515B22D` read `0x3F800000`, t 53.41 s |
| `ExecuteCommandLists` ok / refused | 4,555 / 0 | 3,653 / 0 |
| slot-133 creations ok / failed | 4,517 / 84 | 4,598 / 3 |
| buffer placements refused `E_OUTOFMEMORY` | 81 | **0** |
| placed-texture refusals `E_INVALIDARG` | 3 | 3 |
| title spans imported / draws armed | 2 / 6 | 2 / 6 |
| content-frame readback | black (window) | black (window) |
| presents / captures | 2,277 / 12 black | 1,826 / 10 black |
| wall | 84.7 s | 74.8 s |

So raising the arena removes 81 buffer-placement refusals and changes the stop site and its timing, but it brings no
content pixel and no frame the 1024 MiB arm does not reach: both arms capture the content frame, both are black, and
both stop in the same title-side family. The refusals are placements the renderer never needs for the frame that
reaches the screen, while the arena at 1024 MiB is measured exactly full in the same window (section 2.3). The
decision is to keep `kArenaDefaultMiB = 1024`: a larger default doubles the address space, moves the stop, and the A/B
shows no measured benefit. `arena_size_test` is unchanged.

### 10.5 The other stop family measured while reaching for the picture

Three of the first four content-capture boots stopped before stage 3, and they are a different family, recorded here so
the numbers are not confused with the renderer stop: `content1` stopped at `game+0x28CD3CC` (read `0xD60800F680`,
t 52.36 s, four cascading exceptions on title task threads), `content1b` at `game+0x86F981` (read `0x9B25`, t 58.81 s,
three exceptions) and `contentr3` at `0x0` (execute, four exceptions); the first two show `loading 2`, `session 1` and
machine word 2/2. All three had a `[rt] replay: the Vulkan device was lost` line followed by `ExecuteCommandLists`
refusals as `unsupported`; the device-lost line also appears in earlier arms (`boot-vfix2`, `boot-vmeas3`,
`boot-vmeas5`). That is a separate, pre-existing runtime-answer candidate (a GPU command that faults the device) and
is not this slice's stop.

### 10.6 Limits and the next falsifier

- **One boot per configuration.** Stage 3 ranged 52.0-58.0 s and the stop moved between `game+0x13360A3`,
  `game+0x515B22D` and the section-5 sites; the content draw indices also vary (`3674`-`3679`, `4545`-`4550`,
  `3642`-`3647`), which is why a control armed by a fixed draw index did not land in a content submission.
- **The writer is unnamed.** The next falsifier is a hardware write watchpoint on the faulting object's first qword,
  armed from the object address a `PALMETTO_POKE_LOG` line at `game+0x1335A20` entry prints, plus a poke line on the
  `job+0x40` state stores in `0x1856D0`/`0x186170`; that would name the thread and the store that reuses the object.
- **Same-frame raster control.** Arming the known-good control triangle (or the PS-only control) on the same
  title-import trigger used for the readback would put the control's 32 160 240 output into a content submission's own
  target, closing the last gap between "the copy path works" and "the content submission's raster path works".
- **The full-target dump is one boot's pixels.** It is the frame's bytes, not a movie of frames; the title presents
  after the content frames, but the presenter capture's fixed 200-present schedule still misses those presents.
- **The content is the loading-era frame.** The measured frame is the first stage-3 content the renderer submits; a
  later stage of the boot (stage 4) is not reached, so nothing here says the title's picture stays black later.

### 10.7 Tests and gates

- `port/test/frame_dump_test.cpp` gains four checks for `frame_dump::draw_readback_arms`: a listed draw arms, a
  title-import submission arms every draw it holds, both together arm, and an unlisted draw of an ordinary submission
  stays unread. The target exits 0.
- Gate subset run on the changed tree: `frame_dump_test`, `unimplemented_policy_test` (98 checks),
  `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test`,
  `deferred_compile_test` (18 checks), `arena_size_test`, `xbox_present_test` and `xs_proxy_mock_test` all exit 0;
  `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check is the one that requires `PALMETTO_READBACK_PPM`
  and `PALMETTO_READBACK_PPM_LIMIT` to be named in a page, which this section does).

### 10.8 Attribution

This section was implemented from this project's own measurements: the capture trigger's imports, the readback
windows, the presenter plane ids, the in-process fault registers and memory, the 300 s arms and the arena A/B. The
stop sites and the job/command chain were decompiled with the project's Ghidra tooling from the user's own image; no
peer code was used and no game data is added. RVAs, VAs, field offsets, counts, shapes and outcomes only.

## 11. The executor's object is a reused block; the two in-process watches this slice built did not name the writer

**Status: measured negative, with two new in-process watch methods proven and one method failure named.** Section 10
attributed the stop at `game+0x13360A3` to a title object whose memory is reused while the per-command executor still
virtual-calls through it, and named one falsifier: a hardware write watchpoint on the object's first qword, armed from
a `PALMETTO_POKE_LOG` line at the executor entry. This section builds that measurement (and a second, suspension-free
one), runs it in the content arm of section 10, and reports what it measured: the control watches work, the executor
chain is resolved, and the loading-phase object the probe tracks is **not** the block the stop calls through - so the
writer is still unnamed. No layer call was changed: the boot's stop and the black content frame are unchanged.

### 11.1 The two watches, and the layer timeline

| file | change |
|---|---|
| `port/src/main.cpp` | `PALMETTO_EXECUTOR_WATCH`: hardware (DR0..DR3) probe. DR0 execution breakpoint on the executor entry `game+0x1335A20`, DR1 on the title's allocator helper `game+0x515ABB0` (optional, delayed by `@seconds`), DR2 a write-only 8-byte data watch on the tracked object's first qword, DR3 a write-only data watch on the probe's own control slot (written every 50 ms). A sweep thread arms the four registers in every thread. From the hit at the entry it reads job = RCX, graph = *(job+0x28), object = *(graph+0x10), job state = *(job+0x40), logs the title stack and the object/graph memory, and notices a first-qword change between two executor passes |
| `port/src/main.cpp` | `PALMETTO_OBJECT_WRITE_WATCH`: suspension-free probe. A self-managed int3 over the executor entry (re-written on the single-step after each pass) reads the same chain, and on the first sight of a plausible object makes the 4 KiB page holding its first 0x100 bytes `PAGE_READONLY`; every thread's store into it traps, is logged with the writer's thread, instruction, title stack and the value after the store, and the page is re-protected. A probe-owned page is protected the same way and written every 50 ms as the control |
| `port/src/vulkan_replay.h` | `palmetto::timeline` (`PALMETTO_TIMELINE`): one line at each ExecuteCommandLists entry/return, queue Signal, CPU fence signal, fence Wait and deferred-compile submit wait, with the steady_clock tick and microseconds. `main.cpp` prints the clock origin at the run's start |
| `port/shims/d3d12_vulkan_device.cpp` | the deferred-compile submit wait (`prepare_deferred_compute_pipelines`) emits its timeline line with the wait's microseconds |

All three switches are off by default. The probes read title memory and log RVAs, field offsets, counts and outcomes
only: no game data.

### 11.2 The hardware watch: the executor chain is resolved, and the control proves the method

`boot-probe4`..`boot-probe8` used the hardware probe in the section-10 environment (deferred compile arm, first-use
title-page import readback). The control data watch fired on every 50 ms write from the first second on (196 events in
the first 10 s, 1,791 by 90 s), so the debug registers and the write watch are honoured on this machine; a silent
object watch is therefore a measurement, not a method failure.

The execution breakpoint on `game+0x1335A20` fired from about +18 s in every one of those boots and measured the
chain section 10 decompiled, with stable relative layout in three different boots (ASLR moves the heap base only):

| measurement | `probe4` | `probe5` | `probe8` |
|---|---:|---:|---:|
| executor entries by 90 s | 60,089 | 30,000+ (30 s sample: 8,713) | 51,097 |
| caller of the entry (top of stack) | `game+0x186424` | `game+0x186424` | `game+0x186424` |
| job - graph (heap offset) | `0x1FA10` | `0x1FA10` | `0x1FA10` |
| object - graph (heap offset) | `0xEB0` | `0xEB0` | `0xEB0` |
| object's first qword | `image+0x658DB60` | `image+0x658DB60` | `image+0x658DB60` |
| job state `*(job+0x40)` at first sight | 0 | 0 | 0 |
| different objects named by `*(graph+0x10)` in the whole boot | 1 | 1 | 1 |
| object first-qword writer hits | **0** | **0** | **0** |

So in the loading phase the executor runs on one job/graph/object triple, the object's first qword is a constant
vtable `image+0x658DB60`, and **no thread ever writes it**: the tracked object is not the block that is corrupted in
the section-10 stop. The watch was armed for 90 s over 51,097-60,089 executor entries and never saw a store; a
`PALMETTO_OBJECT_WRITE_WATCH` boot that protected the object page saw no fault in that page either.

The runner-up measurement is a correction to section 10.3: the function at `game+0x515ABB0` is **not** a per-block
free entry. Armed from boot it took 46,329,648 traps in a 90 s boot (about half a million per second), and after the
delayed arm (`@55`) 13,958,734 traps in 35 s, most of them the same pointer repeating with first qword `0x7`: it is a
hot allocator helper (lock/owner lookup) that the allocator calls on ordinary traffic, so a stop inside it does not by
itself name a release, and it must not be read as "the free of the faulting block".

### 11.3 The suspension-free watch: the page method works, the int3 over the entry does not trap

The hardware sweep suspends and re-configures every thread, and every boot in which it ran **did not get past
loading**: the loading word stayed at 1 from 16-18 s and the boot then stopped on a title task thread
(`game+0x2215C35` or `game+0x258B000`, a write of 0), four boots in a row (`probe4`, `probe5`, `probe6`, `probe8`). The
same environment without the sweep reaches loading 2 and stage 3 (section 11.5). The probe is therefore not safe for
this boot, and section 11.2's object measurement is a loading-phase measurement.

`PALMETTO_OBJECT_WRITE_WATCH` was built to avoid the suspension. Its control page watch fired on every 50 ms write
(3,379 faults by 90 s), so the access-violation -> single-step -> re-protect path is sound. The int3 on the executor
entry, however, **never trapped** in five boots (`smoke5`, `smoke7`, `probe7`, `probe8`; 0 entries, the site byte still
`0xcc` at every 10 s check). In the same `probe8` boot the hardware execution breakpoint at the identical address
fired 51,097 times, so the title does execute that address and the int3 route failed for a reason this slice did not
establish. A control int3 at `game+0x2215EA0` (the title thread wrapper) trapped normally in the same build (20
entries in 10 s), so the handler, its re-arm and the vectored-handler order are sound; the failure is specific to that
entry page. The int3 route is recorded as a failed method here, not as a measurement.

### 11.4 The layer's completion order, measured

`PALMETTO_TIMELINE=1` gives the layer's own returns the same tick as the watches. The last ordering arm
(`boot-timeline2`, 130 s, same content environment) measured 4,412 `ExecuteCommandLists`, each with an entry and a
return line (4,412 of each, **0 held**), each followed by a queue `Signal` entry and return, **0 fence waits** and 2
deferred-compile submit waits, with the tick origin printed at the run's start. The return microseconds are
submission-scale (examples: 2 us and 8,331 us on the last content submissions; 21.2 s for one submission in the
readback arm `probe2`, where a 4K readback copy is done inside the call). So in these arms the layer returned from
`ExecuteCommandLists` after `vkQueueSubmit` (no deferred replay), submitted each fence Signal immediately after the
batch it follows, and never had a fence Wait or a held batch in flight; that ordering is what an early-completion bug
would have to break, and it is not broken here.

### 11.5 The 300 s default-path boot (A/B negative)

One boot with the probes off, the section-10 content environment (`PALMETTO_READBACK_CENTRE=1`, presenter captures,
stage-3 pump, deferred compile), 300 s timeout, under the boot lock. Build `17369510...E243CC`:

| measurement | `boot-final300` |
|---|---|
| verdict | STOP `game+0x13360A3`, read `0xFFFFFFFFFFFFFFFF`, trace t 59.04 s, wall 80.7 s |
| stage 3 / loading / session | 56.0 s (machine current 3 next 3) / loading 2 at 54.0 s / session 3 at 56.0 s |
| `ExecuteCommandLists` ok / refused | 3,755 / **0** |
| `PresentX` ok / captures | 1,877 / 10 (every capture mean rgb 0.00 0.00 0.00) |
| content-frame readback | black: mean 0 0 0, non-black 0/16,384 at the centre window; write control 51 102 204, 16,384/16,384, peak 204 |
| device-lost lines | **0** |
| D3D12 calls | 144,073 |

The stop is the section-10 site with the same registers: `RCX = RSI = 0x1e23df3f440` (the dump is
`float4(1,1,1,0)` repeated), `RAX = 0x3F8000003F800000`, `RBX = graph+0x18` with `graph = 0x1e23d27f090`. The picture is
the same black content frame. Nothing moved, which is the honest A/B for a slice that changes no layer call.

### 11.6 Verdict, the corrected reading, and the next wall

- **The writer is not named.** Both watch methods were proven on their controls, and both fail to observe the write
  for two different measured reasons: the object the loading-phase executor calls through is never written, and the
  suspension-free int3 route does not trap on the entry page. This is a negative, not a fix.
- **Ours vs title-side is unchanged from section 10**: no runtime producer was measured. The layer's returns are
  post-submit, its fence signals are submitted after their batch, and it had no wait or held batch in the measured
  arms; the fault is inside the title's own job/command-graph object with no layer frame. What is new is the measured
  asymmetry that blocks the watch: at the stop the object the executor calls through is `0x1e23df3f440`, about 13 MB
  from its graph, while in every loading-phase pass `*(graph+0x10)` was `graph+0xEB0` - the field changed between the
  loading-phase runs and the stage-3 stop, so an object watch armed from a loading-phase entry cannot see the
  corrupted block.
- **Correction to section 10.3:** `game+0x515ABB0` is a hot allocator helper, not a per-block free entry (see 11.2);
  the 2047 arm's stop inside it does not name a release.
- **Next falsifier** (one boot): a hook on the executor entry that does not suspend threads **and** does trap. The
  int3 route must be made to trap at that page first - candidate: an int3 at a mid-function site (`game+0x1335B1F`,
  where the executor reads `[graph+0x28]`) instead of the entry byte, or a page-guard/execute-only protection of the
  entry page - and only then arm the object's page from the stage-3 entry and log the writer. If the mid-function int3
  also fails to trap, the failure is a property of the page/alias and must be named before any watch can be trusted
  there.

### 11.7 Tests and gates

- `port/src/main.cpp` probes are diagnostics behind their switches; the default path is unchanged (`git diff` of the
  layer files is the timeline lines only, gated by `PALMETTO_TIMELINE`). No new pure helper was added, so no test
  target changed; the test subset of section 10 stays green in the gate run below.
- Gates on the changed tree: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints
  `TESTS_BUILT`; the section-10 test subset exits 0; every `tests/test_docs_*.py` check passes.

### 11.8 Attribution

This section was implemented from this project's own measurements: the two in-process watches, the layer timeline,
the probe boots named above and the 300 s default-path boot. The stop sites and the executor/job chain were
decompiled with the project's Ghidra tooling from the user's own image; no peer code was used and no game data is
added. RVAs, VAs, field offsets, counts, shapes and outcomes only.

## 12. The int3 route works once the hardware point is gone; the writer is not the executor thread; pacing moves the stop and the frame stays black

**Status: measured, with one method failure named, one object corrected and one negative result on the writer.** Section 11
left three open items: its int3 over the executor entry never trapped and the failure was called a page/alias property; the
object it tracked was never written; and the writer of the corrupted vtable was unnamed. This section measures the first
two: the int3 was masked by the section-11 hardware execution breakpoint at the same linear address, and the object the
executor virtual-calls through is its own argument (`RCX`), not the graph's `+0x10`. With the hardware point removed the
int3 traps and names the stage-3 job, including the job the stop later calls through. Four hardware data points in the
executor thread and a 104-thread sweep armed on that job's first qword then never fire, so no armed thread is measured
making the store; the arm carries no live control in the same boot, which is the limit this negative is stated under. Two
opt-in pacing knobs move the stop site (one moves it into a different, pre-existing family) and change nothing about the
black content frame. Counts, RVAs, VAs and register values only; no game data.

### 12.1 The int3 method failure, named

Section 11 planted an int3 at `game+0x1335A20` while `PALMETTO_EXECUTOR_WATCH` armed a hardware execution breakpoint
(DR0) at the same linear address; it measured 0 int3 entries and 51,097 hardware hits in one boot, while a control int3 at
`game+0x2215EA0` trapped normally. The arms here isolate the cause: the same int3 traps as soon as no debug register is
armed at its address. A hardware instruction breakpoint is a debug fault, and the resume path sets `EFLAGS.RF`, which makes
the processor ignore the debug exception of the next instruction - at the entry that instruction is the INT3. The two
routes cannot share an address, and the failure was not a page or alias property at all.
`port/src/main.cpp`'s `PALMETTO_WRITER_WATCH` is that arm: an int3 over the executor entry, re-written on the single-step
after each pass, with no debug register at the site.

| arm | debug register at the int3 site | int3 entries | hardware entry hits |
|---|---|---|---|
| `boot-probe8` (section 11.2) | DR0 at `game+0x1335A20` | 0 | 51,097 |
| `boot-w1` | none | 2 by 19 s (the boot stopped before stage 3) | - |
| `boot-w2` | none | 3, the third at the stage-3 entry | - |
| `boot-w3` | none | 3, the third at the stage-3 entry | - |

### 12.2 The object is the executor's own argument

Section 11 tracked `object = *(*(job+0x28)+0x10)` (the graph's `+0x10`) and never saw a store. The executor's own
disassembly shows that field is not what it calls through:

| site | instruction | what it means |
|---|---|---|
| `game+0x1335A4E` | `MOV RAX,qword ptr [RCX] ; CALL qword ptr [RAX+0x28]` | the entry calls the argument's own vtable slot 0x28 |
| `game+0x1335A51` | `MOV RSI,RCX` | the argument is kept in RSI for the whole function |
| `game+0x1335B1F` | `MOV RBX,qword ptr [RSI+0x28]` | the graph is the argument's `+0x28` field (section 11's "job+0x28") |
| `game+0x133609A` | `MOV RAX,qword ptr [RSI] ; CALL qword ptr [RAX+0xB0]` | the stop's call reads the argument's vtable |

The stop confirms it. In `boot-w3` the first trace exception at `game+0x13360A3` has `RCX = RSI = 0x2d40f338520`, exactly
the job the int3 logged at the stage-3 entry a few seconds earlier, `RBX = graph+0x18 = 0x2d40e67f0a8` and
`RAX = 0x3F8000003F800000`; the fault dump at the job is `float4(1,1,1,0)` repeated, the same 1.0f constant block section
10 measured. `boot-p1` stops at another call in the same executor (`game+0x1335DCB`) through a different argument
(`RCX = 0x13c0453e4e0`) whose first qword is the same constant block: one race, two windows.

### 12.3 The writer arms: no armed thread made the store

| arm | what was armed | int3 entries | stage-3 entry | data hits | first trace exception |
|---|---|---|---|---|---|
| `boot-w2` | 4 points in the executor thread: job first qword, `+0x28`, `+0x30`, `+0x40` | 3 | yes, machine stage 3 | 0 | `game+0x13360A3` read -1, t 57.12 s |
| `boot-w3` | the job first qword in 104 threads (suspend, Get/SetThreadContext, resume) plus the executor thread's 4 | 3 | yes, machine stage 3 | 0 | `game+0x13360A3` read -1, t 64.48 s |
| `boot-w4` | the same with the arming guard shortened | 2 (loading only) | none: the boot stopped at `game+0x86F981` t 61.88 s | 0 | `game+0x86F981` read `0xa5d5` |

In `boot-w2` and `boot-w3` the job's vtable was valid at the stage-3 entry and 1.0f constants at the stop, so the store
happened between the two, and no armed thread fired. Two explanations remain and neither is measured. The store may have
come from a thread the sweep had not armed yet (it arms a thread 500 ms after its first sight, and a stage-3 writing
thread can be younger; `boot-w4` shortened that but its boot stopped in a pre-existing family before stage 3). The store
may also not be a user-mode store at all (a driver or DMA write would not raise a debug register). The honest limit is
that the arm has no live control in the same boot: the `+0x40` state point, which the job executor writes after a
returning pass, did not fire either, so this run cannot separate "the writer is another thread" from "the points were not
honoured". Section 11's control showed the suspend/SetThreadContext mechanism works on this host, and that is why the
sweep is still the named next falsifier rather than a dead end.

The page-granular watch built first is a measured dead end and is recorded so it is not retried: `boot-w1` made the
stage-3 job's own 4 KiB page `PAGE_READONLY` and the title's allocator-side code (`game+0x538D5xx`, `game+0x515B25x`) then
made 18,409 writable stores into that page in one pass (19,770 page faults), none of them at the object's first qword.
The pass slowed by tens of seconds, the boot stopped at `game+0x28CF870` before stage 3 and the device was lost once.
A 4 KiB page in that heap is shared with hot allocator traffic, so only an 8-byte point is usable there.

### 12.4 The pacing knobs and what they measured

`port/src/vulkan_backend.h` gains two opt-in knobs; both are off by default and the default path changes no call.
`PALMETTO_PACE_IDLE` drains the queue with `vkQueueWaitIdle` after each accepted `ExecuteCommandLists`;
`PALMETTO_PACE_DELAY_US` sleeps that many microseconds after the submission. The pure mapping is `submission_pace_from`
and `port/test/frame_dump_test.cpp` pins it. One boot per arm, the section-10 environment, one knob at a time:

| measurement | default (`boot-final300`, section 11.5) | `PALMETTO_PACE_IDLE=1` (`boot-p1`) | `PALMETTO_PACE_DELAY_US=1000` (`boot-p2`) |
|---|---|---|---|
| stage 3 (machine current 3) | 56.0 s | 51.7 s | 51.0 s |
| first trace exception (the stop) | `game+0x13360A3` read -1, t 59.04 s | `game+0x1335DCB` read -1, t 54.69 s | `game+0x4E5984B` read `0x100000040`, t 56.24 s, then 6 cascading exceptions |
| `RAX` at the stop | `0x3F8000003F800000` | `0x3F8000003F800000` | not the vtable family |
| title pages imported / content submissions | 2 | 2 | **0** |
| content-frame readback, first capture per target | black `0 0 0`, 0/16,384 | black `0 0 0`, 0/16,384 | not reached |
| write control on the same target | `51 102 204`, 16,384/16,384 | `51 102 204`, 16,384/16,384 | not reached |
| presenter captures black | 10 of 10 | 9 of 9 | 8 of 8 |
| pacing cost | none | 3,575 idle waits, 1.7 s in total, longest 7.7 ms | 3,160 sleeps of 1 ms, about 3.2 s |
| device-lost lines | 0 | 0 | 0 |

So the race is timing-induced (both knobs move the stop site) but neither knob removes it: the idle knob moves the same
constant-block vtable to another call in the same executor, and the delay knob moves the failure into the pre-existing
`game+0x4E5984B` cascade and the content submission never happens. Neither knob is console-faithful. `PALMETTO_PACE_IDLE`
makes the CPU wait for the GPU, which a console submission does not; `PALMETTO_PACE_DELAY_US` adds latency the console
does not have. The smallest measured knob costs 1.7 s of GPU wait over 3,575 submissions, and it did not let stage 3
continue. The verdict is therefore "default path untouched, knobs emulation-only, race not fixed".

### 12.5 The content pixels are still black

`boot-w3` reached the same 3-draw content submission as section 10 (2 title-page imports, draws 5038, 5039 and 5041 on
the presented resource ids, `format 37`, 3840x2160): the first capture of each target reads `mean 0 0 0,
non-black 0/16,384`, the intermediate target reads the same, and the write control on the same target reads
`mean 51 102 204, non-black 16,384/16,384, peak 204`, so the copy path works and the zero is the target's content.
`boot-p1` reads the same on its content draws (3564, 3566, 3569 all `0 0 0`). All presenter captures in every arm read
mean rgb 0.00. The pacing knobs change the stop and nothing about the picture, which is the same statement section 10
made for the arena A/B: the first content frames draw black, and the renderer stop is a separate event.

### 12.6 Verdict and the next falsifier

- **The writer is not named.** The corrections stand: the int3 route is fixed (no debug register at the same address) and
  the object is the executor's argument. The store is not observed in the executor thread or in 104 armed threads, and
  the arm's missing same-boot control is named rather than hidden.
- **Next falsifier (one boot).** Arm the job's first-qword point together with a point that is measured live in the same
  pass (the job's `+0x40` state dword, or a page the caller writes), log both, and only then trust the vtable point's
  silence. A second falsifier: arm every thread at stage 3 with no creation-window guard (`boot-w4` shortened it but its
  boot never reached stage 3) and log each arm's thread id, so a young writer thread is either armed or named as missed.
- **The pacing answer is a negative.** Both knobs are emulation-only, both move the stop site, neither lets stage 3
  continue; the default path stays as it is.
- **The content frame stays black.** No knob changed it, which closes the question section 6 left open for the
  scheduling side: the black is the frame the title draws, not the schedule.

### 12.7 Tests and gates

- `port/test/frame_dump_test.cpp` gains six checks for `palmetto::submission_pace_from`: no value and `"0"` leave both
  knobs off, the idle value alone sets only that knob, the delay is decimal microseconds, and both together keep both
  values. The target exits 0.
- Gate subset on the changed tree: `frame_dump_test`, `unimplemented_policy_test`, `graphics_record_test`,
  `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test`, `deferred_compile_test`,
  `arena_size_test`, `xbox_present_test` and `xs_proxy_mock_test` all exit 0; `d3d12_fence_timing_test` remains the
  documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check is the one that requires `PALMETTO_WRITER_WATCH`,
  `PALMETTO_PACE_IDLE` and `PALMETTO_PACE_DELAY_US` to be named in a page, which this section does).

### 12.8 Attribution

This section was implemented from this project's own measurements: the int3 arms, the four-point and sweep write watches,
the executor disassembly at the four sites above, the fault registers and memory, the two pacing arms and the readbacks.
The stop sites and the executor were decompiled with the project's Ghidra tooling from the user's own image; no peer code
was used and no game data is added. RVAs, VAs, field offsets, register values, counts, shapes and outcomes only.

## 13. The writer is a host-CRT fill of 1.0f over the live job, on the executor thread; the prewarm knob leaves one submit stall; the later content frames are still black

**Status: measured. The writer is named.** Section 12 left three open items: its data points were armed from inside the
int3 handler with `GetThreadContext`/`SetThreadContext` on the current thread and never fired (not even the job's
`+0x40` state dword), the arm had no live same-boot control, and the stage-3 content frame was only read where the
section-10 trigger armed it. This section corrects the arming method, proves it with a same-pass control in the
executor thread in the same boot, catches the store that replaces the job's vtable - **a host-CRT `memcpy` of a
1.0f block executed by the executor thread itself, 2 ms after the stage-3 entry** - excludes an alias or GPU import
for that page in the captured boot, measures the deferred-compile submit-wait knob (the wait count collapses, the one
real driver build does not), reads three content submissions instead of two, and serialises only at the content
boundary. The boot still stops in the same title-side family and the content stays black at every later capture.
Counts, RVAs, VAs, module names and register values only; no game data and no package paths.

### 13.1 What this slice changes

| file | change |
|---|---|
| `port/src/main.cpp` | `PALMETTO_WRITER_WATCH` v2: the four write points are armed in the exception's own `CONTEXT` record (the one `NtContinue` restores) instead of a `SetThreadContext` on the current thread; at machine stage 3, DR1/DR2 become a **same-pass control** on the graph lock word the executor's first call writes (`graph+0x18`, `graph+0x20`) and DR3 keeps the job state dword; the every-thread sweep arms the job's first qword in every thread with no per-thread creation-window guard, its first round delayed 1500 ms (`PALMETTO_WRITER_SWEEP_DELAY_MS`; `PALMETTO_WRITER_SWEEP=0` disables it) after the undelayed arm moved the stop out of the vtable window; a 1 ms poller dates the first-qword change; the stage-3 entry prints the job page's mapping, the loaded modules' bases and names, and every writer line resolves the storing RIP through `rt::shims::module_offset` |
| `port/src/vulkan_backend.h` | `PALMETTO_READBACK_AFTER=<n>` widens the section-10 content-capture window (`palmetto::title_import_capture_window_from`, default 3, bounded 512); `PALMETTO_PACE_CONTENT=1` is a third pacing knob that drains the queue only after a submission inside that window; both mappings are pure and tested |
| `port/src/deferred_compile.h` | `CompileQueue` takes a `newest_first` order: the workers take the newest queued build, so a pipeline the title creates immediately before naming it is compiled before older queued builds |
| `port/shims/d3d12_vulkan_device.cpp` | `PALMETTO_PIPELINE_PREWARM=1`: the compile queue runs newest-first and the submit path treats a deferred task the worker already finished as prebuilt instead of counting a submit wait. The deferred switch (`PALMETTO_PIPELINE_DEFER_COMPILE`) is still the only way in; the default order and counters are unchanged |
| `port/test/frame_dump_test.cpp` | the pacing truth table gains the content-boundary knob; the readback-window mapping is pinned (default, empty, "0", a value, the bound) |
| `port/test/deferred_compile_test.cpp` | the newest-first order against the default FIFO order (one blocked worker, two queued tasks: the newest runs first only under the prewarm order) |

All switches are off by default. No layer call is changed when they are unset.

### 13.2 The method correction, with the same-boot control

Section 12 armed from inside the handler and read silence; this slice arms the *exception context* that the handler
returns with, and the same boots show the difference on a control that must fire. The loading-phase job state dword
`+0x40` (the caller stores 2 after a returning pass) fired **both** times it was armed this way
(`game+0x186435`, value after `0xc0010100000002`), which section 12 never observed with its method. At stage 3 the
same-pass control is stronger: the executor's first call after its two entry virtual calls is the graph lock helper on
`graph+0x18`, and its write fired in the executor thread in the same pass as the job-first-qword point.

| arm | points armed in the executor thread | loading-phase state control | stage-3 same-pass control | every-thread sweep |
|---|---|---|---|---|
| `boot-s13-w` (no sweep delay) | object[0], graph+0x18, graph+0x20, job+0x40 | fired 2x | **fired 2x** (`game+0x4ba0`, value after `0x7e9800000001`) | 104/104 threads armed, 0 failed |
| `boot-s13-we` (sweep delayed 1500 ms) | the same four | fired 2x | **fired 1x** (`game+0x4ba0`, value after `0x7e0c00000001`) | 104/104 threads armed, 0 failed, first round after the hit |
| `boot-s13-wi` (no writer watch, the plain default) | none | - | - | - |

The evidence that the section-12 arm was the failure and this one is live is therefore in the same boot as the job
point, which is what section 12 said its negative lacked.

**The observer effect, named.** With the sweep's first round at the stage-3 entry, both writer boots that reached
stage 3 stopped outside the object-vtable window (`0x515acf5`, read `0x18`, inside the allocator path the same
executor pass calls), while a boot with no writer probe stopped at `0x13360a3` at the same stage-3 second (64.7 s).
The sweep suspends every thread once, so its first round is now 1500 ms after the entry; the hit boot then reached
`0x13360a3` and the store was measured at +2 ms - before the sweep - in the executor thread itself, whose four points
were armed from the entry. The sweep's contribution is the non-hit boots' negative: no other thread was measured
making the store.

### 13.3 The writer, named

`boot-s13-we` (trace t, same binary `00D83662...`): stage 3 at 52.2 s, executor entry at steady t `97842.450`, the
same-pass control at `97842.466`, and then

```
[rt] writer watch t=97842.467 OBJECT VTABLE WRITER hit 4 slot 0 tid 32268 writer 0x7ff95435cd56 value after 0x3f8000003f800000 machine stage 3 job 0x2ea1833e440
  bytes before writer rip: c5 fd 7f 99 c0 00 00 00 c5 fd 7f a1 e0 00 00 00
  rax 2ea1833d550 rbx 2ea1833d550 rcx 2ea1833e360 rdx e51b1fea20 rsi 0 rdi 2ea17680f40 r8 1f0 r9 fffffffffffffff0
[rt] writer watch t=97842.472 POLLER: object first qword changed ... was game+0x658da80 now 0x3f8000003f800000
[rt] writer watch t=97842.472 job state writer (control) hit 5 slot 3 tid 32268 writer 0x7ff95435cdbd value after 0x3f8000003f800000
```

- `tid 32268` is the executor thread; the trap is the DR0 point armed in that same pass, and the poller confirms the
  change at the same tick.
- The 16 bytes before RIP are a complete instruction pair: `vmovdqu ymm3,[rcx+0xC0]` then `vmovdqu ymm4,[rcx+0xE0]`.
  `RCX = 0x2ea1833e360`, so `RCX+0xE0 = 0x2ea1833e440` is the job: the store that replaced the vtable is a 32-byte
  AVX store of `ymm4`. The state control at `job+0x40` is hit `0x67` bytes later in the same loop with the same
  value, i.e. the loop covers the job object.
- The writing module is named by that byte pair: it is byte-identical to the code at RVA `0x1cd46..0x1cd55` of the
  host CRT `vcruntime140.dll` (the `memcpy`/`memmove` 256-byte unrolled AVX copy loop: `vmovdqu ymm1..4,[rdx+...]`,
  `vmovdqa [rcx+...],ymm1..4`, `add rcx,100h`). The implied image base is `0x7ff954340000`, a 64 KiB-aligned module
  base, and `VCRUNTIME140.dll` loads at exactly that base in this host's module table (`boot-s13-wg`); module bases on
  this host were measured stable across two processes (`vcruntime140.dll` and `ucrtbase.dll` at the same bases in both
  runs), so the storing instruction is `vcruntime140.dll+0x1cd56`.
- The copy's source is `RDX+0xE0` with `RDX = 0xe51b1fea20` - the executor thread's own stack - so the block copied
  into the job is a **stack-local all-1.0f constant block**: the job's first qword, `job+0x40` and the intervening
  qwords all become `0x3f800000`. That is the same `float4(1,1,1,0)`-family constant block section 10 measured in the
  fault dump, now measured at the store that writes it.

Plain answer to "who corrupts the renderer job": **title-side code on the executor thread itself, via the host CRT's
`memcpy`, copies a 1.0f constant block over a command object the executor still holds**. The job's block is live and
the new `memcpy` destination overlaps it (a freed-and-reused command block, or a size/pointer that reaches the live
one); the store is not the layer's. `palmetto.exe` does not contain the instruction pair (byte scan of the built
image: 0 hits), the copy's source is the title thread's stack, and the writing instruction is inside the host CRT.

### 13.4 Alias and GPU-import test for the same page

- The job page is `MEM_COMMIT`, `MEM_PRIVATE`, `PAGE_READWRITE`, `file-backed no`, inside the title heap's private
  reservation (`region allocation 1ce63a00000 size 212c9000 type 0x20000 protect 0x4`), so no section or image maps
  it and there is no file-backed alias of the page.
- The boot's title-page imports are two ranges, `VA 0x2e922c00800 + 0x120000` and `VA 0x2e922d20880 + 0x60000`; the
  job at `0x2ea1833e440` is in neither, so the layer did not import this page as a host-pointer Vulkan buffer and a
  driver/GPU write into it is not the mechanism in this boot.
- And the store itself was measured on the CPU by a debug register, so the mechanism is a user-mode host-CRT store,
  not a DMA fill.

The capture above is from the build before the module resolver was added (`00D83662...`); the later build prints the writer RIP as a module+offset, and it compiled and printed the module table at stage 3 in `boot-s13-wg`, but that boot did not hit the vtable store, so the resolved label has not yet been observed firing. The limit: the `memcpy` caller is not named. The title-image stack scan found no main-image frame (`(none)`), and
the scan filters to the main image only, so a caller in one of the title's mapped DLLs would not be printed; the
executor thread's own points were live, so the caller is on the executor thread, but which title function called
`memcpy` is the next falsifier.

### 13.5 The deferred-compile submit waits, removed as a knob

The section-12 environment defers compute-pipeline compilation (4 workers, bound 4,096, 2,742 submissions, ~2,420
pending). Every submission that names a deferred pipeline first waits for it. In the default arm the submit counter
reports 163 waits, of which one is a driver build (498.7 ms) and the rest are ~15-20 us checks of tasks already
finished. The knob is `PALMETTO_PIPELINE_PREWARM=1` with `PALMETTO_PIPELINE_COMPILE_WORKERS=8`: the queue serves the
newest build first and a finished task is not counted as a wait.

| measurement | default, 4 workers (`boot-s13-wi`) | prewarm + 8 workers (`boot-s13-pre`) |
|---|---:|---:|
| verdict | STOP `game+0x13360a3` read -1, trace t 69.02 s | STOP `game+0x13360a3` read -1, trace t 60.70 s |
| stage 3 (machine current 3) | 64.7 s | 57.0 s |
| deferred: submitted / built / pending | 2,742 / 317 / 2,425 of 4,096 | 2,742 / 167 / 2,575 of 4,096 |
| submit waits / max single | **163 / 498.7 ms** | **1 / 585.8 ms** |
| content submissions / draws | 2 / 6 | 2 / 6 |
| content-frame first capture per target | `0 0 0`, 0/16,384 | `0 0 0`, 0/16,384 |
| write control on the same target | `51 102 204`, 16,384/16,384, peak 204 | `51 102 204`, 16,384/16,384, peak 204 |
| presenter captures / pixels | 12, every mean rgb 0.00 | 10, every mean rgb 0.00 |
| device-lost lines | 0 | 0 |

So the knob removes the 162 cheap submit-side queue checks but **not the one real driver build**: the pipeline the
stage-3 submission names is created in the same burst the title is submitting from, so the newest-first worker cannot
finish it before the submission. The remaining stall is 585.8 ms, ~87 ms longer than the default arm's single build
(different boot, one sample). The race is therefore not caused by the wait *count*, and the wait *stall* remains:
the next lever would have to build at the recording site (out of this slice's files), not in the compile queue. The
default path is untouched; the knob is opt-in and reports honestly.

### 13.6 The later content frames

The section-10 trigger arms the import submission and the next two; both measured content submissions were captured
from it (`boot-s13-wi`: draws 5204, 5206, 5209, six draws). `PALMETTO_READBACK_AFTER=64` widened the window and the
same boot's content arrived in **three** submissions:

| measurement | `boot-s13-wi` (window 3) | `boot-s13-late2` (window 64, prewarm) | `boot-s13-pace` (window 64, `PALMETTO_PACE_CONTENT=1`) |
|---|---:|---:|---:|
| stop | `game+0x13360a3`, t 69.02 s | `game+0x1335dcb`, t 60.69 s | `game+0x1335dcb`, t 60.89 s |
| stage 3 | 64.7 s | 57.3 s | 57.3 s |
| content submissions / captured draws | 2 / 6 | **3 / 9** | **3 / 9** |
| first capture per distinct target | `0 0 0` (3 targets) | `0 0 0` (3 targets) | `0 0 0` (3 targets) |
| later capture of the same target | `51 102 204` (the within-boot artefact) | `51 102 204` | `51 102 204` |
| write control on every capture | `51 102 204`, 16,384/16,384, peak 204 | the same | the same |
| readback PPM files | 4 | 9 | 9 |
| content-boundary drains / cost | - | - | 6, 19-63 us each (the queue was already idle) |
| presenters | 2,612 presents, 12 captures, all black | 1,730 presents, 9 captures, all black | 2,358 presents, 12 captures, all black |

Every later content submission's first capture of each distinct target is still all-zero; the only non-zero value a
later capture holds is the layer's own write-control colour, which the instrument clears the target to after a
capture (the named in-window artefact of section 10.2) - no title pixel is measured at any later content point. The
content-boundary drain moves the stop to the other call in the same executor (`0x1335dcb`) and lets the boot live
only ~3.6 s past stage 3, with the same number of content submissions as the window alone, so it does not serialise
the race away.

**Answer to the picture question:** within the stage-3 window the black is the frame the title draws, not a first
transition frame that later opens: three separate content submissions, up to 9 readbacks and 9 written windows, and
every first-capture texel is zero (or the layer's own control colour). The boot does not reach a fourth content
submission or a later stage, so nothing here says the title's picture stays black forever; it says no non-black
content frame exists in this window.

### 13.7 The boots

One boot per arm, under the boot lock, the section-12 content environment plus the arm's switches, 300 s timeout, one
binary per build group. All stop with 0xC0000005; no arm lost the device.

| arm | binary sha256 (first 16) | wall | stage 3 | stop / trace t | imports | content readbacks | deferred waits / max | present captures |
|---|---|---:|---:|---|---:|---:|---:|---:|
| `boot-s13-wi` default | `A7F0F57496FAAF25` | 95.9 s | 64.7 s | `0x13360a3` read -1, 69.02 s | 2 | 6 black | 163 / 498.7 ms | 12 black |
| `boot-s13-w` writer v2, no sweep delay | `10C4825D04BBCDC3` | 113.6 s | 81.2 s | `0x515acf5` read `0x18`, 86.41 s | 2 | 6 black | 95 / 506.5 ms | 11 black |
| `boot-s13-we` writer v2, sweep at +1500 ms | `00D83662ECD4824E` | 82.2 s | 52.2 s | `0x13360a3` read -1, 55.68 s | 2 | 6 black | 144 / 480.7 ms | 9 black |
| `boot-s13-pre` prewarm + 8 workers | `761488B015B6E497` | 80.2 s | 57.0 s | `0x13360a3` read -1, 60.70 s | 2 | 6 black | **1 / 585.8 ms** | 10 black |
| `boot-s13-late2` window 64 + prewarm | `761488B015B6E497` | 74.1 s | 57.3 s | `0x1335dcb` read -1, 60.69 s | 2 | 9, firsts black | 1 / 652.3 ms | 9 black |
| `boot-s13-pace` window 64 + content drain | `761488B015B6E497` | 87.1 s | 57.3 s | `0x1335dcb` read -1, 60.89 s | 2 | 9, firsts black | 150 / 508.6 ms | 12 black |

The plain default arm (`boot-s13-wi`, sha `A7F0F574...`) is the section-13 control: 172,512 D3D12 calls, 4,096
submissions by 39.1 s, state words init 3 / loading 2 / session 3 and machine current 3 next 3 at the stop - the
furthest the title gets is unchanged. Its binary is an earlier build than the later arms; the only code difference is
inside the writer probe, which is off in this arm, and the pure-knob code the default path does not enter. Boots that
never reached the stage-3 executor entry (`boot-s13-wb`, `boot-s13-wd`, `boot-s13-wf`) are not evidence for the
writer and are not tabulated; their failure is the same environment variance sections 10-12 recorded, and the writer
arms' stop distribution (two `0x515acf5`, one `0x13360a3`, one `0x1335dcb`) matches the family section 10.3 already
measured across the arena A/B. The two writer arms that reached stage 3 without a vtable hit (`boot-s13-wc`,
`boot-s13-wg`) are the non-hit samples behind the negative that no other armed thread was measured storing.

### 13.8 Limits and the next falsifier

- **One boot per arm.** Stage 3 ranged 52.2-81.2 s across the arms and the stop moved within the family; the
  writer catch is one boot and the content windows are one boot each.
- **The `memcpy` caller is unnamed.** The next falsifier is the same arm with the writer stack scanned with
  `RtlVirtualUnwind` and every frame resolved by module (main image, game DLLs, host modules), armed before the +2 ms
  store; that names the title function that copies the 1.0f block, and the block's source and length.
- **The module base confirmation is cross-boot.** The byte-exact RVA match and the aligned module base agree on
  `vcruntime140.dll+0x1cd56`; the module table that shows the base is another boot's, so the next writer catch should
  print the module table into the same log line (the probe now does).
- **The content window perturbs.** Every armed submission is followed by a queue drain and a control clear, so the
  window arm's stop moved to `0x1335dcb`; the black pixels are still measured under a synchronisation the title's
  console does not have.
- **No later stage.** Stage 4 is not reached in any arm, so "the picture stays black" is a statement about this
  window only.
- **The prewarm knob is not console-faithful and does not remove the stall.** It is opt-in and the default is
  unchanged; a real no-wait arm needs the build at the recording site.

### 13.9 Tests and gates

- `port/test/frame_dump_test.cpp` gains the content-boundary pacing row (own knob, all others off) and five checks
  for `palmetto::title_import_capture_window_from` (default 3, empty, "0", a value, the 512 bound); the target exits
  0.
- `port/test/deferred_compile_test.cpp` gains the newest-first order check (default FIFO runs the oldest queued build
  first; the prewarm order runs the newest), and the target exits 0 with 18 checks.
- Gate subset on the changed tree: `frame_dump_test`, `deferred_compile_test`, `unimplemented_policy_test`,
  `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test`,
  `arena_size_test`, `xbox_present_test` and `xs_proxy_mock_test` all exit 0; `d3d12_fence_timing_test` remains the
  documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check requires the new switches to be named in a page,
  which this section does: `PALMETTO_WRITER_SWEEP`, `PALMETTO_WRITER_SWEEP_DELAY_MS`, `PALMETTO_READBACK_AFTER`,
  `PALMETTO_PACE_CONTENT` and `PALMETTO_PIPELINE_PREWARM`).

### 13.10 Attribution

This section was implemented from this project's own measurements: the context-record arming, the same-pass control,
the every-thread sweep and its delay, the 1 ms poller, the module table and the `vcruntime140.dll` byte match, the
prewarm arm, the widened content window, the content-boundary drain and the boots named above. The executor, the job
layout, the graph lock helper and the stop sites were decompiled with the project's Ghidra tooling from the user's own
image; the host CRT instruction pair was re-read from the system's own `vcruntime140.dll`. No peer code was used and no
game data is added. RVAs, VAs, field offsets, register values, module names, counts and outcomes only.

## 14. The copy is the renderer's own 256-entry table write, made while the executor runs the command it belongs to; no layer frame is in the chain

**Status: measured. The caller and the copy are named; the runtime answer this slice looked for is not measured, and
the default path is unchanged.** Section 13 named the writer - a host-CRT `memcpy` executed by the renderer executor
thread, copying a stack block over the live command object - and left one falsifier: the same arm with the writer
thread's stack resolved by `RtlVirtualUnwind`, every frame labelled by module. This section adds that payload, catches
the store in three boots, and reports the chain: the executor's own virtual call enters `game+0x550E2D0`, a
command-list method that selects a descriptor entry with `game+0x13367A0`, builds a 256-entry `float4` table with
`game+0x13368F0` in its stack frame and copies it with the host CRT `memcpy` to the pointer that entry carries. The
copy is measured exactly: **0x1000 bytes from the executor thread's stack** (`RAX .. RAX+0x1000`, the CRT's unrolled
loop and its tail), and the live command object sits 0xE30-0xEF0 bytes after the destination: the object's header is intact
when the copy starts, and the last 0x110-0x1D0 bytes of the copy land on it while the executor is running it. No frame of the chain is in the layer: the store is title code, the
source is the executor thread's stack, the destination is title heap, and the table's size is title data. Counts,
RVAs, VAs, module names and register values only; no game data and no package paths.

### 14.1 What this slice changes

| file | change |
|---|---|
| `port/src/main.cpp` | the `PALMETTO_WRITER_WATCH` payload: the writer hit and a title-code control hit print the full call chain through `RtlVirtualUnwind` (`writer_watch_full_stack`, the walker the stage-3 pump probe already uses), each frame labelled by `rt::shims::module_offset` (a title module+RVA, `host:<dll>+offset`, or the raw address), all sixteen general-purpose registers, the copy's arithmetic from `RAX` (the CRT's saved destination) and the trapped pass base/left, the destination window before and after the block, the caller frame at `[rsp+8+0x40]` (the out struct `game+0x13367A0` returns), the job's 0x180-byte layout and the vtable identities at `job+0x30`/`job+0x58`; `PALMETTO_WRITER_STACK_LOG=<file>` writes the same payload to a file as well as to stderr |
| `port/src/main.cpp` | at machine stage 3, the second same-pass point (`graph+0x20`, a duplicate word of the `graph+0x18` control's store) becomes a provenance point on `job+0x50` (kind 4), the field the executor reads before its slot-0x40 call; the `graph+0x18` control stays armed in DR1 and is still the same-boot proof (it fired in all three hit boots) |

The default path is unchanged: all of the payload is inside the `PALMETTO_WRITER_WATCH` probe, every switch is off by
default, and no layer call or answer changes. The copy's arithmetic is the CRT's own: `vcruntime140.dll+0x1cb40`
opens with `mov rax,rcx` (the destination), rounds it up to a 32-byte boundary, copies 0x100 bytes per unrolled pass
and finishes with the tail from `rcx+r8`; at the trapped store `RCX` is the pass base, `RDX` its source and `R8` the
bytes from `RCX` to the end, so `RAX .. RAX+0x1000` is a measured range, not an inferred one. The one deviation from
the committed arm is the kind-4 point above; it replaced a word that no measured store wrote separately, and the
same-pass control is unchanged.

### 14.2 The caller stack

Both hit boots resolve the same chain, 24 frames, with the CRT at frame 0 and the title image at frames 1-23; no
frame of the chain is in a mapped title DLL, so every title frame is `game_scarlett_master.exe+<RVA>` (the module
table printed at the stage-3 entry lists the main image and host modules only, and a mapped title DLL would be named
by the loader's own registry the same way the main image is). RVAs are the function entries from the image's `.pdata`,
and the offsets are the return addresses the unwind data reports:

| # | frame | function | in the chain it is |
|---:|---|---|---|
| 0 | `host:VCRUNTIME140.dll+0x1cd56` | the CRT `memcpy` unrolled loop | the storing instruction (one of its 32-byte stores; `+0x1cd0c`, `+0x1cd4e` and `+0x1cd56` were caught in the three boots) |
| 1 | `game+0x550E613` | `game+0x550E2D0` | the table builder's `memcpy` call (`CALL qword ptr [rsp+0x38]` at `game+0x550E60F`) |
| 2 | `game+0x1335BEA` | `game+0x1335A20` | the per-command executor's virtual call `CALL qword ptr [r10+0x40]` at `game+0x1335BE6`, slot 0x40 of `*(job+0x30)` |
| 3 | `game+0x186424` | `game+0x186170` | the job executor |
| 4 | `game+0x18573B` | `game+0x1856D0` | the submit/wait |
| 5 | `game+0x184F42` | `game+0x184EB0` | the flush |
| 6 | `game+0x1847D8` | `game+0x184770` | create/find job |
| 7 | `game+0x1837A2` | `game+0x1836D0` | the command opcode dispatch |
| 8 | `game+0x182EB4` | `game+0x182D90` | command build |
| 9 | `game+0x182BBA` | `game+0x182B60` | command build |
| 10 | `game+0x182AB2` | `game+0x1829C0` | command build |
| 11,13,15,17 | `game+0x183AAF` | `game+0x183A20` | the command graph's repeated-list walk |
| 12,14,16,18 | `game+0x18435F` | `game+0x184250` | the same walk |
| 19 | `game+0x1826CB` | `game+0x182600` | command graph |
| 20 | `game+0x503D5F5` | `game+0x503D4C0` | renderer frame path |
| 21 | `game+0x180F2D` | `game+0x180F00` | renderer frame path |
| 22 | `game+0x503CE73` | `game+0x503CDE0` | renderer frame path |
| 23 | `game+0x41F3F8` | `game+0x41F380` | the outer frame entry |

The copy is therefore made **inside the executor's own pass**: the frame chain that builds the command (walk, build,
dispatch, create/find job, flush, submit/wait) is the same chain that runs the executor, so the job is executed
synchronously by its builder, not handed to another thread.

### 14.3 What the copy is

The decompiled chain (this project's Ghidra tooling, the user's own image) reads:

- `game+0x1335BE6` calls slot 0x40 of the object at `job+0x30` with `RDX` = the descriptor and `R8` = a copy
  function pointer the object at `job+0x58` returns from its slot 0x10 (the executor also passes the value `0x18` in
  `R9`). The measured `RAX`/`RBX` at the trap show that call is what reaches the CRT `memcpy`.
- `game+0x550E2D0` (the slot-0x40 method) resolves the descriptor's entry with `game+0x13367A0(descriptor, 0, &out)`
  and, in this branch, runs `for (i = 0; i < this->+0x3c; ++i) copy(out.+0x18, stack_table, this->+0x38 << 4)`.
  Measured: `[this+0x38] = 0x100` and the call's size is `0x100 << 4 = 0x1000`.
- `game+0x13368F0` fills that stack table: a **256-entry `float4` table** (0x1000 bytes) that starts as one
  `float4(1,1,1,0)` value repeated and then gets a computed prefix for the mip levels it covers; the measured source
  block is exactly that shape - its first 0x868 bytes are the computed prefix and the rest is the `float4(1,1,1,0)`
  default, which is why the copied bytes are `0x3f8000003f800000, 0x000000003f800000` alternating (section 13 called
  the block "all-1.0f"; the dump shows the 0.0 lane).
- `game+0x13367A0` returns the descriptor entry: at the hit the out struct in `game+0x550E2D0`'s frame at
  `[rsp+0x80]` holds `{0x00000100, 0x00000001, 0, 0, destination}` - a 256-wide, one-high element and the destination
  pointer, i.e. 256 16-byte elements = 0x1000 bytes, the copy's length.
- The destination pointer itself is written by the **allocation `game+0x1445594F0` makes**: that helper allocates a
  0x68-byte descriptor object, then for each of its entries allocates a block with the title's allocator
  (`CALL qword ptr [allocator+0x50]`), records its size and address in the 0x20-byte entry, and the copy follows
  `entry[0].+0x18`. That is the "constant upload" path on this title: a CPU-built table into a title-heap block, with
  no D3D12 call for the transfer.

### 14.4 The destination, its provenance, and our answers

Three catches, three different blocks, one layout:

| boot | job | destination (`RAX` at the trap) | job - destination | copy end | job header before the copy |
|---|---|---:|---:|---:|---|
| `boot-s14-stack6` | `0x1e7433394e0` | `0x1e7433386b0` | 0xE30 | job + 0x1D0 | vtable `game+0x658DA80`, graph at `job+0x28` |
| `boot-s14-stack7` | `0x19f17141440` | `0x19f17140560` | 0xEE0 | job + 0x120 | the same vtable and graph fields |
| `boot-s14-stack11` | `0x1ea4133e440` | `0x1ea4133d550` | 0xEF0 | job + 0x110 | the same vtable and graph fields |

So in every catch the copy starts 0xE30-0xEF0 bytes before the live command object and its 0x1000 bytes run past the
object's first bytes: the job's vtable, its state dword and the fields the executor calls through are overwritten by
the table's `float4(1,1,1,0)` default. The command object's own `job+0x100` field already held the destination
address before the copy (it is outside the copied range at the trap), i.e. the object and the table's block are
related by construction, and the object's header was intact until the copy wrote over it.

The destination is title-private memory: `VirtualQuery` at the block reports `MEM_COMMIT`, `MEM_PRIVATE`,
`PAGE_READWRITE`, file-backed no, inside the title's reserved region (the same region the job page is in). It is not
a range the layer registered: in the same boot the layer imported two title-page spans for the content draws (for
example `0x1e94bc00300 + 0x120000` and `0x1e94bd20380 + 0x60000` in `boot-s14-stack11`) and the destination is in
neither, it is not in the 32-bit arena window the replay places buffers in, and the console placed-resource ranges the
layer recorded at slot 133 are in the `0x1F3...` range. The copy also has no layer frame between the executor entry
and the store: the `[d3d12-trace]` stream shows `ID3D12CommandQueue::ExecuteCommandLists ok` before the entry and
`Xbox CreatePlacedResourceX`/`CreateShaderResourceView` about 10 ms before it, and nothing between the entry and the
store is a layer call.

What our answers show, and what they do not: the copy's length, source, destination and the allocator that produced
it are title-computed values (`this->+0x38`, the executor stack, the descriptor entry, the title allocator), and no
layer call participates in computing them, so **this slice measures no runtime answer as the producer of the
collision**. Two title-side mechanisms fit the measured layout and are not separated yet:

- the table's block may be allocated with a size below the 0x1000 bytes the copy writes, so the copy runs past its
  own block into the next live object in the same heap page (the entry's `+0x10` size field read 0 at the hit, so this
  is not yet measured);
- or the allocator may return a range that overlaps the still-live command object (the object's header survived
  because the allocator's bookkeeping is at the block's start, not at the object's offset).

The next falsifier distinguishes them and is named in section 14.5.

### 14.5 Verdict, the fix decision, and the next falsifier

- **The caller and the copy are named; the runtime answer is not.** The full cross-module chain is section 14.2, the
  copy's extent is section 14.3 and the destination's layout is section 14.4. No layer frame, no layer range, no layer
  size is in the chain, so there is no measured producer in this tree to fix. The honest statement is the one section
  13 made and this section sharpens: the title writes a 0x1000-byte table into a block that abuts (and is overwritten
  onto) the command object the same pass is executing.
- **No default-path change, no new emulation.** A layer change cannot resize a block the title allocates from its own
  heap with its own allocator, and adding a fence or a delay would not move two live blocks apart. The default path is
  untouched, the two candidate mechanisms are named, and the next falsifier is a probe, not a fix.
- **Next falsifier (one boot).** Read the allocator call's size argument at the hit: the out struct's `+0x10` is this
  entry's size field (`0` for the selected entry here, so the size must be read from the executor's own descriptor
  locals, the two 0x20-byte structs the executor builds), and log `(allocator+0x50)(size, 0)`'s `size` from
  `game+0x1445594F0`. A size below 0x1000 makes the mechanism an overrun of the block; a 0x1000 size whose range
  contains the live job makes it a title allocator reuse, and only then is a lifetime/ordering emulation the next
  question.
- **A same-frame control for the write itself is still missing.** The same-pass control proves the data watch is
  live in the pass, but no control store *inside the copied block* has been armed; the payload's pattern scan and the
  copy arithmetic are the cross-check instead.

### 14.6 The boots and the A/B

One boot per arm, under the boot lock, the section-12 content environment, 300 s timeout; the three probe boots are
the same binary per build group and the A/B arm is the probe off. All stop with 0xC0000005 and none loses the device.

| arm | binary sha256 (first 16) | wall | stage 3 | stop / trace t | `ExecuteCommandLists` ok / refused | content readback, first capture per target | write control | presents / captures |
|---|---|---:|---:|---|---:|---|---:|---:|
| `boot-s14-stack6` writer payload, hit | `AF0185F50AB23DF8` | 77.2 s | 58.3 s | `0x13360a3` read -1, 62.05 s | 4,037 / 0 | `0 0 0`, 0/16,384 (draws 4026, 4028, 4031) | `51 102 204`, 16,384/16,384 | 2,018 / 12 black |
| `boot-s14-stack7` writer payload, hit | `E62582CBD4B2CF72` | 127.7 s | 111.2 s | `0x13360a3` read -1, 114.78 s | 5,375 / 0 | `0 0 0`, 0/16,384 (draws 5362, 5364, 5367) | `51 102 204`, 16,384/16,384 | 2,687 / 13 black |
| `boot-s14-stack11` writer payload, hit | `7035DE1267CCA7E2` | 85.7 s | 68.3 s | `0x13360a3` read -1, 71.63 s | 4,633 / 0 | `0 0 0`, 0/16,384 (draws 4620, 4622, 4625) | `51 102 204`, 16,384/16,384 | 2,316 / 13 black |
| `boot-s14-ab300` default path, A/B | `7035DE1267CCA7E2` | 105.1 s | 84.0 s | `0x13360a3` read -1, 88.14 s | 6,127 / 0 | `0 0 0`, 0/16,384 (draws 6116, 6118, 6121) | `51 102 204`, 16,384/16,384 | 3,063 / 13 black |

The A/B arm is the same binary as `boot-s14-stack11` with the probe off: it reaches the same stage-3 second family
(84.0 s), stops at the same site with the same operand, imports the same two title-page spans and captures the same
black content frame, so the probe payload does not move the default path. The picture is section 13's: the first
capture of every content target is all-zero, the layer's own write control on the same target reads `51 102 204`, and
every presenter capture reads mean rgb 0.00. `PALMETTO_READBACK_PPM` wrote 4 PPM files in every arm; no arm records a
`Vulkan device was lost` line. The content-frame draws are 6116/6118/6121 in the A/B arm; the draw indices differ per
boot as section 10.2 recorded.

The tree's gate build after the measurement is 4CE24B84341FA718...; it differs from the measured arms only in
PE timestamp and comments (the probe payload is the same), the way section 13 recorded its gate build.

### 14.7 Limits

- **The hit is a race.** Eight writer-arm attempts in this slice produced three catches; two more reached the stage-3
  entry without the store and three stopped before it (`0x4D9F636`, `0xF229C`, `0x0`). The three catches are three
  different boots and heap layouts, which is what makes the 0xE30-0xEF0 layout statement credible, not one boot's
  coincidence.
- **The allocation's declared size is not read yet.** The selected descriptor entry's size field is 0, so the two
  mechanisms of section 14.4 are still open; the next falsifier above is the decisive measurement.
- **The one-boot A/B and the compile stall.** This environment's compute-pipeline driver build measured 63 s in the
  A/B boot (`max single compute 63031.7 ms`), which is what moves stage 3 to 84 s; it is the same pre-existing stall
  family section 13.5 measured at 0.5 s in an earlier arm, not a property of this probe.
- **No reference cross-check.** No page of the reference project describes this table or its allocation, so the
  console meaning of the block could not be cross-checked from the reference; the attribution below is this project's
  measurements only.
- **The table's builder is title code.** The 0x13368F0 fill was read with the project's decompiler; its mip-level
  inputs were not decoded further because the deciding measurement is the allocation size, not the table's values.

### 14.8 Tests and gates

- The slice changes no layer call and adds no pure helper, so no test target changed: the writer payload is a
  diagnostic behind `PALMETTO_WRITER_WATCH` and the default path is unchanged (section 11.7 and 12.7 did the same for
  their probe-only slices). The gate subset stays green on the changed tree: `frame_dump_test`,
  `unimplemented_policy_test`, `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`,
  `xapu_chad_test`, `deferred_compile_test`, `arena_size_test`, `xbox_present_test` and `xs_proxy_mock_test` exit 0;
  `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check requires `PALMETTO_WRITER_STACK_LOG` to be
  named in a page, which this section does).

### 14.9 Attribution

This section was implemented from this project's own measurements: the module-aware unwind payload, the provenance
point and its negative, the three writer catches with their registers, memory windows and destination layout, the
copy arithmetic from the CRT's own disassembly, the decompiled chain at `game+0x1335A20`, `game+0x550E2D0`,
`game+0x13367A0`, `game+0x13368F0`, `game+0x1445594F0`, `game+0x186170` and `game+0x1856D0`, and the four boots above.
The title functions were decompiled with this project's Ghidra tooling from the user's own image; the host CRT
instruction pair was re-read from the system's own `vcruntime140.dll`. No peer code was used and no game data is
added. RVAs, VAs, field offsets, register values, module names, counts and outcomes only.

## 15. The table's block is allocated with zero declared bytes; the overlap is the copy's 0x1000-byte overrun, not an allocator reuse

**Status: measured, and negative for a producer in this tree. The block's declared size is 0 because the title's own
row-stride table has no entry for the table's format class (0xa); the copy writes 0x1000 bytes into it anyway, and the
live command object is inside that overrun. The default path is unchanged: the probe is opt-in and every site is behind
`PALMETTO_ALLOC_WATCH`.**

Section 14 left two title-side mechanisms for the overlap: the block was allocated smaller than the copy, or the
allocator returned a range that still held the live object. This section decides between them at the allocation itself,
before the copy runs: it puts int3 sites on the executor entry (the stage-3 gate), on the flush's job allocation, on
the constant helper's block allocation, on the helper's own entry and on the descriptor entry the helper writes, and it
records the allocator's answer, the requested size, the format class, the row stride and the entry as written. In three
caught allocations the block is requested with **0 bytes**; the allocator returns a real block for that request (its
small-size class rounding is `(size + 0xf) & 0x7f0`, so a request of 0 stays 0), the entry declares `stride 0, size 0`,
and the copy's 0x1000 bytes then land on whatever follows the block: on the live job in two catches (stop
`game+0x13360A3`) and past the job on the allocator's own free-tree nodes in the third (stop `game+0x538D0E6`). The
same producer therefore explains both stop families. No layer call is in the chain, and the overlap is the overrun of a
zero-byte block, not the allocator reusing live memory.

### 15.1 What this slice changes

| file | change |
|---|---|
| `port/src/main.cpp` | `PALMETTO_ALLOC_WATCH`: a stage-gated int3 probe. A 20 ms poll of the state machine waits for machine stage 3 and then puts the int3 on the executor entry `game+0x1335A20`; its first entry arms `game+0x184F0C` (after the flush's `[allocator+0x70]` 0xa0-byte job allocation), `game+0x455964C` (after the constant helper's `[allocator+0x50]` block allocation), `game+0x45594F0` (the helper's entry: class, plane count, dims) and `game+0x4559684` (after the helper stores its 0x20-byte descriptor entry). Each payload logs the tick, thread, addresses, the requested size, the class, the width/height/stride, the job the executor is running, whether the copy's range contains it, the allocator object and its vtable slots by module+RVA, and the `VirtualQuery` mapping of both objects. `PALMETTO_ALLOC_WATCH_FREE=1` additionally arms the title allocator's free entry `game+0x515ABB0` at the stage-3 entry and prints a free only within 4 KiB of the job or the block |
| `port/shims/xmem.cpp`, `port/shims/xmem.h` | the range registry behind the probe's attribution question: while `PALMETTO_ALLOC_WATCH` is set, every successful `XMemVirtualAlloc` answer is recorded as a bounded (1024-entry, append-only) range with the title caller that asked for it, and `rt::xmem::served_range()` answers which recorded range contains an address. Off by default: one cached switch read, no lock, in the default path |
| `docs/vertex_buffer_va.md` | this section |

The executor entry is deliberately patched late (at the stage-3 poll, not at startup) so the probe never traps during
loading: the stage-3 reach is the environmental race sections 10-14 record, and the arm's own reach is reported rather
than assumed. The four allocation sites are armed from the first stage-3 executor entry, which is the pass that crashes.
All payloads are bounded (24 lines per site class, then counters) and the handler is the writer watch's own
restore/trap/re-poke discipline. No layer call or answer changes; reverting the slice leaves the default path identical.

### 15.2 The allocator and the two allocations

The caught boot names the allocator from the title's own global and reads its vtable, so the whole chain is title code
over title-private memory:

| item | measured |
|---|---|
| allocator | `image+0xabcc310` -> a title-heap object; its vtable is `game+0x663BDE0` |
| slot 0x50 (alloc) | `game+0x538D930`, which locks the allocator's own mutex and calls `game+0x538D220` |
| slot 0x60 (free) | `game+0x515ABB0` (the site section 10 measured, and the one the executor watch arms) |
| slot 0x70 (alloc-classed) | `game+0x538D040`, which searches the allocator's address tree for a class and calls `game+0x538D220` |
| small-size classes | `game+0x538D220`: a request below 0x201 is rounded to `(size + 0xf) & 0x7f0`, i.e. 0x10-byte classes; a request of 0 rounds to 0 |
| arena | one `MEM_PRIVATE`, `PAGE_READWRITE` committed region (this boot: allocation `0x192ffa00000` + `0x212c3000`); the block, the job and the allocator object all live in it |
| arena provenance | the probe's XMem registry answers that the region's range is inside an `XMemVirtualAlloc` answer this shim returned to title caller `game+0x1e1d28d`; the shim is a host `VirtualAlloc` pass-through for that call type, and the range decision inside the arena is the title allocator's own |

The two allocations the same pass makes, both through this allocator:

| allocation | site | request | returned |
|---|---|---|---|
| the command object (job) | `game+0x184F0C`, after the flush's `[vtable+0x70]` call | `0xa0` bytes, class tag `0x48` | a title-heap block; the same thread (tid 55972 in the third catch) then enters the executor with it in `RCX` |
| the constant table's block | `game+0x455964C`, after the helper's `[vtable+0x50]` call | **`0x0` bytes** | a title-heap block (the allocator's zero class), inside the same executor pass, 13 ms after the entry |

Section 14 named the helper `game+0x1445594F0`; that is the same function written as a VA with the image's
`0x140000000` preferred base, and its RVA is `game+0x45594F0` - the site this slice arms, and the function whose
decompilation section 14.3 quotes. The allocator call sites the helper uses (its own `[vtable+0x50]` calls at
`game+0x4559547` for the 0x68-byte descriptor and `game+0x4559649` for the block) are the two this slice watches.

### 15.3 The trace table

Three catches, three build groups, one shape: the request is 0, the copy is 0x1000, the job is the executor's own `RCX`
in every catch. Addresses are per boot; the low bits repeat because the arena layout is deterministic.

| boot | binary sha256 (first 16) | job | block | block relative to job | requested size | width x height | entry stride / size | copy range contains job | stop |
|---|---|---|---:|---:|---:|---:|---|---|---|
| `boot-s15-alloc5` | `BE2C0110CCA20F21` | `0x2a466b3e440` | `0x2a466b3d550` | job - 0xEF0 | `0x0` | 256 x 1 | - | yes (last 0x110 bytes) | `game+0x13360A3` |
| `boot-s15-alloc23` | `851F27B6124DB1A6` | `0x19303b3e440` | `0x19303b3d550` | job - 0xEF0 | `0x0` | 256 x 1 | `0x0` / `0x0` | yes (last 0x110 bytes) | `game+0x13360A3` |
| `boot-s15-alloc34` | `D0E84F4D2DD9C5F5` | `0x2250d9374e0` | `0x2250d937580` | job + 0xA0 | `0x0` | 256 x 1 | `0x0` / `0x0` | no: the copy runs `job+0xA0`..`job+0x10A0` | `game+0x538D0E6` |

The pattern is the same in all three; what moves is which side of the job the zero-class block comes from (the
allocator's zero-class free tree in the first two, the next arena chunk in the third), and therefore which live bytes
the 0x1000-byte copy hits. In the third catch the job survives but the overrun reaches the allocator's own classed
lookup: the stop at `game+0x538D0E6` is `game+0x538D040`'s tree walk reading a null node's `+0x18`, the same family
section 10 recorded at `game+0x515ACF5` in the free path. One producer, two crash windows.

### 15.4 The mechanism, named at both ends

The helper's entry (site `game+0x45594F0`) and the descriptor entry it writes (site `game+0x4559684`) name the two ends
of the mismatch:

- the class is **0xa**: the job's `+0x58` object's slot 8 (reached from the job's own slot 0x28, `game+0x6FECC0`,
  which tail-calls `*(*(job+0x58) + 8)`) returns 10; the helper stores it at `descriptor+0x28`;
- the size computation is `game+0x455A610`: `sum over plane-count of game+0x1555B60(class, width, height)`, where
  `game+0x1555B60` is `height * row_stride` and `game+0x1555C10` is the row-stride table. That table has bytes-per-row
  answers for classes 1-4, 9, 0x15, 0x32-0x42 and the 100/101/200/201 families, and **returns 0 for class 0xa** (its
  `default:` path). The helper passes plane count 1 and the job's own `+0x38` dims {0x100, 1}, so
  `size = 1 * (0x100 * 0) = 0`;
- the helper asks the allocator for 0 bytes and stores `{0x100, 1, 0, 0, block}` in the entry; the entry-written site
  reads exactly that: `width 256, height 1, stride 0x0, size 0x0`;
- the slot-0x40 builder (`game+0x550E2D0`) then copies `*(int*)(command-list + 0x38) << 4` bytes per iteration: the
  measured `0x100 << 4 = 0x1000` (section 14's copy arithmetic: `RAX .. RAX+0x1000`), i.e. 16 bytes per element, not the
  stride the size computation used.

So the block is not "smaller than the copy" by a few bytes: it is allocated with a declared size of **zero** and the
whole 0x1000 bytes are an overrun. The allocator did what it was asked (its zero-size class is a real allocation); the
mismatch is the title's own class/stride computation against its own fixed 16-byte copy step. This is the "block
undersized" mechanism of section 14.4, and it refutes the reuse mechanism: the allocator never returned a range that
its own bookkeeping considered live.

### 15.5 Verdict, the attribution, and the fix decision

- **A title-side producer, measured.** The request is 0, the entry is `{256, 1, 0, 0, block}`, and the copy is 0x1000;
  every input is title code or title data (the job's format object, the title's stride table, the title's allocator,
  the command-list's own width). No layer call appears between the executor entry and the copy.
- **No measured producer in this tree, so no default-path change.** The layer cannot resize a block the title allocates
  from its own arena with its own allocator, and it cannot change the class value the title's format object returns.
  Adding a guard, delay or fence would not change a 0-byte request into a 0x1000-byte one.
- **What our runtime does participate in, measured and excluded.** The arena's address range was served by our
  `XMemVirtualAlloc` hook (registry answer above), so the pages are ours to the extent that the shim's host
  `VirtualAlloc` pass-through created them - but the *range decision inside the arena* (size class, block address,
  free tree) is the title allocator's own code (`game+0x538D930`, `game+0x538D220`, `game+0x538D040`), and no answer of
  ours changes the class or the copy length. The slot-9 allocator callback, placed-VA bookkeeping and the page-import
  path are not in the chain; the block and the job are in a `MEM_PRIVATE` region, not an imported span or a placed
  range.
- **The next falsifier is on the title side, not the layer.** Decode the object at `job+0x58` and the job slot-0x28
  answer (class 0xa) to name where the class comes from - a title format-table gap, a resource description the title
  fills from asset data, or a field set from a D3D12 description the layer happens to translate. The first two need no
  layer change; only the third would name a layer input worth modelling. Until that is measured, a layer fix would be a
  guess.
- **A console-behaviour question this raises is named, not answered.** The console allocator may treat a zero-byte
  request differently (a minimum block, or a refusal). The measured title's own allocator treats it as a real
  zero-size class; no console service we answer participates, so this is a title-side compatibility question, not a
  shim answer to change blind.

### 15.6 The boots and the A/B

Thirteen full-length probe boots and three default-path boots under the boot lock, the section-12 content environment,
300 s timeout; the probe is stage-gated so its pre-stage-3 cost is zero. Four probe boots read stage 3; three of those
caught the allocation (the table above). One default-path boot reached stage 3. The A/B pair is the same binary with the
switch off and on:

| arm | binary sha256 (first 16) | wall | stage 3 | stop / trace t | `ExecuteCommandLists` ok / refused | content readback, first capture per target | write control | presents / readback windows | presenter captures | device lost |
|---|---|---:|---:|---|---:|---|---:|---:|---:|---|
| A: `boot-s15-off2`, probe off | `D0E84F4D2DD9C5F5` | 70.5 s | 56.0 s | `0x13360a3` read -1, 58.67 s | 3,557 / 0 | `0 0 0`, 0/16,384 | `51 102 204`, 16,384/16,384 | 1,778 / 6 | 9, all mean rgb 0.00 | no |
| B: `boot-s15-alloc34`, probe on | `D0E84F4D2DD9C5F5` | 74.7 s | 64.0 s | `0x538d0e6` read `0x18`, 65.33 s | 4,703 / 0 | `0 0 0`, 0/16,384 | `51 102 204`, 16,384/16,384 | 2,351 / 6 | 12, all mean rgb 0.00 | no |

Both arms stop in the same title-side family (A on the job's vtable, B on the allocator's classed tree, both after the
same 0x1000-byte overrun), both import two title-page spans, both read the first capture of every content target as
`0 0 0` while the layer's own write control on the same target reads `51 102 204`, every presenter capture in both arms
reads mean rgb 0.00, and neither loses the device. The four readback PPM files per arm are the window captures; the
presenter PPM files are 9 and 12. The probe arm's stop moved to the allocator window and its stage 3 arrived later; the
int3 traps are a timing perturbation, the same cost sections 11-14 record for their arms, and the B arm's allocation
records are the measurement, not the A arm's stop. No title pixel is measured in either arm.

### 15.7 Limits

- **The class is measured; where it comes from is not.** The class value 0xa is read at the helper's entry and in the
  descriptor, but the object at `job+0x58` (whose slot 8 returns it) and the job slot-0x28 dispatch were not decoded past
  the two `game+0x6FECC0` instructions. The row-stride table's "no entry for 0xa" is a static reading of
  `game+0x1555C10`; the dynamic size 0 is measured.
- **Three catches, three builds.** The first catch is on the binary before the helper-entry/entry-written sites were
  added; its entry fields were inferred from section 14's read of the same descriptor, not re-read. The second catch has
  the helper and entry points but not the XMem range registry; the third is the A/B pair's binary and has both. The
  stage-3 reach is a race: of the probe boots, four reached stage 3 and nine stopped before it, and the off arm needed
  two attempts.
- **The probe perturbs the window.** The B arm stops on a different site than the A arm (both in the same family) and
  its stage 3 is 8 s later; the allocation records are taken at the allocation, before the copy, so the stop site does
  not decide them.
- **The copy's length is section 14's measurement.** This slice measures the requested size (0) and the entry's stride
  and size (0), not the copy again; the `0x1000` length and its exact range are section 14's CRT arithmetic, and the
  entry's width 256 matches the copy's per-iteration `width << 4`.
- **The arena provenance is a range answer, not a call log.** The registry records the `XMemVirtualAlloc` answer that
  contains the arena's range and the title caller (`game+0x1e1d28d`), but it does not record that call's requested
  size/type (the traced attribute events dedupe per attribute word), and it is append-only: a freed range still answers
  yes. It is enough to attribute the *pages*, not the *range decision*, which is the point.
- **The free point was built but not measured.** `PALMETTO_ALLOC_WATCH_FREE` is implemented and documented, but no arm
  enabled it: the allocation trace already decides the mechanism (the allocator returned exactly what was requested),
  so the free history is not needed for this verdict. It is the next arm if the allocation record is ever disputed.

### 15.8 Tests and gates

- **No test target changed.** The slice adds an opt-in diagnostic probe (`port/src/main.cpp`) and a range registry whose
  only consumer is that probe (`port/shims/xmem.cpp`/`.h`); the default path is unchanged. The existing `xmem_test`
  already exercises the shim's allocation paths and still exits 0 (286 checks), so the new registry code does not regress
  it; adding a test for a diagnostic table that only matters while a probe switch is set would test the test, which is
  the same call sections 11.7, 12.7, 13.9 and 14.8 made.
- Gate subset on the changed tree, all exit 0: `xmem_test` (286 checks), `frame_dump_test`, `unimplemented_policy_test`
  (98 checks), `graphics_record_test` (180 checks), `graphics_pso_test` (312 checks), `buffer_va_index_test`,
  `dstorage_test` (224 checks), `xapu_chad_test` (87 checks), `deferred_compile_test` (18 checks), `arena_size_test`,
  `xbox_present_test`, `xs_proxy_mock_test`; `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check requires `PALMETTO_ALLOC_WATCH` and
  `PALMETTO_ALLOC_WATCH_FREE` to be named in a page, which this section does).

### 15.9 Attribution

This section was implemented from this project's own measurements: the stage-gated int3 sites and their payloads, the
XMem range registry, the three catches in 15.3, the allocator identity in 15.2, the decompiled size chain
(`game+0x45594F0`, `game+0x455A530`, `game+0x455A5E0`, `game+0x455A610`, `game+0x1555B60`, `game+0x1555C10`,
`game+0x538D220`, `game+0x538D930`, `game+0x538D040`, `game+0x515ABB0`, `game+0x6FECC0`, `game+0x663BDE0`,
`game+0x658DA80`) and the two A/B boots. The title functions were decompiled with this project's Ghidra tooling from the
user's own image; no peer code was used and no game data is added. RVAs, VAs, field offsets, register values, class
numbers, sizes, module names, counts and outcomes only.

## 16. The class is a compile-time entry in the title's own format table; game+0x23E550 returns the literal 10 and the row-stride switch has no case for it

**Status: measured, and negative for a layer input. The zero-byte request's class (0xa) is a constant the title compiles
in twice: the image's own format table at game+0x658DC80 has a 16-byte record keyed 0xa, whose three words are
materialised into the format object the job holds, and the constant-upload command object answers its slot 0x20 with the
literal 10 (`mov eax,0xa; ret` at game+0x23E550). The job's slot 0x28 returns that format object's `+8` dword, and the
title's own row-stride switch game+0x1555C10 has no case for 0xa, so the helper asks for 0 bytes and section 14's
0x1000-byte copy overruns the block. No D3D12 description and no answer of this layer is in the chain. The default path
is unchanged: the slice adds probe sites only.**

### 16.1 What this slice changes

| file | change |
|---|---|
| `port/src/main.cpp` | `PALMETTO_ALLOC_WATCH` gains a seventh int3 site and two payloads. The site at `game+0x1847A1` is the instruction after the constant-upload command object's slot-0x20 getter returns; the same 20 ms stage-3 poll arms it before the executor entry, so it catches the class key before the job exists, and it prints the key, the command object and creator object with their vtables and the slots the decompiled code calls, the command object's `+0x38` dims and the caller. The first four helper entries and the stage-3 entry print the class-origin chain (`job+0x28`/`+0x30`/`+0x58` with their vtables and called slots, the format object's 0x40 bytes and the command object's 0x80 bytes). All bounded (24 lines per site class, then counters) and under the writer watch's restore/trap/re-poke discipline; the executor site is still skipped when `PALMETTO_WRITER_WATCH` is on. No layer call or answer changes |
| `docs/vertex_buffer_va.md` | this section |

### 16.2 The class from the format-table record to the zero request

Two ends are measured: the static reading of the decompiled functions and of the image's own `.rdata` table, and one
`PALMETTO_ALLOC_WATCH` boot (`boot-s16-probe1`) that reads the same objects at run time. The table's rows are this
section's evidence; the addresses in the right-hand column are that boot's.

| # | step | what the code does (decompiled) | measured in `boot-s16-probe1` |
|---:|---|---|---|
| 1 | format table record | `game+0x658DC80` is an array of 0x18-byte records ending in a zero record; record 8 is `0x000010020000000a, game+0x1336700, game+0x1336700` | the row's three qwords are exactly the format object's `+0x8`/`+0x10`/`+0x18` |
| 2 | cache seeding | `game+0x1B373F0` walks the records and `game+0x1B37440` allocates a 0x20-byte object with vtable `game+0x6597C60`, writes the record's three qwords at `+8`/`+0x10`/`+0x18` and appends it to the owner's `+0x98` array (count `+0xa0`); the loop is called from `game+0x1DC6D09` | format object vtable `game+0x6597C60`, `+0x8` = `0x000010020000000a` |
| 3 | the virtual key | `game+0x23E550` is slot 0x20 of the command object's vtable `game+0x66314B0`, and its whole body is `mov eax,0xa; ret` | `game+0x1847A1`: key 0xa, command object vtable `game+0x66314B0`, `vtable+0x20` = `game+0x23E550`, command `+0x38` = 256x1 |
| 4 | job creation | `game+0x184770` (reached from `game+0x1837A2`) takes the key from step 3 and calls the creator object's slot 0x10 `game+0x184EB0` with it | creator object vtable `game+0x658DB60` |
| 5 | job constructor | `game+0x185570` stores the command object at `job+0x30`, the creator's `+0x78` at `job+0x28` and the format object the creator's slot 0xa8 `game+0x185010` found by matching slot 8 against the key at `job+0x58` | job vtable `game+0x658DA80`, `job+0x30` = the step-3 command object, `job+0x58` vtable `game+0x6597C60` |
| 6 | the class getter | the format object's slot 8 is `game+0x6FECE0` (`return *(u32 *)(this + 8)`); slot 0x10 is `game+0x80B2C0` (returns `*(u64 *)(this + 0x10)`) | `+0xc` dword 0x1002, `+0x10`/`+0x18` = `game+0x1336700` |
| 7 | job slot 0x28 | `game+0x6FECC0`: `mov rcx,[rcx+0x58]; mov rax,[rcx]; jmp qword ptr [rax+8]` | the executor's class is 0xa |
| 8 | helper entry | `game+0x45594F0`, called from the executor at `game+0x1335D30` | ECX 0xa, plane count EDX 1, dims 256x1 |
| 9 | size computation | `game+0x455A610` sums `game+0x1555B60` over the planes; for this class that is `height * game+0x1555C10(0xa, 256)` | 1 x (1 x 0) = 0 |
| 10 | allocation and entry | the helper's allocator call at `game+0x4559649` asks for the step-9 size and the entry at `game+0x4559684` stores stride and size | request 0x0, entry `{256, 1, 0, 0, block}`, block at job+0xA0 this boot |
| 11 | the copy | `game+0x550E2D0` (slot 0x40 of the command object) copies `this->+0x38 << 4` = 0x1000 bytes into `entry+0x18` | section 14's measured copy, now into the step-10 block |

The `.rdata` table itself, record by record (class = the low dword, second = the high dword of the row's first qword,
then the two words; all RVAs relative to the image):

| record | class | second dword | first word | second word |
|---:|---:|---:|---|---|
| 1 | 1 | 0x41c | `game+0x1336700` | `game+0x1336700` |
| 2 | 2 | 0x457 | `game+0x1336700` | `game+0x1336700` |
| 3 | 3 | 0x41c | `game+0x4583830` | `game+0x4589760` |
| 4 | 4 | 0x457 | `game+0x4583830` | `game+0x4589760` |
| 5 | 9 | 0x141 | `game+0x1336700` | `game+0x1336700` |
| 6 | 0x15 | 0x41c | `game+0x4583A50` | 0 |
| 7 | 0x15 | 0x41c | `game+0x4589790` | 0 |
| 8 | **0xa** | **0x1002** | `game+0x1336700` | `game+0x1336700` |
| 9 | 0x65 | 0x41c | `game+0x4583AD0` | 0 |
| 10 | 0x65 | 0x41c | `game+0x4583AD0` | 0 |
| 11 | 0x32 | 0x47 | `game+0x1336700` | `game+0x1336700` |
| 12 | 0x33 | 0x4a | `game+0x1336700` | `game+0x1336700` |
| 13 | 0x34 | 0x4d | `game+0x1336700` | `game+0x1336700` |
| 14 | 0x35 | 0x62 | `game+0x1336700` | `game+0x1336700` |
| 15 | 0xc8 | 0x13d | `game+0x1336700` | `game+0x1336700` |
| 16 | 0xc9 | 0x13d | `game+0x1336700` | `game+0x1336700` |
| 17 | 0 | 0 | 0 | 0 |

So the class value is not written by the renderer, by a resource description or by an asset: it is a word of a
compile-time table inside the image, materialised once at renderer init (`game+0x1DC6D09`), and the object that answers
it as a class is a function whose whole body is `mov eax,0xa; ret`. The job's format lookup then only has to match it:
the cache finder `game+0x185010` returns the record-8 object whose slot 8 is 0xa, and `job+0x58` holds that object.

### 16.3 The row-stride switch's coverage

The decompiled `game+0x1555C10` (`class = param_1 & 0xfff`, `width = param_2`) is a byte-per-row switch. At the
measured width 256 its answers are:

| class | `game+0x1555C10(class, 256)` | in the format table above |
|---|---:|---|
| 1, 2 | 0x400 | yes (4 bytes per unit) |
| 3, 4 | 0x300 | yes (3 bytes per unit) |
| 9 | 0x100 | yes (1 byte per unit) |
| **0xa** | **0** | **yes - the job's class** |
| 0x15 | 0x200 | yes (2 bytes per unit) |
| 0x32, 0x3f | 0x200 untiled / 0x80 at high nibble 0xa000 | 0x32 only |
| 0x33, 0x34, 0x35, 0x40, 0x41 | 0x400 untiled / 0x100 at high nibble 0xa000 | 0x33, 0x34, 0x35 |
| 0x36, 0x37, 0x3a, 0x3c, 0x3e, 0x42 | 0x80 | no (another table's formats) |
| 0x38, 0x39 | 0x40 | no |
| 0x3d | 0x100 | no |
| 0x65 (100/101 family) | 0x100 | yes (two records) |
| 0xc8, 0xc9 (200/201 family) | 0x100 | yes |
| every other low-12 value | 0 | - |

Among the 16 values of the image's own format table, **0xa is the only one the switch answers 0 for**. The copied data
is 16 bytes per element (0x100 x float4 = 0x1000, section 14.3), and the row's second dword `0x00001002` reads as the
target format byte `2` in its low byte and 0x10 in its high word; that byte is the title's format numbering for
`R32G32B32A32_FLOAT` (16 bytes per element), the numbering this title copies straight into
`D3D12_RESOURCE_DESC.Format` (3D textures, section 2). A case for 0xa returning `256 * 16 = 0x1000` would make the
block and the copy agree exactly; the switch does not have one.

### 16.4 Verdict and the fix decision

- **A compile-time title constant, measured at both ends.** Step 3's `mov eax,0xa; ret` and the record-8 table word
  measured in steps 2 and 6 are the only inputs; the boot reads the same key, the same command-object vtable, the same
  format object and the same class. No D3D12 description, no resource-creation argument, no heap-creation argument and
  no answer of this layer is between the format table and the zero-byte request.
- **No default-path change.** The layer cannot change a `mov eax,0xa` or a word of the image's `.rdata`, and it cannot
  size a block the title's own allocator allocates from its own arena; a guard, delay or fence would not turn a 0-byte
  request into the 0x1000-byte request the copy needs. This is the same decision section 15 took, now with the class's
  producer named.
- **The console-faithful alternative is named, not answered.** The record exists in the image's own table, so the same
  image on the console takes the same class; the honest reading is that this build's row-stride switch is inconsistent
  with its own format table for 0xa (a title-side mismatch between two title components), and no console service or
  layer answer participates. The only fixes are title-side (a switch case, or a different upload path); the layer has no
  measured input to model. The next falsifier is a boot on a build whose switch handles 0xa, or a renderer path that
  does not take this command object's slot-0x40 copy - neither is a shim answer.
- **What our runtime still participates in, unchanged from section 15.** The block and the job live in pages the
  `XMemVirtualAlloc` pass-through created, and the range decision inside the arena is the title allocator's; the class,
  both sizes and the copy length are the title's own code and data.

### 16.5 The boots and the A/B

The probe arm is the section-12 content environment, 300 s timeout, `PALMETTO_ALLOC_WATCH=1`; the A/B arm is the same
environment with the switch unset, repeated until a boot read machine stage 3 (the race sections 10-15 record). Both
boots stop with 0xC0000005 and neither loses the device.

| arm | binary sha256 (first 16) | wall | stage 3 | stop / trace t | `ExecuteCommandLists` ok / refused | content readback, first capture per target | write control | presents / readback windows | presenter captures | device lost |
|---|---|---:|---:|---|---:|---|---:|---:|---:|---|
| A: `boot-s16-probe1`, probe on | `8B4C22C92CB69CC7` | 61.8 s | 56.9 s | `0x538d0e6` read `0x18`, 59.89 s | 3,639 / 0 | `0 0 0`, 0/16,384 - `51 102 204`, 16,384/16,384 - `0 0 0`, 0/16,384 | `51 102 204`, 16,384/16,384 | 1,801 / 6 | 10, all mean rgb 0.00 | no |
| B: `boot-s16-offb1`, default path | `8B4C22C92CB69CC7` | 73.2 s | 67.4 s | `0x515b22d` read `0x3f800000`, 71.07 s | 4,883 / 0 | `0 0 0`, 0/16,384 - `51 102 204`, 16,384/16,384 - `0 0 0`, 0/16,384 | `51 102 204`, 16,384/16,384 | 2,201 / 6 | 12, all mean rgb 0.00 | no |

The probe arm measures the chain in section 16.2 and the section-15 mechanism unchanged: the requested block size is 0,
the entry is `{256, 1, 0, 0, block}`, and the block starts at `job+0xA0` (the section-15 third-catch layout), so the
0x1000-byte copy runs past the block into the allocator's own nodes and the stop is the allocator's classed-tree read
`game+0x538D0E6` (section 15's third stop family). The three content readbacks are two black targets plus one target whose
window holds the layer's write-control colour (`51 102 204`, 16,384/16,384); the write control itself reads
`51 102 204` on every target, and every presenter capture is mean rgb 0.00. The default-path arm, the same binary with
the switch unset, reaches stage 3 later, runs further (4,883 command lists to the probe arm's 3,639), stops in the same
title allocator family at `game+0x515B22D` reading `0x3F800000`, and repeats the same three content readbacks, the
`51 102 204` write control, the 12 black presenter captures and no device loss. So the probe payload does not move the
default path: both arms stop in the title's own allocator code with no layer frame, and neither measures a title pixel.
The off arm is the fifth off boot: the stage-3 reach is the race sections 10-15 record (three off boots stopped before
stage 3, one reached it and lost the Vulkan device during a 2.3 s `ExecuteCommandLists` before any content readback); the
reported pair is the clean one.

### 16.6 Limits

- **One class in the stage-3 window, one boot.** The probe boots of this slice reached one executor pass, one job and
  one helper entry before the stop, so the measured class table is that job's (0xa) plus the static coverage of 16.3.
  Other jobs' classes in later windows were not enumerated; the probe's helper payload would print them (bounded at 24
  entries) if a boot reached them.
- **The table's second dword is a cross-page reading.** The facts measured here are the record's bytes, the class 0xa,
  the 0-byte request, the 0x1000-byte copy and the stop. Reading `0x1002` as the target format byte 2 plus 0x10 leans on
  the earlier measurement that this title copies its format byte into `D3D12_RESOURCE_DESC.Format`
  ([3d_textures.md](3d_textures.md) section 2); the stride switch and the record are the deciding evidence regardless.
- **The format-object creator was read, not caught.** `game+0x1B373F0` -> `game+0x1B37440` is the decompiled seeding
  path and the measured object's three words match record 8 exactly; the boot did not trap the seeding call itself.
- **The probe perturbs the window.** Two more int3 sites than section 15 (the format-getter return and the executor
  entry) and a longer payload; stage 3 arrived at 56.9 s and the stop at 59.89 s in this boot, and the off arm needed
  attempts before it read stage 3. The measurements are at the allocation and the format site, before the copy, so the
  stop site does not decide them.
- **The copy length is section 14's measurement.** This section measures the class, the requested size, the entry and
  the block; the 0x1000 length and its exact range stay section 14's CRT arithmetic.
- **No console cross-check.** No reference page describes this format table or the record, so the console meaning of
  class 0xa is read from the image's own table, not from a peer.

### 16.7 Tests and gates

- **No test target changed.** The slice adds diagnostic sites and payloads behind `PALMETTO_ALLOC_WATCH`; the default
  path is unchanged and no pure helper was added, so the existing targets keep covering the changed tree (sections 11.7,
  12.7, 13.9, 14.8 and 15.8 made the same call). The gate subset on the changed tree exits 0: `frame_dump_test`,
  `unimplemented_policy_test`, `graphics_record_test`, `graphics_pso_test`, `buffer_va_index_test`, `dstorage_test`,
  `xapu_chad_test`, `deferred_compile_test`, `arena_size_test`, `xbox_present_test` and `xs_proxy_mock_test`;
  `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes. No new environment variable was added (the section names the existing
  `PALMETTO_ALLOC_WATCH` and `PALMETTO_WRITER_WATCH`, which the environment-variable page already carries).

### 16.8 Attribution

This section was implemented from this project's own measurements: the decompiled chain at `game+0x184770`,
`game+0x184EB0`, `game+0x184F60`, `game+0x185570`, `game+0x185010`, `game+0x23E550`, `game+0x1B373F0`,
`game+0x1B37440`, `game+0x6FECC0`, `game+0x6FECE0`, `game+0x80B2C0`, `game+0x66314B0`, `game+0x6597C60`,
`game+0x658DB60`, `game+0x1555C10`, `game+0x1555B60`, `game+0x455A610` and `game+0x550E2D0`, the image's own `.rdata`
format table at `game+0x658DC80` (its records as RVAs and header words, no payload), and the one probe boot
`boot-s16-probe1` with the class-origin payload. The title functions were decompiled with this project's Ghidra tooling
from the user's own image; no peer code was used and no game data is added. RVAs, VAs, field offsets, register values,
class numbers, sizes, module names, counts and outcomes only.

## 17. The stage-3 content draws: three passes into two 4K targets, a title-drawn black clear, and no covered pixel under the constant-output control

**Status: measured. The content window's draws are recorded command by command, their targets, viewports, draw kinds,
input layouts, descriptors and root arguments are tabulated, and the black is placed: the title itself clears the
presented target to black before the compose draw, the draws are the last writers of both targets (no clear or copy
after them), the readback path is validated by its write control, and a substituted constant-output pixel shader drawn
with each content draw's own vertices, pipeline and raster state writes **no non-black pixel over the whole 4K target** -
with the documented-default culling and again with culling switched off. The pixel shaders' data, the clear/copy order,
the target identity, the readback path, the viewport/scissor and the cull state are therefore not the measured reason
the frame is black: the content geometry produces no covered fragment. The remaining candidate is the vertex stage's own
transform, whose falsifier and the arms run so far are named in 17.6.**

### 17.1 What this slice changes

| file | change |
|---|---|
| `port/src/vulkan_replay.h` | `PALMETTO_CONTENT_PROBE` and `PALMETTO_CONTENT_PROBE_DRAWS`: the pure arming helpers `content_probe_enabled_from` / `content_probe_draw_limit_from` / `content_probe_arms`, a bounded per-draw row for every armed content draw (`content probe: draw`), and `content_submission()`, the command table of each content submission (bounded to three) with the per-command fields and the ordered writers of every draw target. The per-draw diagnostics the replay already had (colour path, vertex path, sampled tables) now arm on the content window's draws as well as on a `PALMETTO_READBACK_AT_DRAW` index; `armed_submission` arms the window's first three submissions at draw ordinals 0, 1 and 2, so the PS-only control measures a different one of the frame's three passes in each submission |
| `port/src/vulkan_backend.h` | the content-window read once per submission (`title_import_capture_active`), the `content_submission()` call and the PS-only control's content-window arming |
| `port/test/frame_dump_test.cpp` | 14 checks: the content-probe enable value (unset/empty/zero/one), the arming truth table (probe off, outside the window, first draw, last draw inside the bound, the draw at the bound, a zero bound) and the draw bound (default 9, empty, settable, capped at 12) |
| `docs/vertex_buffer_va.md` | this section |

All switches are off by default. With them unset the recorded commands, the rendered output and the default path are
identical: the probe only reads, and the one command it can add is the off-by-default PS-only control the shim already
builds.

### 17.2 The per-draw table

One content submission (boot `d2`, list 2 of its 4 command lists) holds the frame's three passes. The table is what the
replay prepared for each draw and what the command table of 17.3 shows it emitted. Draw indices are the command dump's;
the content-probe row for the same draws prints one higher in this arm because the PS-only control's re-prepared draw
goes through the same prepare path (17.7 names that instrument artefact).

| # | draw (s1 / s2) | title pipeline | shaders (translated corpus) | PS image samples / VS loads | colour target | draw | input | root arguments |
|---:|---|---|---|---|---|---|---|---|
| A | 4468 / 4471 | 0x1874ebe0770 | `fspx/lib_vs_00821` + `fspx/lib_ps_00820` | 0 / 77 | intermediate 0x18def1eef70, format 37, 3840x2160 | triangle list, 3 vertices / 1 instance | vb0 4,080 B stride 24; 3 vertices read | 2 of 32 dwords non-zero (s0/s1, the 64-bit table VA pair) |
| B | 4469 / 4472 | 0x187538c1920 | `fspx/lib_vs_00107` + `fspx/lib_ps_00106` | 0 / 157 | intermediate 0x18def1eef70, format 37, 3840x2160 | triangle strip, 30 indices / 1 instance | u16 index buffer 393,216 B; vb0 1,179,648 B stride 16; loc0/loc1 RGBA8 4 B, loc2 R16G16B16A16 8 B; layout problems 0x0 | 2 of 32 dwords non-zero |
| C | 4470 / 4473 | 0x187535e42c0 | `fspx/lib_vs_00823` + `fspx/lib_ps_00822` | 6 / 213 | display 0x1871d799100 (s2 0x1871d799370), format 37, 3840x2160 | triangle list, 3 vertices / 1 instance | vb0 4,080 B stride 24; 3 vertices read | 2 of 32 dwords non-zero |

Every row shares the rest of the measured state: one colour target at 3840x2160, no depth attachment (pipeline depth
format `VK_FORMAT_UNDEFINED`), viewport 3840x2160 at 0,0 with depth 0..1, scissor 0,0..3840,2160 (the whole target), a
built host pipeline, and the fixed-function stand-in every slot-57 pipeline carries (blend off, depth/stencil off,
back-face culling with clockwise front, triangle topology, one sample) because every fixed-function field of the
undecoded state record is refused by name ([state_blob.md](state_blob.md)). The descriptor tables are:

| # | table VA | table shape | slot 0 V# | the block the V# names | what the pull reads |
|---:|---|---|---|---|---|
| A | 0x1871f3ee200 | 64 slots, 0 T#, 8 data/V# slots | 0x9af210 | 96-byte raw (`32_UINT`) block | 3 of 24 dwords non-zero; the 4 dwords at block+0x20 are zero ([colour_path.md](colour_path.md)'s black-by-design constant pass) |
| B | 0x1871f3ee1a0 | 64 slots, 0 T#, 8 data/V# slots | 0x9ae1c0 | 64-byte raw block | 12 of 16 dwords non-zero after the table pull; block+0x20 = `38000c1b ab1ffffe 00000000 b3eae81e` |
| C | 0x1871f3ed140 | 64 slots, 1 T#, 8 data/V# slots | 0x9ad210 | 96-byte raw block | the one T# is slot 1, resolving to draw A/B's target image (the intermediate), 3840x2160 format 37 |

Two more properties of the table are measured and matter below: the intermediate's own pages and draw B's vertex/index
pages are imported from the title's memory in place at first use (the two `title pages imported at first use` spans in
the same boot, 0x120000 and 0x60000 bytes), and the content draw's vertex bytes are non-zero and varied (component
classes over the 16-byte record: all four components varied, 242/256 records non-zero).

### 17.3 The command table of a content submission

The submission's draw list in replay order (boot `d2`, submission 1; submission 2 is the same shape with different
resource addresses). The ordered writers of each draw target are printed by the same dump and are quoted after the
table.

| # | command | what it is |
|---:|---|---|
| 0 | barrier | a compute-visible resource's layout pair; no content write |
| 1 | dispatch | a 5 x 5 x 33 compute dispatch (the frame's own tile/visibility pass) with its own user data |
| 2 | barrier | the display target, layout 31 -> 2 |
| 3 | clear | the **display target** cleared to `0.000 0.000 0.000 0.000`, whole view, by the title |
| 4 | barrier | the intermediate, layout 6 -> 2 |
| 5 | barrier | the intermediate, layout 2 -> 6 |
| 6 | barrier | the intermediate, layout 6 -> 2 |
| 7 | draw A | sibling constant pass -> intermediate |
| 8 | draw B | the content pass -> intermediate |
| 9 | barrier | the intermediate, layout 2 -> 6 (render target -> shader read) |
| 10 | draw C | compose pass, samples the intermediate -> display target |
| - | barrier | list 3 holds one barrier returning the display target to layout 2 -> 31 |

The writers: the intermediate's writers are `#7 draw`, `#8 draw` with the last writer `#8` and no write after the last
draw; the display target's writers are `#3 clear`, `#10 draw` with the last writer `#10` and no write after the last
draw. So nothing clears or copies either target after the draws - the display target's black is the title's own clear
plus whatever the compose draw writes over it, and the intermediate's content is only what draws A and B write.

### 17.4 The measured pixels

Three boots, one binary. The content arm `c2` reads the 128x128 centre window of every armed content draw and its
write control; the control arms `d2` and `f3` read the **whole 3840x2160 target** (8,294,400 pixels) for the frame's
own content and again after the substituted-PS control draw (`d2` with the title's documented-default state, `f3` with
the control's culling switched off).

| arm | target | first capture | write control on the same target |
|---|---|---:|---|
| `c2` | intermediate, draws A and B | `mean 0 0 0, non-black 0/16,384, alpha 0/16,384` | skipped (the image is the compose draw's armed sampled input) |
| `c2` | display, draw C | `mean 0 0 0, non-black 0/16,384, alpha 0/16,384` | `mean 51 102 204, non-black 16,384/16,384, peak 204` |
| `c2` | the intermediate as the compose draw's sampled T# (pre-draw and post-submission copies) | `mean 0 0 0, non-black 0/16,384` | - (title texture, never modified) |
| `d2` | the intermediate, whole target | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped (armed sampled input) |
| `d2` | the display target, whole target, frame content | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped (armed sampled input in this arm) |
| `d2` | the intermediate after the constant-output PS control on draw A | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |
| `d2` | the intermediate after the constant-output PS control on draw B | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |
| `f3` | the intermediate, whole target, frame content | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |
| `f3` | the intermediate after the constant-output PS control (culling off) on draw A | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |
| `f3` | the intermediate after the constant-output PS control (culling off) on draw B | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |
| `f3` | the display target, whole target, frame content | `mean 0 0 0, non-black 0/8,294,400`, all 64 tile counts 0 | skipped |

All 12 presenter captures in `c2` read `mean rgb 0.00 0.00 0.00`. The constant-output control is the project's own
`fs_solid` pixel shader (32 160 240, alpha 1) substituted into the armed draw's pipeline; the vertex stage, input
layout, vertex/index buffers, counts, viewport, scissor, render target and fixed-function state are the title's own
recorded draw (the same instrument [frame_capture.md](frame_capture.md) section 6 used to prove coverage on the loading
frame). Its target reading back zero over the whole 4K image is the measurement this section adds: the target's
8,294,400 pixels are copied and every one of them is zero, so the draw that produced them covered none of them.

### 17.5 The black-frame verdict

1. **The display target is cleared to black by the title itself**, command #3, every content submission, whole view.
   That is a legitimate title-side clear (a background/compose clear), not a layer write, and it stays black because
   no covered fragment of draw C lands on it.
2. **No clear, copy or resolve writes either target after its draws.** The draw sequence of 17.3 is the complete writer
   order; the black is not a later overwrite, and the previous slices' "written after the last draw" candidate is
   measured negative here.
3. **The readback path is validated on the same resource geometry.** The display target's write control reads
   `51 102 204` over its 128x128 window in `c2`; the earlier slices' 4K format-37 controls agree. The zero is the
   target's content, not a copy, mapping or format artefact.
4. **The content draws produce no coverage.** The constant-output control drawn with each intermediate pass's own
   vertices, pipeline, raster state, viewport and target writes 0 of 8,294,400 pixels for both draw A and draw B. A
   pixel shader that outputs a constant colour and writes nothing can only be a raster result: no fragment of the
   recorded geometry survives to the target. So the black is not the title pixel shaders' colour arithmetic, not a
   zero blend/alpha output and not a masked/targeted wrong render target - there is no covered pixel for any of those
   to colour. The same control with its documented-default back-face culling switched off (`f3`) also writes 0 of
   8,294,400 pixels for both passes, so culling is not what discards the geometry: the cull axis is closed.
5. **Why black then follows for the display.** Draw C (the compose/upscale pass, `lib_ps_00822`) samples the
   intermediate through its only T# (slot 1), whose content reads zero (17.4), and writes over the title's own black
   clear. Even a correctly rasterised compose pass has a black input; and with the geometry feed unproven for the
   translated stages, draw C's own coverage is not established either.

### 17.6 The remaining candidate and its falsifier

The measured negative is that the content geometry does not rasterise; the candidate the measurements point at is the
**vertex-stage feed of the translated FSPX passes**. All three passes are translated FSPX programs: the port's
translation reads the console driver block/root arguments out of the 32-dword user-data block, the graphics stages form
their fetches from the s0/s1 pair, and the graphics bind point's console layout (the console slots from s8) is a
separate, opt-in feed (`PALMETTO_GRAPHICS_CONSOLE_ROOT`) that stays off by default. In this boot every content draw's
recorded block is 2 of 32 dwords non-zero - the descriptor-table VA pair - and s8..s31 are zero, which is exactly the
shape whose earlier A/B ([vertex_constant_feed.md](vertex_constant_feed.md) section 5) was measured on the loading
frame, not on a content pass.

Falsifier (one boot): the same arm with `PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`, the documented geometry fix that
synthesises the console driver block's vertex-stream V# from the draw's bound vertex view and emits it at s0/s1
([vertex_path.md](vertex_path.md)), plus the PS-only control and the full-target readback. If the control then writes
32 160 240 over the content geometry's rectangle, the missing driver-block feed is the measured cause of the zero
coverage; if it stays at 0/8,294,400 the next one-line arm is the cull axis
(`PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE`, [frame_capture.md](frame_capture.md) section 8), because the documented
stand-in's back-face culling is the other state that can discard an otherwise-covered triangle.

That first form of the falsifier was run and is **negative for the arm, not for the hypothesis**: with
`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` two boots (`e1`, `e2`) did not reach stage 3. The geometry fix writes its
driver blocks from the second it starts (`vertex stream descriptor: ... driver block ... (emitted at s0/s1)`), and the
title's own state words then read garbage (from 62 s on, `init` changes from 2 to values like `1043002015` and
`1998005352`, and `boot-media gate` likewise), with the stop an execute fault at a heap address (`e1`:
`0x313975619e`). The fix's own page already records that it stalls the boot; the honest statement is that this arm
cannot carry the content-window acceptance, so the feed falsifier needs an arm that changes only the substituted
control draw and leaves the title's draw state as the default path records it.

The arm that does change only the control draw is the cull axis, the other state that can discard a covered triangle:
`PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1` rebuilds only the substituted control pipeline with back-face culling
off, every other title field kept. That arm ran (`f3`, its third attempt) and reached the content window: the
constant-output control with culling off writes **0 of 8,294,400 pixels** for draw A and for draw B, all 64 tiles
zero, and the frame's own display target reads 0 of 8,294,400 as in `d2`. So the documented stand-in's winding is not
what discards the content geometry either, and the cull axis is closed.

With the pixel shader (constant-output control), the clear/copy order, the target identity, the readback path, the
viewport/scissor and the cull state all measured negative, the remaining candidate is the **vertex stage's own
transform**: the clip positions the translated `lib_vs_00821`/`lib_vs_00107` modules write, which are built from the
inputs already measured above - the bound vertex/index data (non-zero, imported in place), the descriptor table's V#
blocks (the s0/s1 pair, non-zero after the pull) and the rest of the 32-dword user-data block (30 of 32 dwords zero).
The next falsifier is the vertex-path dump's position read at the content draws (a bounded read of the clip-position
output or of the fetch slots the module uses) and, if a feed is missing there, the console-layout A/B
(`PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, which changes the recorded user-data block only). The driver-block form of the
same falsifier (`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`) is named above as negative for the arm: it corrupts the title's
own state before stage 3 in this environment, so it cannot carry the acceptance.

No layer fix is made: the default path is unchanged, no measured conversion bug is named in this window yet, and the
fix candidate above is a default-path policy decision whose acceptance this content window now makes measurable (the
full-target control and the frame's own pixels), not a blind change.

### 17.7 The boots

One boot per arm, under the boot lock, the section-12 content environment (`PALMETTO_STAGE3_PUMP`, deferred compile,
presenter captures, centre-window readback, PPM files), 300 s timeout, one binary for `c1`, `c2`, `d1` and `d2`
(SHA-256 `9458919E3135ABBE...`). The stage-3 reach is the environmental race sections 10-16 record; the arms below that
did not read stage 3 are tabulated so the reach is not assumed.

| arm | switches added | wall | stage 3 | content window | verdict / trace t | what it measured |
|---|---|---:|---:|---|---|---|
| `c1` | content probe, sample, colour, vertex path | 162.7 s | 94 s | not reached | STOP `game+0x54bde92` write 0x14 | no content draw before the stop |
| `c2` | the same | 89.7 s | 68.1 s | 2 submissions, draws 5368-5373 | STOP `game+0x515acf5` read 0x18, 85.9 s | command table, per-draw table, colour/vertex/sample dumps, 6 window readbacks and controls, 4 readback PPMs |
| `d1` | + PS-only control, full-target readback | 83.0 s | none | not reached | STOP `game+0x28cd3cc` read | no content draw before the stop |
| `d2` | the same | 124.5 s | 64 s | 2 submissions, draws 4468-4473 | STOP `game+0x13360a3` read -1, 121.2 s | PS-only control on draws A and B, full 4K target readbacks, colour/vertex dumps |
| `e1` | `d2` + `PALMETTO_VERTEX_STREAM_DESCRIPTOR=1` | 89.0 s | none | not reached | STOP `0x313975619e` execute | the geometry fix writes driver blocks from 62 s and the title's own state words read garbage; no content draw before the stop |
| `e2` | the same | (killed) | none | not reached | - | same state corruption from 68 s; the arm was stopped |
| `f1` | `d2` with `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1` | 89.6 s | 56 s | not reached | STOP `game+0xb87495` read | stage 3 reached, the title's state words read garbage from 66 s, no content draw before the stop |
| `f2` | the same | 68.9 s | none | not reached | STOP `0x0` execute | no stage 3 |
| `f3` | the same | 171.2 s | 56 s | 2 submissions, draws 4426-4433 | STOP `game+0x515acf5` read 0x18 | cull-off control on draws A and B: 0/8,294,400 each, all tiles zero; the frame's display target 0/8,294,400 |

`ExecuteCommandLists` in `c2`: 5,379 accepted, 0 refused; `d2`: the same binary, 0 refused. No arm records a
`Vulkan device was lost` line. The readback PPM files (`c2`, four 128x128 windows) are all zero where the frame drew.
The content window holds **two** submissions in both measured arms; a third content submission did not arrive before
the stop.

### 17.8 Limits

- **One to three boots per arm.** The stage-3 reach is the race sections 10-16 record (`c1` and `d1` read stage 3 but
  stopped before the content window; the cull-off arm reached it on its third attempt; the driver-block arm corrupted
  the title's own state in both of its attempts). The content-window measurements are three boots and two submissions
  each.
- **The content probe's draw index is one higher than the command dump's in PS-control arms.** The control's
  re-prepared draw goes through the same `prepare_draw`, so it is counted by the probe (its row prints pipeline 0x2,
  the reserved control pipeline id) and shifts the per-draw indices; the command dump's indices are the frame's own.
- **The sample dump's V# read happens before `prepare_draw`'s page pull** and reads the pre-pull arena mirror (zeros
  for the blocks whose pages were not yet copied this submission); the colour-path reader pulls the table first and is
  the one that reflects what the shader sees. The two lines in the same boot differ by exactly that pull and the
  colour-path value is the one used above.
- **The full-target arm's write control is skipped on every captured target**, because the sampled-texture arming
  marks the intermediate and the display target as sampled inputs in that arm; the copy path's validation is the
  content arm's display-target control (`51 102 204`, 16,384/16,384) and the earlier slices' 4K format-37 controls.
- **The display target's own compose coverage is not controlled.** The three ordinals covered draw A and draw B; the
  compose draw C was never the armed control draw in these two submissions. Its input and the title's clear are
  measured, its raster is not.
- **The PS output arithmetic is not traced at content.** The structural reading is the earlier pages' (lib_ps_00820 and
  lib_ps_00822); this section's claim about the pixel shaders is only the measured one - the constant-output control
  writes nothing, so the black is not their colour result.
- **The pixel data windows are single-boot.** The V# block values, the 2-of-32 user-data shape and the imported spans
  are one boot's; the draw structure and the zero readbacks repeat in the second content boot (`c2` vs `d2`) and in
  sections 10-15's five earlier content arms.

### 17.9 Tests and gates

- `port/test/frame_dump_test.cpp` gains 14 checks for the content-probe arming rule, its enable value and its draw
  bound; the target exits 0.
- Gate subset on the changed tree, all exit 0: `frame_dump_test`, `unimplemented_policy_test` (98 checks),
  `graphics_record_test` (180 checks), `graphics_pso_test` (312 checks), `buffer_va_index_test`, `dstorage_test`
  (224 checks), `xapu_chad_test` (87 checks), `deferred_compile_test` (18 checks), `arena_size_test`,
  `xbox_present_test`, `xs_proxy_mock_test`; `d3d12_fence_timing_test` remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and every
  `tests/test_docs_*.py` check passes (the environment-variable check requires the new switches to be named in a page,
  which this section does: `PALMETTO_CONTENT_PROBE`, `PALMETTO_CONTENT_PROBE_DRAWS`, and the reused
  `PALMETTO_FRAME_DUMP_CONTROL_PS`, `PALMETTO_READBACK_FULL` and `PALMETTO_VERTEX_STREAM_DESCRIPTOR`).

### 17.10 Attribution

This section was implemented from this project's own measurements: the content-probe trigger's imports, the per-command
and per-draw tables, the writer order, the readback and full-target pixel statistics, the PS-only control's zero
coverage, the translated module names and the two boots above. The shader names are the shader corpus's own index names;
the structural reading of the pixel shaders is the earlier pages' and is cited, not re-derived here. The title functions
and the fixed-function stand-in are the earlier slices' measurements ([state_blob.md](state_blob.md),
[frame_capture.md](frame_capture.md)). No peer code was used and no game data is added: RVAs, VAs, resource ids,
counts, formats, viewport/scissor values and outcomes only.
