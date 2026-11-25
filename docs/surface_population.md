# The display surfaces: console device slot 127 was the null

**Status: diagnosed and fixed at the measured source.** The two surfaces the present names, and two of the frame's
fifteen render-target surfaces, are created through **console device slot 127**, a console extension past the PC
device's vtable. The proxy used to answer that slot from the generic beyond-vtable stub: `S_OK` and a zero result,
**without writing the out pointer**. The title checks the HRESULT, saw success, and kept the surface whose resource
field the proxy had left null - which is what `docs/display_path.md` measured the title writing into its own present
state. Slot 127 is now mapped to a real PC `CreateCommittedResource`; both display surfaces carry real PC resources,
the present plane's first resource is non-null, and the frame's clears now reach the PC command list. Nothing was
displayed at this measurement because the present had no presenter yet; the presenter and window were wired next
(`docs/present_wiring.md`, `docs/window.md`), so the frames now present into the title's own window - and they are black
for the frame-content reasons in `docs/frame_capture.md` (see section 7).

## 1. The measurement that separated the populations

The exact next measurement from `docs/display_path.md` section 3, run as one 150 s boot with
`PALMETTO_XS_CALLERS=1` and `PALMETTO_POKE_LOG=193E1BA,193E2DB,CC29D8,CD74D7` (a second 150 s boot repeated the
pokes `193E1BA,193E2DB,CC29D8` after the fix; section 6). Verdict both times: **ALIVE 150.2 s, no exception**.

`PALMETTO_XS_CALLERS` named the caller of device slot 127 and its first four argument words:

| device slot | caller (return address) | first argument words |
|---|---|---|
| 127 | `game+0x193E1BA` | `0x……030`, `0x8`, `0x……050`, `0x3` |
| 133 | `game+0x193E2DB` | a title VA, a description, `0x6`, an info block |
| 133 | `game+0x1E86853` | a title VA, a description, `0xFFFFFFFF`, `0` |
| 133 | `game+0x6DD8B` | a title VA, a description, `0xFFFFFFFF`, `0` |

The return address `game+0x193E1BA` is the instruction after the indirect call at `game+0x193E1B4`, inside the
title's resource-creation routine `game+0x193DE10` - the same routine that calls console slot 133 through
`[device + 0x428]` at `game+0x193E2D4`. Slot 127 is reached through `[device + 0x3F8]` at `game+0x193E1B4`, and the
out pointer it is handed is the **surface object's `+0x20`** (the same field `docs/display_path.md` measured as the
null present resource and the null render-target `pResource`).

Two `PALMETTO_POKE_LOG` probes read the two ends of that call over the whole run:

* **`game+0x193E1BA` (slot 127 return) - 2 hits in 150 s.** `RAX = 0` (`S_OK`) on both, and the memory at the out
  pointer (`surface+0x20`) was **zero**: the stub's answer was success, its "creation" wrote nothing.
* **`game+0xCC29D8` (the render-target surface load, before `CreateRenderTargetView`) - 15 hits**, one per
  render-target surface of the frame, with the surface pointer in `RAX`.
* **`game+0xCD74D7` (the present-to-display load) - 34,579 hits** over the run, **2 distinct surface pointers**.

Cross-referencing the pointers inside that one boot (heap addresses are only comparable within a run):

* the 2 objects slot 127 was asked to create **are** the 2 surfaces the present path names, and **are** 2 of the
  15 render-target surfaces;
* the other 13 render-target surfaces are among the 30 objects created through slot **133**
  (`CreatePlacedResourceX`, which the proxy already models) and they carry **real** resources - the reason the
  resource creation "translates correctly for a different population" in `docs/display_path.md` section 3;
* the present's plane 0 resource at calls 1, 2, 1,000 and 10,000 was null in this boot, matching the poked out cell.

So the null-carrying population is exactly the surfaces whose resource creation went through slot 127. Candidate (a)
in the task description is the measured one, with one correction: the title does not ignore an HRESULT - **the shim
answered `S_OK` for a creation it did not perform**. The title's own failure paths were never entered: the boot's
`int3_skipped` events are the same four addresses as every baseline boot, and neither the constructor's assert at
`game+0x32522D` nor the resource routine's assert at `game+0x193E00F` fires.

## 2. What slot 127 is

The call site and the handler's own one-time log give its shape, and it is an Xbox resource-creation extension:

