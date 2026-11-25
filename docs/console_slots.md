# Console vtable slot map

This page is the durable inventory of the console D3D12 vtable slots the retail title's build
calls. For each console object it records the slot, the argument shape that was measured or
decompiled, the title caller's RVA, what the runtime does with the call today, what the called
function does where that is proven, and how strongly it is established.

The page exists because the console ABI is not the Windows SDK order. A console object's vtable
is the PC interface order with measured insertions and substitutions, and a slot number alone
does not name a method. A question like "what is device slot 70?" or "is console list slot 59
`BeginEvent`?" costs one boot of tracing plus one decompile - the point of
[tooling/ghidra/README.md](../tooling/ghidra/README.md) - and the answer belongs here, not in a
commit message.

## 1. How to read this page

**Slots.** A slot is the absolute vtable index including `IUnknown`, on x64, so the byte offset is
`slot * 8`. Slot 0 is always `QueryInterface`; the object's own first method sits after the
inherited chain (for the device and the command queue, slot 8; for the command list, slot 9).

**Caller RVAs.** RVAs are relative to the title image base. The port maps the title at the fixed
base `0x140000000` with `relocated: false`, so a caller address printed by the runtime probe minus
`0x140000000` is the RVA, and `0x140000000 + 0x32520` is the function start. The caller is the
**return address** - the instruction after the call - because that is what the runtime records,
and the containing function's range comes from the PE exception directory, not from a guess.

**Argument shapes.** The runtime logs the first four argument words of the console call. Unused
registers hold whatever the caller left there, so a word being non-zero does not make it an
argument. A shape in this page is what the decompiled call site sets up: "3 words (desc, IID,
out)" means the code loads those three registers before the call. Words printed as residue are
not part of the shape.

**Confidence.**

| Marker | Meaning |
|---|---|
| `measured` | the boot census saw this slot called, with this caller and these words |
| `decompiled` | the containing function was decompiled and the call site's argument setup was read; no boot of this build saw the call |
| `both` | both of the above agree |
| `unmodelled` | the call was seen or the function decompiled, but the method's identity or semantics is not proven - do not build on it without a new measurement |

**What a row does not claim.** A slot absent from a table was not called in the census run. That
is not proof it is never called: it may be cold on the measured path. A row marked `unmodelled` is
an observation, not an identification.

**The port's proxy tables.** The device, queue, allocator, fence and command-list proxies answer
with a table whose entry number is the console slot, so the probe names the slot the title's
compiler emitted. Any slot the table has no meaning for is answered with a logged refusal (or,
in the queue/fence/allocator tails, `E_NOTIMPL`). A slot past a proxy's table faults in a
reserved page, which the port's handler names.

## 2. How the measurements were taken

The census comes from a boot with the caller probe on
(`PALMETTO_XS_CALLERS=1`, `PALMETTO_XS_CALLERS_PROFILE=1`). The probe prints one line per
`(object, slot, call site)` the first time it is reached and a per-site hit count in the profile
dump. A caller address inside the title's mapping is a title RVA; a caller outside it (a shipped
DLL or the runtime's own module) is printed by its raw address and listed here as external.

