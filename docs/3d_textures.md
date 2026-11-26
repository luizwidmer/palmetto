# Console depth-stencil textures with unordered access: the stop after the arena fix

**Status: measured, attributed and fixed in our structure; the boot then advances to a new wall.** The stop at
`game+0x146F23D` (read of `0x30`, [deferred_compile.md](deferred_compile.md) section 9.7) is the title's resource
factory returning a wrapper whose GPU-resource field is null, because three of the loading stage's placed-texture
creations were refused by our PC validation. The three descriptions are **2D depth-stencil textures that also allow
unordered access** - a pair the console allows and the host PC runtime refuses - and this slice translates that pair on
the console's placed-creation path. With the fix the `0x146F23D` stop is gone and the boot reaches a new title-side
null at `game+0x5EBEA38`. Section 2 corrects the previous page's reading of the two log lines: `dimension 3` in the
slot-25 line is `D3D12_RESOURCE_DIMENSION_TEXTURE2D` (the enum is 0-based, so 3 is not "3D"), and `format 118` is a
different, older refusal of an allocation query, not the format of the refused creations. Section 10 takes the two
walls after this one: the `0x5EBEA38` null is attributed and fixed, the boot requests and enters stage 3, and the new
stop is the title's own hang watchdog at `game+0x2215C35`.

All numbers are one machine (NVIDIA RTX 4090, Vulkan 1.3), one boot at a time under the repository's boot lock, harness
`tooling/boot_report.py`. Logs and captures are outside the repository under the local work folder
(`D:\leonida-work\threedee`). No game data is in this page: RVAs, field offsets, format and flag numbers, counts and
outcomes only.

## 1. Why this slice

[deferred_compile.md](deferred_compile.md) section 9 measured the arena default to 1024 MiB and the boot then moved
from the buffer factory's null (`game+0x6EB759`) to a new one at `game+0x146F23D`, a read of `0x30` on the title's
task-pool path. Section 9.7 recorded the correlation this page starts from: the three D3D12 calls immediately before
the fault are three failed `Xbox CreatePlacedResourceX (device slot 133)` creations, and the caller of the faulting
function passes a registry entry's object pointer that was never filled.

That correlation is now a measured chain:

1. the loading stage's texture-set builder `game+0xF7D7C0` creates three textures through the engine texture factory
   (`game+0x5204E60 -> 0x34BEB0`) and registers the returned wrappers in three per-set vectors at
   `image+0x849C0E0` (entry stride `0x10`, per-set sizes/records at `image+0x849CE60`, stride `0x340`);
2. each creation goes through `game+0x34BEB0 -> 0x164DB0` (engine record) and `game+0x193DE10` (the GPU-resource
   finalize), which builds a `D3D12_RESOURCE_DESC` with `game+0xC4B030` and calls console device slot 133 with it;
3. our layer refuses those three descriptions, so the finalize returns failure, the factory returns 0, and the title's
   own assert on that failure is the `int3` at `game+0x14A63xx` that `PALMETTO_SKIP_INT3=1` steps over;
4. the wrapper is still returned - non-null - with its `+8` field (the GPU resource) left as its constructor set it,
   zero (`game+0x14A6290` writes `[obj+8] = 0`; `game+0x14A63A0` fills it only on success);
5. the builder registers the wrapper, then transitions it through `game+0x146F230`, which reads
   `[[obj+8]+0x30]`; with `[obj+8] == 0` that is the read of `0x30` the previous page recorded.

The three refused descriptions, measured by the layer's own bounded record of the slot-133 texture path (a diagnostic
this slice added; see section 4):

| # | width x height | depth/array | mips | samples | format | flags | HRESULT |
|---|---|---:|---:|---:|---|---|---|
| 1 | 768 x 768 | 6 | 1 | 1 | R16_TYPELESS (53) | 0x6 | `0x80070057` `E_INVALIDARG` |
| 2 | 2048 x 2048 | 1 | 1 | 1 | R16_TYPELESS (53) | 0x6 | `0x80070057` |
| 3 | 1024 x 1024 | 3 | 1 | 1 | R16_TYPELESS (53) | 0x6 | `0x80070057` |

All three are `D3D12_RESOURCE_DIMENSION_TEXTURE2D`, one mip, one sample, `D3D12_TEXTURE_LAYOUT_UNKNOWN`, flags
`0x6` = `ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS`. They are the shadow/depth arrays of the renderer the loading
stage initialises: a 768x768 six-slice volume of one, a single 2048x2048 one, and a 1024x1024 three-slice one.

## 2. The measured description decode

### 2.1 The engine description behind the D3D12 one

`game+0xF7D7C0` builds three 14-byte engine texture descriptions with `game+0x501F4A0` (width word at `+0`, height
word at `+2`, depth/array word at `+4`, dimension byte at `+6`, format byte at `+7`, layout byte at `+8`, mip byte at
`+0xA`) and calls the factory for each. The measured fields:

| engine field | measured value | meaning |
|---|---|---|
| `+6` dimension | 1 | 2D |
| `+7` format | 0x37 = 55 | `D16_UNORM` |
| `+4` depth/array | 6, 1, 3 | the array layers |
| `+0xA` mips | 1 | one mip |
| `+8` layout | 0xFF | the engine default |
| flags/class | `ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS` | from the renderer's class word |

The finalize's description builder `game+0xC4B030` copies the format byte straight into the
`D3D12_RESOURCE_DESC.Format` field (`+0x20`), with one rewrite: when the engine class word names a depth resource, the
three depth format bytes become their typeless equivalents (`0x37 -> 0x35` = `R16_TYPELESS` 53, `0x14 -> 0x13`,
`0x28 -> 0x27`). That is where format 53 comes from: the console title describes a 16-bit depth texture, and the PC-side
description the console layer receives is the typeless form the depth views are made from. The resource flags come from
the class word's bits 5..7 and are `0x6`, so the description is a depth-stencil resource that also allows unordered
access.

This pair is **console-valid**: the console lets a depth buffer also be read and written through typed UAVs, and the
title uses exactly that. The host PC D3D12 runtime refuses it - our `committed_resource_refusal` mirrors the host with
the rule `(render_target && depth_stencil) || (depth_stencil && unordered_access) || (deny_shader_resource &&
!depth_stencil)` (the "resource flag combination" reason) - so a faithful host check returns `E_INVALIDARG`, the
factory returns null, and the title dereferences it.

