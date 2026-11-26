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
