# The null object that stops the boot: a default render target the shim cannot size

**Status: measured on one machine, 2026-11-24, against the 1.0.2.1 target console executable and one `--run` of the
Vulkan backend. This page is static analysis of the image plus the startup trace of one boot; it is a measurement for
the boot frontier (RT-5), not a claim about the title's intent.** Nothing here quotes executable bytes, game data or
package addresses: only RVAs, register numbers, opcode names, structure field offsets and counts. The failing boot is
the one that stops at `game.exe+0xcc2861` (a read of `0x10`), after `PALMETTO_SKIP_INT3` steps over the
assert that should have caught it. [boot.md](boot.md) is the runbook; [retail_boot_chain.md](retail_boot_chain.md) is
the earlier stop list.

## 1. What the cached null object is

The object cached in the image global at `image+0x75EAC68` is an engine **GPU texture/resource object** — the
renderer's default colour resource: a render target with a clear value, 1×1 texel, one mip, format R8G8B8A8_UNORM
(`0x1C`), created with the white clear colour. The very next factory call in the same function creates its pair, a 1×1
depth resource in format D32_FLOAT_S8X24_UINT (`0x14`). Evidence for the render-target reading:

* `0x164DB0` writes the 16-byte value the caller passes (a broadcast of the float constant `1.0` from
  `image+0x5FC6ED8`) into the engine's per-render-target table at `image+0xA943AC0`, indexed by the object's word at
  `+0x12` (`0x164E80`..`0x164EAA`).
* `0x193DE10` builds a `D3D12_CLEAR_VALUE`: format from the object's copy of the descriptor (`object+0x3F`) at
  `0x193E070` and the colour floats into the union at `+4` (`0x193E089`..`0x193E0B5`); the depth branch reads a float
  and a stencil byte for the same struct (`0x193E0BE`..`0x193E0F8`).
* The object's class is created by `0x501F390` (class id `0x28`; vtable `image+0x61ECDB0`, second vtable
  `image+0x61ECDA0` linked from `object+0x28`/`+0x50`), and its descriptor is the 14-byte record the texture factory
  turns into a `D3D12_RESOURCE_DESC` (`0x4ECC940` and `0xC4B030`).

So the earlier "null pipeline" reading is not this object: the object is a resource, not a pipeline state. The
pipeline refusals of the surrounding pass are a different stop on a different path.

## 2. The chain, in RVAs

Absolute call targets in this image are image base + RVA, so the call the earlier notes name as `0x145204E60` is RVA
`0x5204E60`.

