# Console device slot 55 is CreateDerivedPipelineStateX, and it is wired

**Status: diagnosed and wired.** Console device slot **55** is the console's **`CreateDerivedPipelineStateX
(base, count, patches, riid, out)`**, a console extension that sits in the console device vtable past the PC one's
`#43`. It is **not** PC slot 55, `ID3D12Device4::CreateReservedResource1(desc, state, clear, session, riid, out)`.
The proxy forwarded console #55 by number until this change, so the PC method read the console's base pipeline as a
resource description and its `riid` as a protected session, refused every call (41,281 `E_INVALIDARG` in one 150 s
boot), and the title - which does not test that HRESULT - kept a null pipeline state for every pipeline it made this
way. `port/shims/d3d12_xs_device.cpp` now answers the console call itself: it remembers the description of every
graphics pipeline this device creates (slot 57), rebuilds it with the patched fixed-function parts and creates the
result with the PC device's own `CreateGraphicsPipelineState`, returning the real HRESULT. The first post-fix boot made
**41,545 slot-55 calls, created 41,545 PC pipeline states (all `S_OK`) and made 0 `CreateReservedResource1` calls**,
ALIVE 120.3 s with no exception and no device removal. The presented frame is still black, as it was before, so this
slot is not by itself the cause of the black frame (section 6).

## 1. What the console slot is, from its call site and the surrounding measurements

The retail executable calls console device slot 55 from **`game+0x247A5C2`**. `PALMETTO_XS_CALLERS=1` in the post-fix
boot (section 4) names the return address and the first four argument words:

```
[rt] d3d12_xs: device slot 55 caller 0x14247a5c8 (args 0x24d1cdd8090 0x1 0x758d5fe5e0 0x1461e06b8)
```

The image is loaded at `0x140000000` (the boot's stdout map line), so the return address is `game+0x247A5C8` - one
six-byte indirect call after the `game+0x247A5C2` call site the display investigation already measured
(docs/display_path.md section 4). The four register words and the fifth argument (the out pointer, on the stack)
read as a five-argument pipeline-state creation:

| argument | measured value | meaning |
|---|---|---|
| 1 | `0x24d1cdd8090` (heap pointer) | the base pipeline state |
| 2 | `0x1` | patch count |
| 3 | `0x758d5fe5e0` | the patch records |
| 4 | `0x1461e06b8` = `game+0x61E06B8` | the IID pointer, **`IID_ID3D12PipelineState`** (the same IID cell docs/display_path.md section 4 measured at slot 86) |
| 5 | on the stack | the out pointer the title reads back as the new pipeline |

That is exactly `CreateDerivedPipelineStateX(base, count, patches, riid, out)`, and it cannot be
`CreateReservedResource1(desc, state, clear, session, riid, out)`: the PC method's session argument is the console's
fourth word, so the title's **`riid` lands in the protected-session position**. The pre-fix boot says so directly: the
D3D12 backend logged `protected resource session refused (not supported) by CreateReservedResource1: 1` and then
`ID3D12Device4::CreateReservedResource1 failed` for every call.

Two independent measurements fix the slot's identity:

* **Its neighbours.** The console devices the tier measured next to it are `#57` (a deserialized graphics pipeline
  record, wired through `graphics_pso`, docs/slot57_wiring.md), `#62` (create compute) and `#67` (deserialize a compute
  record). Those same numbers are the console's pipeline-state block in the public HLE's device table
  (a public runtime project,
  `console_slots`/`device_x`): `#55 CreateDerivedPipelineStateX`, `#56/#66 SerializePipelineStateX`,
  `#57/#67 DeserializePipelineStateX`, `#60` create graphics, `#62` create compute, `#86 CreatePipelineStateX`. The
  tree's own measured #57/#62/#67 already agree with that table, so its #55 is the same table's #55.
* **The patch record's own layout.** The console table gives the patch record as a `u32` part at `+0` and the part's
  payload at `+4`, one `0x14C`-byte record: part 0 a `D3D12_DEPTH_STENCIL_DESC` (`0x34` B), 1 a
  `D3D12_RASTERIZER_DESC` (`0x2C` B), 2 a `D3D12_BLEND_DESC` (`0x148` B), 3 a `UINT` sample mask. The three payload
  sizes are the PC structures' own sizes, which is a second, independent check that the shape is this one.

The boot's patch mix confirms it: over the first 40,000 calls the histogram records **part 0 (depth-stencil) 23,760**
and **part 2 (blend) 32,080** and no other part - 15,840 calls carried both, 24,160 carried one. A resource-creation
call would not carry those parts.

The console device vtable is the PC one only up to `#43`; from `#44` it is the console's own extension table (the
public HLE's `console_extensions(Device) = 44`, and this tree's measured `#127` CreateCommittedResource-with-a-layout
and `#133` CreatePlacedResourceX sit there too, docs/surface_population.md). A console slot number past `#43` is
therefore not a PC method number, and the two tables collide exactly at 55.

