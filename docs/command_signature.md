# Console command signatures at device slot 41

**Status: measured, implemented, tested, measured again.** At HEAD `9732aaa8a` the title's renderer device setup
`game+0x2478440` creates five one-argument command signatures through console device slot 41 and returns 0 at the third,
which the PC validation refuses with `0x80070057` `E_INVALIDARG`; the setup's caller then hits the title's `int3` at
`game+0x19f4f58`. This page records all five measured descriptions, decodes the console description, maps the
console-only argument type 105 to `DISPATCH`, and measures the setup returning 1: the `game+0x19f4f58` assert no longer
fires, the only remaining title assert is the `game+0x247af2d` range check, the boot stays exception-free, the frame loop
keeps running and the presented frames stay black. The fifth description is a `DISPATCH_MESH` signature, which this
layer cannot carry; it is answered with a named stand-in whose `ExecuteIndirect` is refused by name, and no
`ExecuteIndirect` naming it occurred in the measured window.

This slice follows [upstream_port.md](upstream_port.md): the public GPL-3.0 fork's slot-41 work is an evidence source and
a cross-check, not a code source. Everything here comes from our own disassembly, our own boot and our own tests.

## 1. Why these calls are needed

[issue1_baseline.md](issue1_baseline.md) section 6 attributes the title's last fatal-on-startup assert to this producer:
`game+0x19f4df0` calls the renderer device setup `game+0x2478440` and asserts when it returns 0. The setup returns 0 at its
first failing console **device slot 41** call; slot 41 is `CreateCommandSignature` at its PC position
`(desc, root signature, iid, out)`. The baseline's one-boot instrumentation measured call 1 and call 2 answering `S_OK`
and call 3 (return address `game+0x24795cc`) answering `E_INVALIDARG`, while the proxy forwarded the console description
to `ID3D12Device::CreateCommandSignature` unchanged; the layer's PC validation refuses an argument type it does not know.

The five signatures are the title's indirect-draw family, created once at startup: each is executed later through console
command-list slot 61 (`ExecuteIndirect`) with `MaxCommandCount` 1 (§2.3). With the setup returning 0 the rest of renderer
initialisation is skipped and the title's assert fires; nothing before this slice measured what the fourth and fifth
descriptions are, because the setup aborted at the third.

## 2. Measured shapes

### 2.1 The five descriptions

All five calls come from one straight-line block at `game+0x2479468..0x247952e`, each passing the same interface
(`IID_ID3D12CommandSignature`, the 16-byte constant at `image+0x61e0678`), a **null root signature** and the output
pointers into five adjacent image globals at `image+0x75e75d8..0x75e75f8`. Every description has `NumArgumentDescs` 1,
`NodeMask` 0 and one 8-byte `D3D12_INDIRECT_ARGUMENT_DESC`.

| # | call site | return address | description (stack) | ByteStride | argument type | out (image+) | answer at HEAD | answer with this slice |
|---|---|---|---|---|---|---|---|---|
| 1 | `game+0x247954e` | `game+0x2479554` | `rsp+0xd0` | `0x10` (16) | 0 `DRAW` | `0x75e75d8` | `S_OK` | `S_OK` (unchanged) |
| 2 | `game+0x247958a` | `game+0x2479590` | `rsp+0x78` | `0x14` (20) | 1 `DRAW_INDEXED` | `0x75e75e0` | `S_OK` | `S_OK` (unchanged) |
| 3 | `game+0x24795c6` | `game+0x24795cc` | `rsp+0x60` | `0xc` (12) | **105** (console-only) | `0x75e75e8` | `E_INVALIDARG` | `S_OK` (created as `DISPATCH`) |
| 4 | `game+0x2479605` | `game+0x247960b` | `rsp+0xb8` | `0xc` (12) | 2 `DISPATCH` | `0x75e75f0` | not reached (setup returned 0) | `S_OK` (unchanged) |
| 5 | `game+0x2479646` | `game+0x247964c` | `rsp+0xa0` | `0xc` (12) | 10 `DISPATCH_MESH` | `0x75e75f8` | not reached (setup returned 0) | `S_OK` (named stand-in, §3) |

Numbers on the wire, from the runtime probe (§2.4) and the decompile of `game+0x2478440`:

- `ByteStride` and the count are the first 8 bytes of the description (`[rsp+0xd0] = 0x0000000100000010`,
  `[rsp+0x78] = 0x0000000100000014`, `[rsp+0x60] = [rsp+0xb8] = [rsp+0xa0] = 0x000000010000000c`);
- the argument array pointer is the description's `+0x08` word (`rsp+0x1a0`, `rsp+0x190`, `rsp+0x180`, `rsp+0x170`,
  `rsp+0x160`);
- each argument's type word is its first dword: `0`, `1`, `0x69`, `2`, `0xa` (the `0x69` comes from
  `mov qword ptr [r9], 0x69` at `game+0x24794de`; the other four are single dword stores);
- the union after the type is zero for all five (8-byte argument description, 24-byte command-signature description with
  a zero `NodeMask` at `+0x10`).

The description layout read by the PC method is the public `D3D12_COMMAND_SIGNATURE_DESC`
(`ByteStride` +0x00, `NumArgumentDescs` +0x04, `pArgumentDescs` +0x08, `NodeMask` +0x10) and
`D3D12_INDIRECT_ARGUMENT_DESC` (`Type` +0x00, union +0x04). The measured strides equal the PC argument-struct sizes for
every type: 16 = `D3D12_DRAW_ARGUMENTS`, 20 = `D3D12_DRAW_INDEXED_ARGUMENTS`, 12 = `D3D12_DISPATCH_ARGUMENTS` and
12 = `D3D12_DISPATCH_MESH_ARGUMENTS`. Calls 1, 2, 4 and 5 are therefore ordinary PC descriptions; call 3 is
PC-shaped except for its type word.

### 2.2 What console argument type 105 is

`D3D12_INDIRECT_ARGUMENT_TYPE` ends at 11 on the PC (`INCREMENTING_CONSTANT`); 105 is outside it. The measured facts
that together identify it:

- its record is **12 bytes**, the size of `D3D12_DISPATCH_ARGUMENTS` — the same stride the `DISPATCH` (call 4) and
  `DISPATCH_MESH` (call 5) signatures use;
- its one measured `ExecuteIndirect` site (call `game+0x94445`, function `game+0x94360`) is a console list slot-61 call
  with `(signature, 1, argument-buffer VA, offset, null, 0)`, after its own deferred-state flush (`game+0x248c6b0`, the
  same shape the `DISPATCH` sites' `game+0x248c2a0` has);
- it is created in the same startup block, between the `DRAW_INDEXED` and `DISPATCH` signatures, all with the same
  no-root-signature shape.

The mapping taken is therefore **105 -> `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH`**. **HYPOTHESIS:** what the console does
differently for it (which command stream it feeds) is not identified; the public fork's measured row reads it the same
way (§4). The alternative — refusing it — is a measured negative (the setup returns 0 and the assert fires), so the
mapping is the only forward path the measurement supports.

### 2.3 The ExecuteIndirect sites

Each of the five signatures has one or two static execution sites, all console command-list slot 61
(`[object + 0x1e8]` = `ExecuteIndirect`), all passing `MaxCommandCount` 1 and an argument buffer VA plus an offset into
the caller's packet:

| signature | execute sites | decompiled function | shape |
|---|---|---|---|
| 1 `DRAW` | `game+0x248d198`, `game+0x248d33a` | `game+0x248d000`, `game+0x248d1b0` | `(sig, 1, packet VA, offset, null, 0)` |
| 2 `DRAW_INDEXED` | `game+0x248d4e8`, `game+0x248d68a` | `game+0x248d350`, `game+0x248d500` | same |
| 3 type 105 | `game+0x94445` | `game+0x94360` | same |
| 4 `DISPATCH` | `game+0x248cbba`, `game+0x248cd09` | `game+0x248ca90`, `game+0x248cbe0` | same |
| 5 `DISPATCH_MESH` | `game+0x4e33f91`, `game+0x555e976` | `game+0x4e33e10`, `game+0x555e800` | same |

The execute paths read the title's command packet (`+0x10` flags, `+0x14` a count/offset word, `+0x20` the argument buffer
VA), flush the deferred state and then call slot 61. No `ExecuteIndirect` reached the device proxy in the measured boot
(§5): the signatures exist for later passes.

### 2.4 Runtime probe