| # | RVA | What happens |
|---|---|---|
| 1 | `0x5BFE430` | The renderer startup function (`0x5BFE430`..`0x5BFF347`), called from `0x3750BB2`. It builds many resources through the same factory. |
| 2 | `0x5BFEAFB` | `0x501F4A0(rcx=&[rsp+0x162], …)` builds the 14-byte texture descriptor for the default colour resource. `rdi` is set to that address at `0x5BFEACC`. |
| 3 | `0x5BFEB8B` | `call 0x5204E60` (the factory thunk). Arguments: `rcx` = 64-bit key `0xAD8DF9F665B4D922`, `rdx` = `&descriptor` (the `rdi` above), `r8b` = 1, `r9d` = 2, and six stack words `(0, 0x28, 0, &[rsp+0x180], 0, 0)`. |
| 4 | `0x5204E60` | `jmp 0x34BEB0`: a thunk; 93 direct call sites reach the factory through it. |
| 5 | `0x34BEB0`..`0x34C146` | "Get the texture resource with this key, creating it if absent": validate, create, initialise, finalize. |
| 6 | `0x34BECF` | `call 0x2478140(rcx=descriptor)`: the descriptor gate of section 3. Returns true here. |
| 7 | `0x34BF88`..`0x34BFF1` | Creation path (`arg10` = 0 means "create"; an existing object would arrive there instead). `0x501F390` allocates the object, `0x501F330` stores the key in the object's list at `object+0x30`, `0x164DB0` copies the descriptor into `object+0x38`..`+0x45` and initialises the base fields through `0x320090`. |
| 8 | `0x34BFFC` | `call 0x193DE10(object, rdx=0)`: finalize — build, size and create the GPU resource. Returns `al` = success. |
| 9 | `0x34C001` / `0x34C003` | `test al,al; je 0x34C0C0`: failure goes to the destroy-then-null epilogue. |
| 10 | `0x34C0C0` | `call 0x4D9C130(object)`: destroy the partly built object (class id `0x28`). |
| 11 | `0x34C0C8` | `xor esi,esi`, then `0x34C0CA` `mov rax,rsi` and the epilogue: the factory returns 0. (This is the shared "return null" tail; the validator's own false branch at `0x34BED6` jumps straight here.) |
| 12 | `0x5BFEBA6` | `mov [image+0x75EAC68], rax`: the null is cached. It is the only store to that global. |
| 13 | `0x5BFEC10` | `call 0xCC2840(rcx=image+0x75EAC68, rdx=&[rsp+0x198], r8=image+0x75EAC80)`. `rcx` is loaded at `0x5BFEBD5`; `r8` at `0x5BFEBCE` (that global is only written at `0x5BFEC26`, after this call, so it is still 0 on the first pass). |
| 14 | `0xCC2840` | Entry: `mov rdi,rcx; test rcx,rcx; je 0xCC2936`. In the boot `rcx` = 0, so the branch is taken to an `int3` — the title asserting on the null object. |
| 15 | `0xCC2936` | `int3` (the assert). `PALMETTO_SKIP_INT3` steps over it, so the boot resumes at `0xCC2937` with `rdi` still 0. |
| 16 | `0xCC2937` | `jmp 0xCC2858`: the shared tail of the function body, reached without entering through the body's own entry. |
| 17 | `0xCC2858` / `0xCC2861` | `mov qword [rsp+0x28],0`; then `test byte ptr [rdi+0x10],4` with `rdi` = 0 — the access violation at `0xCC2861` reading `0x10`. |

The second factory call in the same function is `0x5BFEBC9` with key `0x0521E36B36FFACF9`, `rdx=&[rsp+0x154]`
(the depth descriptor), `r8b`=1, `r9d`=2; its result is cached in the *other* global `image+0x75EAC70` at `0x5BFEBE4`.
The held message attributed `0x0521E36B36FFACF9` to the store site; that key belongs to this second call, not to the
object cached at `image+0x75EAC68`.

## 3. The descriptor 0x2478140 validates

The descriptor is the engine's 14-byte texture record used by the factory. It is not a `D3D12_RESOURCE_DESC`: it is
the record that `0x4ECC940` and `0xC4B030` convert into one (the finalize uses the second through `0x193E06B`).
Field offsets, as read by `0x2478140`, `0x164DB0`, `0x4ECC940` and `0xC4B030`:

| Offset | Size | Meaning |
|---|---|---|
| `+0x0` | word | Width in texels (`0xC4B030` writes it to `D3D12_RESOURCE_DESC.Width` at `+0x10`). |
| `+0x2` | word | Height in texels (`+0x18` of the D3D12 desc). |
| `+0x4` | word | Depth for 3D, array size for 2D (`+0x1C`). |
| `+0x6` | byte | Kind/dimension; `0xC4B030` maps 0 → 1D, 1 (and any other value) → 2D, 3 → 3D. |
| `+0x7` | byte | Format, a DXGI-format number, copied to `D3D12_RESOURCE_DESC.Format` (`+0x20`). `0x14`, `0x28` and `0x37` are the depth formats and are rewritten to their TYPELESS equivalents before resource creation (`0xC4B29C`, `0xC4B36C`, `0xC4B390`). |
| `+0x8` | byte | Layout/format-aux byte; `0xFF` means "default". `0xC4B030` writes a console layout tag into `D3D12_RESOURCE_DESC.Layout` (`+0x2C`): `0x100` when the byte is 0, `0x102` for the depth/stencil shape, `0x10A` on the flagged path. `0xABFAD0` replaces a remaining 0 with `0x105` (261) on the console dimension — the value the shim reports as `UNKNOWN`. |
| `+0x9` | byte | Sample-count selector, one-based into the 17-entry table at `image+0x61EDB14`; `SampleDesc.Count` = 1 << table[byte-1] (`+0x24`). |
| `+0xA` | byte | Mip levels (`+0x1E`). |
| `+0xB` | byte | Flag byte 1; `0x501F4A0` sets 1 for the four accepted formats, otherwise `(value-1) < 13`. |
| `+0xC` | byte | Flag byte 2; `0x501F4A0` sets 1 only for formats `0x14` and `0x76`. |
| `+0xD` | byte | Resource-flag byte; bit 0 sets D3D12 flag `0x400000` (`0xC4B2BB`), bit 1 sets `0x800000` (`0xC4B2AB`). `0x501F4A0` always leaves it 0. |

The record is produced entirely by `0x501F4A0` and copied into the object by `0x164DB0` (`object+0x38` = bytes
`0..7`, `object+0x3E` = bytes `6..0xD`). Counts for the two descriptors in this chain (both built by `0x501F4A0` at
`0x5BFEAFB` / `0x5BFEB32` from constants computed in `0x5BFE430`):

* colour resource: width 1, height 1, depth/array 1, kind 1 (2D), format `0x1C`, layout hint `0xFF`, mip count 1,
  sample selector 3 or 2 (8× or 4×, chosen by comparing the renderer feature word `+0x4C4` against 8 at `0x5BFEAD4`),
  flags `+0xB`=1, `+0xC`=0, `+0xD`=0.
* depth resource: same extents and counts, format `0x14`, flags `+0xB`=1, `+0xC`=1, `+0xD`=0.

## 4. Validator `0x2478140`: exactly what it accepts

`0x2478140` is a leaf (`0x2478140`..`0x247819B`, no `.pdata` entry). It is also called from `0xCBE78F`, `0xCC26CF`,
`0xCC2C7F`, `0x1D11ADF` and two more sites, so it is the engine's general texture-descriptor gate. In pseudo-code
(branch order preserved):

```text
al = byte[rcx+0xD]
if word[rcx+0x0] == 0: return false                    # R1: zero width
if word[rcx+0x2] == 0: return false                    # R2: zero height
if (al & 2) == 0: return true                          # bit 1 clear: accepted
if (al & 1) != 0: return true                          # both bits set: accepted
# here (al & 3) == 2
f = byte[rcx+0x7]
if f <= 0x37 and bit f of 0x80010000100000: goto L1    # f in {0x14, 0x28, 0x37}
if f == 0x76: goto L1
goto L2
L1:
if byte[rcx+0xB] & 1: return true
L2:
if f != 0x14 and f != 0x76: return false               # R3
return (byte[rcx+0xC] & 1) != 0                        # R4
```

Accept conditions, in words: both extents non-zero, and either flag byte `+0xD` does not have bit 1 set, or it has
both bits set, or (bit 1 set alone) the format is one of `{0x14, 0x28, 0x37, 0x76}` with flag byte `+0xB` bit 0 set,
or the format is `0x14`/`0x76` with flag byte `+0xC` bit 0 set.

Refusal conditions are therefore only R1 (zero width), R2 (zero height), R3 and R4 — and R3/R4 need flag byte `+0xD`
with exactly bit 1 set, which the factory descriptor builder never produces: both of its branches join at `0x501F502`
and the single store of 0 is at `0x501F508`. **For any factory-built descriptor the validator is a
width/height-nonzero check; it returned true for the descriptor in this boot.** It is not the refusal that stops the
run.

## 5. Where the structure and arguments come from

At the call site `0x5BFEB8B` the second argument is `rdi`, which the caller sets at `0x5BFEACC` to
`&[rsp+0x162]` — a stack descriptor built a few instructions earlier by `0x501F4A0` at `0x5BFEAFB`, not one of the
enclosing function's parameters. The 64-bit key in `rcx` names the resource. Inside `0x34BEB0`:

* `rcx` (the key) is kept in `r12` and stored into the object through `0x501F330`.
* `rdx` (the descriptor) is validated, then passed to the finalize's callees and copied into the object.
* `r8b` = 1 reaches `0x164DB0` as its `r8d` and from there the base initialiser `0x320090` as its `r9d` arg.
* `r9d` = 2 becomes the object's word `+0x4C` (via `0x164DB0`'s stack argument at `0x164E4A`).
* Stack word 2 (`0x28`) is the factory class/mode handed to `0x501F390`, which allocates it with `0x4D9C140` (the
  pool allocator) and, through `0x501F470`, resolves the class.