### 2.2 Correcting the previous page's log reading

The lines [deferred_compile.md](deferred_compile.md) section 9.7 quoted are real, but neither is the refused
creation's description:

| log line | decode |
|---|---|
| `slot 25 refused description 0/1: dimension 3 format 118 flags 0x800002 layout 0x0 1024x1024x1 mips 1 samples 1` | `dimension 3` is `D3D12_RESOURCE_DIMENSION_TEXTURE2D` (UNKNOWN=0, BUFFER=1, TEXTURE1D=2, TEXTURE2D=3, TEXTURE3D=4), so this is a **2D** texture, not a 3D one. `0x800002` is the console flag word with `ALLOW_DEPTH_STENCIL` (0x2). Format 118 is a console-only `DXGI_FORMAT` number: the PC headers have a gap between `B4G4R4A4_UNORM` (115) and `P208` (130), and the console fills it with its 16-bit depth-stencil family (`116`/`117` the packed 7e3/6e4 float formats, `118` `D16_UNORM_S8_UINT`, `119` `R16_UNORM_X8_TYPELESS`, `120` `X16_TYPELESS_G8_UINT`). Our tables have no entry for 118, so the **allocation query** is refused with the invalid answer. |
| `CreateCommittedResource refused (resource flag combination), dimension 3` | this is the refused slot-133 texture creation. `dimension 3` is again `TEXTURE2D`. The reason is the `ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS` pair above, not a format problem: the flag-combination rule runs before the format rules. The line is printed once per reason and dimension, so the three identical failures share it. |

The format-118 queries are a separate path's allocations (three queries at different trace points; two 2560x1440, one
1024x1024), and they did **not** fail a creation in the measured boot - only the allocation answer was invalid. They
are recorded in section 6 as the next named gap, not fixed here.

### 2.3 The registry entry and the fault

The faulting function `game+0x146F230` operates on a linked list whose head is at `[param+0x30]`. At the fault
`param` (`rcx`) is 0 and the fault address is `0x30`, which is `[0+0x30]`. The caller `game+0xF7D7C0` obtains `param`
as `[wrapper+8]` - the GPU resource of the wrapper it registered - so the null is the failed slot-133 creation's
result, exactly as section 1's chain says. The table at `image+0x849C0E0` holds the wrappers, not the GPU resource
objects; its `+8` entry points at the object whose `+0x30` is read.

## 3. What we implemented

- **`port/src/console_texture.h`** (new, GPU-free): the DXGI format-family helpers moved out of the device .cpp
  (`format_family`, `depth_stencil_family`, unchanged behaviour) and one new translation,
  `palmetto::console_depth_uav_translated(D3D12_RESOURCE_DESC&)`. It returns true only for a non-buffer texture whose
  flags contain **both** `ALLOW_DEPTH_STENCIL` and `ALLOW_UNORDERED_ACCESS` and whose format is in a depth-stencil
  family, and it clears the unordered-access flag. Every other description is left untouched, so the device's existing
  named refusals still fire for genuinely invalid combinations (a colour texture with depth-stencil, a buffer with
  either flag, and so on).
- **`port/shims/d3d12_vulkan_device.cpp`**: the console's placed-texture path (`create_at_host_address`, device slot
  133's handler) applies that translation before `create_committed_validated`, so the resource is created as the
  depth-stencil texture the title renders into. It logs once, by name:
  `a console depth-stencil texture that also allows unordered access is created as its depth-stencil resource;
  unordered-access views of it stay refused (the layer cannot make a storage view of a depth format); flags 6`.
  The PC methods themselves (`CreateCommittedResource`, `CreatePlacedResource`, ...) are unchanged and still mirror
  the host: this is the console translation point, not a change to PC-runtime parity.
- The same path now records a bounded `placed texture:` line with the flags, sample count and layout of the first 64
  placed textures, and a bounded `placed texture refused:` line with the whole description and HRESULT for the first
  32 refusals. The measured table in section 1 came from that record; it is bounded, once per distinct line, and it
  names no game data.
- The unordered-access **view** of such a texture is refused by the layer's existing rule, now reached for the pair:
  the console-flag translation creates the texture without the unordered-access flag, and `CreateUnorderedAccessView`
  logs `unordered-access view of a texture created without ALLOW_UNORDERED_ACCESS; flags 2` once and leaves the
  descriptor null. That is the honest half the layer cannot host: a Vulkan image cannot carry a storage view of a depth
  format, and the console's depth-UAV is a console feature.

## 4. Boots and numbers

