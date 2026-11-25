# The null container element at the measured stop

**Status: measured on one machine, 2026-11-24**, against the 1.0.2.1 target console executable, one `--run` of the
Vulkan backend with `PALMETTO_D3D12=vulkan` and the shader corpus. This page answers the tracker's O7: what should
populate the null element at `game+0xcc2861`, and why it is empty. Nothing here quotes executable bytes, game data,
package paths or extracted tables: only RVAs, structure field offsets, register and stack values, counts, and the
project's own wording.

**Answer: option (a).** The container element is a render-target **resource** the title creates through its own
texture factory, and the element is null because a query the runtime answers is answered *invalidly*. The factory asks
the device for the resource's allocation info (console device slot 108, which the runtime forwards to the PC
`GetResourceAllocationInfo`); the runtime refuses to size the description because its format is `DXGI_FORMAT_A8_UNORM`
(65) on a resource with an attachment flag, so it returns D3D12's invalid allocation (`SizeInBytes = 0xFFFFFFFFFFFFFFFF`).
The title's own allocation-info helper asserts on that answer (`int3` at `image+0xABFE1B`, traced), the engine-heap
allocation gets no usable size, the factory destroys the object and returns null, and the next thing that consumes the
element - the view registration at `0xCC2840` - hits its own null assert and then dereferences the null at
`game+0xcc2861`. No GPU-written memory and no missing graphics pipeline is involved.

This is the *same* allocation-info assert the older null-object stop in [null_object_chain.md](null_object_chain.md)
saw, reached by a second, independent consumer: the renderer's output render-target set. The slot-108 answer itself is
no longer a stub; what fails now is the format gate inside the real query.

## 1. What the container is

The faulting frame reads element 0 of an inline container at `object+0x60` of a renderer object. The object's class
(measured from the constructor and from a register dump, not guessed):

| item | RVA | notes |
|---|---|---|
| class vtable | `image+0x658DA80` | slot 0 `0x13A7D20`, slot 1 `0x186170` (the faulting method), slot 2 `0x1858E0`, slot 5 `0x6FECC0` (a small getter), slot 12 `0x13A7E30` |
| constructor | `0x185570..0x1856C5` | sets the vtable at `0x1855D9`; refcount at `+8` |
| "create the resources" method (vtable slot 1) | `0x186170..0x186456` | the frame that builds and reads the container |
| "attach to the parent" method | `0x1856D0..0x1858D1` | calls slot 1, then links the object into the parent |
| "create one target set" method of the parent | `0x184EB0..0x184F5E` | constructs the object through the parent's target-description lookup `0x184F60..0x185009` |

The container is a small vector of **0x30-byte (48-byte) records**, count in a byte at `object+0x45`:

* count `< 2`: the data pointer at `object+0x60` points at the object's own inline buffer at `object+0x68`
  (measured: `rax = [rsi+0x60] = rsi+0x68` at the call site; count = 1 here);
* count `>= 2`: the constructor allocates `count * 0x30` bytes and stores the pointer at `+0x60`;
* after zeroing, the constructor plants two sentinels in the first inline record: `0x0FFFFFFF` at record `+0xC` and
  `0x80000000` at record `+0x10`.

One 0x30-byte record is a render-target slot. Measured field use (the `0x186170` method plus the constructor):

| offset | size | content |
|---|---|---|
| `+0x00` | qword | the GPU texture **resource** object (this is the field that is null at the stop) |
| `+0x08` | dword | zeroed by the constructor |
| `+0x0C` | dword | sentinel `0x0FFFFFFF` |
| `+0x10` | dword | sentinel `0x80000000` |
| `+0x18` | qword | the resource's view wrapper, made by `0xCC2840` when the set wants views; zero on the first pass |
| `+0x28` | dword | target width |
| `+0x2C` | dword | target height |

