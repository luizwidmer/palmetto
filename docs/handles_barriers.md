# The two console recording calls that invalidated the frame list: measured and converted

**Status: measured on one machine, 2026-11-24**, against the retail 1.0.2.1 image with the shader corpus and the Vulkan
backend. Nothing here quotes image bytes, package paths, shader bytes or extracted tables: call counts, close codes,
RVAs, descriptor handles and the project's own wording only.

Predecessor: [list_lifecycle.md](list_lifecycle.md) section 5 (the two calls bisected out with `PALMETTO_XS_LIST_MUTE`
and the measurement the next step needed). This page records the arguments of both calls, the conversion each one got
in `port/shims/d3d12_xs_device.cpp`, what is refused instead, and the boot outcome with both converted and no mute.

## 1. What the two calls did to a reopened list

After the lifecycle emulation ([list_lifecycle.md](list_lifecycle.md)) a frame's recording is real: the proxy resets a
closed PC list against its creating allocator before the first recording call that reaches it. Two forwarded calls
still poisoned it:

* **console list slot 52 -> PC slot 48 (`ClearRenderTargetView`)** and
* **console list slot 146 -> PC slot 80 (`ID3D12GraphicsCommandList7::Barrier`)**.

With both muted the 60 s boot showed 57,597 `Close` `S_OK`, 0 failed, 56,915 reopens and 56,983 submits; unmuted it
showed 625 `S_OK` and 104,340 failed. The task was to turn the mutes into real conversions: the clear's CPU descriptor
handle was suspected to need the console-address mapping, and the barrier's resource-less texture group to need an
honest PC encoding or a refusal.

## 2. How the arguments were measured

One environment-gated probe in the proxy, off by default (`PALMETTO_XS_CLEAR_BARRIER_PROBE=1`), prints in one boot,
capped:

* for console slot 52: the four arguments (`handle`, colour pointer, `NumRects`, `pRects`), the colour values, and the
  24-byte target-view capture the Vulkan layer stores at the handle (`resource`, `mip`, `first layer`, `layer count`,
  view format);
* whether that handle was the destination of a recent `CreateRenderTargetView`, and the resource that create was made
  from (a ring of the last 8,192 destinations, recorded unconditionally - the clear conversion uses it);
* for console slot 146: the number of groups and, per group, its type, its barrier count and every entry's raw words;
* for device slot 20 (`CreateRenderTargetView`): the destination handle and the capture written there;
* for the console descriptor heaps: the requested console address and size and the CPU/GPU handles the layer returned.

"Raw words" for a barrier entry: the PC shape is 16 bytes for a global barrier, 32 for a buffer one and 64 for a
texture one, and that many are printed, so the dump assumes nothing about the console's record layout.

## 3. The clear (console slot 52): the handle is right, the view is null

Measured, first frame after the reopen (probe boot, 60 s, ALIVE):

```
slot 20 create handle 0x1dfa6ce5050 resource 0x1e017840480 capture r=0x1e017840480 mip=0 layer0=0 layers=1 format=0
...
slot 52 clear args handle=0x1dfa6ce5010 colour=... count=0x0 rects=0x0
slot 52 clear handle 0x1dfa6ce5010 capture r=0x0 mip=0 layer0=0 layers=0 format=0 created=1 created_with=0x0
```

The frame alternates two clears, on handles `0x...010` and `0x...030` (stride 32 bytes = the descriptor increment the
device reports). Both were created by the proxy's own `CreateRenderTargetView` **with `pResource = 0`** during setup -
a null render-target view - so the capture at each handle is empty: resource 0, layer count 0. The PC layer's
`clear_render_target` returns false for a view without a resource and marks the list invalid; that is the
`E_INVALIDARG` at the next `Close` of every reopened frame list.

**The handle convention is not the disagreement.** The handle the clear passes is exactly the destination the proxy's
`CreateRenderTargetView` wrote, so no console-address -> PC-handle mapping applies to it. The console-address machinery
of `create_console_descriptor_heap`/`console_heap_back` was still checked: the first (shader-visible) heap measured a
64 KiB-aligned console address (`0x1dff53a0000`, 1,245,184 descriptors, 0x2600000 bytes) that the layer backed in
place and handed back unchanged (`address == CPU handle == GPU handle`), while the render-target heap the clear uses
is a plain PC-sized descriptor heap whose CPU handles are the layer's own host storage. Either way the create and the
clear see the same handle.

**Converted** (`translate_console_clear`):

| measured capture at the clear's handle | answer |
|---|---|
| a real view (resource != 0, layer count != 0) | forwarded unchanged with the same CPU handle |
| empty, and the ring says this handle was created from `pResource = 0` | refused as the no-op it is: the console clears a null view and this clears nothing, PC D3D12 would invalidate the list. One-time log with the handle and the slot |
| empty, and the ring says it was created from a real resource (emptied later) | refused, one-time log naming the handle and the resource it was created from |
| empty, and no create ever wrote that handle | refused, one-time log naming the handle and the colour pointer |
| unreadable handle | refused, one-time log |

