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