The decompiles behind the `decompiled` and `both` rows were produced with
[tooling/ghidra/decompile_callers.py](../tooling/ghidra/decompile_callers.py), which takes the
census as input, resolves each caller RVA to its containing function through the PE exception
table, imports the title once with `-noanalysis` and decompiles exactly those functions. The
workflow is in [docs/dev-tools.md](dev-tools.md#console-vtable-slot-maps-with-ghidra).

One consequence of the port's list proxy is worth stating: it pins its fake console recording
cursor at the limit (`ListProxy::pin_cursor_at_limit` in `port/shims/d3d12_xs_device.cpp`), so the
title's command recorder always takes the console method path instead of writing raw inline
packets. The so-called "fallback" slots of the recorder (list 20/21/22/23/24/25/34/36/47/48/50,
and the console-only 70/75/126/142) are therefore the normal recording path, called tens of
thousands of times per boot, not an exceptional case.

## 3. Device (`ID3D12Device`-compatible console object)

The console device keeps the PC device order for the methods the title calls in the compatible
range, and deviates with console extensions past the PC table. The port forwards a slot the real
PC device implements, so the range the title never deviates in is not enumerated row by row; the
rows below are what the census or the decompiler established.

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0 | `QueryInterface` | 2 words (IID, out) | `0x2481cd5` | implemented: answers the modelled interfaces, refuses the rest by name | hands out a proxy, never the real object | measured |
| 1 | `AddRef` | - | external module (`0x7ff6...`) | implemented | proxy refcount | measured |
| 2 | `Release` | - | external module (`0x7ff6...`) | implemented | destroys the proxy at zero and releases the real device | measured |
| 8 | `CreateCommandQueue` | 3 words (desc, IID, out) | `0x1e6c2f2`, `0x247878d` | implemented: wraps the queue in a queue proxy | creates a PC queue and re-answers as the console queue | both |
| 9 | `CreateCommandAllocator` | 3 words (type, IID, out) | `0x161d0a3`, `0x17f6bd2`, `0x2478b30`, `0x2478c3e` | implemented: wraps a tagged allocator, hands a PC one through | creates the allocator; only a Vulkan-layer allocator is wrapped | both |
| 14 | `CreateDescriptorHeap` | 3 words (desc, IID, out) | `0x1c74542`, `0x1cf32fd`, `0x1f75fb4` | implemented: wraps the heap in a heap proxy | creates the heap; the proxy converts slots 9/10 to the Xbox RAX form | both |
| 15 | `GetDescriptorHandleIncrementSize` | 1 word (heap type) | `0x2478e45`, `0x2478e62`, `0x2478e74`, `0x5c5ff32` | forwarded | returns the PC increment size | both |
| 16 | `CreateRootSignature` | 5 words (node mask, blob, size, IID, out) | `0xfa0b3b` (and an external module) | forwarded | creates the root signature | both |
| 17 | `CreateConstantBufferView` | 2 words (desc, CPU handle) | `0x4d9f627`, `0x4da8f63` | forwarded | writes the CBV into the CPU descriptor | both |
| 18 | `CreateShaderResourceView` | 3 words (resource, desc, CPU handle) | `0x164f01` | forwarded | writes the SRV | both |
| 19 | `CreateUnorderedAccessView` | 4 words (resource, counter, desc, CPU handle) | `0x1647616` | forwarded | writes the UAV | both |
| 20 | `CreateRenderTargetView` | 3 words (resource, desc, CPU handle) | `0xcc29ea` | forwarded | writes the RTV | both |
| 21 | `CreateDepthStencilView` | 3 words (resource, desc, CPU handle) | `0x14a69e8` | forwarded | writes the DSV | both |
| 24 | `CopyDescriptorsSimple` | 4 words (count, dst, src, range) | `0x4ecc884` | forwarded | copies one descriptor; the measured call is `(1, dst, src, 1)` | both |
| 25 | `GetResourceAllocationInfo` | hidden return + 3 words (desc, 1, out) | `0x344145`, `0x344188` | forwarded with the thunk's register layout kept | answers the PC allocation info | both |
| 26 | `GetCustomHeapProperties` | hidden return + (node, heap type) | `0x2478713`, `0x2478737`, `0x247875e` | implemented: fills the caller's return buffer | answers the PC heap properties | both |
| 27 | `CreateCommittedResource` | 5 words (heap props, flags, desc, state, out) and more on the stack | `0x2481bd5` | forwarded | creates the resource | both |
| 36 | `CreateFence` | 4 words (initial value, flags, IID, out) | `0x1d0b300` | implemented: wraps the fence in a fence proxy | creates the fence and re-answers as the console fence | both |
| 39 | `CreateQueryHeap` | 3 words (desc, IID, out) | `0x2481b31` | forwarded | creates the query heap | both |
| 41 | `CreateCommandSignature` | 4 words (desc, root signature, IID, out) | `0x2479554`, `0x2479590`, `0x24795cc` | forwarded | creates the command signature | both |
| 48 | `OpenExistingHeapFromAddress` (PC index; console `CreateCommandQueueX`) | 3 words (desc, IID, out) | `0x2478819`, `0x2478859`, `0x2478899`, `0x24788d9` | implemented: named console core, wraps the queue in a queue proxy | creates a console queue; the port builds a PC queue | both |
| 55 | `CreateReservedResource1` (PC index; console `CreateDerivedPipelineStateX`) | record pointer, out | `0x247a5c8` | implemented: record converts to a PC graphics pipeline (see [device slot 55](device_slot55.md)) | console derived-pipeline creation | both |
| 57 | `CreateLifetimeTracker` (PC index; console `CreateGraphicsPipelineState`) | record pointer, out | `0x247757e` | implemented: the record is probed and converted (see [slot 57 wiring](slot57_wiring.md)) | console graphics pipeline creation | both |
| 61 | `CreateMetaCommand` | 2 words (pointer, pointer) | `0x12b981e` | refused by name with `E_NOTIMPL` | console call has the PC method's name but not its argument shape | measured |
| 62 | `CreateStateObject` (PC index; console `CreateComputePipelineState`) | 5 words (desc, 0, 0, IID, out) | external module `0x18000c4e4` (shipped BVH builder, base `0x180000000`) | implemented: converts to PC slot 11 | console compute-PSO creation | measured |
| 67 | `CreateProtectedResourceSession1` (PC index; console `DeserializeComputePipelineStateX`) | 3 words (console desc, IID, out) | `0x24800db` | implemented: routes to PC compute-PSO creation | deserializes a console compute pipeline description | both |
| 70 | `CreatePlacedResource1` (PC index; console descriptor publication) | 3 words (buffer desc, count, destination) | `0x4d9f369`, `0x4ecc821` | implemented: buffer-only publication, texture-shaped descriptions refused by name | writes a descriptor record; see [device slot 70](device_slot70.md) | both |
| 73 | `CreateShaderCacheSession` (PC index; console `CreateCommandListX`) | 5 words (desc, allocator, initial state, IID, out) | `0x17f6d3b`, `0x2478b77`, `0x2478c76` | implemented: PC `CreateCommandList` with the console desc's type | creates the command list | both |
| 74 | `ShaderCacheControl` (PC index) | 3 words (pointer, integer, pointer) | `0x1e6c2c1` | forwarded to PC slot 74 by index | not identified: the measured three-word console call does not fit the PC two-word method | measured |
| 75 | `CreateCommandQueue1` | - | not called in this census | implemented: wraps the queue in a queue proxy | creates a queue with a creation context | unmodelled |
| 76 | `CreateCommittedResource3` (PC index; console resource creation) | 7 words (heap props, heap flags, desc, initial state, clear value, IID, out) | not called by the title in this census (the shipped FSR DLL calls it) | implemented | console committed-resource creation, including the `0x101` tiled layout | measured (in the FSR dispatch test, [d3d12_vulkan.md](d3d12_vulkan.md)) |
| 78 | `CreateReservedResource2` (PC index; console `SetFrameIntervalX`) | 4 words (output, interval, latency, flags) | `0x247abde` | implemented: records interval and latency, `S_OK` | vblank interval for the pacer; nothing paces yet | both |
| 79 | `CreateSampler2` (PC index; console `ScheduleFrameEventX`) | 4 words (event, offset, reserved, flags) | `0x2478dea`, `0x247ac02` | implemented: records the scheduled event, `S_OK` | schedules a frame event | both |
| 80 | `GetResourceAllocationInfo3` (PC index; console `WaitFrameEventX`) | 5 words (event, timeout, reserved, flags, token out) | `0x248240b`, `0x248244e` | implemented: issues a fresh non-zero token and reports it to the presentation bridge | waits for a frame event; the console call is the title's frame clock | both |
| 81 | `OpenExistingHeapFromAddress1` (PC index) | 4 words (address word, size, out, out) | `0x2482716` | stubbed with `S_OK`/zero when past the PC table (the port names it) | not identified; a console extension at this index | measured |
| 86 | - (past the PC table) | 3 words (pointer, pointer, pointer) and a zero | `0x2479a8d` | stubbed with `S_OK`/zero and logged once | not identified; a console extension | measured |
| 108 | - (past the PC table; console `GetResourceAllocationInfo` order) | hidden return + (device, 1, 1, description) | `0xabfdaf`, `0xabfe0d` | implemented: asks the PC device and writes the console struct-return layout, 64 KiB fallback | sizes a resource description in the console's struct-return order | both |
| 109 | - (past the PC table) | - | not called in this census | implemented: `S_OK` and publishes zero into the caller's stack word | console extension measured like 108 | measured (in an earlier boot) |
| 127 | - (past the PC table; console committed resource with a barrier layout) | 8+ words (heap props, count, desc, state, barrier group, ...) | `0x193e1ba` | implemented: creates the display surface resource from the barrier layout | creates a resource whose initial layout is a CPU barrier group | both |
| 133 | - (past the PC table; console `CreatePlacedResourceX`) | 3+ words (title VA, desc, IID/out) | `0x6dd8b`, `0x193e2db`, `0x1e86853` | implemented: creates a buffer on the title's pages, pages hook runs | creates a placed resource at a title-chosen address | both |

Slots 3-7, 10-13, 22-23, 28-35, 37-38, 40, 42-47, 49-54, 56, 58-60, 63-66, 68-69, 71-72 and 77
were not called by the title in the census run or are forwarded PC-identically; the proxy answers
exact PC order for them and the real device's table bounds what it can answer.

## 4. Command queue

The console queue inserts two methods before `Signal`. Measured: `Signal` is console slot 16 and
`GetDesc` is console slot 20, so from `Signal` on the console slot is the PC slot plus two. The
inserted methods (slots 14 and 15) have not been called by the title yet.

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0 | `QueryInterface` | - | first call seen, no caller line | implemented | proxy identity | measured |
| 1, 2 | `AddRef`, `Release` | - | first calls seen | implemented | proxy refcount | measured |
| 3-7 | object/device-child chain | - | not called | forwarded PC-identically | PC answers | unmodelled |
| 8, 9 | `UpdateTileMappings`, `CopyTileMappings` | - | not called | forwarded PC-identically | PC answers | unmodelled |
| 10 | `ExecuteCommandLists` | - | not called | forwarded, list proxies unwrapped | PC answers | unmodelled |
| 11, 12, 13 | `SetMarker`, `BeginEvent`, `EndEvent` | - | not called | forwarded PC-identically | PC answers | unmodelled |
| 14, 15 | - (console inserted methods) | - | not called | forwarded by index to PC `Signal`/`Wait` | unknown; the inserted methods sit before the console `Signal` at 16 | unmodelled |
| 16 | `Signal` (console) | 2 words (fence proxy, value) | `0x248260e` | implemented: unwraps the fence, forwards to PC `Signal`, refreshes the fence cell | signals a fence from the GPU queue | both |
| 17 | `Wait` (console) | - | not called | forwarded to PC slot 15 | PC answers | unmodelled |
| 18 | `GetTimestampFrequency` (console) | 1 word (`UINT64*` out) | `0x24841ac` | forwarded to PC slot 16 | reports the GPU clock rate | both |
| 19 | `GetClockCalibration` (console) | - | not called | forwarded to PC slot 17 | PC answers | unmodelled |
| 20 | `GetDesc` (console) | hidden return | not called | forwarded to PC slot 18, struct-return unchanged | returns the queue description | unmodelled |
| 21-31 | - | - | not called | refused with `E_NOTIMPL`, logged once ("past the public chain") | unknown console methods, if any | unmodelled |
| 32 | - (console `PresentX`) | 3 words (plane count, plane array, present parameters) | `0x24824c7` | implemented: the presentation service presents the planes (see [present wiring](present_wiring.md)) | presents a frame; the title has no swapchain | both |
| 33, 34 | - | - | not called | refused with `E_NOTIMPL` | unknown; `SuspendX`/`ResumeX` live past the public chain with no proven slot | unmodelled |
| 35 | - (console submit) | 4 words (count, command-list array, frame token, reserved) | `0x24821e3`, `0x248268f` | implemented: forwards the array to PC `ExecuteCommandLists` | submits command lists with the frame token; there is no PC `ExecuteCommandLists` call in the boot | both |
| 36-127 | - | - | not called | refused with `E_NOTIMPL` | unknown console methods | unmodelled |

## 5. Command allocator

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0-2 | `QueryInterface`, `AddRef`, `Release` | - | `Release` called from the title (`0x17f6c68`, `0x1f75e23`) | implemented | proxy identity | measured |
| 3-7 | object/device-child chain | - | not called | forwarded PC-identically | PC answers | unmodelled |
| 8 | `Reset` | no meaningful words (the logged word is residue) | `0x4e9b57e` | forwarded | resets the allocator for the next list | both |
| 9 | - (console callback registration) | 2 words (function pointer, context) | `0x188501` | refused with `E_NOTIMPL`, logged once as "beyond the PC allocator vtable" | decompiled caller `FUN_140188430` registers a callback on the allocator it just created and ignores the result; called for every allocator creation | both |
| 10-63 | - | - | not called | refused with `E_NOTIMPL` | unknown | unmodelled |

## 6. Fence

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0-7 | object/device-child chain | - | not called | implemented/forwarded | proxy identity | unmodelled |
| 8 | `GetCompletedValue` | no arguments | `0x4dad09b` | forwarded; its result is also the proxy's cell value | reads the completed GPU value | both |
| 9 | `SetEventOnCompletion` | - | not called | forwarded | PC answers | unmodelled |
| 10 | `Signal` | - | not called | forwarded; also publishes the cell | advances the fence from the CPU | unmodelled |
| 11 | `GetCreationFlags` (`ID3D12Fence1`) | - | not called | forwarded when the real fence has it | PC answers | unmodelled |
| 12 | - (console completed-value address) | 3 words (target, continuation, flags) | `0x4fb0aef` | implemented: returns the address of a 64-bit cell carrying the completed value (see [fence slot 12](fence_slot12.md)) | hands the title a CPU-visible completed-value word it polls with `monitorx`/`mwaitx` | both |
| 13-63 | - | - | not called | refused with `E_NOTIMPL` | unknown | unmodelled |

## 7. Descriptor heap

The title's heap objects come from device slot 14 and are wrapped by the heap proxy. The census
does not instrument heap slots, so this table's evidence is the shipped FSR DLL's measured calls
and the port's own probes (see [d3d12_command_objects.md](d3d12_command_objects.md) and
[d3d12_vulkan.md](d3d12_vulkan.md)).

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0-2 | `QueryInterface`, `AddRef`, `Release` | - | - | implemented | proxy identity | measured (port tests) |
| 3-6 | `ID3D12Object` | - | - | forwarded | PC answers | unmodelled |
| 7 | `GetDevice` | - | - | implemented: answers the device proxy | returns the Xbox-ABI device | measured (port tests) |
| 8 | `GetDesc` | hidden return | - | forwarded | PC answers | unmodelled |
| 9 | `GetCPUDescriptorHandleForHeapStart` | no arguments; handle returned in RAX | - | implemented: the Xbox RAX convention | returns the CPU handle | measured (FSR DLL, [d3d12_vulkan.md](d3d12_vulkan.md)) |
| 10 | `GetGPUDescriptorHandleForHeapStart` | no arguments; handle returned in RAX | - | implemented: the Xbox RAX convention | returns the GPU handle | measured (FSR DLL, [d3d12_vulkan.md](d3d12_vulkan.md)) |