| position | value measured | meaning |
|---|---|---|
| 1 | a heap-properties pointer | `D3D12_HEAP_PROPERTIES` |
| 2 | `0x8` | console heap flag `D3D12_HEAP_FLAG_ALLOW_DISPLAY` |
| 3 | a description pointer | a 56-byte `D3D12_RESOURCE_DESC` with console words above it |
| 4 | `0x3` | a `D3D12_BARRIER_LAYOUT` in the initial state's place (`UNORDERED_ACCESS`) |
| 5 | a clear-value pointer | `D3D12_CLEAR_VALUE` |
| 6, 7, 8 | `0`, `0`, `0` | protected session, castable-format count, castable formats |
| 9 | `IID_ID3D12Resource` (`game+0x63FA690`) | the interface the title asks for |
| 10 | `surface+0x20` | the out pointer the title reads back as the surface's resource |

The handler's one-time log on the fixed boot names the description it was handed: a **2D texture, 3840 wide,
format 28 (`R8G8B8A8_UNORM`), console resource flags `0x300005`** (rendering + unordered-access bits below the console
ones), **xbox layout word 0**, **barrier layout 3**, **heap flags 0x8**. That is a display target: 3840 wide, in a
display heap, created in the unordered-access layout.

Independent corroboration of the slot's identity: XCL (a public Xbox compatibility layer) numbers console device slot
**127** as `CreateCommittedResource` with a barrier layout in the initial state's place, with exactly these ten
arguments (heap properties, flags, desc, layout, clear, protected session, castable format count, castable formats,
riid, out), and numbers **133** as `CreatePlacedResourceX` with a barrier layout - which is the shape this proxy
already handled. Source: a public runtime project
(the console slot table and `dev_create_committed_resource_layout_x`). The mapping was not copied from public code;
it is the measured call shape first, and the public source names it.

## 3. The fix

`port/shims/d3d12_xs_device.cpp`:

* new `proxy_create_committed_resource_layout()` handles console slot 127 (the ten arguments above), and the dispatch
  table routes `Slot == 127` to it before the beyond-vtable refusal. It is no longer reachable from
  `refuse_beyond_device`; slot 81 and 86 stay stubbed as before.
* The description is translated the way slots 76 and 133 translate theirs: the PC layout word is set from the
  dimension (ROW_MAJOR for buffers, UNKNOWN otherwise), console resource flag bits are masked to the PC's (`0x5ff`),
  and a console placement alignment that is not one of the PC's two values is dropped to 0.
* The barrier layout is mapped to the PC resource state it names (1 generic read, 2 render target, 3 unordered
  access, 4 depth write, 5 depth read, 6 all shader resource, 7 copy source, 8 copy dest, 9 resolve source, 10
  resolve dest; 0 and `0xFFFFFFFF` are COMMON), and the heap flags are passed through.
* Honest failure: the out pointer is cleared first and the HRESULT is the host's. If the host refuses the mapped
  state, the call is retried once as `COMMON`; if it refuses the console heap flags, it is retried once with no heap
  flags; both retries are logged with both HRESULTs. A failed creation now returns a failure the title can act on,
  instead of a success over a null out pointer. The title's own failure path then returns no surface at all, which is
  the honest outcome and visible in the log.
* `slot_name(127)`, the `kMaxSlots`/`refuse_beyond_device` comments and `device_slot_overridden(127)` are updated.
* `port/test/xs_proxy_mock_test.cpp` gains a slot-127 contract test: the console call reaches the host's
  `CreateCommittedResource` with the translated description (2D, layout UNKNOWN, flags `0x5`, alignment 0), the state
  the barrier layout names (unordered access) and the console heap flags; the host's out pointer reaches the title's
  pointer. The mock's slot 27 (`CreateCommittedResource`) records what it was handed and writes a resource out, as
  the PC device does.

## 4. Measured before and after

Both runs: retail image, Vulkan backend, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, 150 s timeout.
The runs differ in instrumentation (the before run also poked the per-present load and logged callers), so the call
counts are not a performance claim; they show the frame loop stayed healthy in both.