The set's `+0x46` word carries the creation flags (measured `0x0500`), `+0x44` a byte argument (`1`), `+0x45` the
element count (`1`), and `+0x58` a target-description object that supplies the texture format (read at that object's
`+0xC`). The parent chain, read off the crash stack, is
`game+0xcc2861 <- 0x186324 <- 0x18573B <- 0x184F42 <- 0x5B132D3 <- 0x1D4AD94`: a renderer output object's method
`0x5B13290` creates the target sets (this one with target kind `9`, then two half-size ones with kind `2`), the set's
slot 1 (`0x186170`) creates the resources and registers their views, and the renderer init `0x1D4AC70` (class vtable
`0x6568DF0`, slot 7) is the frame below that.

## 2. The write path that is supposed to fill element 0

Inside `0x186170`, for each of the `count` records, in order:

1. `0x1861C4..0x1861ED` - ask `0x455A5E0` for the target's extent (the parent's `+0x38`/`+0x3C` pair, halved for a
   special view id) and store it at record `+0x28`;
2. `0x18623B..0x186346` - build a 14-byte engine texture descriptor with `0x501F4A0` (extent from the record, format
   from the description object's `+0xC`), hand it to the engine texture factory at `0x1862E3`
   (`0x5204E60 -> 0x34BEB0`), and store the factory's return at **record `+0x00`** (`mov [r15+r13-0x2C], rax` at
   `0x1862E8`);
3. if the record's set wants views (`+0x47` bit 2, measured set), build a 12-byte view descriptor (zero, `0xFFF`, the
   format byte) and call `0xCC2840(record+0x00, &view descriptor, record+0x18)`, storing its return at record `+0x18`.

Step 2 is the write that is missing its value: the factory returns 0, the method records that as failure, but the
failure path still falls into step 3, and `0xCC2840` begins with `mov rdi, rcx; test rcx, rcx; je 0xCC2936` - its own
null assert. `PALMETTO_SKIP_INT3` steps over the `int3` at `0xCC2936`, so the very next instruction,
`test byte ptr [rdi+0x10], 4` at `0xCC2861`, reads `0x10` and faults. So the title is not misreading an empty
container as a valid object: it is calling the view registration with the *known-null* resource, i.e. this stop is a
downstream null-assert, and the question is only why the factory returned null.

## 3. What element 0 must hold, measured

The failing descriptor was read out of the stack at the factory call (four qwords at `rdx` at the poke on `0x1862E3`),
from the record and the set. Decoded:

| descriptor field | value |
|---|---|
| width | `0x0F00` = 3840 |
| height | `0x0870` = 2160 |
| depth / array size | 1 |
| dimension (kind byte) | 1 = 2D |
| format | `0x41` = 65 = `DXGI_FORMAT_A8_UNORM` |
| layout byte | `0xFF` (default) |
| sample selector | 0 (one sample) |
| mip levels | 1 |

The set's extent field `+0x38` measured `0x087000000F00` - the same 3840 x 2160 - and the set's element count is 1, so
**element 0 must hold a 3840 x 2160, 2D, one-mip, one-sample `A8_UNORM` render-target resource**, created by the
engine's texture factory from the descriptor above, and then a view wrapper for it. The size itself is sound: the
output object is created from the renderer's resolution pair (`0x5BC2B00` copies `*size` into object `+0x10`), and at
`0x5BC2A56` that pair measured 3840 x 2160.

## 4. Why it is empty: the runtime refuses to size A8 render targets

The factory's own descriptor validator (`0x2478140`, called at `0x34BECF`) accepts this description: both extents are
non-zero and the record's flag byte `+0xD` is 0. The failure is in the finalize (`0x193DE10`, called at `0x34BFFC`),
which builds the `D3D12_RESOURCE_DESC` (`0xC4B030` at `0x193E06B`) and asks its allocation-info helper `0xABFAD0` for
the size; that helper calls console device slot 108.

The runtime's slot-108 conversion (`port/shims/d3d12_xs_device.cpp`, `Slot == 108`) forwards the description to the
PC `GetResourceAllocationInfo`. The PC device's `allocation()` sizes through `DeviceState::texture_format()`, and that
function has an explicit rule (`port/shims/d3d12_vulkan_device.cpp:957`):