One pre-fix arm (130 s, no captures), one 160 s post-fix arm and the required 300 s post-fix arm with a presenter
capture, all under the boot lock with the deferred-compile arm (`PALMETTO_PIPELINE_DEFER_COMPILE=1`,
`PALMETTO_PIPELINE_COMPILE_BOUND=4096`, `PALMETTO_PIPELINE_COMPILE_WORKERS=4`), the standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_STATE_TRACE=2,<timeout>`, `PALMETTO_STAGE2_READY=20`, `PALMETTO_PIPELINE_CALL_STATS=5`,
`PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1`, `PALMETTO_ARENA_LOG=1`) and the arena default of 1024 MiB.

| measurement | pre-fix (130 s) | post-fix, 300 s boot (`boot-final300`) |
|---|---:|---:|
| verdict / exceptions | STOP `game+0x146F23D`, read `0x30`, trace 89.6 s (wall 112.3 s) | **STOP `host:VCRUNTIME140.dll+0x1e579`, write `0x0`**, trace 63.5 s (wall 82.7 s); the `0x146F23D` stop is **gone** |
| the three slot-133 placements | 3 x `CreatePlacedResourceX failed` (`E_INVALIDARG`) immediately before the fault | created; the depth-stencil + unordered-access stand-in line once; their SRV/DSV creations succeed; the UAV view is refused once by name |
| D3D12 calls | 202,438 | 183,283 |
| loading word / session word | loading 2 / session 1, both last changed at 88.0 s | loading 2 at 62.0 s, session 1 at 60.0 s |
| machine word (current / next) | 2 / 2 | 2 / 2; the stage-3 request at `0x81B2EF` is not written |
| stage-2 probe `wait` | +23.1 s: pending `0x7b` (123), 1 ready bit | +23.5 s: pending `0x7b` (123), 1 ready bit |
| stage-2 probe `steady` | +29.2 s: pending `0x19` (25), 0 ready bits, aggregate cleared | +29.6 s: pending `0x19` (25), 0 ready bits, aggregate cleared |
| submissions / PresentX / captures | 4,096 at 41.9 s / 3,483 / none | 4,096 at 27.3 s / 3,090 / 1 capture (2564x1421, mean rgb 0.00 0.00 0.00) |
| deferred submitted / built / failed / fallback / submit waits | 2,742 / 315 / 0 / 0 / 0, pending 2,427 of 4,096 | 2,742 / 316 / 0 / 0 / 0, pending 2,426 of 4,096, build 4,526.8 ms (max single 142.0 ms) |
| arena allocations / arena-full lines | 2,960 / 0 | 3,099 / 0 (default 1024 MiB) |

The post-fix 300 s boot ran binary SHA-256 `5195F403...F97F97CC` (the tree above, built at 20:44:57). The loader contract of
[deferred_compile.md](deferred_compile.md) section 9 survives: the stage-2 holder's poll has returned (pending
`0x7b -> 0x19`, ready set empty), the loading and session words move, and the frame loop keeps submitting and
presenting (4,096 submissions at 27.3 s, 3,090 presents in the 82.7 s the boot lasted). The stage-3 request is still not
written.

**The moved stop.** Four post-fix boots stop at the same new site (trace 55.2 s, 61.2 s, 63.5 s and 68.1 s across the
arms; the spread is this shared machine's load): `host:VCRUNTIME140.dll+0x1e579` called from
`game+0x5EBEA38 <- 0x5B82EB9 <- 0x5B82E26 <- 0x2620A8B <- 0x1DC76FF <- 0x5BC2E99 <- 0x63E78` (job runner)
`<- 0x4EAA05A` (item executor) `<- 0x221C29C` (worker loop). At the fault `rcx = r8 = 0x40000`, `rdx = 0`, `rax = 0`:
`game+0x5EBEA10` memsets a global pointer with `dimensions * 4` bytes, the destination global (`image+0x76B4260`) is
null, and the dimensions are 256 x 256 (`0x40000` bytes). The caller `game+0x5B82EB0` writes the shadow/lighting
state block around `image+0x76B40xx` but not that pointer; the null is a title-side allocation the loading stage has
not made. No failing D3D12 call precedes it, and no layer frame is in the stack, so this is the next title-side wall,
not a refusal of ours. It is deterministic at the same address in every post-fix arm, and the pre-fix arms cannot
reach it because the loader stops first.

## 5. Limits

- **The unordered-access half of the console pair is not implemented.** The texture is created as the depth-stencil
  resource and its UAV view is refused by name. A draw or dispatch that samples/writes through that UAV gets a null
  descriptor - the console would have a real UAV. Making the depth image writable as a UAV needs a separate
  architectural decision (Vulkan cannot expose a storage view of a depth format; the measured option would be a
  paired R16 image), and this slice does not attempt it.
- **The pair is accepted at the console's placed-creation path only.** The PC methods still mirror the host runtime
  and refuse depth-stencil + unordered access, because that is what the host does and the device test compares with it.
  A console description that reaches the device through another console path (for example a committed creation)
  would still be refused by the PC rules; none was measured here.
- **Console-only format numbers are still unmapped.** Format 118 (the console's `D16_UNORM_S8_UINT`) is refused in
  allocation queries, so the textures behind the three slot-25 lines of section 2.2 cannot be created yet. That is
  the named next gap (section 6); hosting a 16-bit depth-stencil as a Vulkan depth format has its own bit-width and
  depth-UAV semantics and needs its own measurement.
- **One boot per arm.** Stage times vary with the shared machine's load (the same tree produced 55-68 s trace times
  to the new wall); the counters and the stop site are stable, the timing is not.
- **The new wall is not attributed beyond its stack and its null global.** The 256x256 buffer's allocator was not
  traced; section 6 names the falsifier.
- **The captures are black.** One capture frame per arm, mean rgb 0.00; rendering is not this slice's subject.

## 6. The next wall and its falsifier

Two candidates, in measured order:

1. **The new null global.** `game+0x5EBEA10` memsets `[image+0x76B4260]`, a 256x256x4-byte host buffer, and that
   pointer is null. One boot with an in-process register/memory probe at `game+0x5B82EB0`'s call
   (`PALMETTO_POKE_LOG`) and at the writer of `image+0x76B4260`, plus the job item's data, would name the allocator
   that should have filled it. This is the next slice's measurement, not this page's claim.
2. **The console-only depth formats.** Format 118 (and 119/120) appear in allocation queries but are unmapped. If a
   later boot reaches their creation, the refusal will be a format refusal, not the flag pair fixed here. Mapping them
   needs the console format table (`116..120`) and a decision on hosting 16-bit depth-stencil on a Vulkan depth
   format.

## 7. Tests

- `port/src/console_texture.h` is shared by the device and the GPU-free tests, so the translation is tested without a
  GPU. `port/test/xs_proxy_mock_test.cpp` (gate target, `TESTS_BUILT`) gains
  `check_console_texture_translation`:
  - the measured shape at all three layer counts (R16_TYPELESS, 2D, flags 0x6, 6/1/3 slices) is translated, the
    unordered-access flag is cleared, and every other field is left as asked;
  - the other depth-stencil families (R32_TYPELESS, D32_FLOAT, R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT,
    R24G8_TYPELESS, D24_UNORM_S8_UINT) translate the same way;
  - the refusal cases stay untouched: depth-stencil alone, unordered access alone, a colour texture with
    depth-stencil, and a buffer with the pair;
  - the shared family helpers (`format_family`, `depth_stencil_family`) answer as the device's validation expects.
- The test's line: `xs_proxy_mock: the console's depth-stencil + unordered-access pair is translated to the
  depth-stencil resource for the measured R16_TYPELESS 6/1/3-layer shapes and the other depth families, while a colour
  texture, a buffer and either flag alone stay unchanged`.
- Gate results: `build_gta6rt.cmd` **PALMETTO_OK**, `build_tests.cmd` **TESTS_BUILT**, `xs_proxy_mock_test` PASS, and
  the GPU-free gate subset passes (`frame_dump_test`, `unimplemented_policy_test` 98 checks, `graphics_record_test`
  180, `graphics_pso_test` 312, `xbox_present_test`, `buffer_va_index_test`, `dstorage_test` 224, `xapu_chad_test` 87,
  `deferred_compile_test` 18). `d3d12_fence_timing_test` remains the documented pre-existing crash.

## 8. Cross-check against the reference (peer evidence, no code copied)

The console-only format numbers are not in the PC headers, so their meaning was cross-checked against the public
reimplementation projects that model the same console interface (`D:\leonida-work\peers`, evidence only, read while
measuring; no code, comment or table was copied): their console-format translation maps the same three numbers to the
host's 24-bit depth-stencil family and names `118` as the console's `D16_UNORM_S8_UINT`. That agrees with our decode
(the console's 16-bit depth-stencil family) and is why section 2.2 calls it console-only. The depth-stencil +
unordered-access **pair** we could not attribute to any peer mapping: none of them translates it, and we did not copy
one; it is our own measurement (the three descriptions above) and our own named stand-in. The dimension correction
(`3` = `TEXTURE2D`) is our own reading of the D3D12 enum and the measured descriptions.

## 9. Attribution

This slice was implemented from this project's own measurements. The console-format table cross-check used the public
reimplementation projects recorded in [cleanroom_peers.md](cleanroom_peers.md) as evidence only. No code from any of
them was copied; the header, the translation, its tests and this page are this project's own. The upstream projects
remain the copyright of their contributors and their licenses continue to apply to their own trees.

## 10. The null shadow-buffer global, attributed and fixed; the boot reaches stage 3

**Status: measured, attributed and fixed in our structure; the boot now requests and enters stage 3, and the next
stop is the title's own hang watchdog.** Section 4 left the boot at the title-side null `game+0x5EBEA38`: the title's
lighting/shadow init clears ten 256x256 host buffers through pointers it obtains from a console resource's virtual slot
0x58, and every pointer was null. Slot 0x58 is `ID3D12Resource::GetGPUVirtualAddress` on our proxy resource, and the
answer we gave was the PC texture answer (no GPU virtual address) for textures the title had placed through console
device slot 133. The console's placed pages are CPU-addressable, so this slice answers the placement address on that
console path only. With it the clears land, the machine word advances to 3/3 - the stage-3 request at `0x81B2EF` is
written - and the boot stops about 29 s later in the title's own hang watchdog at `game+0x2215C35`, with no layer frame
in the stack.

### 10.1 The measured stop, its registers and its global

The stop of section 4 is a write of 0 inside the CRT's memset. At the fault `rdi` is 0 (the intrinsic's destination),
`rcx` is `0x40000` (the byte count), `rax` is 0 (the fill value), and the call is the one at `0x5EBEA33` inside
`game+0x5EBEA10`, whose first statement is

```text
memset(_DAT_1476B4260, 0, _DAT_1476B41DC * _DAT_1476B41D8 * 4)
```

Both dimension dwords read 256 at that point (so `0x40000` bytes): the init stores one 64-bit value into both dwords at
`0x1D957D1`, and the fault's byte count leaves only `256 x 256` for that value. `_DAT_1476B4260` is `image+0x76B4260`,
the descriptor's `+0x10` slot of the first of ten descriptors. A scan of every RIP-relative reference to `image+0x76B4260`
in `.text` finds exactly two: the load above (at `0x5EBEA22`) and a store of zero in the state-reset routine at
`0x5EB6AEC`. The pointer is filled through the descriptor's base plus `0x10`, not by a direct store, which is why an
exact-address write scan finds no allocator.

The clear helper is ten memsets of the same 256x256x4 span, called at `0x5EBEA22`, `0x5EBEA46`, `0x5EBEA6A`,
`0x5EBEA8E`, `0x5EBEAB2`, `0x5EBEAD6`, `0x5EBEAFA`, `0x5EBEB1E`, `0x5EBEB42` and `0x5EBEB66`; their destinations are
the `+0x10` slot of each descriptor (image RVAs `0x76B4260`, `0x76B4300`, `0x76B4280`, `0x76B4320`, `0x76B42A0`,
`0x76B4340`, `0x76B42C0`, `0x76B4360`, `0x76B42E0`, `0x76B4380`).

### 10.2 The decompiled producer chain

The job is the stage job of section 9's task-pool path: `0x5BC2E90` -> `0x1DC7650` -> `0x26205C0` -> `0x5B82DD0` ->
`0x5B82EB0` -> `0x1D957B0` (RVAs). `game+0x1D957B0` is the lighting/shadow init: one 64-bit load from its config
object (`+0x48`) writes both dimension dwords, then it calls `game+0x2913410` ten times with five descriptor pairs
(first arguments `0x76B4250`/`0x42F0`, `0x4270`/`0x4310`, `0x4290`/`0x4330`, `0x42B0`/`0x4350`, `0x42D0`/`0x4370`; a
second argument of `0x29` for the first of a pair and `0x10` for the second; the same 256x256 size words).

`game+0x2913410` fills a descriptor. It first builds the engine record and the resource (`game+0x1429134E0`:
`game+0x140CC24A0` then `game+0x141647200`, stored at descriptor `+0` and `+8`), then, while the descriptor's `+0x10`
is still 0, calls `game+0x4FD2940` with the two fields of the resource at `+0x18`/`+0x20`. `game+0x4FD2980` in turn
calls **virtual slot `0x58`** on the object in `+0x20` - the resource the title created through console device slot
133 - and rebases the answer, and `game+0x2913410` stores that answer at descriptor `+0x10`. The job's trace tail shows
ten slot-133 creations immediately before the clears, each followed by an SRV, an RTV and a UAV view call.

Slot `0x58` is absolute vtable index 11, `ID3D12Resource::GetGPUVirtualAddress` (IUnknown 0-2, ID3D12Object 3-6,
ID3D12DeviceChild 7, Map/Unmap/GetDesc 8-10). Our `Resource` proxy returns its console VA, and that field is set only
on the buffer paths; for a texture it stays 0, the intended PC answer.

### 10.3 The producer, measured in-process

Two probe boots used `PALMETTO_POKE_LOG` at `0x291347B` (before `game+0x4FD2940`), `0x2913480` (after it),
`0x4FD298F` (the slot-0x58 call) and `0x4FD2995` (its return), in the same deferred arm as the 300 s boot.

| probe | before the fix | after the fix |
|---|---|---|
| slot-0x58 call at `0x4FD298F` | the called object's vtable is in the runtime's own module | the proxy's bounded line prints the same vtable pointer |
| return at `0x4FD2995` | 0, all ten times | the title's placement address, ten different values |
| descriptor slot after the call at `0x2913480` | 0 | non-zero |
| next event | `memset` of `0x40000` bytes at 0 | the job continues and the boot requests stage 3 |

The proxy line names the object (`a title-placed texture answers GetGPUVirtualAddress ... proxy vtable <pointer>`),
and the pointer it prints is the same pointer the title's slot-0x58 call uses. The ten calls were on ten textures
created through console slot 133 (256x256, the shapes this job creates); the answer our layer gave was the PC answer
for a texture, not the console's.

### 10.4 The fix

- `port/shims/d3d12_vulkan_device.cpp`: `Resource` keeps a second address, `console_placed_va_`, set by
  `set_console_placed_va`; `GetGPUVirtualAddress` returns it when the resource has neither an acceleration structure
  nor an arena VA, and the console's placed-texture path (`create_at_host_address`, device slot 133) sets it to the
  placement address it was given. Every PC path is unchanged: a committed texture still answers 0.
- One bounded line (first eight answers) names the placement address, the resource's shape and the proxy's vtable
  pointer, so a later boot can tie the answer to the call site again.
- The committed image is still a stand-in: the title's CPU clears and writes go to its own pages, not into that image,
  and the opt-in texture import ([texture_import.md](texture_import.md)) remains the only path that reads those pages
  into it. That is the half this slice does not fix, and it is named again in section 10.7.

### 10.5 The 300 s boot

One boot under the boot lock, the standard environment of section 4 plus a capture and
`PALMETTO_WAIT_SAMPLE=60,80,100,120,140,160,180,200,220,240,260,280`, binary SHA-256 `5E2C4622...2B939DE1` (built at
21:09:23). It stops at 97.3 s wall (trace 95.3 s) on the new wall:

| measurement | section 4's arm (`boot-final300`) | this arm (`boot-nullfinal300`) |
|---|---:|---:|
| verdict / exceptions | STOP `host:VCRUNTIME140.dll+0x1e579`, write `0x0`, trace 63.5 s (wall 82.7 s) | **STOP `game+0x2215C35`, write `0x0`**, trace 95.3 s (wall 97.3 s); the `0x5EBEA38` stop is **gone** |
| init word | 2 at 22.0 s | **3 at 66.0 s** |
| loading word / session word | loading 2 at 62.0 s, session 1 at 60.0 s | loading 2 at 64.0 s, **session 3 at 66.0 s** |
| machine word (current / next) | 2 / 2; the stage-3 request at `0x81B2EF` is not written | **3 / 3 at 66.0 s; the stage-3 request is written and stage 3 is entered** |
| stage-2 probe `wait` | +23.5 s: pending `0x7b`, 1 ready bit | +27.3 s: pending `0x7b`, ready 0/0, 1 ready bit (id 152) |
| stage-2 probe `steady` | +29.6 s: pending `0x19`, 0 ready bits | +33.4 s: pending `0x19`, ready 0/0, 0 ready bits |
| stage-2 probe `steady2` | not taken | +79.6 s: `b0 0xf9`, queue `0x0`, armed `0x0` - the poll returned |
| D3D12 calls | 183,283 | 172,563 |
| submissions / PresentX / captures | 4,096 at 27.3 s / 3,090 / 1 (2564x1421, mean rgb 0.00) | 4,096 at 28.9 s / 2,771 / 1 (2564x1421, mean rgb 0.00) |
| deferred submitted / built / failed / fallback / submit waits | 2,742 / 316 / 0 / 0 / 0, pending 2,426 of 4,096 | 2,742 / 323 / 0 / 0 / 0, pending 2,419 of 4,096 |
| deferred build time, max single | 4,526.8 ms, 142.0 ms | 63,362.6 ms, 58,938.3 ms (machine load, section 10.7) |
| arena-full lines / refusals | 0 / 0 (default 1024 MiB) | 0 / 0 (default 1024 MiB, no refusal line) |

### 10.6 The moved stop: the title's own hang watchdog

The new stop is not a resource answer of ours. Its stack is `game+0x2215C35` <- `game+0x2215ECA` <-
`host:KERNEL32.DLL+0x2dda7` (thread start), with no layer frame. The function containing the fault (`game+0x2215A60`)
is the title's hang watchdog, run as a thread started through the generic thread wrapper `game+0x2215EA0`: it calls
`QueryPerformanceCounter`, sleeps 1000 ms per pass, and when the heartbeat dword at `image+0x1471B6338` has not
advanced for `0x1E` passes it calls the callback at `image+0x1471B6348` and then executes the deliberate null write
(`_DAT_00000000 = 0`) followed by `int3` - the fault this boot records. The heartbeat is petted by the per-frame tick
functions whose call sites are `0x120799` and `0xC9E368` (both call `game+0x22159F0`, which stores a
`QueryPerformanceCounter` value in that dword), so the watchdog's abort dates the frame loop's stop.

The state words date it to the stage transition: the machine word reads current 3 / next 3 from 66.0 s, the init word 3
and the session word 3 also last change at 66.0 s, and nothing changes again before the abort at 95.3 s. The stage-3
request at `0x81B2EF` was therefore written and the pump entered stage 3; the pump then stopped returning to the
per-frame tick, the same class of block [stage2_blocker.md](stage2_blocker.md) recorded for stage 2 (the pump inside
the entered handler). The wait samples put 96 threads (75 in a wait or busy title site) at 60 s and 100 threads (86) at
80 s; they do not tie the pump's own stack to a wait in this arm, so the next falsifier is a thread-stack sample taken
2-5 s after the machine word reads 3, or a probe at the stage-3 handler entry the pump runs.

### 10.7 Limits

- **One 300 s boot.** The deferred compile's single worst build is 58,938.3 ms here against 142.0 ms in section 4's
  arm, and a 130 s diagnostic boot on the same tree reached stage 3 at 106.0 s instead of 66.0 s (its worst build
  70,857.8 ms). The shared machine's load stretches every timing; the counters and the stop site did not move.
- **The title's CPU clears do not reach the image.** The console answer lets the title's init proceed; the stand-in
  texture is not backed by those pages, so rendering that depends on those bytes is not fixed (the capture is still
  black, mean rgb 0.00).
- **The watchdog stop is title-side and is not implemented.** This slice names it; the stage-3 handler's wait and its
  producer are the next measurement named in section 10.6.
- **The answer is on the console path only.** The PC methods keep the PC texture answer; only the slot-133 placed path
  translates, as section 3's depth-stencil translation does.

### 10.8 Tests and gates

- `port/test/d3d12_existing_heap_test.cpp` (`check_title_placed`) gains two checks: a title-placed texture's
  `GetGPUVirtualAddress` is the placement address it was given, and an ordinary committed texture still answers 0.
  The target exits 0 and its slot-133 lines pass.
- Gate subset: `frame_dump_test`, `unimplemented_policy_test` (98 checks), `graphics_record_test` (180),
  `graphics_pso_test` (312), `xbox_present_test`, `buffer_va_index_test`, `dstorage_test` (224), `xapu_chad_test`
  (87), `deferred_compile_test` (18), `xs_proxy_mock_test` and `arena_size_test` all exit 0; `d3d12_fence_timing_test`
  remains the documented pre-existing crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and the
  `tests/test_docs_*.py` checks pass.

### 10.9 Attribution

This slice was implemented from this project's own measurements: the crash registers, the decompiled chain above, the
in-process poke boots and the 300 s boot's counters. No peer code was used and no game data is added: RVAs, field
offsets, counts, shapes and outcomes only.

## 11. The stage-3 hang, measured and fixed: the frame thread was inside the session-3 loading poll because a deferred-compile submission had blocked; the stop moves to a refused vertex-buffer draw

**Status: measured, attributed and fixed in our structure; the boot now passes stage-3 entry with the heartbeat and the
frame loop still running, and stops at a new title site.** Section 10.6 left the boot in the title's own hang watchdog
at `game+0x2215C35`, with the per-frame pump apparently stopped after stage 3 was entered. This section proves that the
title-side wait was a symptom: on the thread that runs the frame loop, the stage-3 post-transition handler calls the
session dispatcher, whose state-3 handler is a 33 ms poll on the loading progress record; the loading task that should
advance that record was blocked *behind our own deferred-compile submission wait* (FIFO against thousands of queued
builds). The submission never returned, the loading completion never arrived, the heartbeat stopped, and the watchdog
aborted thirty passes later. The fix builds the deferred pipeline a submission names on the submitting thread instead
of waiting for every older queued build. With it, the same 300 s arm enters stage 3, keeps the heartbeat and the
present loop alive, and stops at `game+0x13360A3` (`read` of `0xFFFFFFFFFFFFFFFF`) after three `ExecuteCommandLists`
refusals that name a vertex buffer the replay has no arena mapping for. Section 11.7 names that next wall.

### 11.1 What ran

| boot | fix | probe switches | stage word 3 (probe) | stop | wall |
|---|---|---|---:|---|---:|
| `boot-stage3pump300` | none | none (the env array was mis-quoted, so only the standard arm ran) | 110.0 s | watchdog `game+0x2215C35`, t 162.5 s | 165.1 s |
| `boot-stage3pump300b` | none | `PALMETTO_STAGE3_PUMP=2,5,10,20,40`, wait sample 76/96 | 69.1 s | watchdog, t 99.1 s | 101.1 s |
| `boot-stage3diag` | none | the same probe plus `PALMETTO_WAIT_TRACE=75,95,115,135` | 75.5 s | watchdog, t 106.2 s | 106.2 s |
| `boot-stage3final300` | none | the same probe, captures on | 92.1 s | watchdog, t 121.2 s | 123.2 s |
| `boot-stage3fix300` | **4,096-bound submit-wait fix** | the same probe, captures on | 69.6 s | **`game+0x13360A3`, read `0xffffffffffffffff`, t 73.6 s** | 75.7 s |

All five are one boot at a time under `_setup/boot_lock.ps1` (`-Who stage3`), harness `tooling/boot_report.py`, logs and
captures outside the repository in `D:\leonida-work\threedee`. The standard environment is section 10.5's: the
deferred-compile arm (`PALMETTO_PIPELINE_DEFER_COMPILE=1`, `PALMETTO_PIPELINE_COMPILE_BOUND=4096`,
`PALMETTO_PIPELINE_COMPILE_WORKERS=4`), `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_STATE_TRACE=2,<timeout>`, `PALMETTO_STAGE2_READY=20`,
`PALMETTO_PIPELINE_CALL_STATS=5`, `PALMETTO_DUMP_REGS=1`, `PALMETTO_FAULT_MEM=1`, `PALMETTO_ARENA_LOG=1`, the arena
default of 1024 MiB (no arena switch set) and one presenter capture per arm. The fixed binary is SHA-256
`96593942...A762C5`; the pre-fix measurement arm is `96F273B0...B99A8`.

The five arms put stage 3 between 69.1 s and 110.0 s (the shared machine's load) but the same pre-fix stop site, and
the watchdog aborts 30 s after the heartbeat's last advance in every pre-fix arm but the first, where the deferred
driver compile (a single build of 62.9 s) stretched the watchdog's one-second passes.

### 11.2 The probe: unwinding the pump thread, and a wrong address in section 10.6

The earlier arms could not name the pump: `PALMETTO_WAIT_SAMPLE` and `PALMETTO_THREAD_STACKS` scan raw stack words, and a
suspended title thread's stack holds stale return addresses, so the 80 s detail in section 10.6 shows frames from calls
that had already returned. `port/src/main.cpp` gains an opt-in probe, `PALMETTO_STAGE3_PUMP=<offsets>` (unset: no thread
starts and the default path is unchanged). It polls the state machine object at `[[image+0x71273C8]]` every 25 ms and,
when the current-stage u16 at `+0x10` first reads 3, samples every other thread's **real** call chain at the named
offsets after that moment (`2,5,10,20,40`; only the first two offsets ran before the fixed boot stopped). The chain is
walked with `RtlLookupFunctionEntry`/`RtlVirtualUnwind` from each suspended thread's context, grouped by identical chain,
and tagged with the functions this section names. The probe also reads the hang watchdog's heartbeat and prints how many
times it advanced and when it last changed, so "the pump stopped" is a measurement.

The heartbeat needs a correction of section 10.6's own text: `image+0x1471B6338` there is the Ghidra address of the
watchdog instruction's RIP-relative operand (the image is based at `0x140000000`), and the image **RVA is `0x71B6338`**
(the state machine pointer in the same decompile is `0x1471273C8` = `image+0x71273C8`, the address the state trace has
always used). A read of `image+0x1471B6338` is past the image mapping; the first probe boot measured that wrong address
and read a constant 0. With the correct RVA the heartbeat reads a QPC value that advances once per frame, ~24 times a
second.

### 11.3 The stop before the fix, dissected

`boot-stage3final300` (and the diagnostic arm) put stage 3 at 92.1 s (75.5 s in the diagnostic arm). Three thread chains
explain the whole stop; each was confirmed against the code's own call sites and, in the diagnostic arm, against
`PALMETTO_WAIT_TRACE`'s outstanding-wait table.

| thread | unwound chain (deepest first, RVAs) | what it is |
|---|---|---|
| frame loop, tid 32072 | `NtDelayExecution`(`SleepEx(33)`) <- `game+0x21B70A5` <- `0x1D83996` <- `0x81AFB9` <- `0x53EC079` <- `0x53EBF66` <- `0x661F0` <- `0x1D109C9` <- `0x5A91C0D` <- `0x56A1E5A` | the frame thread: `0x56A1D54` (game frame update) -> `0x5A91BD0` (runtime frame pump) -> `0x1D109B0` (per-frame tick dispatch) -> `0x661E0` (state-machine wrapper) -> pump `0x53EBEA0` -> transition executor `0x53EBFE0` -> stage-3 post-transition handler `0x81AFB0` -> session dispatcher `0x1D83960` -> **`0x21B7070`**, a `while (!pred) { SleepEx(33); tick(); }` loop |
| loading task, tid 47728 | `NtWaitForSingleObject(INF)` on a semaphore <- `0x56A0D` thunk <- `0x4E7F358` <- `0x6259` <- `0xCD73D9` <- `0xCD72E7` <- `0x24848BD` <- `0xC9E603` <- ... <- `0x11FDBD` <- `0x4FD1E88` <- `0x4FD1AE5` <- `0x23CF247` <- `0x23CF104` | the title task-pool thread running the loading task `0x4FD1AD0`: its loading update `0x4FD1B20` -> monitor tick `0x11FD60` -> the registered frame tick `0x120790` (which pets the heartbeat at `0x120799`) -> progress tick -> the title's own completion wait |
| submitting title thread, tid 49400 | `NtWaitForAlertByThreadId(INF)` <- `MSVCP140+0x176ea` <- `rt::d3d12::DeviceState::prepare_deferred_compute_pipelines` <- `VulkanContext::prepare_pipelines` <- `VulkanQueue::execute_now` <- `VulkanQueue::execute` <- `rt::d3d12::CommandQueue::ExecuteCommandLists` <- `child_slot<0,35>` <- `game+0x24821E3` | the title thread that submits a command list naming a deferred compute pipeline: **blocked inside our shim** in the compile queue's wait |

The console slot-35 handler (`child_slot<0,35>`, `port/shims/d3d12_xs_device.cpp`) calls the PC
`ExecuteCommandLists`, which calls the queue hook that waits for the deferred pipeline. The probe resolves those shim
frames by symbol, so this is our call, not a title guess.

The timeline is one event, not three coincidences. The loading word moves 1 -> 2 at 90.0 s, the same instant the
loading task's completion wait starts (the wait trace's outstanding entry is 2,063 ms old at the 75 s dump and 22,063 ms
at the 95 s dump in the diagnostic arm) and the submitting thread enters our wait. The heartbeat's last advance is
90.1 s. Stage 3 is entered at 92.1 s, and the frame thread's own clock in the pump's session-3 poll never completes
because the loading progress record the loader should advance is frozen. The watchdog's passes then run from 90.1 s to
121.2 s (30 one-second passes plus the last-pet offset) and the null write at `game+0x2215C35` follows.

The wait trace names the completion the loader waits for: a title semaphore, `handle 0x96c` in the diagnostic arm
(`handle 0x948` in the final arm), 16 wait entries, 15 releases, the last release at 72,906 ms from title code (the
release site `game+0x6380` is the return after the image's `ReleaseSemaphore` thunk inside a shared dispatch helper), and
the outstanding wait has no matching set call. So the missing completion is title-side, but it is missing **because the
submission that would complete it is parked in our wait**.

### 11.4 The fix

`port/src/deferred_compile.h`'s `CompileQueue` gains `wait_or_build(task)`: the submission path calls it, and when the
task a submission names is still queued, the **submitting thread claims that task out of the FIFO and builds it
itself**; only a task a compile worker has already started is waited for. The worker loop and the new path share one
build body, so the counters (built/failed, build time, longest build, outstanding, the bound) and the waiter's condition
variable keep their meaning. `port/shims/d3d12_vulkan_device.cpp`'s `prepare_deferred_compute_pipelines` calls
`wait_or_build`, and the queue bookkeeping records the call in the same "submit waits" counters as before.

Why this is the right shape: the deferred design ([deferred_compile.md](deferred_compile.md) section 3.1) says a
submission that first names a deferred pipeline waits for it on the submitting thread, "which is exactly where a D3D12
runtime is allowed to wait". That contract is a wait for *one* driver build, not for every older queued build. The
measured arm defers 2,742 compute pipelines at the 4,096 bound, the four workers build ~320 in 90 s, and the submitted
list names a pipeline that is still queued; the plain wait then blocks for the whole queue ahead of it. Claiming the
task keeps the wait at one build. The switch surface is unchanged: `PALMETTO_PIPELINE_DEFER_COMPILE=0` (the default) never
reaches this code, and the synchronous fallback path (queue at its bound) is untouched.

### 11.5 The boot after the fix

`boot-stage3fix300`, same environment and probe as `boot-stage3final300`, binary `96593942...A762C5`:

| measurement | before the fix (`boot-stage3final300`) | after the fix (`boot-stage3fix300`) |
|---|---|---|
| verdict / stop | STOP `game+0x2215C35` (watchdog null write), t 121.2 s, wall 123.2 s | **STOP `game+0x13360A3`, read `0xffffffffffffffff`, t 73.6 s, wall 75.7 s**; the watchdog stop is gone |
| init / loading / session words | init 3 at 94.0 s, loading 2 at 90.0 s, session 3 at 94.0 s | init 3 at 70.0 s, loading 2 at 68.0 s, session 3 at 70.0 s |
| machine word (current / next) | 3 / 3 at 92.1 s, pending `0xffff` | 3 / 3 at 69.6 s, pending `0xffff`; stage 4 is still not requested |
| heartbeat | 2,462 advances at stage 3, then **0 since** (last advance 90.1 s) | 1,719 advances at stage 3, then **1 advance after stage 3** (the sample 2 s later reads it 2.0 s old); the watchdog never fires |
| deferred submit waits | 0 (the FIFO wait never returned, so the counter never moved) | **42, max single 563.8 ms** |
| deferred submitted / built / pending | 2,742 / 323 / 2,419 | 2,742 / 320 / 2,422 (last periodic line) |
| D3D12 calls | 189,061 | 199,837 |
| PresentX | 3,300; the last present is between the 88.0 s and 90.0 s state lines (before stage 3) | 3,411; the last present is at the end of the boot, **after stage 3** |
| captures | 1 frame, 2564x1421, mean rgb 0.00 (black) | 1 frame, 2564x1421, mean rgb 0.00 (black); no non-black capture |
| new layer lines | none after the stage-3 entry | 3 x `ExecuteCommandLists failed: unknown resource; nothing was submitted` |

The heartbeat line deserves its plain reading: after the fix it advanced once after the machine word read 3 and then did
not advance again for the 2 s the sample covers, because the frame thread is inside the stage-3 transition (the session-3
poll runs with 33 ms sleep passes) and the boot dies 4 s after stage 3. It is 1 advance versus 0 in the pre-fix arm; a
longer arm is the falsifier that would show the cadence returning to ~24/s once the load has nothing left to do.

### 11.6 The new wall

The three refusals are one shape: a draw's `draw.vertex_buffer` names title VA `0x15c08800900` (size `0x120000`) or
`0x15c08800980` (size `0x11ff80`), the replay's `buffer_at` lookup has no arena mapping for it, the whole command list
(`list 2 of 4`, 11 commands) is refused in prepare, and `ExecuteCommandLists` returns failure with nothing submitted.
The boot then stops at `game+0x13360A3` in `game+0x1335A20..0x13361ED` (a read of `0xFFFFFFFFFFFFFFFF`) with an eleven-frame
title stack from the same renderer path (`0x1829C0`..`0x184395`), i.e. the title faults on its own failed submission
rather than in our layer. This is the first boot that reaches stage-3 rendering at all; the vertex buffer's
registration path is the next slice's measurement, not this one's claim.

### 11.7 Limits

- **One boot per arm.** Stage 3 ranged 69.1-110.0 s across five arms on the shared machine, and the deferred driver
  compile's worst single build was 62.9-65.1 s in three of them; the stop sites did not move but every timing did.
- **The causal link is measured as one changed input, one changed boot.** The loader's completion and the submitting
  thread's wait start in the same second and the fix removes the hang, but no signal trace proves the semaphore is
  released by the refused submission itself; the named next falsifier is a wait-trace arm on the fixed binary that
  correlates the semaphore's set calls with the submit returns.
- **The 33 ms poll is not the fault.** Before the fix it was the visible block, but it is the title waiting for real
  loading progress; the boot now runs it while the loader advances. Making the poll return without the record would be a
  fake completion, and this slice does not.
- **The capture is still black.** Rendering remains outside this slice: one capture per arm, mean rgb 0.00.
- **The fix changes the deferred path's wait shape, not the default path.** With `PALMETTO_PIPELINE_DEFER_COMPILE`
  unset the compile queue is not used and the boot path is section 4's.
- **The `game+0x13360A3` crash is not attributed beyond its stack and the three refusals.** Section 11.6 names the
  falsifier.

### 11.8 Tests and gates

- `port/test/deferred_compile_test.cpp` gains `test_submit_builds_queued_task`, in the file's `CHECK` group: with one
  worker blocked in an older build, a task that is still queued is claimed and built on the calling thread (the calling
  thread's id is the one that runs the closure), the older build and the later queued task are untouched, the counters
  move once, a finished task is not rebuilt, a cancelled queued task is claimed and skipped (the caller gets false), a
  null task returns false, and a task a worker already owns is waited for normally.
- The target exits 0 (`deferred compile: ok (18 check(s))`; the printed count is the file's `CHECK_COUNT` group, the new
  group uses the file's `CHECK` style).
- Gate subset: `frame_dump_test`, `unimplemented_policy_test`, `graphics_record_test`, `graphics_pso_test`,
  `xbox_present_test`, `buffer_va_index_test`, `dstorage_test`, `xapu_chad_test`, `xs_proxy_mock_test`,
  `arena_size_test` and `deferred_compile_test` exit 0; `d3d12_fence_timing_test` remains the documented pre-existing
  crash.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and the
  `tests/test_docs_*.py` checks pass.

### 11.9 Attribution

This slice was implemented from this project's own measurements: the unwound chains, the probe and wait-trace arms, the
300 s boots and the A/B above. No peer code was used and no game data is added: RVAs, field offsets, counts, shapes and
outcomes only.
