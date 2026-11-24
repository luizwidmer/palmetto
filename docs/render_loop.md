# The render loop: closed lists, no draws, and the console submit at queue slot 35

**Status: measured on one machine, 2026-11-24**, against the retail 1.0.2.1 image with the shader corpus and the
Vulkan backend. Nothing here quotes image bytes, package paths or extracted tables: image-relative RVAs, call counts,
register values and the project's own wording only.

This page answers the frontier question "why does the title's frame loop record into command lists that are already
closed and never execute anything", and it corrects one earlier measurement: **the console's submit is queue slot 35,
not a clock-calibration call**, and the current answer for that slot writes into the title's own command-list array.

Predecessor pages: [boot_frontier.md](boot_frontier.md) (the boot that runs), [fence_slot12.md](fence_slot12.md)
(the waitable), [d3d12_command_objects.md](d3d12_command_objects.md) (the console list's inline recording model and
the measured console list-slot map), [d3d12_xs_surface.md](d3d12_xs_surface.md) (queue layout and `PresentX`).

## 1. Answers

**Q1 - why the lists are not re-opened.** The title never calls a command-list `Reset`: no list slot 10 is reached in
the whole boot (measured), and the frame loop's per-frame calls contain no such call. It does not need one on the
console: the recording memory of a console list belongs to its allocator, and a list the title created, configured
and closed during startup is recorded into again without any open call. Measured: two startup paths create lists and
close them immediately - the setup's five `(allocator, list)` pairs (`game+0x2478BD5..0x2478CDD`) and 620 one-shot
lists from `game+0x17F6CF0` - and the frame loop's first recording call lazily fetches one of those closed lists
through `game+0x188430`. Our PC backend enforces the PC rule (a closed list accepts
no recording and `Close` returns `E_FAIL`), so every per-frame recording call is silently dropped by the layer and
the following `Close` fails.

**Q2 - where submission happens.** Not `ExecuteCommandLists`: the queue's PC slot 11 is never reached (measured: the
queue slots the title uses are 0, 1, 16, 18, 32 and 35 only). The submit is **console queue slot 35**, called from
`game+0x2482660` as `queue->slot35(1, &lists[index], frame_token, 0)`, where `&lists[index]` is the same global
array (`image+0x75E7300`) that the startup setup filled with the command lists. `PresentX` is queue slot 32,
called from the same frame function at `game+0x24824C1`. The current proxy answers slot 35 as a "clock calibration
pair" and writes two `QueryPerformanceCounter` values into that list array; that is a mistranslation and it
overwrites command-list pointers. Section 6 has the evidence.

**Q3 - why the title does not detect the failures.** Every command-list recording method the title uses is `void`;
there is no HRESULT to test. The layer drops calls on a closed list before it does anything (several of them also
before their `d3d12_trace::record` line, which is why the trace shows only four list methods). The one place the
title does look is its frame's close helper (`game+0x7270`, `Close` at `game+0x729E`, caller return address
`game+0x72A1`): it tests the result and, on failure, marks its recording record failed and goes on. Measured
consequence: 168,653 failed `Close` calls, 84,327 presents and 84,112 allocator resets in 140 s - the loop never
stops. Also relevant: the slot-35 answer above is a *shim answer* the title trusts while the proxy corrupts the
array it was given.

**Q4 - the smallest change.** The frame cannot execute or present with one change: three things stand between the
loop and a real frame. In order of size and safety:

1. **Queue slot 35 must be the submit**: `ExecuteCommandLists(count, lists)` on the PC-list slots of the array
   entries (and stop writing the clock pair into the caller's array). This is the smallest change and it fixes an
   active corruption; it is also the answer to "where does submission happen".
2. **Console list lifecycle**: a list the title closes during startup is recorded into again without `Reset`. The
   measured emulation (reset the real list against its creating allocator before the first recording call that
   reaches it closed) makes the frame's recording real; with the barrier call muted, the frame's `Close` returns
   `S_OK` for every logged call (4,000 of 4,000). Without this every recording call is dropped.
3. **Console slot 146 barrier conversion**: once the recording is real, the first `Close` returns `E_INVALIDARG`
   because one of the frame's barriers is a texture barrier with no resource (`pResource` word 0), which the PC
   runtime refuses and the console accepts. Measured by muting slots one at a time: slot 146 alone reproduces the
   `E_INVALIDARG`; muting it removes it (section 7). The conversion (a resource-less texture barrier becomes a
   global barrier) is a measurement decision, not a guess: dump the title's barrier groups first.
4. `PresentX` (queue slot 32) is still refused; [xbox_present.h](../port/shims/xbox_present.h) already
   implements the presenter that would consume its planes.

Even after all four, the first executed frame draws nothing: the loop binds render targets, heaps, root signatures,
descriptor tables, viewports, scissors and vertex/index buffers and clears one target, but it never calls
`SetPipelineState` and never calls a draw (section 8). The first frame will be a clear-and-present frame until the
renderer's pipeline/draw path is measured.

## 2. How this was measured

Two independent instruments, both committed in this tree:

* `port/shims/d3d12_xs_device.cpp`, all environment-gated and off by default:
  * `PALMETTO_XS_CALLERS=1` logs the title's return address for the first four distinct call sites of every
    device/queue/fence/allocator/list proxy slot, with the four argument words of that call.
  * `PALMETTO_XS_CALLERS_PROFILE=1` adds per-site hit counts and prints the hottest sites every 20 s (a boot killed by
    its timeout has no shutdown summary; the per-frame sites are the ones whose counts keep growing).
  * `PALMETTO_XS_LIST_REOPEN=1` emulates the console list lifecycle: a recording call that reaches a closed real list
    first resets it against the allocator it was created with (section 7).
  * `PALMETTO_XS_LIST_MUTE=slot[,slot...]` answers those console list slots with 0 instead of forwarding them, to
    bisect which recorded call makes the list report `E_INVALIDARG`.
  * `PALMETTO_XS_LIST_DUMP=slot[,slot...]` prints the arguments and the first 0x40 bytes of the pointed-to structure
    of the first four calls of a console list slot.
* `tooling/inline_scan.py <image> --sites` for the title's own inline writers into the console list (section 8).

The return addresses are raw (the GPU-free test binaries do not link `shims`' module lookup); the boot's JSONL map
event names the image base (`0x140000000` in these runs), so `game+RVA` is `address - 0x140000000`. The RVAs below
were converted that way.

Boot environment for every measurement: `PALMETTO_SHADERS=<corpus>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time; 60-150 s per iteration (a boot runs to its timeout).

## 3. The frame loop, measured

The trace mix of one 140 s boot (probe boot, 2,238,851 traced calls, ALIVE, no exception): the title spends its time
in a loop whose shape per frame is (rate = calls / frame):

| call | per frame | caller return address |
|---|---:|---|
| `ID3D12GraphicsCommandList7::Barrier` (console list 146) | 5.0 | `game+0x32113` |
| list slot 72 (console-only) | 4.0 | `game+0x2482B37` |
| list 32 `SetDescriptorHeaps` | 3.0 | `game+0x4E4CAC9` |
| list 50 `OMSetRenderTargets` (console 50 -> PC 46) | 3.0 | `game+0x33317` |
| list 34 `SetGraphicsRootSignature` (console 34 -> PC 30) | 3.0 | `game+0x327CB` |
| list 70 (console-only) | 3.0 | `game+0x33396` |
| list 71 (console-only) | 2.0 | `game+0x4ED46AA` |
| list 139 (console-only) | 2.0 | `game+0x2474CD4` |
| queue 16 `Signal` | 2.0 | `game+0x248260E` |
| list 9 `Close` | 2.0 | `game+0x72A1` |
| list 20 `IASetPrimitiveTopology` | 2.0 | `game+0x325C0` |
| list 48 `IASetVertexBuffers` (console 48 -> PC 44) | 2.0 | `game+0x32966` |
| list 36 `SetGraphicsRootDescriptorTable` (console 36 -> PC 32) | 2.0 | `game+0x32206` |
| fence 12, allocator 9, queue 18, queue 35, queue 32, device 80, list 74, list 52, list 126, list 75, list 47, list 24, list 23, list 22, list 21, device 17, device 70, list 142, allocator 8 | 1.0 each | see the log |

Totals over that boot: 421,632 barriers, 252,981 `SetGraphicsRootSignature`, 252,975 `SetDescriptorHeaps`,
168,653 failed `Close`, 168,654 `CreateConstantBufferView`, 168,652 signals, 84,327 clears, 84,326 refused
`PresentX`, 84,325 slot-35 calls, 84,323 allocator slot 9 calls, 625 successful `CreateCommandList` and exactly 625
successful `Close` (all during startup). **No `ExecuteCommandLists`, no list `Reset`, no `SetPipelineState`, no
draw.**

The functions behind those addresses:

* `game+0x2481E90..0x2482653` - the frame/present function. It waits a frame event (device slot 80 at
  `game+0x2482405` and `game+0x2482448`), submits (via `game+0x2482660`, below), calls `PresentX` at
  `game+0x24824C1` with `(1, planes, params)`, and signals the queue at `game+0x2482608`.
* `game+0x2482660..0x2482695` - the submit helper: `queue->slot35(1, &[image+0x75E7300 + type*8], token, 0)`.
* `game+0x32520..0x33CB5` - the recording function of the renderer's frame record; it and its small wrappers
  (`game+0x320E0` barrier, `game+0x32130` root tables, `game+0x4E4CA80` heaps, `game+0x50A5EB0` clear) record into
  the list at `owner+0x48`.
* `game+0x188430..0x18857D` - "get the current recording list": called once per frame by the first recording wrapper
  when `owner+0x48` is null; it calls console allocator slot 9 and stores the list at `owner+0x48`.
* `game+0x7270..0x7323` - finish a recording: `Close` at `game+0x729E`, tests the result, marks the record failed
  and unlinks it.
* `game+0x4E9B530..0x4E9B614` - the allocator `Reset` wrapper (`game+0x4E9B57E`).

## 4. The startup setup creates five list/allocator pairs and closes every list

`game+0x2478440..0x2479682` is the renderer's device setup. It creates one pair before its tail loop (the list of
index 0, stored at `game+0x2478C0E`) and four more in the loop `game+0x2478C1C..0x2478CE4`:

```
device slot 9  CreateCommandAllocator       -> allocator
device slot 73 Xbox CreateCommandListX      -> list        (call at game+0x2478C70)
list console slot 132 (0x100000, 0, 0, ...)
list console slot 131 (0xC380, 0x20000, ...)
list console slot 9  Close                  (call at game+0x2478CC2)
[image+0x75E72A0 + i*8] = allocator        ; the pool's allocator array
[image+0x75E7300 + i*8] = list             ; the pool's list array
```

A second path creates and closes lists one at a time: `game+0x17F6CF0` (620 calls measured) does
`CreateCommandListX` and immediately `Close`, returning the closed list to its caller. Measured at startup: 625
`CreateCommandList` and 625 `Close` with `S_OK` - the 620 from that helper plus the five pairs. `game+0x72A1` closes
the lists of the frame loop's records and fails from the first frame on.

That is the whole lifecycle the title uses: **create, configure, close, keep for later recording**. The title calls
no list `Reset` at all.

## 5. Recording goes into a list that is already closed

The recording wrappers in `game+0x32520` use `[owner+0x48]` as "the current list". The first wrapper of a frame
finds it null and calls `game+0x188430`, which:

* picks an entry of its own table, calls console **allocator slot 9** with `(game+0x555DF10, owner)` - a code
  pointer (`game+0x555DF10` is a function that allocates a 2^n-byte recording buffer and stores it at
  `owner+0x12798`) and the owner. Our proxy returns `E_NOTIMPL` for that slot (84,323 times in 140 s) and the title
  ignores it; the call is a per-allocator buffer setup, not an open.
* looks the allocator up in a registry and stores the mapped list at `owner+0x48`.

Every later recording call of the frame goes to that list, which was closed during startup. In the layer the four
methods that record before their closed check (`Barrier`, `SetDescriptorHeaps`, `SetGraphicsRootSignature`,
`ClearRenderTargetView`) appear in the trace; the rest (fixed-function state, IA, `OMSetRenderTargets`, ...) return
immediately and silently. `Close` then reports `E_FAIL` because the PC list is closed.

So the trace is not "the title does nothing"; it is "the title records a frame that the PC backend throws away".
The console itself keeps that recording: its list object is a recording buffer (`+0x18` write pointer, `+0x28`/`+0x30`
limits, `+0x344` root-parameter headers - [d3d12_command_objects.md](d3d12_command_objects.md) section 3.5a) and the
recording memory belongs to the allocator. The list proxy already pins the write cursor at the limit so every
command takes the vtable path; that part of the translation is right. What is missing is the reopen rule.

## 6. The submit is queue slot 35, and the current answer corrupts the title's list array

`game+0x2482660` is the submit helper:

```
movzx eax, dl                     ; type byte from the frame record (game+0x2481FC8 loads it from record+8)
lea   rdx, [image+0x75E7300]      ; the list pool array
lea   r8,  [rdx + rax*8]          ; &lists[type]
mov   r9,  [image+0x75E0E88]      ; the frame-event token the frame function took from WaitFrameEventX
mov   rax, [rcx]                  ; rcx is the queue (r15 in game+0x2481E90)
mov   edx, 1
mov   dword ptr [rsp+0x20], 0
call  qword ptr [rax+0x118]       ; queue slot 35
```

The frame function calls it at `game+0x24821B4` and `game+0x2482493` with `rcx = r15`, the same object whose slot
`+0x100` is `PresentX` and whose slot `+0x80` is `Signal`. The array at `image+0x75E7300` is the one the startup
setup filled with the command lists (section 4). The third word is the frame token: `game+0x2482429` passes
`&[image+0x75E0E88]` to `WaitFrameEventX` as its token output, and the submit reads that word back. So the call is

```
queue->slot35(1, &lists[type], frame_token, 0)
```

which is the console's submit: `ExecuteCommandLists(NumCommandLists = 1, ppCommandLists)`, plus one console argument
(the frame token). The RVAs `game+0x2481E90` (frame function) and `game+0x2482660` (submit) are the proof that the
title submits and where.

The current proxy (from the previous frontier) answers slot 35 as a clock calibration pair: `out[0] = out[1] =
QueryPerformanceCounter` at the pointer in the second argument. That pointer is `&lists[type]`, so the answer writes
two timestamps over command-list pointers - exactly the kind of shim-answer corruption the earlier queue and fence
work was about. The call shape above is the measurement; the earlier "the caller reads two 64-bit values back"
reading does not hold at this call site (the helper reads nothing back after the call).

## 7. Measured experiments

All with the reopen probe unless stated. `Close` codes are logged by the probe (the trace only says "failed").

| experiment | measured result |
|---|---|
| baseline (no probes) | 625 `S_OK` startup closes, then every frame's two closes fail; ALIVE, no exception |
| `PALMETTO_XS_LIST_REOPEN=1` | the first frame's recording is real: first close `0x80070057` (`E_INVALIDARG`) on each of the two frame lists, then `0x80004005` (`E_FAIL`) for the rest |
| reopen + mute all forwarded recording slots (20,21,22,23,24,32,34,36,47,48,50,52,146) | no `E_INVALIDARG` |
| reopen + mute 32 only | `E_INVALIDARG` still (SetDescriptorHeaps is not the cause) |
| reopen + mute 32,34,36,52,146 | no `E_INVALIDARG` |
| reopen + mute 34,36,52,146 (only slot 32 forwarded) | no `E_INVALIDARG`; 4,000 of 4,000 logged closes `S_OK` |
| reopen + mute 36,52,146 (only slot 34 forwarded) | no `E_INVALIDARG`; 4,000 of 4,000 logged closes `S_OK` |
| reopen + mute 34,36,52 (only slot 146 forwarded) | **`E_INVALIDARG` on the first close of both frame lists** |

So the invalid recording call is the **console barrier at list slot 146**, and the lifecycle emulation alone is
enough for every frame to close cleanly (4,000 consecutive `S_OK` once the barrier call is muted). The `E_INVALIDARG`
is the PC runtime's validation, not the title's: after it, the PC list is closed with an error and the following
frames fail with `E_FAIL`, which is why a reopen without the barrier fix does not get further.

Why the barrier is rejected: the proxy forwards console list slot 146 to PC `ID3D12GraphicsCommandList7::Barrier`
(the mapping [d3d12_command_objects.md](d3d12_command_objects.md) section 3.3 measured on the BVH builder). A dump
of the arguments at the first calls shows a well-formed `D3D12_BARRIER_GROUP` (`Type = 1` = `TEXTURE`,
`NumBarriers = 1` or `2`, `pTextureBarriers` set) whose entries decode as 64-byte PC `D3D12_TEXTURE_BARRIER`
records:

```
SyncBefore 0x1000 (ALL_SHADING)   SyncAfter 0x40 (RENDER_TARGET)
AccessBefore 0x10                 AccessAfter 0x8
LayoutBefore 3 (RENDER_TARGET)    LayoutAfter 2 (GENERIC_READ)
pResource 0
Subresources.IndexOrFirstMipLevel 0xffffffff (all subresources), the other fields 0
```

`pResource` is 0. PC D3D12 refuses a texture barrier without a resource, so the layer's `Barrier` records an invalid
list (`check(false)`), and the next `Close` reports `E_INVALIDARG`. Another of the frame's texture barriers carries a
real resource and decodes to a depth-stencil-read -> generic-read transition; the resource-less one reads as a
"all textures" barrier, which the console evidently accepts and which has no direct PC equivalent. The exact
conversion rule is the next measurement on this path, not something to guess: dump the group at every count and map
`pResource == 0` to a `D3D12_GLOBAL_BARRIER` (the sync/access words are already PC values in the measured samples).

## 8. Why nothing is drawn (a separate, measured gap)

The frame binds render targets (`OMSetRenderTargets`, console 50, three times per frame), descriptor heaps, root
signatures and descriptor tables, a viewport, a scissor, blend factor, stencil reference, topology, vertex and index
buffers, creates constant-buffer views and clears one target. It never calls `SetPipelineState` (console 25) and
never calls a draw: `game+0x31D50` is the title's draw wrapper (console slots 12/13, `DrawInstanced` /
`DrawIndexedInstanced`) and `game+0x32520` has a `SetPipelineState` wrapper call at `game+0x327B6`, and **neither is
reached in the boot** - the trace records draw and `SetPipelineState` when they happen, and it has zero of either.

`tooling/inline_scan.py` on the retail image finds 110 inline-write sites into the console list, **all checked**
(none unchecked), with fallback vtable slots 20, 24, 35, 43, 45 and 47. So no draw is being lost to an unchecked
inline write: the renderer simply is not issuing draws in this loop. That is consistent with a clear-and-present
frame (a loading or "no scene yet" frame); why the renderer takes that branch is a separate measurement and is not
answered here.

## 9. Boot outcome

* Baseline: ALIVE, no exception, 5,195,024 traced calls in 300 s (the earlier owner's run) and 1,794,897 in 120 s in
  the final run of this work (the call count of a timeout-bounded run depends on machine load; the verdict and the
  call mix are the same). 0 `ExecuteCommandLists`; 625 list creations and 625 successful closes, all during startup.
* Probe boots: all ALIVE with no exception; the probe switches are off unless set, and the final baseline run above
  was made without any of them.
* Gates: `build_palmetto.cmd` -> `PALMETTO_OK`, `the test build script` -> `TESTS_BUILT`;
  `crash_unwind_test` (54 checks), `graphics_record_test` (101), `unimplemented_policy_test` (98),
  `xs_proxy_mock_test` pass. `d3d12_fence_timing_test` is the known pre-existing failure (0xC0000005);
  `d3d12_existing_heap_test` returns 1 on this host on its GPU/WARP comparison (it has no failing check line;
  it does not use the list proxy).

Nothing functional was changed: the only edits are the environment-gated diagnostics listed in section 2, so the
boot outcome is the one the frontier already had.

## 10. Smallest next change

**Make queue slot 35 the submit.** In the queue proxy's console slot 35 branch, take `(count, lists)` and forward
them to PC `ID3D12CommandQueue::ExecuteCommandLists` (PC queue slot 11) with every entry of the array unwrapped
(`proxy_real_object` already covers the list proxies), keep the third word only for a log line (it is the frame
token), and **delete the clock-pair write** into the caller's array. Expected measured result: the title's pooled
command lists keep their pointers, slot 35 stops being `E_NOTIMPL`-equivalent, and the frame's submission reaches
the backend. It makes nothing appear on screen by itself (the lists it executes are the recordings section 5
describes as dropped); it is the smallest correct step, and items 2 and 3 of section 1 are what make those
recordings worth executing.

After that, in order: the list lifecycle emulation (measured here, four `S_OK`-per-frame runs), the resource-less
texture barrier conversion (measured here, muted-slot bisection), and `PresentX` at queue slot 32
([xbox_present.h](../port/shims/xbox_present.h) is the prepared presenter). The first frame after those will be a
clear-and-present frame; the draw path of section 8 is the next frontier after that.