```text
if (desc.Format == DXGI_FORMAT_A8_UNORM &&
    (desc.Dimension != TEXTURE2D || desc.SampleDesc.Count != 1 ||
     (desc.Flags & (ALLOW_RENDER_TARGET | ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS))))
    return {};                       // "A8 is a sampled format, not an attachment format"
```

The description the title passes has `ALLOW_RENDER_TARGET`. That flag is derived, not assumed: the set's flag word
`0x500` produces the factory's class word `0x128` (`0x186207..0x18622E`), the factory copies it to the object's word
`+0x0E` through `0x320090` (`0x164E45` and `0x3200D3`), and the finalize reads it into `r9d` at the `0xC4B030` call
(`0x193E063`), where the builder extracts bits 5..7 as the D3D12 resource flags (`bextr`, `0xC4B0DE`): `0x128 >> 5 &
7 = 1`, i.e. `ALLOW_RENDER_TARGET`. So the rule fires, `texture_format()` returns an empty `Format`, and
`allocation()` answers D3D12's invalid allocation `{SizeInBytes = UINT64_MAX, Alignment = 0x10000}`.

The slot-108 bridge then publishes that value: its "answered" test is `info.SizeInBytes != 0`, and D3D12's invalid
allocation is *not zero*, so the 64 KiB fallback (used when the host declares it cannot size a description) does not
run. The title receives `SizeInBytes = 0xFFFFFFFFFFFFFFFF`, and its own helper asserts on the high dword
(`0xABFE1B`, the same `dword[out+4] == 0` assert documented in [null_object_chain.md](null_object_chain.md) section 7).
The engine-heap allocate (`0x2474690`, called at `0x193E243` with `SizeInBytes` in `r8`) gets that size and returns
no block - the run records no out-of-memory assert (`0x2219650`) and no placed-resource creation for this resource - so
the finalize returns false at `0x193E310`, the factory takes its destroy-then-return-null tail (`0x34C0C0`,
`0x34C0C8`), and `0x1862E8` stores the null into element 0.

Option (b) is ruled out by the same measurement: element `+0x00` is written by CPU code from a factory return (step 2
above), never by a GPU buffer, and the failure happens at the allocation-info query, before any placed-resource
creation reaches the device. Nothing about unified memory or a GPU-written range is on this path.

## 5. The measurement

One boot with the repo's own harness:

```
PALMETTO_D3D12=vulkan PALMETTO_SHADERS=... PALMETTO_SKIP_INT3=1 PALMETTO_SEED_GLOBAL=75DF268:D8
PALMETTO_POKE_LOG=5BC2A56,1862E3,1862E8,18631F
```

Baseline and result, all three runs:

| run | max traced call | stop |
|---|---|---|
| baseline, before the diagnostic change below | **#11986** | `game+0xcc2861`, read at `0x10` |
| after the diagnostic change, no pokes | **#11986** | `game+0xcc2861`, read at `0x10` |
| with the four pokes | **#11986** | `game+0xcc2861`, read at `0x10` |

(The harness counts 8.5k-9.1k matching `[d3d12-trace]` lines per run because the runtime writes its trace from more than
one thread and the fault interrupts interleaved output; the comparable number - the highest traced call sequence - is
11,986 in all three runs, and the stop never moves.)

What the pokes show, in stderr order:

1. `0x5BC2A56` (output object creation): the size pointer holds `0x087000000F00` - the output is 3840 x 2160;
2. `0x1862E3` (texture factory call, before the call): the set's first qword is `image+0x658DA80`; its `+0x38` is
   `0x087000000F00`; `+0x44`=1, `+0x45`=1, `+0x46`=0x0500, `+0x47`=0x05; and `rdx` points at the descriptor whose
   decoded fields are the table in section 3;
3. **between the poke at `0x1862E3` and the one at `0x1862E8` the runtime prints `[rt] d3d12:
   GetResourceAllocationInfo format not supported yet: 65`** - i.e. inside the factory call, the allocation query
   refused the description;
