# Replay resource resolution: why the recorded draws are refused

**Status: measured** on the retail 1.0.2.1 package, Windows 11, RTX 4090, `PALMETTO_D3D12=vulkan`, shader corpus in
`PALMETTO_SHADERS`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, 150 s boots. Counts, pointers-as-classes
and error codes only; no package bytes, paths or shader data.

The boot reaches the draw path (`d0095ca70`): per 150 s, 43,750 `SetPipelineState` and 43,750 `DrawInstanced` are
recorded, 10,241 device-slot-57 records build 10,241 PC graphics pipelines with no refusal, and every `Close` is
accepted. But every submission that carries one of those draws is refused by the replay layer, before anything is
submitted.

## 1. What the refusal is

`ExecuteCommandLists` returns `ReplayResult::unresolved_resource`, which `port/shims/d3d12_com.cpp` prints as

```
[rt] d3d12: ExecuteCommandLists failed: unknown resource; nothing was submitted
```

Measured on an unmodified boot (`F:\palmetto-work\boot-replay-baseline2`, 150 s): `ExecuteCommandLists` ok 43,755,
failed 43,750 — one failed submission per draw, each carrying a separate `[d3d12-trace] ... failed` line. No `Close`
fails, and the run is ALIVE for the whole timeout with no exception, so the recording side is consistent; only the
replay's lookup refuses.

## 2. Which check fails, with evidence

The replay returns `unresolved_resource` from several places. Bounded diagnostics were added (section 6) and a fresh
150 s boot (`F:\palmetto-work\boot-replay-diag1`) names the one that fails:

```
[rt] replay: buffer_at miss for VA 0x25780b3df10 size 0xff0
[rt] replay: unresolved resource at draw.vertex_buffer: id 0x25780b3df10, extra 0xff0
[rt] replay: list 0 of 2 refused in prepare: result 2, 7 command(s)
```