* Stack word 4 (`&[rsp+0x180]`) is the 16-byte data `0x164DB0` writes into the per-render-target table (`0x164E98`).
* `arg10` = 0 selects the create path (`0x34BEFB`); a non-null `arg10` would select the "existing object" path.

The object created is therefore not an abstract handle: it is the engine's resource object for a GPU texture with a
render-target clear value, created empty (`0x193DE10` is called with `rdx` = 0, i.e. no upload/source data).

## 6. Why the failure path yields null

The null comes from `0x193DE10` returning `al` = 0, not from the validator. For this object (`rdx` = 0, so `rdi` = 0
inside the finalize) the taken path is:

* `0x193E02B`..`0x193E06B`: `0xC4B030` builds the `D3D12_RESOURCE_DESC` at `[rsp+0x90]` from `object+0x38`
  (Width, Height, DepthOrArraySize, MipLevels, Format, SampleDesc.Count, Flags and Layout).
* `0x193E089` / `0x193E0BE`: builds the `D3D12_CLEAR_VALUE` — colour floats for a colour target, a float plus a
  stencil byte for a depth target.
* `0x193E1CD`..`0x193E1DA`: since `rdi` = 0, `0xABFAD0(out=&[rsp+0x70], desc=&[rsp+0x90])` computes the resource's
  allocation info (section 7).