`PALMETTO_XS_SIG41=1` prints each slot-41 call once: caller, stride, count, node mask, root signature argument, the
argument-type words and the host's answer. Boot B printed (the caller is the title's fixed-base address; the runtime
publishes the title image base only later, so the probe's caller-text fallback names it by its raw address - minus the
image's fixed preferred base, `0x142479554` is `game+0x2479554`):

```text
[rt] d3d12_xs: slot 41 call at 0x142479554 (not the title image): stride 16 count 1 node 0x0 root 0x0 types 0 -> 0x00000000
[rt] d3d12_xs: slot 41 call at 0x142479590 (not the title image): stride 20 count 1 node 0x0 root 0x0 types 1 -> 0x00000000
[rt] d3d12_xs: slot 41: console argument type 105 (stride 12) has no PC value; created as D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH (docs/command_signature.md)
[rt] d3d12_xs: slot 41 call at 0x1424795cc (not the title image): stride 12 count 1 node 0x0 root 0x0 types 69 -> 0x00000000
[rt] d3d12_xs: slot 41 call at 0x14247960b (not the title image): stride 12 count 1 node 0x0 root 0x0 types 2 -> 0x00000000
[rt] d3d12_xs: slot 41: console DISPATCH_MESH description (stride 12) is created as a named dispatch-shaped stand-in; ExecuteIndirect with it is refused (mesh indirect replay is not implemented)
[rt] d3d12_xs: slot 41 call at 0x14247964c (not the title image): stride 12 count 1 node 0x0 root 0x0 types a -> 0x00000000
```