4. `0x1862E8` (after the factory returned): `rax = 0`, `rsi` still the set - the factory returned null;
5. `0x18631F` (the `0xCC2840` call): `rcx = 0` (the element), `r8 = 0` (no existing view), and `rax = rsi+0x68`
   (the inline buffer) with `rax[0]=0`, `rax[+0xC]=0x0FFFFFFF`, `rax[+0x10]=0x80000000`, `rax[+0x18]=0`.

The structured trace of the same run records, in order, `int3_skipped` at `0xABFE1B` (the allocation-info assert),
then `int3_skipped` at `0xCC2936` (the view registration's null assert), then the access violation at `0xCC2861`. The
runtime never logs its "slot 108 could not size the description, reporting 64 KiB" fallback, which is the direct
corollary of the published `UINT64_MAX` described in section 4.

## 6. Correction to the tracker's description of the class

O7 in `docs/issues.md` names class vtable `image+0x6597C60` with constructor `game+0x1B37440`. That is not the class of
the object whose container the faulting code reads; the measurements here put that object at `image+0x658DA80` (the
constructor stores that vtable at `0x1855D9`, and the register dump at `0x1862E3` shows it at `[rsi]`). The vtable
`image+0x6597C60` / constructor `0x1B37440` belongs to a different 0x48-byte record: the constructor allocates 0x48
bytes, copies 0x18 bytes of a caller-supplied descriptor into record `+8..+0x20`, and appends it to an owner object's
array at `+0x98`/`+0xA0`. Nothing on the measured stop reads it, so it should not be in O7's row.

## 7. What was changed

`port/shims/startup_trace.h` - the `PALMETTO_POKE_LOG` memory dump was extended from "four qwords at `rax` and at
`rsi`" to a small table (`rax`/`rbx`/`rdx`/`r8`/`rdi` four qwords each, `rsi` twelve) because the container's fields
live in the second half of a 0x60-byte object and the descriptor travels in `rdx`. The diagnostic stays behind
`PALMETTO_POKE_LOG`, so an unset boot is unchanged; the baseline table in section 5 is the measurement that it is.
Nothing else in the repository was touched.

## 8. The smallest next change

Teach the format gate that `A8_UNORM` is a render-target format: in `DeviceState::texture_format()`
(`port/shims/d3d12_vulkan_device.cpp:957`) accept `A8_UNORM` for a 2D, single-sample description whose flags are
`ALLOW_RENDER_TARGET` and neither depth-stencil nor unordered access, returning the same `VK_FORMAT_R8_UNORM` colour
mapping the creation path already uses for A8 (`texture_storage_format`, line 374) - the view code already places an
A8 view's bits in the alpha channel (`A8_UNORM` swizzle at line 5067) and the PSO path already has the alpha-only
target case. Also report `D3D12_FORMAT_SUPPORT1_RENDER_TARGET` for A8 in `CheckFeatureSupport` (line 8145), so the
title's own feature queries agree with creation. Then re-measure: the expected signature of success is that the
`format not supported yet: 65` line disappears, the `int3` at `0xABFE1B` stops firing, a `CreatePlacedResourceX`
appears for this target, element 0 is non-null, and the boot passes `game+0xcc2861` to a new stop.

A second, smaller robustness change is worth making at the same time but is not sufficient alone: the slot-108 bridge
(`port/shims/d3d12_xs_device.cpp:2297`) should treat `SizeInBytes == UINT64_MAX` as "the host cannot size it" and
use its 64 KiB fallback instead of publishing D3D12's invalid allocation. Without the A8 fix, a 64 KiB answer only
moves the failure to the placed-resource creation, because the 3840 x 2160 target needs far more than that.

## 9. Limits

* One machine, one copy of 1.0.2.1, one Vulkan backend build, one measured boot per configuration.
* The class and field names are inferred from code shape, not symbols. "Render-target set" and "output object" are
  descriptions of the measured behaviour.
* The `ALLOW_RENDER_TARGET` flag on the failing description is derived statically (the flag-word path in section 4)
  and corroborated by the refusal itself: the only way `texture_format()` returns empty for this format is that rule,
  which requires one of those three flags, and the two non-attachment clauses (2D dimension, one sample) were
  measured as satisfied.
* Which render pass later binds these targets is not established here.