## 8. Present and swapchain

There is no separate console swapchain object in this title: it creates no `DXGI` swapchain and
calls no `CreateSwapChain*`/`EnumOutputs` ([display_path.md](display_path.md)). Presentation is
three slots on objects already in the tables above:

| Object | Slot | What it does | Confidence |
|---|---:|---|---|
| queue | 32 | `PresentX(planeCount, planes, parameters)` - the frame is presented | both |
| device | 78 | `SetFrameIntervalX(output, interval, latency, flags)` - the frame interval for the pacer | both |
| device | 79 | `ScheduleFrameEventX(event, offset, reserved, flags)` - schedules the frame event | both |
| device | 80 | `WaitFrameEventX(event, timeout, reserved, flags, &token)` - the frame clock and token issue | both |

The wiring, the window choice and the frame-token bridge are in
[present_wiring.md](present_wiring.md). The queue also has console slot 35 as its submit, which
is easy to confuse with a second present: slot 35 takes a command-list array, not a plane array
(see section 4).

## 9. Command list

The console command list is the object with the most measured deviation. Up to slot 26 the console
order matches the PC `ID3D12GraphicsCommandList`; slots 27-30 are console-only insertions; from
slot 31 the console slot is the PC slot minus four until slot 58; slots 59-61 are the PC
`BeginEvent`/`EndEvent`/`ExecuteIndirect` (the console has no `SetMarker`); and past 61 the
console adds methods the PC chain does not have. `console_to_pc_list_slot` in
`port/shims/d3d12_xs_device.h` is the mapping, and `port/test/xs_proxy_mock_test.cpp` asserts it.