* `0x193E1E5` / `0x193E233`: the finalize reads `SizeInBytes` from `[rsp+0x70]` and `Alignment` from `[rsp+0x78]`.
* `0x193E243`: `0x2474690(size, alignment, …)` — the engine-heap aligned allocate. With a size of 0 it returns 0
  immediately (`0x24746C2`); otherwise it goes `0x221AD20` → `0x4FAC3C0`, and a failed allocation calls the
  out-of-memory handler `0x2219620` and returns 0.
* `0x193E248` / `0x193E24B`: `test rax,rax; je 0x193E310`; `0x193E310` is `xor eax,eax` — the finalize returns false.
* `0x193E2FC`..`0x193E310`: the other false path in the same function is a negative HRESULT from the device call at
  `0x193E2D4` (`[r10+0x428]`, Xbox device slot 133, `CreatePlacedResourceX`); the D3D12 trace of this boot has no
  failed call there, so this path is not the one taken.
* Back in `0x34BEB0`: `0x34C001` sees the false and takes `0x34C0C0` (destroy) → `0x34C0C8` (return 0).

## 7. The refusal that does happen: the console allocation query at device slot 108

`0xABFAD0` (entry `0xABFAE0`) is the engine's "resource allocation info" helper for the console path:

* It estimates a size from the descriptor's format and extents, then calls the device's console method at vtable
  offset `+0x360` — device slot 108. For a small non-attachment resource it first sets
  `D3D12_RESOURCE_DESC.Alignment` to `0x1000` (`0xABFD8E`) and calls at `0xABFDA9`; when the answer's alignment is
  not `0x1000` it clears the alignment and calls again at `0xABFE07`. The attachment/large path enters at `0xABFE07`
  directly (`0xABFDDA`..`0xABFDF3`). The call shape both times is the console struct-return order: `rcx` = concealed
  16-byte return buffer, `rdx` = device, `r8` = 1, `r9` = 1, stack = the `D3D12_RESOURCE_DESC` pointer and an
  auxiliary out pointer.
* After each call it validates the 16-byte answer: `[out+8] == 0x1000` decides the retry (`0xABFDB7`), and
  `dword[out+4] == 0` is asserted at `0xABFE15` / `0xABFE1B` (the upper half of `SizeInBytes`).
* The 16 bytes are then copied to the caller's `[rsp+0x70]` (`0xABFE0D`..`0xABFE11`).

The shim answers that console extension as `hr_to_u64(S_OK)` and, at most, writes a single 32-bit zero to the pointer
it treats as the out parameter, and only when that pointer lies within 16 MiB of its own frame. In this call the out
buffer travels in `rcx` and the device proxy in `rdx`, so the write is suppressed (the proxy pointer is not a stack
address); even when a write does happen it is 4 bytes, not `SizeInBytes` plus `Alignment`. The engine therefore never
receives a valid allocation info for this call: it reads stack contents that were not written with one. The measured
assert at `0xABFE1B` proves the upper half of `SizeInBytes` was non-zero, i.e. not even a 4-byte-zeroed struct.

The startup trace of the boot that stops at `0xCC2861` shows exactly this chain, in order, within 70 µs:

* `[rt] d3d12_xs: device slot 108 (forwarded) first call` and two `device slot 108 publishes 0 to its first argument`
  lines are the last `[rt]` lines before the stop;
* `int3_skipped` at `0xABFE1B` — the `dword[out+4] == 0` assert in the allocation-info helper fires, i.e. the engine
  saw an invalid allocation info;
* `int3_skipped` at `0x2219650` — the engine-heap out-of-memory assert (`0x2219620`, reached only from the allocate
  core `0x4FAC3C0` at `0x4FAC40E`) fires; the allocation of the bogus size failed;