A refused clear is not an error the title can see (the method is `void`); the list keeps recording, so the frame's
`Close` is kept. The one-time log line:

```
[rt] d3d12_xs: console ClearRenderTargetView (list slot 52) on a null view: handle 0x... was created by
CreateRenderTargetView with pResource=0, so clearing it clears nothing; PC D3D12 refuses it. Refused as the no-op it
is (the list stays valid and its Close is kept)
```

## 4. The barrier (console slot 146): resource-less texture barriers become global barriers

Measured groups (probe boot, the first three calls of the segment):

| call | group type | barriers | entry (words in PC field order) |
|---|---:|---:|---|
| 1 | 1 (`TEXTURE`) | 1 | `SyncBefore 0x1000` (ALL_SHADING), `SyncAfter 0x40` (RENDER_TARGET), `AccessBefore 0x10` (UNORDERED_ACCESS), `AccessAfter 0x8` (RENDER_TARGET), `LayoutBefore 3` (UNORDERED_ACCESS), `LayoutAfter 2` (RENDER_TARGET), `pResource 0`, `IndexOrFirstMipLevel 0xffffffff`, all other range fields 0, flags 0 |
| 2 | 1 (`TEXTURE`) | 1 | the same shape, measured on the frame's second list |
| 2 | 1 (`TEXTURE`) | 2 | entry 0: `0x1000 -> 0x40`, `0x80 (SHADER_RESOURCE) -> 0x8`, layout `6 (SHADER_RESOURCE) -> 2 (RENDER_TARGET)`, resource `0x1dfa7088200`, all subresources; entry 1: `0x1000 -> 0x20 (DEPTH_STENCIL)`, `0x80 -> 0x20`, layout `6 -> 4 (DEPTH_STENCIL_WRITE)`, resource `0x1dfa7088420`, all subresources |