The recorder reaches most recording calls through its inline-writer fallbacks (section 2), so the
slots below are the console method numbers the title's compiler emitted, not the port's own
choice.

| Slot | PC method at that index | Measured shape | Title caller RVA | Port handling | What the called function does | Confidence |
|---:|---|---|---|---|---|---|
| 0-2 | `QueryInterface`, `AddRef`, `Release` | - | not called directly | implemented | proxy identity | unmodelled |
| 7 | `GetDevice` | - | not called | implemented: answers the device proxy | returns the Xbox-ABI device | unmodelled |
| 8 | `GetType` | - | not called | forwarded | PC answers | unmodelled |
| 9 | `Close` | no meaningful words (the logged words are residue) | `0x72a1`, `0x17f6d4b`, `0x2478bd0`, `0x2478cc5` | forwarded to PC `Close`; a `S_OK` marks the proxy closed | closes the list; the decompiled caller tolerates a removed device and tears the list pool down | both |
| 10 | `Reset` | (allocator, initial state) | not called directly | forwarded; the proxy also pins the recorder's cursor at its limit | resets the list and the fake recording buffer | unmodelled |
| 11 | `ClearState` | - | not called | forwarded | PC answers | unmodelled |
| 12 | `DrawInstanced` | 4 words (vertex count per instance, instance count, start vertex, start instance) | `0xbff06` | forwarded PC-identically | records a draw; the decompiled function is the recorder's draw path | both |
| 13 | `DrawIndexedInstanced` | - | not called | forwarded | PC answers | unmodelled |
| 14 | `Dispatch` | 3 words (x, y, z) | measured in the FSR/BVH clients | forwarded PC-identically | records a dispatch | measured (external clients) |
| 15 | `CopyBufferRegion` | 5 words (dst, dst offset, src, src offset, size) | measured in the FSR client | forwarded PC-identically | records a buffer copy | measured (external client) |
| 16 | `CopyTextureRegion` (PC index; console identity unproven) | - | not called in this census | forwarded to PC slot 16 | the console's own texture copy is measured at slot 85, so what the console has at 16 is not established | unmodelled |
| 17-19 | `CopyResource`, `CopyTiles`, `ResolveSubresource` | - | not called | forwarded PC-identically | PC answers | unmodelled |
| 20 | `IASetPrimitiveTopology` | 1 word (topology; measured 4 = `TRIANGLELIST`) | `0x325c0` | forwarded to PC slot 20 | sets the topology; the decompiled recorder writes the same state as an inline packet before falling back | both |
| 21 | `RSSetViewports` | 2 words (count, viewport array) | `0x33437` | forwarded to PC slot 21 | sets the viewports | both |
| 22 | `RSSetScissorRects` | 2 words (count, rect array) | `0x33409` | forwarded to PC slot 22 | sets the scissor rectangles | both |
| 23 | `OMSetBlendFactor` | 1 word (4-float array pointer) | `0x32a0c` | forwarded to PC slot 23 | sets the blend factor | both |
| 24 | `OMSetStencilRef` | 1 word (stencil ref; measured 0) | `0x329a1` | forwarded to PC slot 24 | sets the stencil reference | both |
| 25 | `SetPipelineState` | 1 word (pipeline state pointer) | `0x327bc` | forwarded to PC slot 25 | binds the pipeline; the recorder also writes the pipeline's fields into the list's console body at `+0x544` | both |
| 26 | `ResourceBarrier` | 2 words (count, barrier array) | measured in the FSR/BVH clients | forwarded PC-identically | records barriers | measured (external clients) |
| 27-30 | - (console insertions) | - | not called by the title in this census | answered `0` (void) and not forwarded | unknown console methods; slot 29 is called by the title's copy path with a 4-word shape (`0x142482ae0`) and slot 30 is never seen | unmodelled |
| 31 | `ExecuteBundle` (PC slot 27) | - | measured in the FSR client | forwarded to PC slot 27, bundle unwrapped | records a bundle | measured (external client) |
| 32 | `SetDescriptorHeaps` (PC slot 28) | 2 words (count, heap array) | `0x4e4cac9` | forwarded to PC slot 28, heap proxies unwrapped | binds descriptor heaps | both |
| 33 | `SetComputeRootSignature` (PC slot 29) | 1 word | measured in the FSR client | forwarded to PC slot 29 | binds the compute root signature | measured (external client) |
| 34 | `SetGraphicsRootSignature` (PC slot 30) | 1 word (root signature pointer; measured 0 = NULL) | `0x327cb` | forwarded to PC slot 30 | binds the graphics root signature | both |
| 35 | `SetComputeRootDescriptorTable` (PC slot 31) | 2 words (index, GPU descriptor handle) | measured in the FSR client | forwarded to PC slot 31 | binds a compute descriptor table | measured (external client) |
| 36 | `SetGraphicsRootDescriptorTable` (PC slot 32) | 2 words (index, GPU handle) | `0x32206` | forwarded to PC slot 32 | binds a graphics descriptor table | both |
| 37-40 | `SetCompute/GraphicsRoot32BitConstant(s)` (PC slots 33-36) | - | not called as slots; the recorder inlines them | forwarded by the -4 mapping when a recorder falls back | sets root constants | unmodelled |
| 41-46 | root CBV/SRV/UAV setters (PC slots 37-42) | 2 words (index, GPU VA) when measured | measured in the FSR client (41) | forwarded by the -4 mapping | binds a root view | measured (external client) |
| 47 | `IASetIndexBuffer` (PC slot 43) | 1 word (index-buffer view pointer; measured 0 = NULL) | `0x328be` | forwarded to PC slot 43 | binds the index buffer | both |
| 48 | `IASetVertexBuffers` (PC slot 44) | 3 words (start slot, count, view array) | `0x32966` | forwarded to PC slot 44 | binds vertex buffers | both |
| 49 | `SOSetTargets` (PC slot 45) | - | not called | forwarded to PC slot 45 | PC answers | unmodelled |
| 50 | `OMSetRenderTargets` (PC slot 46) | 3+ words (count, view array, single-handle flag) | `0x33317` | forwarded to PC slot 46 | binds render targets | both |
| 51 | `ClearDepthStencilView` (PC slot 47) | - | not called | forwarded to PC slot 47 | PC answers | unmodelled |
| 52 | `ClearRenderTargetView` (PC slot 48) | 4 words (GPU handle, CPU handle, resource, colour) | `0x50a62de` | implemented: cleared by translation (an empty target view is refused, a real one forwarded) | clears the target | both |
| 53 | `ClearUnorderedAccessViewUint` (PC slot 49) | 6 words | measured in the FSR client | forwarded to PC slot 49 | clears a UAV | measured (external client) |
| 54 | `ClearUnorderedAccessViewFloat` (PC slot 50) | 6 words | measured in the FSR client | forwarded to PC slot 50 | clears a UAV | measured (external client) |
| 55 | `DiscardResource` (PC slot 51) | 2 words (resource, rect) | measured in the FSR client | forwarded to PC slot 51 | discards a resource | measured (external client) |
| 56-58 | (PC slots 52-54 under the -4 mapping) | - | not called | forwarded to PC slots 52/53/54 | query methods by the mapping; not identified at runtime | unmodelled |
| 59 | - (console `BeginEvent`) | 3 words (metadata, data, size) | measured in the FSR client (profiling-gated) | forwarded to PC slot 55 `SetPredication` - a mapping error (section 10) | begins a GPU event; the shape and the profiling gate are what identify it | measured (external client) |
| 60 | `EndEvent` (PC slot 58) | 2 words | measured in the FSR client | forwarded to PC slot 58 | ends the GPU event | measured (external client) |
| 61 | `ExecuteIndirect` (PC slot 59) | 6 words | measured in the FSR client | forwarded to PC slot 59 | records an indirect dispatch | measured (external client) |
| 62-69 | - | - | not called | answered `0` (void), not forwarded | unknown console methods | unmodelled |
| 70 | - (console `WriteBufferImmediate` shape) | 2 words (count, `D3D12_WRITE_BUFFER_IMMEDIATE_DATA` array) | `0x33396` | answered `0` (void), not forwarded | the recorder's immediate-write fallback; shape matches PC slot 66 `WriteBufferImmediate` but the console slot is not proven to be it (section 10) | both |
| 71 | - | 4 words (handle, size, 0, 0) | `0x4ed46aa` | answered `0`, not forwarded | decompiled caller `FUN_144ed4670` calls it with a console object handle and a zero-size payload | decompiled |
| 72 | - | 4 words (pointer, 0, 0, 1) | `0x2482b37` | answered `0`, not forwarded | decompiled caller `FUN_142482ae0` swaps this call with slot 29 depending on a record byte; a copy/submission-shaped path | decompiled |
| 74 | - | 4 words (pointer, 7, pointer, 1) | `0x2484b12` | answered `0`, not forwarded | decompiled caller `FUN_142484ad0` bridges a small structure | decompiled |
| 75 | - | 1 word (boolean) | `0x32820` | answered `0`, not forwarded | recorder fallback for a boolean state derived from a byte of the pipeline record (`+0x55` bit 6), stored at console body `+0x47A88` | both |
| 85 | `CopyTextureRegion` (PC slot 16) | 6 words (destination location, 0, 0, 0, source location, null) | measured in the FSR client | forwarded to PC slot 16 | console texture copy with two `D3D12_TEXTURE_COPY_LOCATION`s | measured (external client) |
| 126 | - | 2 words (packed state dword, 16-byte block) | `0x3263e` | answered `0`, not forwarded | recorder fallback for the packed state word at console body `+0x12380` | both |
| 131 | - | 2 words (size, alignment) | `0x156099a`, `0x2478bc5`, `0x2478cba` | answered `0`, not forwarded | allocation-shaped console method: a size word and an alignment word | decompiled |
| 132 | - | 1-2 words (size) | `0x2478ba4`, `0x2478c99` | answered `0`, not forwarded | allocation-shaped console method: a size word | decompiled |
| 139 | - | 3 words (pointer, 0, 0), returns a checked result | `0x2474cd4` | answered `0`, not forwarded | decompiled caller `FUN_142474cc0` treats a negative result as fatal; the zero answer satisfies it | both |
| 142 | - | 1 word (a value from console body `+0x12360`) | `0x329e9` | answered `0`, not forwarded | recorder fallback when that state word changes | both |
| 146 | - (console enhanced barrier group) | 4 words (count, group array, ...) | `0x32113` | implemented: translated to global barriers (texture barriers with no resource become global barriers, see [handles and barriers](handles_barriers.md)) | records a barrier group | both |
| 148 | - (console `AllocateUploadMemory`) | 2 words (size, alignment) | measured in the FSR client | stubbed `0` with a one-time log | console upload-memory allocation; the port allocates its own upload memory | measured (external client) |