* `int3_skipped` at `0xCC2936` — the null-object assert in the crash function, skipped;
* the access violation at `0xCC2861` reading `0x10`.

That is the causal signature: the texture/render-target resource cannot be sized because the console allocation query
is answered without its result, its allocation fails, the factory deletes it and returns null, the caller caches the
null, and the next consumer asserts on it.

For completeness, the standard `ID3D12Device::GetResourceAllocationInfo` (PC slot 25, handled by the same proxy) does
answer, but it reports the title's console layout tag `0x105` (261) as `UNKNOWN` in this boot, because the shim has no
console tiling model. That is a difference in the layout field of the `D3D12_RESOURCE_DESC`; it is not what returns
the null here, but it is the same console/PC difference one call away and worth checking once slot 108 is fixed.

## 8. The single most likely cause

**The shim's Xbox device extension slot 108 (vtable offset `+0x360`) is a stub that returns `S_OK` without filling the
concealed `D3D12_RESOURCE_ALLOCATION_INFO` return buffer.** The title's texture factory uses that console method, not
the PC `GetResourceAllocationInfo` (slot 25), to size a 1×1 render-target resource; it reads uninitialised stack,
passes a bogus `SizeInBytes` to its allocator, the allocation fails, `0x193DE10` returns false, `0x34BEB0` destroys
the object and returns 0, and the null is cached at `image+0x75EAC68` and dereferenced at `0xCC2861`.

This is not a descriptor the game's own validator rejects: the descriptor is built by the game, passes `0x2478140`,
and the refusing predicate is the shim's incomplete answer to the console method that follows.

## 9. How to confirm

1. **Read the trace signature.** Boot with `--trace out.jsonl` and `PALMETTO_SKIP_INT3=1`: the trace must end, in
   order, with `int3_skipped` at `0xABFE1B`, `int3_skipped` at `0x2219650`, `int3_skipped` at `0xCC2936`, then the
   exception at `0xCC2861`. That already happened on the measured run; it is the falsifier for any other causal story
   (a different refusal path would not produce the `0xABFE1B` assert immediately before the allocator's).
2. **Watch the slot-108 answer.** Add a temporary log to the slot-108 branch: the two pointers it receives, and the
   16 bytes at the console return buffer before and after. Expect the buffer unchanged, `[out+8] != 0x1000` on the
   first call (so the title retries) and `[out+4] != 0` (so `0xABFE1B` fires). The failing call is invisible in
   `--trace-d3d12` — console slots are not among the traced `ID3D12Device` methods — so the `[rt]` slot lines and the
   `int3` events are the only in-trace evidence.
3. **Answer it instead, then rerun.** Fill the concealed buffer with a real `D3D12_RESOURCE_ALLOCATION_INFO` for the
   `D3D12_RESOURCE_DESC` the caller passes (the same forwarding slot 25 already performs through the real device,
   with the console layout field cleared), or ensure `Alignment` is `0x1000` when the descriptor asks for it. The
   `0xABFE1B` and `0x2219650` asserts should disappear and the boot should pass `0xCC2861`; the next first-failure
   (next `int3`/`exception` event or next `[rt]` line) then names the next stop.
4. **Pin the API.** Once the buffer is filled, log the descriptor the caller passes to slot 108: it is the
   `D3D12_RESOURCE_DESC` built by `0xC4B030` (Dimension, Width/Height, DepthOrArraySize, MipLevels, Format,
   SampleDesc, Layout at `+0x2C`, Flags at `+0x30`). Comparing it with the descriptor of section 3 shows which
   consoles-only field the engine relies on.

## 10. Limits of this page

* One machine, one 1.0.2.1 copy, one Vulkan backend build; no symbol table — class names and the engine's intent are
  inferred from code shape, field use and the clear-value construction, not from symbols.
* The exact Xbox name of device slot 108 is not established (it is past the PC device table). What is measured: it is
  called with a concealed 16-byte return buffer, the device, two 1-words, a `D3D12_RESOURCE_DESC` pointer and an
  auxiliary out pointer, and its result is consumed as the resource allocation info.
* The flag bytes `+0xB`..`+0xD` are described mechanically (which instruction tests them and what they become in the
  `D3D12_RESOURCE_DESC`); the engine's intent for each combination is not established.
* The per-render-target table at `image+0xA943AC0` and the `1.0` clear colour are measured, but which render pass binds
  these default resources is not.