Every answer is `S_OK` after the mapping (call 3 answered `0x80070057` at HEAD); the type word `69` is the console
type 105, translated inside the call before the host sees it (the probe prints the console's own words).

## 3. What we implemented

- **Slot 41 is a proxy-handled branch** (`port/shims/d3d12_xs_device.cpp`, `proxy_create_command_signature`): the four
  PC-shaped descriptions are forwarded unchanged — the host receives the title's own pointer. When an argument type the
  measurement names has to be translated, the description is copied into a bounded stack array (at most 8 arguments) and
  the copy is forwarded, so the title's memory is never written. A type other than 105 or 10 is not translated: it goes
  to the host unchanged and the host's refusal is the answer, so an unmeasured console type cannot become a silent
  success. A description with more arguments than the bound, or one whose array is unreadable, is forwarded unchanged.
- **Type 105 becomes `DISPATCH`**, one `log_once` line names the mapping and the stride. The stride, argument count,
  `NodeMask`, root-signature argument and IID are unchanged.
- **`DISPATCH_MESH` is a named stand-in.** This layer's `CommandSignature` carries a draw / draw_indexed / dispatch kind
  only and the replay has no mesh indirect path, so the proxy copies that description with type `DISPATCH` (so a
  signature object exists and the title's setup can finish), logs once what it did, and registers the object it handed
  out. The command-list proxy's console slot 61 (`ExecuteIndirect`) refuses an `ExecuteIndirect` that names a registered
  stand-in, with one log line and the title's call-site RVA, instead of recording a dispatch the console would have run
  as mesh work; the list stays valid and other signatures are forwarded normally. No such execution happened in the
  measured window (§5).
- `slot_name(41)` and `device_slot_overridden(41)` declare the branch; `command_signature_standin` in
  `port/shims/d3d12_xs_device.h` is the test helper. The probe switch is `PALMETTO_XS_SIG41` (off by default).

## 4. Cross-check against the reference

The public GPL-3.0 fork `ubiquitarianism/leonida` carried the same slice in PR #377 (`28a32e190`), commits `c8247cdfa`,
`20936e845`, `4c04c0f4e` (author mrp1no; merged by aslan2142), and later added the mesh kind in `d1f8a6566` (author
NiiRoZz). Agreement and differences:

| Topic | Reference | This implementation | Why |
|---|---|---|---|
| Console type 105 | created as `DISPATCH`; the row "Indirect argument type 105" in its `docs/d3d12_vulkan.md` says the same and leaves the console difference **Open** | same mapping, our own proxy branch, bounded copy, probe and tests | **Agree on the measurement.** The mapped signature shares the record size, stride, creation block and execute shape above |
| Where the mapping happens | in its Xbox-ABI device proxy, before the layer's validation | same place, our function and comments | **Agree** |
| Fifth description (`DISPATCH_MESH`) | adds `IndirectKind::dispatch_mesh` to the layer (`runtime/src/command_list.h`), so creation succeeds on a mesh-capable device; `ExecuteIndirect` with it records nothing yet | scope for this slice is the proxy and the device file, not `command_list.h`/the replay; creation is answered with a named dispatch-shaped stand-in and `ExecuteIndirect` with it is refused by name | **Deliberate difference, recorded.** Both avoid running mesh work; ours names and refuses the execution instead of recording nothing |
| Doc row | `docs/d3d12_vulkan.md` "Indirect argument type 105" | this page | independent write-up |

## 5. A/B measurement

One machine, the repository boot lock, one boot at a time, the standard 130 s measurement recipe
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1`,
`PALMETTO_FILE_TRACE=1`, presenter captures every 2,000 frames up to 8) plus `PALMETTO_XS_SIG41=1` for the fix boots.

- **A (control)**: the tree at HEAD `9732aaa8a`, booted by the slice this page builds on and recorded in
  [issue1_baseline.md](issue1_baseline.md) section 5 (arm B; same machine, same recipe, 130 s).
- **B (fix 1)** and **C (fix 2)**: the same tree with the three changed source files, two boots, 130 s each. C is the
  final source: `d3d12_xs_device.cpp` SHA-256 `54F8C97D3F6103F7DE9E95F31E89AE5EAD81A568DC8DD8E61F9FDDFE232C00E9`,
  `d3d12_xs_device.h` `D9386122A5D2FFD02246721C7B318E66806A1EAB9BF5D80D051CD2C0219BCB7C`,
  `xs_proxy_mock_test.cpp` `63BEA8A0631462223FD858E49C99FF05C635C09B3BA5539862F222F5E68C7291`; `palmetto.exe`
  `1864FE173C95B6346ADDD265EF29371EF4EC5092EC15288F35B1DF2FB5DEE7B4`. B differs only in the probe's caller word
  (a log string, corrected after B and before C); its numbers are shown as the replicate.

| | A control | B fix 1 | C fix 2 |
|---|---|---|---|
| boot outcome | ALIVE, killed by the 130 s limit, 0 exceptions | ALIVE, killed by the 130 s limit, 0 exceptions | ALIVE, killed by the 130 s limit, 0 exceptions |
| `int3_skipped` | `0x247af2d` +4.81 s, **`0x19f4f58` +6.73 s** | **`0x247af2d` +4.58 s only** | **`0x247af2d` +4.52 s only** |
| startup trace events | 19,819 | 20,071 | 19,953 |
| `[d3d12-trace]` calls | 277,273 | 286,434 (max #288,912) | 268,172 (max #270,482) |
| timeline at +120 s | submits 11,101 / presents 5,550; compute 639 asked / 355 created | submits 11,744 / presents 5,871; compute 702 / 444 | submits 10,834 / presents 5,416; compute 882 / 535 |
| captures | frames 1 / 2,001 / 4,001, mean RGB 0.00 | frames 1 / 2,001 / 4,001 / 6,001, mean RGB 0.00 | frames 1 / 2,001 / 4,001, mean RGB 0.00 |
| trace events after the load | file opens 25, no DirectStorage/ReadFile repair event | file opens 121; repaired `ReadFile` first-touch at +25.2 s, repaired `IDStorageQueueX::EnqueueRequest` at +25.2 s and +104.9 s | file opens 121; repaired `ReadFile` first-touch at +24.9 s, repaired `IDStorageQueueX::EnqueueRequest` at +25.9 s and +95.3 s |
| verdict | setup returns 0, two title asserts | **setup returns 1: the `0x19f4f58` assert is gone**, the `0x247af2d` assert still fires, the boot advances to new streamed reads, and the frame loop keeps running black | same as B |

The five slot-41 answers are in §2.4. The `0x247af2d` assert is the separate `<= 2 MiB - 1` range check inside
`game+0x247ad10` (called from the same setup at `game+0x247942b` before the signature calls); it is unchanged by this
slice and remains the only title assert.

What did **not** move: the presented frames are still uniformly black (mean 0.00 in every capture, no non-black pixel in
any of B's 4 or C's 3 frames); no `ExecuteIndirect` reached the proxy. What did move beyond the assert: with the setup
returning 1 the title continues its renderer initialisation past the point the control arm resumed at, which opens 121
files instead of 25 and performs repaired first-touch reads after 24.9 s (a 271,044,608-byte `ReadFile` over 4,136
pages) and DirectStorage requests at +25 s and +95-105 s - the first post-load streaming activity measured on this
frontier. The two fix boots agree on the assert set, the file-open count and the post-load streaming; the submit,
present and compute numbers move run to run, as the baseline page records.

## 6. Limits

- **Type 105 is a hypothesis, not an identified method.** Its record size, stride and execute shape place it in the
  dispatch family, but which console command stream it feeds is not measured. Refusing it leaves the setup returning 0
  and the assert firing; the mapping is the measured forward path and the reference agrees.
- **The mesh stand-in is a model, not a mapping.** A console `DISPATCH_MESH` signature runs mesh-shader work; this
  implementation creates a dispatch-shaped stand-in so the title's setup can finish, and refuses (with a name and the
  caller RVA) any `ExecuteIndirect` that names it. No such execution was seen in the two measured boots, so the
  stand-in changed nothing in the window; a pass that executes it will lose that mesh indirect work until the layer
  gains `IndirectKind::dispatch_mesh` and a `vkCmdDrawMeshTasksIndirectEXT` replay — that is the next falsifier.
- **The copy is bounded** at 8 argument descriptions; a longer or unreadable description is forwarded unchanged and the
  host's answer (a PC validation refusal) is returned rather than a guessed translation.
- **The mapping is creation-only.** The console `ExecuteIndirect` argument shape (a GPU VA plus offset, where the PC
  method takes an `ID3D12Resource*`) is not translated here; no `ExecuteIndirect` was measured reaching the proxy in the
  130 s window, so that conversion is a later slice (the console list slot-61 mapping already exists).
- **The probe is opt-in** (`PALMETTO_XS_SIG41`, bounded to the calls this branch handles); the default boot prints only
  the one-time mapping and stand-in lines.
- **One machine, one 130 s control and two fix boots**; the timeline numbers move run to run (the same variation the
  baseline page records). The assert set and the streamed reads are the stable part of the result.
- **No game data**: this page has call-site RVAs, interface shapes, argument types, counts, HRESULTs and outcomes only.

## 7. Tests

[`port/test/xs_proxy_mock_test.cpp`](../port/test/xs_proxy_mock_test.cpp) extends the GPU-free proxy target with the
mock's PC slot 41 (it records the description the host is handed and refuses an argument type outside the PC range, as
the host does). Check `check_console_command_signature` (the target is **PASS, 0 failures**):

| Check | What it proves |
|---|---|
| the DRAW / 16 and DRAW_INDEXED / 20 descriptions reach the host as the title's own pointer and types | a PC-shaped console description is not copied or translated |
| the type-105 / 12 description reaches the host as the proxy's copy with type `DISPATCH`, stride 12, NodeMask 0 and root signature 0, and the title's description is unchanged | the measured field mapping, and that the title's memory is not written |
| a type-106 description reaches the host unchanged and the host's `E_INVALIDARG` is returned, with no output | no silent success for an unmeasured console type |
| the `DISPATCH_MESH` / 12 description creates an object the proxy marks as a stand-in (the host sees `DISPATCH`) | the fifth description is answered, not refused into the title's assert |
| `ExecuteIndirect` (console list slot 61) with the stand-in is not forwarded to the host's `ExecuteIndirect` (PC slot 59), and a normal signature is | mesh work is refused by name, not replayed as a dispatch |
| `device_slot_overridden(41)` | the slot is a declared override, not a forwarding entry |

`_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and the docs checks pass
(`tests/test_docs_env_vars.py` scans tracked Markdown, so it sees the new `PALMETTO_XS_SIG41` name once this page is
committed; no git writes are made by a slice).

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), files
`runtime/shims/d3d12_xs_device.cpp`, `runtime/shims/d3d12_vulkan_device.cpp`, `runtime/src/command_list.h` and
`docs/d3d12_vulkan.md`, commits `c8247cdfa`, `20936e845`, `4c04c0f4e` (mrp1no, PR #377), `28a32e190` (aslan2142) and
`d1f8a6566` (NiiRoZz). No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree.