## 10. Cheap fixes this map makes available

These are proposals, scoped to a shim file and a slot; no shim file was edited for this page.

1. **List slot 59 is `BeginEvent`, and the proxy maps it to `SetPredication`.** `port/shims/d3d12_xs_device.h` (`console_to_pc_list_slot`) returns `55` for `59` (`59 - 4`), but console slots 60 and 61 are `EndEvent` and `ExecuteIndirect`, which are PC slots 58 and 59 - a two-slot shift, so 59 is PC slot 57, `BeginEvent`. The measured console call at 59 is `(metadata, blob, size)`, which is `BeginEvent`'s signature and not `SetPredication`'s; it is profiling-gated. Forwarding it as `SetPredication` gives the PC runtime a bogus resource and can record an invalid list. Change the one mapping arm to `57`, and add `console_to_pc_list_slot(59) == 57` to `port/test/xs_proxy_mock_test.cpp` (and keep the copy in `port/test/fsr_dispatch_test.cpp` in step). Risk: none on the measured boot (the title never calls 59); the path is the FSR test's profiling branch.
2. **Queue slots 14 and 15 are the console's two inserted methods, but the proxy forwards them to PC `Signal`/`Wait`.** `port/shims/d3d12_xs_device.cpp` (`child_slot`): the forwarding map only shifts slots 16-20, so 14/15 go to PC slots 14/15 with their own arguments - a fence pointer the console method does not take. No caller has been measured, so refuse them by name with a logged line instead. Risk: none measured; pure hardening.
3. **List slot 70 is dropped while its shape matches `WriteBufferImmediate`.** `port/shims/d3d12_xs_device.cpp`: list slot 70 is answered `0` and not forwarded. The recorder calls it `(count, &D3D12_WRITE_BUFFER_IMMEDIATE_DATA)` - the PC slot 66 signature - tens of thousands of times per boot. Add an opt-in switch (e.g. `PALMETTO_XS_LIST_SLOT70=1`) that forwards the two words to PC slot 66 and logs the first three calls, leaving the default path byte-identical until a boot shows the write lands in the right buffer. Risk: a wrong `pResource`/mode invalidates the list, which is why it stays opt-in.
4. **Allocator slot 9 is a console callback registration, not an unknown tail.** `port/shims/d3d12_xs_device.cpp`: the refusal log says "beyond the PC allocator vtable". The decompiled caller `FUN_140188430` registers `(function pointer, context)` on each new allocator and ignores the result. Name the slot in the one-time log (and, if a later boot shows the result used, answer `S_OK` instead of `E_NOTIMPL`). No behaviour change needed for the current boot.
5. **Device slot 74 has a three-word console call but no identified method.** `port/shims/d3d12_xs_device.cpp` (`slot_name` and the device thunk): the title calls it before creating a queue, checks the result for a negative value (fatal on failure), and passes `(pointer, integer, pointer)`, while PC slot 74 `ShaderCacheControl` takes two arguments. Forwarding by index is what keeps the boot alive today; when the real device's table ends before 75 the proxy already stubs the slot with `S_OK`/zero, so the title continues with an unwritten output. Add a one-time name and call-shape log for the slot (no behaviour change), then decide from the next boot's arguments whether to model it or stub it by name.

## 11. Extending this page

The next slot question costs one boot and one command:

1. boot with `PALMETTO_XS_CALLERS=1` and `PALMETTO_XS_CALLERS_PROFILE=1` and keep the stderr log;
2. `python -I tooling/ghidra/slot_census.py <stderr.log> --image <title.exe> --title-base 0x140000000 --size 0xb06d000 --out census.csv`;
3. `python -I tooling/ghidra/decompile_callers.py --census census.csv --image <title.exe> --ghidra <ghidra-install> --project <project-dir> --out decompiled/`;
4. read the argument setup in the `.c` file for the caller RVA, and the PC method at the mapped slot from `python -I tooling/d3d12_slot_table.py --header <sdk>/d3d12.h`.

The complete workflow, the tool options and the limits of `-noanalysis` are in
[tooling/ghidra/README.md](../tooling/ghidra/README.md) and
[docs/dev-tools.md](dev-tools.md#console-vtable-slot-maps-with-ghidra). Add the new row to the
object's table, mark it with the confidence it earned, and, for a fix, put the evidence and the
risk next to the slot rather than in a commit message.