## 2. The bug: every derived pipeline was null

Before this change the generic thunk forwarded console `#55` to `self->real_vtable[55]` - PC
`CreateReservedResource1`. The PC method read the console's arguments in unrelated positions, refused the non-null
"session" (the title's `riid`) and returned `E_INVALIDARG`; the title's call site does not test the HRESULT and kept
the null the method left in the out pointer. Measured in a 150 s boot (same tree, pre-fix): **41,281
`ID3D12Device4::CreateReservedResource1 failed` calls, 0 pipeline states created this way**.

The mis-forward is also unsafe, not just useless: in one 120 s A/B boot the pre-fix binary **crashed at 53.6 s** with an
access violation, `write` to address `0xf`. The crash stack's return addresses are
`palmetto+0x2fa954 <- palmetto+0x1616f3 <- game+0x247a5c8` - the slot-55 call site is the caller, `+0x1616f3` is the proxy
thunk's forward frame, and the faulting frame is the PC method the thunk called, which read its out pointer from a
stack word the five-argument console call never wrote. The post-fix binary built from the same tree was ALIVE for the
whole timeout with no exception. (The runtime does not carry symbols; the frame attribution is by the call chain: the
thunk's slot-55 branch calls only `real_vtable[55]`.)

## 3. The mapping

A PC device has no "create a pipeline from another pipeline with some state replaced", so the proxy rebuilds the
description:

1. **Remember what was created.** `convert_slot57_record` (the slot-57 graphics-record conversion,
   docs/slot57_wiring.md) now calls `track_graphics_pipeline_state` on every `S_OK`, which deep-copies the
   `D3D12_GRAPHICS_PIPELINE_STATE_DESC` it built and owns every pointer in it: the stage bytecode, the input-element
   array and its semantic names (the description's pointers otherwise point into title memory). The table is keyed by
   the PC pipeline-state pointer the device handed out, so a base is accepted only when *this device* created it - the
   same proof policy slot 57 applies to the root signatures its records name. A pipeline derived through slot 55 is
   tracked the same way, so chains of derivations work.
2. **Rebuild with the patches.** `proxy_create_derived_pipeline_state` copies the base's description and applies each
   patch record by part: 0 -> `DepthStencilState`, 1 -> `RasterizerState`, 2 -> `BlendState`, 3 -> `SampleMask`. Each
   payload is range-checked before it is read. The description's other fields (root signature, bytecode, input layout,
   attachment stand-in, sample description) stay the base's, i.e. the state slot 57 built and the documented stand-in
   it logs.
3. **Forward honestly.** The rebuilt description, the title's `riid` and the title's out pointer go to the PC device's
   own `CreateGraphicsPipelineState`, and its HRESULT comes back unchanged. A PC failure returns that failure with no
   object. There is no success for a pipeline the device did not create.
4. **Refuse by name what cannot be proved.** A base this device did not create (or a compute base, whose patched parts
   are graphics state) returns `E_NOTIMPL` with a named log and clears the out pointer; a patch part with no measured PC
   field, a patch array that is not readable, a null base and an impossible patch count are refused with the field
   named. On this title none of those refusals occur (section 4).
5. **Measurement.** Slot 55 has its own histogram, printed every 5,000 calls: calls, distinct bases, how many bases are
   this device's graphics pipelines (vs untracked/compute/null), how many calls reached the PC device and how many were
   created, the patch-part histogram and the PC HRESULT histogram. The first call logs what the slot is; the first
   occurrence of each refusal and each HRESULT logs with the base and the patch parts.

`slot_name(55)`, the header comment's list of modelled console extensions and `device_slot_overridden(55)` are updated.
`d3d12_vulkan_device.cpp`, `vulkan_backend.h`, `vulkan_replay.h`, `graphics_record.*` and `graphics_pso.*` are
untouched.

## 4. Measured before and after

Two boots, retail image, Vulkan backend, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, one boot at a time,
harness `tooling/boot_report.py`, 150 s (before) and 120 s (after) - the durations are what the two trees were booted
with; the counts are absolute.

| measurement | before (`boot-rt-readback`, 150 s) | after (`boot-slot55-1`, 120 s) |
|---|---:|---:|
| verdict | ALIVE to the timeout; 12 exceptions, all `host:KERNELBASE.dll+0xc569a` from the presenter's window/UIAutomation host threads, none from the title | **ALIVE 120.3 s, 0 exceptions** |
| D3D12 calls traced | 615,981 | 229,288 |
| console slot 55 calls | 41,281, every one forwarded | **41,545, every one answered by the proxy** |
| `CreateReservedResource1` calls | 41,281, all failed | **0** |
| slot-55 PC pipeline states created (`S_OK`) | 0 | **41,545 of 41,545 (0 refusals, 0 untracked bases)** |
| distinct base pipeline states | - | 6,037 in the first 40,000 calls |
| console slot 57 calls | 7,905 | 8,438 |
| `SetPipelineState` / `DrawInstanced` | 15,696 / 15,696 | 9,367 / 9,367 |
| `ExecuteCommandLists` `S_OK` | 31,393 | 9,368 |
| `Close` failures | 0 | 0 |
| presents (`PresentX` `S_OK`, queue slot 32) | 15,696 | 4,684 |
| replay's first submissions | 0.3 s | 57.7 s |
| replay rate after start (submission 8192 - 4096) | 16.5 s | 16.6 s |

The slot-55 histogram at its last checkpoint (40,000 calls):

```
slot 55 histogram #8 at 40000 call(s), 6037 distinct base(s): base this device created 40000, untracked 0,
  a compute pipeline 0, null 0; reached PC CreateGraphicsPipelineState 40000, created 40000, refused before the call 0
slot 55   patch parts (0 depth-stencil, 1 rasterizer, 2 blend, 3 sample mask): 0 23760, 2 32080
slot 55   PC CreateGraphicsPipelineState results: S_OK 40000
```

No refusal reason and no non-`S_OK` result was ever logged, so the last 1,545 calls are created too.

**The cost is real and measured:** 41,545 PC graphics pipelines on this Vulkan backend delay the title's replay phase
by ~57 s (first replay submissions 0.3 s before, 57.7 s after), because each derivation is a real
`CreateGraphicsPipelineState` and the title makes them all during loading. After the replay starts, its submission rate
is the same as before (16.5 vs 16.6 s per 4,096 submissions). **The draw count per presented frame changes from 1.0 to
2.0** (before 15,696 draws / 15,696 presents; after 9,367 / 4,684, and the same 2:1 in the A/B boot below), so the
derived pipeline states do reach the draw path and one more draw is recorded per presented frame. Follow-up: dedupe
identical (base, patch) derivations to remove the load delay; that needs its own measurement.

## 5. Frame content: black before and after

An A/B pair was run on one frozen tree state, two binaries built from it differing only in the slot-55 dispatch, both
120 s, both with `PALMETTO_CAPTURE=<dir>` (the presenter's own swapchain copies, `PALMETTO_CAPTURE_EVERY=30`):

| | before | after |
|---|---|---|
| boot | crashed at 58.5 s: AV `write` at `0xf`, stack under the slot-55 call site (section 2) | **ALIVE 120.2 s, 0 exceptions** |
| slot-55 calls / created | 41,441 forwarded to `CreateReservedResource1`, all failed | 40,736 answered, **40,736 created `S_OK`** |
| draws / presents | 67 / 68 | 872 / 436 |
| presented frames captured | 3 (`frame_00001/31/61`) | 15 (`frame_00001`..`frame_00421`) |
| mean brightness of every captured frame | **0.0000 / 255** (max channel value 0) | **0.0000 / 255** (max channel value 0) |

Every presented frame is pure black with the slot-55 pipeline states null and with all 40,736 of them real. So
**slot 55 is not, by itself, the cause of the black frame**: giving the title every derived pipeline state it asks for
does not put a pixel on the presented surface. The replay's own draw path is the frontier documented in
docs/replay_resolution.md; the composition/display passes still need their own account.

## 6. Tests and gates

`port/test/xs_proxy_mock_test.cpp` gains `check_slot55_derived_pipeline_state`, which creates a base through the real
slot-57 record branch and then:

* derives with a rasterizer (part 1) and a sample-mask (part 3) patch - the PC device's `CreateGraphicsPipelineState`
  receives the base's description with those parts replaced (rasterizer/blend/sample mask fields as patched, the base's
  depth-stencil/blend/attachments/root signature kept, the record's PS/VS bytecode contents preserved from the proxy's
  own copy), the title's IID and out pointer, and no call reaches the mock device's slot 55;
* derives from the derived pipeline (a blend part 2 patch) and reads the chained state back;
* refuses an untracked base, an unnamed patch part, a null base and returns a PC failure unchanged with no object;
* derives count 0 as an unchanged copy of the base, and checks `device_slot_overridden(55)`.

Gates run for this change:

```
_setup\build_palmetto.cmd   -> PALMETTO_OK
_setup\the test build script    -> TESTS_BUILT
xs_proxy_mock_test        -> PASS (the new slot-55 check included)
unimplemented_policy_test -> 98 checks, 0 failed
graphics_record_test      -> 156 checks passed
graphics_pso_test         -> 170 checks passed
d3d12_fence_timing_test   -> still crashes (known pre-existing failure, untouched)
```

Scope: `port/shims/d3d12_xs_device.cpp`, `port/test/xs_proxy_mock_test.cpp`, this page. No git writes.
Artifacts: `F:\palmetto-work\boot-slot55-1` (the post-fix boot), `F:\palmetto-work\boot-rt-readback` (the pre-fix boot),
`F:\palmetto-work\boot-slot55-ab-before` / `ab-after` and `F:\palmetto-work\cap-slot55-before` / `cap-slot55-after`
(the A/B and its captures).

## 7. Limits

* One machine, one copy of the retail image, one runtime build, one measured boot per configuration.
* The slot identity is measured (call site, argument shape, the IID cell, the patch record layout) and corroborated by
  one public HLE's device table and by this tree's own measured #57/#62/#67; it is not from a console header.
* The console patch record's stride and part numbers are the public HLE's; on this title the only parts the boot
  carries are 0 and 2, whose payload sizes match the PC structures. Parts 4, 5, ... are refused by name, not mapped.
* The retained descriptions are the ones slot 57 builds, i.e. they carry the documented fixed-function stand-in and
  the logged attachment stand-in; a derived pipeline inherits those, which is what makes its patched parts the only
  measured console state in it.
* The derived pipelines' PC objects are created but not retained by the proxy; a pipeline pointer that comes back for a
  released object replaces the stale entry, and a derived pipeline keeps its own storage reference.
* The capture in section 5 is the presenter's swapchain copy, not a window-compositor screenshot; the leader's window
  bitmap measured the same black frame.
