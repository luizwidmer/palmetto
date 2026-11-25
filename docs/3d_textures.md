# Console depth-stencil textures with unordered access: the stop after the arena fix

**Status: measured, attributed and fixed in our structure; the boot then advances to a new wall.** The stop at
`game+0x146F23D` (read of `0x30`, [deferred_compile.md](deferred_compile.md) section 9.7) is the title's resource
factory returning a wrapper whose GPU-resource field is null, because three of the loading stage's placed-texture
creations were refused by our PC validation. The three descriptions are **2D depth-stencil textures that also allow
unordered access** - a pair the console allows and the host PC runtime refuses - and this slice translates that pair on
the console's placed-creation path. With the fix the `0x146F23D` stop is gone and the boot reaches a new title-side
null at `game+0x5EBEA38`. Section 2 corrects the previous page's reading of the two log lines: `dimension 3` in the
slot-25 line is `D3D12_RESOURCE_DIMENSION_TEXTURE2D` (the enum is 0-based, so 3 is not "3D"), and `format 118` is a
different, older refusal of an allocation query, not the format of the refused creations.

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