The pipeline resolves, the render-target and depth bindings resolve (the frame's views are null and skipped), and the
vertex-buffer lookup is what fails: `prepare_draw`'s `vk.buffer_at(view.va, ...)` cannot find a registered buffer that
covers the console VA the draw binds. It is the same check on every draw.

## 3. Where the missing binding was created

Earlier in the same boot:

```
[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=0000025780B00000 dimension=1 width=182452224 ...
[rt] d3d12: a title-placed buffer (device slot 133) could not import its pages, so it is an ordinary buffer
            the title's writes do not reach; HRESULT -2147467263
```

The title places one 174 MiB buffer (device slot 133) on xmem physical pages and names it by the console VA
`...0B00000`. The draw's vertex-buffer VA is inside that buffer: it is exactly `...0B00000 + 0x3DF10`, and the
following draws advance `0x20000` each, all within the 182,452,224-byte width.

The page import refused (`E_NOTIMPL`; with `VK_EXT_external_memory_host` present the whole 174 MiB range is still not
importable because xmem commits its pages lazily), so `Device::create_at_host_address` answers with its documented
stand-in: an ordinary buffer of the same description. The stand-in is registered at its own arena VA, **not** at the
console VA the title placed the resource at — unlike the successful import path, whose contract already says "its
console VA is `address` itself, the VA the title chose". So the one creation path that answers with a stand-in lost
the VA mapping, and every draw and copy that reads the pages by VA is refused.

## 4. The candidate fix and what it measured

The smallest honest change at the source of truth is to register the stand-in at the console VA it stands in for,
where the VA is still known: in `Device::create_at_host_address`'s fallback branch, after `create_committed` succeeds,
register the returned buffer under the address the resource was placed at (a small `VulkanContext` helper that
re-registers the binding's `va`; no lookup was loosened). The stand-in's bytes still are not the title's writes, which
the line above the change already says.

Measured with that change (`F:\palmetto-work\boot-replay-fix1`; the boot did not reach the timeout):

| | before (baseline, 150 s) | with the VA registration |
|---|---:|---:|
| `ExecuteCommandLists` refused as `unresolved_resource` | 43,750 (one per draw) | **0** |
| `buffer_at` misses / `draw.vertex_buffer` refusals | every draw | **0** |
| draw-carrying submissions accepted | 0 | the first ones (trace `#53580`, `#53592`, `#53595` return ok) |
| `DrawInstanced` executed | 0 | 3, then the run ends |
| run | ALIVE 150.2 s, no exception | **GPU reset (TDR)** at 22.95 s, title `0xC0000005` at 27.1 s |

The GPU did not merely refuse the work: it faulted. `Get-WinEvent System` shows two `nvlddmkm` event 153 (GPU error /
TDR) entries at the minute of that boot, and with the device-lost line added (`F:\palmetto-work\boot-replay-val1`,
`F:\palmetto-work\boot-replay-zerovtx`) the replay reports

```
[rt] replay: the Vulkan device was lost
```

followed by `ExecuteCommandLists failed: unsupported`, `Signal failed`, and the title's exception.

A control run separates the vertex-fetch data from the shader path: with the same VA registration and **every**
vertex-buffer binding redirected to the replay's zero page (stride 0, so no bytes are fetched from the stand-in), the
TDR still happens at the same trace point (`F:\palmetto-work\boot-replay-zerovtx`). The fault is therefore not the
fetch of the stand-in's bytes; it is what the draw's shaders and descriptors address.

That is the fault mode `docs/d3d12_vulkan.md` section 3.3 already measures: a translated shader computes
`host_address = heap_base + console_va`, and a console VA that is not an offset into the GPU address arena produces a
wild address — "the draw faults and the whole queue submission is lost". The title's pages are named by
two-terabyte title VAs (`...0B00000`), which can never satisfy that equation. The candidate was reverted: the
baseline above is the tree's behaviour.

## 5. What has to change before draws can execute

The VA registration is necessary but not sufficient. The draws can only execute once the title's pages are part of
the single GPU address space:

1. **Give every title-placed buffer an arena range at creation** (slot 133 / `create_at_host_address`), so its
   shader-visible console VA is an arena offset (`heap_base + VA` is its real device address). The buffer is a
   stand-in for the pages, so the arena range is zero-filled stand-in data until page import or a copy fills it; either
   way the address arithmetic is valid.
2. **Keep the title VA as an alias** for the command arguments that name the pages by the VA the title chose (vertex,
   index and indirect views), which is the registration this note measured. One place does both.
3. **Rebase the title VA everywhere a shader can see it**: root descriptor VAs at record time, and V# descriptors —
   the ones the layer writes already come from the resource's arena VA, but a title that writes its own descriptors
   into a CPU-visible console heap needs those bases rebased, or the draw must stay refused. Any VA that cannot be
   rebased has to keep the submission refused with `unresolved_resource`; executing it is a guaranteed GPU fault.

Until step 1 lands, step 2 alone turns a clean refusal into a GPU reset. Step 2 is a single registration in the
creation path, so the later page work (import, or a copy into the stand-in) will not need a second wiring change: the
VA contract is the same one the successful import path already documents, and the stand-in now has exactly one place
that must know it.

## 6. Diagnostics added with this note

Bounded, on by default, and behaviour-neutral:

* `port/src/vulkan_replay.h` — `unresolved_at(site, id, extra)` replaces the draw-path `unresolved_resource`
  returns; it prints the first three occurrences per site (`draw.pipeline`, `draw.render_target`, `draw.depth`,
  `draw.vertex_buffer`, `draw.vertex_zero_page`, `draw.index_buffer`, `draw.index_zero_page`) and then counts
  silently. `PALMETTO_REPLAY_MISS=0` silences it.
* `port/src/vulkan_backend.h` — the first misses of the graphics/compute pipeline maps and of `buffer_at` print
  the id/VA they could not resolve; a list refused in `prepare` prints its index, the `ReplayResult` value and its
  command count; `mark_device_lost()` and a non-`VK_SUCCESS` `vkQueueSubmit` print one line (first three only).

These are what identified the check in section 2 and the TDR in section 4; they are cheap enough to keep for the next
frontier. Remove them if the log is expected to stay byte-comparable.