| measurement | before (null) | after (fix) |
|---|---|---|
| boot | ALIVE 150.2 s, 0 exceptions | ALIVE 150.2 s, 0 exceptions |
| traced D3D12 calls | 1,225,251 | 1,320,681 |
| present plane 0 resource at calls 1 / 2 / 1,000 / 10,000 | null / null / null / null | non-null / non-null / non-null / non-null (two resources, one per back buffer) |
| slot-127 creations | 2, `S_OK`, out cell **null** | 2, host creation succeeded, out cell **non-null** (the resource the present names on call 1) |
| `CreateCommittedResource` `S_OK` | 5 | 7 (+2, the display targets) |
| render-target views created | 15 console calls / 15 PC calls | 15 / 15 |
| render-target views with a null resource | 2 (the two display surfaces) | 0 |
| console `ClearRenderTargetView` | 2 refusals (null view, clears nothing) | 36,306 cleared on the PC list - every frame's clears execute |
| `ExecuteCommandLists` ok / failed | 34,573 / 34,576 | 36,308 / 36,304 |
| `DrawInstanced` / `SetPipelineState` | 34,577 / 34,577 | 36,304 / 36,304 |
| `Close` ok | 69,767 | 73,236 |
| present calls (`PresentX`, queue slot 32) | 34,578, answered `unsupported` | 36,306, answered `unsupported` |
| title asserts (`int3_skipped`) | 4 baseline addresses | the same 4, no new ones |

The submit failure count is the replay layer's unresolved-resource frontier (a draw, not a surface), unchanged by this
fix and owned by `docs/replay_resolution.md`.

## 5. Why the field was zero, in one paragraph

`game+0x193DE10` builds a resource for a surface and hands the creation either to slot 133 (the placed path the proxy
models) or, for an object whose byte at `+0x44` has bit 4 set, to slot 127 with the surface's `+0x20` as the out
pointer. The proxy's beyond-vtable stub answered `S_OK`/0 and wrote nothing. The routine tests the returned HRESULT
(`test esi, esi` / `setns al` at `game+0x193E1C3`-`0x193E1C5`), saw success, returned the surface, and the title stored
it in its own present state and render-target record. The null was written by the title from its own surface record -
as `docs/display_path.md` measured - because the proxy's success answer left it there.

## 6. Boots behind this page

| run | instrumentation | result |
|---|---|---|
| population split, 150 s | `PALMETTO_XS_CALLERS=1`, `PALMETTO_POKE_LOG=193E1BA,193E2DB,CC29D8,CD74D7` | ALIVE, no exception; slot 127 = 2 creations, all `S_OK`, out null; present surfaces = those 2; 13 of 15 render-target surfaces from slot 133 with real resources |
| after the fix, 150 s | `PALMETTO_POKE_LOG=193E1BA,193E2DB,CC29D8` | ALIVE, no exception; slot 127 creations 2, out non-null; present plane non-null at calls 1/2/1,000/10,000; 0 null render-target views; 36,306 clears execute |

Artifacts: `boot-surface-null1` (sections 1-2), `boot-surface-fixed1` (sections 4-6).
Gates: `the local build script` -> `PALMETTO_OK`; `the local test build script` -> `TESTS_BUILT`; `xs_proxy_mock_test` -> `PASS`;
`d3d12_fence_timing_test` is the pre-existing failure and still fails.

## 7. What this does not do

* **Nothing was visible at this measurement.** At this boot's tree the queue's `PresentX` (slot 32) was still answered
  `unsupported` and no window presenter was registered, so the frame that now had display resources had nowhere to
  present them. The presentation core (`port/shims/xbox_present.*`, the window presenter in
  `port/shims/d3d12_com.h`) was wired next (`docs/present_wiring.md`): its `PresentX` refuses a plane whose first
  resource is null, and that refusal no longer applies because the plane names a real resource. The presented frames
  are black for the frame-content reasons in `docs/frame_capture.md`.
* The submit failures (`ExecuteCommandLists` unresolved resource) were the replay layer's frontier at this measurement,
  not this one; the page-VA rebase later resolved them (`docs/page_address_space.md`).
* The 3840-wide display targets are created as ordinary committed textures of their own; the console made them from
  the title's display memory. Nothing in this title's remaining path reads that memory back, so no import is needed
  for visibility, but a future console-memory-fidelity step would have to provide it.

## 8. Limits

* One machine, one copy of the title, one runtime build, one measured boot per configuration.
* The slot-127 identity is measured (call site, argument shape, description contents) and corroborated by one public
  source; it is not from a console header.
* The population cross-reference is by pointer identity inside one boot, which is exact for that boot and cannot be
  compared across boots.