So the console group is a PC-shaped `D3D12_BARRIER_GROUP` of type TEXTURE and each entry a 64-byte PC
`D3D12_TEXTURE_BARRIER`. The resource-less entries are the console's "every texture"; PC D3D12 refuses a texture
barrier without a resource and marks the list invalid (the `check(false)` in the layer's `Barrier`). The entries with a
real resource are resource object addresses this layer already knows (they are the ones device slot 20 was handed as
`CreateRenderTargetView` resources), and they are valid PC texture barriers.

Correction to [render_loop.md](render_loop.md) section 7: it labels `LayoutBefore 3` as RENDER_TARGET and
`LayoutAfter 2` as GENERIC_READ. In the PC/layer enum 3 is UNORDERED_ACCESS and 2 is RENDER_TARGET; the page's
sync/access labels and its other numbers are right.

**Converted** (`translate_console_barrier_groups`, run inside the list proxy before forwarding):

* a TEXTURE entry with `pResource == 0` becomes a `D3D12_GLOBAL_BARRIER` carrying the entry's
  `SyncBefore`/`SyncAfter`/`AccessBefore`/`AccessAfter`. A global barrier orders exactly those stages and accesses for
  every resource, a superset of the console's "every texture"; the layout numbers have no PC encoding without a
  resource, are dropped, and are named in a one-time log (first four shapes);
* a TEXTURE entry with a real resource is passed through unchanged as a texture barrier;
* the translated entries are re-grouped by type (one GLOBAL group and one TEXTURE group at most) and forwarded in one
  `Barrier(group count, groups)` call; the buffers are `thread_local` because the title records on several threads;
* a call with no resource-less texture entry is forwarded untouched, with the console's own pointers and count.

Example log line (first call):

```
[rt] d3d12_xs: console barrier (list slot 146): texture barrier without a resource (sync 0x1000->0x40, access
0x10->0x8, layout 3->2, subresource 0xffffffff) translated to a global barrier; the console's layouts have no PC
encoding without a resource and are dropped
```

The second measured resource-less shape (`0x40 -> 0x0`, access `0x8 -> 0x80000000`, layout `2 -> 31`, all
subresources) is translated the same way; layout 31 is not one of the PC layouts this tree names, which is another
reason the layout is not guessed.

## 5. What is refused, and why

The rule is the project's: translate what is provable, refuse what is not, never forward a call the PC layer would
record as invalid. A refused call answers 0 (both methods are `void`), and the list keeps recording.

| call | refused when | one-time log names |
|---|---|---|
| barrier 146 | the group list is null or unreadable | slot and group pointer |
| barrier 146 | the call asks for more than 64 groups (the measured console call passes one) | slot and group count |
| barrier 146 | a group type is not one of the three PC types | group index and type |
| barrier 146 | a texture- or buffer-barrier entry array is unreadable, or one call carries more than the translation buffers (32 global, 32 texture, 32 buffer, 3 groups) | group index and count |
| barrier 146 | a BUFFER barrier has no resource - a shape not measured for this title, so not translated by analogy | the barrier's sync/access/offset/size |
| clear 52 | the handle or its 24-byte capture is unreadable | slot and handle |
| clear 52 | the capture is empty and the handle was not made by this proxy's `CreateRenderTargetView`, or was made from a real resource and emptied later | slot, handle and the resource it was created from |

A barrier group with a resource-carrying entry that the layer itself later refuses (for example a subresource range
the resource does not have) is a PC-side judgement, not a console-shape one: the proxy forwards it as the console
recorded it.

## 6. Boot outcome

Boot environment as before: retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time, 60 s timeout, a boot runs to the timeout. All three runs are
the same binary; the third is the same-build control with `PALMETTO_XS_LIST_MUTE=146,52`, which restores the old
"recording real, both calls dropped" behaviour for comparison.

| measurement (60 s) | lifecycle only, unmuted (before this page) | both converted, no mutes | same-build control, both muted |
|---|---:|---:|---:|
| verdict | ALIVE, no exception | ALIVE 60.2 s, no exception | ALIVE 60.1 s, no exception |
| traced D3D12 calls | - | 494,521 | 746,323 |
| `Close` `S_OK` | 625 | **30,999** | 55,271 |
| `Close` failed | 104,340 | **0** | 0 |
| list `Reset` (reopens) | 4 | 30,376 | 54,647 |
| `ExecuteCommandLists` ok (submits) | - | 30,376 | 54,647 |
| console `PresentX` (queue slot 32) | - | 15,188 | 27,324 |
| `SetPipelineState` | 0 | 0 | 0 |
| Draw / DrawIndexedInstanced | 0 | 0 | 0 |
| barrier records (console slot 146) | - | 75,937 | 0 (the call is muted) |

What this says:

* the conversion works: with both calls converted and no mute, every frame's recording is valid - one reopen per
  frame and **zero failed `Close`** for the whole 60 s, against 104,340 failed before this change;
* the frames now really carry their barriers (~2.5 per frame) and their clear is the no-op it should be, so the loop
  submits fewer frames per second than the muted control (30,376 against 54,647). That is the cost of the recording
  no longer being discarded, not a dropped submission: every recorded frame is submitted and every `Close` succeeds;
* nothing is drawn yet: `SetPipelineState` and the draw calls are still 0.

## 7. Mock coverage (`port/test/xs_proxy_mock_test.cpp`)

The GPU-free mock device now writes the layer's target-view capture into the destination of a forwarded
`CreateRenderTargetView` (a null resource writes an empty capture, a real one resource + one layer), and the mock real
list at GCL7 records the groups a forwarded `Barrier` is given. `check_console_command_list` asserts:

| assertion | proves |
|---|---|
| a console texture group with one null-resource entry and one real-resource entry reaches the mock `Barrier` as two groups: a GLOBAL one with the same sync/access words, then a TEXTURE one with the real resource and its own words | the resource-less entry is translated and the real one is not |
| a group whose entries all carry a resource reaches the mock with the console's own group pointer and count | untouched calls keep their arguments |
| a group of type 7, and a group pointer at `0x10`, are not forwarded (the mock's slot-80 count does not move) | refusals cannot invalidate the list |
| a clear of a slot created from `pResource = 0` is not forwarded; the null capture is what the mock wrote | the measured null-view no-op |
| a clear of a slot created from a real resource is forwarded with the same CPU handle and colour pointer | real clears still record |
| a clear of an unknown handle, and of a handle created from a real resource whose capture was emptied, are not forwarded | the other two refusal reasons |
| `static_assert`s tie the proxy's five-word capture mirror to `palmetto::TargetView` in `port/src/command_list.h` | the mirror cannot drift |

`xs_proxy_mock_test` passes (`PASS (9 allocations refused on purpose)`); `crash_unwind_test` (54 checks),
`graphics_record_test` (101) and `unimplemented_policy_test` (98) still pass; `d3d12_fence_timing_test` remains the
known pre-existing failure (0xC0000005). Gates: `the local build script` -> `PALMETTO_OK`, `the local test build script` -> `TESTS_BUILT`.

## 8. Next blocker

The frames are now recorded, submitted and read their barriers, and the first frame is still a clear-and-present (or
clear-only) frame: the renderer binds render targets, heaps, root signatures and descriptor tables and clears a null
render-target view, but it probes no pipeline state and issues no draw - `SetPipelineState` and the draw calls are
still 0 in the whole boot, and the target it clears is a null view the title itself created. `PresentX` (queue slot 32)
is still refused ([xbox_present.h](../port/shims/xbox_present.h) is the prepared presenter). The next measurement
is therefore the renderer's pipeline/draw path and the resource binding that would make the null view a real
back buffer, not another recording call.

## 9. Files

* `port/shims/d3d12_xs_device.cpp`: `translate_console_barrier_groups` and `translate_console_clear` (both run
  inside `list_slot`); the destination ring filled by device slot 20; the probe switch
  `PALMETTO_XS_CLEAR_BARRIER_PROBE` (off by default).
* `port/test/xs_proxy_mock_test.cpp`: the mock capture writer, the barrier-group recording, the assertions above.
* This page.
