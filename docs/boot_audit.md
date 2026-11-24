# Boot audit: remaining vtable / argument-order hazards in the XS D3D12 proxy

Read-only audit of the two files the lead is editing, plus the code they call. One new file (this one) was
created; no source, no branch, no commit.

**Snapshot.** `main` at `d35b78470`; both audited files are unchanged since `94c014258`:

| file | HEAD blob | lines |
|---|---|---|
| `port/shims/d3d12_xs_device.cpp` | `7964f6b78` | 2317 |
| `port/shims/d3d12_vulkan_device.cpp` | `052a07723` | 10091 |

All line numbers below are that snapshot. The branch has moved once during the audit (`d35b78470` adds
`tooling/boot_report.py`; the two shims are untouched). Runtime evidence used: the repo sources and the local
boot-harness run of the current build (`tooling/boot_report.py`, 1,197 D3D12 calls), whose stderr contains the
device-slot first-call lines. No game bytes, package addresses or shader data are quoted.

**Index of findings**

| # | Severity | Where | Class | State |
|---|---|---|---|---|
| A3 | high — regression of the slot-26 fix | `d3d12_xs_device.cpp:1910-1929`, falls off the end at `2005` | (a) order | live; an existing test hits it |
| A1 | high — latent | generic device forward for `GetResourceAllocationInfo`, `d3d12_xs_device.cpp:2002-2003` (+ `:166`) | (a) order | not reached in the 1,197-call trace; corrupts/faults when reached |
| A2 | medium — latent | generic device forward for `GetResourceAllocationInfo2` (PC slot 68) | (a) order | same shape as A1; not reached |
| B1 | high — current stop | layer `Heap::from<Heap>(heap_interface)`, `d3d12_vulkan_device.cpp:7307` | caller pointer deref | live; see §5 |
| B2 | high | no guarded out-pointer write anywhere except slot 70; `d3d12_xs_device.cpp:1937-1940` ignores the backing result | writes | live |
| B3 | low | `readable_range` masks `PAGE_GUARD` away, `d3d12_xs_device.cpp:1611-1627` | mapping check | live |
| C1 | low — diagnostics | slots 26 and 70 never call `note_slot`; `device_slot_overridden(70)` is false | coverage | live |
| C2 | low | slot 70 backs a page *before* the slot-count refusal, `d3d12_xs_device.cpp:1937-1941` | ordering | live |
| C3 | unmeasured assumption | heap slot 8 / queue slot 18 `GetDesc` struct-return order | (a) order | no evidence yet |

Class (b) — a `real->Method()` C++ virtual call on an object whose first word is not a vtable — **does not
occur in either file today**. Details and the per-site verdicts are in §2; the one analogous dependency in the
layer is safe too. The section is short on purpose: there is nothing left to fix there.

---

## 1. The two dispatch forms, and where `real` comes from

`DeviceProxy` is vtable-at-0, PC device at `+8`, cached PC vtable at `+16`, slot count at `+24`
(`d3d12_xs_device.cpp:90-108`); heap, child and list proxies follow the same shape
(`:112-118`, `:716-723`, `:929-945`). Two dispatch forms coexist:

1. **C++ member call**: `self->real->CreateCommandQueue(...)`, `device->real->GetCustomHeapProperties(...)`.
   The compiler reads the callee's vtable from `real`'s first word on every call.
2. **Raw cached-table call**: `reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, …)`. The table was
   captured once at proxy creation (`:638`, `:1352`, `:2072`) and is *not* re-read.

The two forms agree at creation time (same word captured at `:638`/`:1352`/`:2072`). They differ only in
failure mode: if anything ever overwrote `real`'s first words (which class (a) did), form 2 keeps working while
form 1 faults. That is the whole reason class (b) was worth listing. It is not a live bug now.

`real` is assigned in exactly four places, always from a COM create call:

| proxy | assignment | source of the object |
|---|---|---|
| `DeviceProxy` | `:2071` | `d3d12_xs.cpp:174` (Vulkan-layer `Device`, see `d3d12_vulkan_device.cpp:5039-5042`) or `d3d12_xs.cpp:202` (host `D3D12CreateDevice`) |
| `HeapProxy` | `:636` | `CreateDescriptorHeap` / the registered console-heap entry point (`:1654-1668`) |
| `ChildProxy` | `:1350` | PC/Vulkan `CreateCommandQueue`/`CreateFence`/`CreateCommandAllocator`/`OpenSharedHandle` (`:1412`, `:1518`, `:1503`, `:1570`, `:1544`) |
| `ListProxy` | `:1174` | PC/Vulkan `CreateCommandList` (`:1487`) |

Every one of those is an interface pointer whose first word is its vtable: the host PC objects are COM by
definition, and the Vulkan-layer objects are one inheritance chain (`d3d12_com_object.h:40`: "one inheritance
chain: every interface shares this pointer"; `Device : public Object<Device, ID3D12Device11Stubs, …>` at
`d3d12_vulkan_device.cpp:5039-5042`, `Object : public Stubs` at `d3d12_com_object.h:24-25`). No proxy ever
stores a data-first struct in `real`, and no code path casts a non-interface pointer into it. The stale
double-dispatch risk of the old bug is gone.

---

## 2. Class (b): every C++ virtual call on `real` — verdicts

All sites in `d3d12_xs_device.cpp` (HEAD blob `7964f6b78`). "Safe" means: at that point `real` is a live COM
interface pointer captured as in §1, so the member call resolves through its real vtable. The proxy holds a
reference to each `real` until its own death (`:313`, `:526`, `:819`, `:1039`), so the pointer cannot dangle at
these sites.

| line | call | context | verdict |
|---|---|---|---|
| 313 | `real->Release()` | `DeviceProxy` last release | safe — PC device |
| 369 | `self->real->QueryInterface(...)` | device QI capability check | safe |
| 408 | `real->GetAdapterLuid()` | `host_adapter` (PC DXGI path only) | safe |
| 435, 439, 443 | `dxgi_owner(self)->real->SetPrivateData*/GetPrivateData` | IDXGIObject forwards | safe — PC device |
| 526 | `real->Release()` | `HeapProxy` last release | safe |
| 819 | `real->Release()` | child proxy last release | safe |
| 854 | `real->QueryInterface(...)` | `supports_iid` | safe |
| 1039 | `real->Release()` | `ListProxy` last release | safe |
| 1410, 1412 | `self->real->CreateCommandQueue` | slot 8 | safe |
| 1487 | `self->real->CreateCommandList` | slot 73 | safe |
| 1516, 1518 | `self->real->CreateFence` | slot 36 | safe |
| 1542, 1544 | `self->real->CreateCommandAllocator` | slot 9 | safe |
| 1568, 1570 | `self->real->OpenSharedHandle` | slot 32 | safe |
| 1636, 1658, 1664 | `self->real->CreateDescriptorHeap` | slot 14 | safe |
| 1688 | `self->real->CreateComputePipelineState` | slot 62 | safe |
| 1760 | `self->real->CreateComputePipelineState` | slot 67 | safe |
| 1797 | `self->real->CreateCommittedResource` | slot 76 | safe |
| 1923 | `device->real->GetCustomHeapProperties(...)` | slot 26 console order | safe **provided** `device` is the proxy: it is, because the call is inside `if (is_device_proxy(a1))` (`:1918`) and `a1` is the device proxy in the measured console order. This is the class-(a) fix; do not replace it with a raw `(buffer, real, …)` call. The PC-order half of this branch is broken — see A3 |
| 2023 | `real->QueryInterface(...)` | `supports()` in `detect_slot_count` | safe |

Two more dispatch sites that read a live vtable but are not `real->` syntax:

* `:1101-1102` (`list_slot<7>`, GetDevice without an output) reads `(*reinterpret_cast<void***>(self->real))[7]`
  — same validity argument, PC order, safe.
* `:2023` is inside `detect_slot_count(real)` called from `:2073` with the freshly received PC device, safe.

`d3d12_vulkan_device.cpp` has **no** `real->` and no `real_vtable` (grep-verified). Its analogous
vtable-at-offset-0 dependency is `identity()` (`:8290`), which returns the Xbox proxy or `this`; both are
vtable-at-0 objects (proxy: `d3d12_xs_device.cpp:90-93`; layer device: `d3d12_vulkan_device.cpp:5039-5042`), and
`Child::GetDevice` (`d3d12_com_object.h:179-183`) only does `QueryInterface` on whichever it holds. Safe.

The class-(b)-shaped hazard that *does* exist in the layer is different: it dereferences **caller-supplied
pointers** as objects. `Object<T>::from` (`d3d12_com_object.h:113-120`) calls `object->QueryInterface(...)`,
which reads `*(void***)object` before any validation. Used on a title value that is not a local object — the
current stop — it faults. That is §5, not class (b).

---

## 3. Class (a): every raw PC-vtable call and its argument order

The rule for the raw form is the x64 ABI of a C++ member: `this` in RCX, a by-value struct's hidden return
buffer in RDX, the declared arguments after that. A call with `(buffer, this, …)` is the bug that corrupted the
device at slot 26 before `1623f5a03`. Every raw call in the current file:

| line | call | order written | verdict |
|---|---|---|---|
| 569 | `Fn(self->real_vtable[9])(self->real, &handle)` | `(this, hidden ptr)` | correct; also tolerates an RAX return (`:562-571`) |
| 580 | same, slot 10 | `(this, hidden ptr)` | correct (`:573-582`) |
| 609-610 | heap generic `fn(self->real, a1..a14)` | `(this, args)` | correct for PC-order callers; see C3 for heap slot 8 |
| 907 | child `GetDevice` `fn(self->real, riid, nullptr)` | `(this, args)` | correct |
| 1101-1102 | list `GetDevice` | `(this, args)` | correct |
| 1145 | list generic, after slot remap and unwrapping | `(this, args)` | correct; no mapped PC list slot returns a struct by value |
| 1283-1284 | child generic | `(this, args)` | correct for PC-order callers; see C3 for queue slot 18 |
| 1385 | `forward_device<Slot>` for 31/34/35/45/46/50 | `(this, args)` | correct — all six return HRESULT |
| 1500-1501 | slot 75 `CreateCommandQueue1` | `(this, args)` | correct |
| 1876-1881 | slot 12 `CreateCommandList` | `(this, nodeMask, type, …)` | correct |
| 1923 | slot 26 (member call, not raw) | `this` + declared args | correct console-order fix; the PC-order path is A3 |
| 1945-1946 | slot 70 forward to `CreatePlacedResource1` | `(this, a1..a8)` | register order correct; the call is semantically wrong because `a1` is not an `ID3D12Heap*` — §5. Do **not** reorder |
| 2002-2003 | generic device forward | `(this, a1..a14)` | correct for PC-order callers; A1/A2 are the console-order exceptions |

**No buffer-first raw call remains anywhere.** The call that used the bad shape (slot 26, in `efdebedb4`) is
gone; grep for calls whose first word is a buffer finds only the sites above, all `this`-first. What remains of class (a) is the
*generic forwarder's inability to detect the console order for other by-value-return slots.*

### A3 (fix this first): the slot-26 branch has no PC-order path — it falls off the end of the function

`:1910-1929` is entered for every call to slot 26. It returns only inside
`if (is_device_proxy(a1))` (`:1918`). When slot 26 is called **PC order** (`self` = the proxy, `a1` = the hidden
return buffer), `is_device_proxy(a1)` is false, the branch ends, and control reaches the closing brace of
`proxy_slot` at `:2005` with no `return` — falling off the end of a `std::uint64_t` function is undefined
behaviour and the caller's return buffer is never written. Same for the impossible `!device->real` case.

This is not theoretical: `port/test/d3d12_fence_timing_test.cpp` wraps a Vulkan device in the proxy
(`:336-340`) and then calls the interface methods, including
`check_custom_heap_properties(device.p)` at `:375`, which calls
`device->GetCustomHeapProperties(...)` at `:280`, `:306` and `:320`. Those are PC-order calls and hit the
fall-through; the test then reads an uninitialized `D3D12_HEAP_PROPERTIES`. The test predates the fix and should
be passing, so this is a regression introduced by `efdebedb4`/`1623f5a03`.

Smallest correction, inside the same branch after the console-order block (buffer is `a1` in PC order):

```cpp
const D3D12_HEAP_PROPERTIES properties = self->real->GetCustomHeapProperties(
    static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3)));
std::memcpy(reinterpret_cast<void*>(a1), &properties, sizeof(properties));
return reinterpret_cast<std::uint64_t>(a1);
```

(Do not restore the raw vtable call for this half: a member call with the declared argument types is the
class-(a)-safe form, and the PC ABI returns the hidden-buffer address in RAX, which is what the PC caller
ignores while reading `a1`.) A cheap regression test: call slot 26 on a mock proxy in PC order and require the
return buffer to hold the mock's properties.

### A1: slot 25 `GetResourceAllocationInfo` is the same class-(a) site as slot 26, just not reached yet

* PC slot 25 is `GetResourceAllocationInfo` (`d3d12_xs_device.cpp:166`), which returns
  `D3D12_RESOURCE_ALLOCATION_INFO` (16 bytes) **by value**, so a PC-order call passes the hidden return buffer
  in RDX. This is the same ABI shape as slot 26.
* The console order is measured for slot 26: `(return buffer, device, …)`
  (`docs/retail_boot_chain.md:61-68`, and the fix comment at `d3d12_xs_device.cpp:1911-1913`). The same document
  says "Other struct-returning slots probably follow the same rule; only slot 26 was reached" (`:67-68`).
* Slot 25 has no override; it goes to the generic forward at `:2002-2003`. In console order that thunk's `self`
  is the title's return buffer: `note_slot(25)` is harmless, then `Slot >= self->slot_count` reads the buffer as
  a `DeviceProxy` (`:1994`) and `self->real_vtable[25]` (`:2002`) reads a pointer out of the title's buffer and
  calls through it. Best case a clean fault, worst case a call through a stale title pointer.
* It has **not** been called in the measured boot: the 1,197-call stderr has first-call lines for slots
  0, 1, 2, 8, 9, 14, 15, 17, 18, 19, 36, 48, 73, 78, 79, 133 and none for 25 (or 43, or 68). So this is a
  latent stop, not the current one.

Smallest correction: a `Slot == 25` branch modelled exactly on slot 26's console-order half —
`is_device_proxy(a1)` → `device->real->GetResourceAllocationInfo((UINT)a2, (UINT)a3,
(const D3D12_RESOURCE_DESC*)a4)`, `memcpy` the 16-byte result to `self`, return `self` — plus the PC-order
fallback from A3 (buffer in `a1`, args in `a2`/`a3`/`a4`) so the branch cannot fall off the end either. Slot 26
and slot 25 should share one helper rather than duplicating the discriminator.

### A2: slot 68 `GetResourceAllocationInfo2` is the same shape

PC slot 68 is `ID3D12Device8::GetResourceAllocationInfo2` and also returns a 16-byte
`D3D12_RESOURCE_ALLOCATION_INFO` by value. It is inside `detect_slot_count`'s Device8 range and is forwarded by
the same generic thunk, so if the console calls slot 68 in console order it fails exactly as A1. It was not
called in the measured trace. If it is worth handling, the console-order call is
`(buffer, device, visibleMask, numDescs, descs, allocInfo1)` → member call with `(UINT)a2, (UINT)a3,
(const D3D12_RESOURCE_DESC1*)a4, (D3D12_RESOURCE_ALLOCATION_INFO1*)a5`, result to `self`. Lower priority than
A1 because the measured boot has not passed Device7's slots by index at all; the cheap alternative is to make
the generic forwarder *detect* a device proxy in `a1` for any slot known to return a struct by value and log
once, so the stop names itself instead of faulting.

### `GetAdapterLuid` (slot 43) is safe

`LUID` is 8 bytes; on x64 it is returned in RAX, not through a hidden buffer, so the console and PC register
layouts coincide and the generic forward at `:2002-2003` is correct. `port/test/device_proxy_test.cpp:196-201`
already calls it through the proxy and compares it with the real device's LUID, and the measured 1,197-call run
never calls slot 43 (the title reaches the adapter through `XboxIDXGIDevice::GetAdapter`, which goes through
`host_adapter`/the provider on the real device, `:396-420`). The comment at `device_proxy_test.cpp:196` that
calls LUID a hidden-pointer return appears inaccurate (LUID is 8 bytes and x64 returns it in RAX), but the code is
right either way because the proxy returns the callee's RAX.

### C3: unmeasured PC-order assumptions for the heap and the queue

Two forwarding sites carry a by-value struct through a raw forward under the assumption that the console keeps
the PC convention:

* heap slot 8 `GetDesc` (`:609-610`; named at `:280`), `D3D12_DESCRIPTOR_HEAP_DESC` is 16 bytes;
* child queue slot 18 `GetDesc` (`:1283-1284`; `child_public_name` at `:749`), `D3D12_COMMAND_QUEUE_DESC` is
  16 bytes.

`d3d12_xs_device.h:26-29` records the heap assumption ("GetDesc and the other heap slots forward the PC
convention unchanged") and `d3d12_xs_device.cpp:665-669` the queue/fence one ("GetDesc returns its struct through
the hidden pointer in RDX, which is forwarded unchanged (no Xbox variant is measured, §6)"). If the title ever calls either one in the console
order, the thunk's `self` will be a title buffer: heap slot 8 would read `self->real_vtable` from the buffer at
`:609`, queue slot 18 would read `self->slot_count` at `:1255` and `self->real_vtable[Slot]` at `:1283` before
faulting. No evidence either way; a one-line probe (log when `is_heap_proxy`/`is_child_proxy(a1)` is true inside
those thunks) would settle it without guessing. No action until a trace shows the call.

### What the measured trace already proves safe

The generic device thunk (`:2002-2003`) has been reached in the 1,197-call run only by slots 15
(`GetDescriptorHandleIncrementSize`, returns `UINT`), 17, 18 and 19 (the three `Create*View` methods, return
`void`). It has therefore never been exercised with a by-value return, which is consistent with A1/A2 being
latent. The device slots that *did* return structs by value were handled by overrides:
slot 26 (fixed), slot 14 (wrapped heap, `:1630-1671`), and slot 133 (the placed-at-VA handler, `:1810-1850`).
Slots 26 and 70 cannot appear in that first-call list at all: those two branches never call `note_slot` (C1), so
the trace neither proves nor disproves that slot 26 was reached after the fix. What it does prove is that the
generic forward was only reached by non-struct-returning slots, which is why A1/A2 are still latent rather than
observed.

---

## 4. Writes to caller-supplied pointers without a mapped check

### 4.1 The measured fact that makes this a real class

The title does pass a **console VA** as a COM output pointer. Device slot 70 is forwarded to PC
`CreatePlacedResource1`, whose `ppvResource` is `a7`; before `94c014258`, the layer's first write
(`d3d12_vulkan_device.cpp:7267`, `if (out) *out = nullptr;`) faulted at that title address — that is exactly the
write the lead's commit describes. The current work-around backs one page at `a7 & ~0xFFFF` in the proxy
(`:1937-1940`) and then forwards. Everywhere else the proxy and the layer assume an out pointer is ordinary local
memory, which is true for every slot measured so far but is not a property of the ABI the title uses.

### 4.2 Write sites

In `d3d12_xs_device.cpp`, every one of these writes a caller-supplied pointer with only a null check (all
`_COM_Outptr_` reasons are in the comments):

`*out = nullptr` early-write sites: `:352`, `:415`, `:418`, `:461`, `:541`, `:553`, `:631`, `:885`, `:903`,
`:1052`, `:1166`, `:1405`, `:1460`, `:1496`, `:1512`, `:1538`, `:1565`, `:1633`, `:1686`, `:1701`, `:1779`,
`:1813`, `:2066`; success writes `*out = self` at `:355`, `:361`, `:373`, `:544`, `:893`, `:1056`, `:1065`;
other success writes `*out = opened` at `:1574`.

Other caller-pointer writes:

* `:1451` `*token = value` in `proxy_wait_frame_event_x` — null-checked only (the token is a `UINT64*` the title
  passes; slot 80 works in the measured run, so it is local memory *today*).
* `:1925` `std::memcpy(reinterpret_cast<void*>(self), &properties, sizeof(properties))` — slot 26 writes the
  32-byte struct into the title's return buffer `self` with no mapping check. Local stack in the measured run;
  same assumption as slot 70's out pointer.
* `:1937-1940` slot 70: the *only* place that tries to make a title VA writable, and it ignores the helper's
  result. If the backing fails (already-committed page with different protection, a page outside the placeholder
  range that cannot be committed, a helper that was not registered because the layer is not linked), the layer's
  `*out` write faults exactly as before, one call later, with no message from the proxy. Check the return, log
  once, and either refuse the slot (`E_INVALIDARG`) or let the call through deliberately.

In `d3d12_vulkan_device.cpp` and `d3d12_com_object.h`:

* `:7267` (`CreatePlacedResource1`) and `:7306` (`create_placed`) write `*out` before anything else — the
  measured title VA write.
* `hand_out` (`d3d12_com_object.h:195-199`) writes `*out` through `QueryInterface`.
* Descriptor destinations are caller-supplied CPU handles that, for a console-addressed heap, point at the
  title's console VA: `:6428` (`CreateConstantBufferView`), `:6449` (`CreateShaderResourceView`), `:6703`,
  `:6767`, `:6787` (RTV/DSV/Sampler/UAV), `:6407`/`:6418` (`CopyDescriptors`/`CopyDescriptorsSimple`), all
  `memset`/`memmove` to `destination.ptr` with no mapped/writable check.
* The backing that is supposed to make those handles writable: `DescriptorHeap` ctor `:1745-1762` calls
  `console_heap_back` (`:1701-1726`) for exactly `desc.NumDescriptors * kDescriptorSize` rounded to 64 KiB, and
  keeps going with `cpu_ = console_address` even when the backing failed (`:1753-1758` logs "NOT backed (writes
  will fault)"). So a heap whose descriptors the title writes beyond that range, or a second console-addressed
  heap at an address the helper cannot map, faults later in a descriptor write rather than at creation.

### 4.3 Finding B2: no `writable_range`, and the one guard is partial

There is a `readable_range` helper (`:1611-1627`) but no writable counterpart. The smallest durable fix is to add
one — `VirtualQuery`, `State == MEM_COMMIT`, `Protect & (PAGE_GUARD|PAGE_NOACCESS) == 0`, and one of the write
protections (`PAGE_READWRITE`, `PAGE_WRITECOPY`, `PAGE_EXECUTE_READWRITE`, `PAGE_EXECUTE_WRITECOPY`) — and to use
it (or the `console_heap_back` helper) before the *first* write in each overridden create path that can receive a
title VA. The measured ABI only needs it for slot 70 so far; but the same code should serve slots 26, 80 and any
future console-shaped call, rather than a special case per slot.

### 4.4 Finding B3: `readable_range` misses guard pages

`:1618` computes `const DWORD protect = mbi.Protect & 0xffu;` and `:1619` tests
`protect == PAGE_NOACCESS || protect == PAGE_GUARD || protect == PAGE_EXECUTE`. `PAGE_GUARD` is `0x100`, so
masking to `0xff` deletes it: a guard page whose protection is `PAGE_READWRITE | PAGE_GUARD` becomes `0x04` and
passes as readable, the dead `== PAGE_GUARD` test can never fire, and the subsequent
`memcpy`/dereference of the console-extended heap description (`:1645-1650`) raises a guard-page exception instead
of taking the clean "no console address" path. Fix: test `(mbi.Protect & PAGE_GUARD) != 0` on the unmasked value
before masking, and treat `PAGE_NOACCESS`/`PAGE_EXECUTE` the same way (or invert: accept only
`PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY`).
Also used once, at `:1645`, so impact is limited to console-extended slot 14 descriptions that sit at a guard
boundary — but the helper is the one the audit is asked about, and a write-side twin with the same mask bug
would be worse.

---

## 5. The current stop: device slot 70 → PC `CreatePlacedResource1`

### 5.1 What is established

* The console call reaches the proxy with `self` = the proxy (PC register order: `this` in RCX), so this is not
  the slot-26 style `(buffer, this, …)` inversion. The proxy's slot-70 branch comments the probe at
  `:1931-1933`; nothing needs reordering.
* The argument positions match PC `CreatePlacedResource1(heap, offset, desc, state, clear, riid, out)`:
  - `a3` is a valid `D3D12_RESOURCE_DESC1*` — the layer dereferences it (`d3d12_vulkan_device.cpp:7268-7270`,
    `texture_mip_region(*desc)`, `resource_desc(*desc)`) before the fault, so it is mapped memory;
  - `a7` is the output pointer — before `94c014258` the layer's `*out = nullptr` at `:7267` was the write into
    the title's address, and backing that page removed the fault;
  - `a1` is the heap argument, and it is **not** an `ID3D12Heap*` of this process: it is a title-space value
    (in the local run, a base with the low bit set — the console naming a heap by an address/handle, the same
    idea as the console-extended slot-14 desc's address).
* The crash is `Object<Heap>::from` on that value: `Heap::from<Heap>(heap_interface)`
  (`d3d12_vulkan_device.cpp:7307`) → `Object::from` (`d3d12_com_object.h:114-120`) → `QueryInterface`, whose
  first act is to read the vtable pointer from `*(void***)heap_interface`. That read is from an unmapped title
  address, which is the access violation the boot harness reports (`access read`).
* So: **not a vtable/argument-order bug**; it is a heap-identifier ABI difference plus an unvalidated
  caller-pointer dereference. That is why the class-(a)/(b) audit found no fault at slot 70: the order is right
  and the object is wrong.

### 5.2 What the fix needs (smallest first)

1. **Do not dereference `heap_interface` until it is known to be a local object.** The clean forms are:
   (a) a registry of live `Heap*` populated in the `Heap` constructor and (important) erased in its destructor,
   with `heap_interface` tested for membership before `Heap::from`; or (b) a `VirtualQuery`-based readability
   gate (the same one §4.3 recommends) before `Heap::from`, which also covers the descriptor heap, resource and
   pipeline `Object::from` sites listed in §4.2. A registry that is never erased will misclassify a recycled
   address after a `Heap` dies; a destroy hook belongs in `Heap`'s destructor (`d3d12_vulkan_device.cpp:2846` is
   the existing-heap constructor, `:7240` the arena one; the class at `:2823` has no destructor of its own).
2. **Mask the title handle's tag bits before using it as a VA.** If `a1` is an address with a tag bit, then a
   console-VA fallback that forms `address + heap_offset` hands an odd address down to
   `create_buffer_at_host_address` (`d3d12_vulkan_device.cpp:8915`) → `Resource::create_host_buffer`
   (`:2020-2057`), which rounds down to `host_pointer_alignment` and imports at offset 1, or falls back. Log the
   raw `a1`/`a2` once (slot 70 today logs only `a7`, and only with `XS_TRACE`, `d3d12_xs_device.cpp:1934-1936`)
   and mask what the trace shows rather than guessing.
3. **Resolve the heap identity, don't guess it.** The runtime already knows every console address it handed out:
   `DescriptorHeap::console_address_` (`d3d12_vulkan_device.cpp:1742`, getters at `:1795`/`:1802`) and the xmem
   physical-page ranges the placed-at-VA handler works on (`d3d12_xs_device.cpp:1807-1849`). A lookup from the
   console value to the heap it names is the correct semantic fix; a placed-at-VA fallback (below) can at least
   give the title a resource at what it asked for.
4. **If a placed-at-VA fallback is added, run the pages hook as slot 133 does.** `proxy_create_placed_resource_x`
   calls `g_placed_at_va_pages` before the handler (`d3d12_xs_device.cpp:1839-1849`) because xmem commits
   physical pages on first touch. A fallback that calls only the handler leaves the range reserved,
   `Resource::create_host_buffer`'s committed-range scan (`d3d12_vulkan_device.cpp:2046-2057`) then returns
   `E_NOTIMPL`, and the title silently gets the ordinary-buffer fallback. The two call sites should share one
   helper (`set_placed_at_va_pages_hook` is registered in `port/src/main.cpp:169`; there is no getter for it
   today, so adding one is the minimal plumbing).
5. **Validate cheap arguments before the pointer is used.** `create_placed` currently dereferences the title
   value in `Heap::from` at `:7307` *before* the combined `if (!heap || !desc)` check at `:7308`; a `!desc` (or a
   readable-pointer) test should come first.

### 5.3 Two smaller notes for the same branch

* **C1:** the slot-70 branch (`:1930-1946`) never calls `note_slot(70)`, and `device_slot_overridden(70)` is
  false (`:2147-2155`). The slot-26 branch (`:1910-1929`) likewise never calls `note_slot(26)`. Consequence: the
  boot report's "last [rt] lines" can never name the two slots that have consumed most of this session's work;
  the current run cannot prove from its own log that slot 70 was reached. Add `note_slot(70)`/`note_slot(26)`
  and `slot == 70` to `device_slot_overridden`, and give slot 70 a `slot_name` case (`:155-189`, today it prints
  "forwarded").
* **C2:** slot 70 backs `a7`'s page at `:1937-1940` *before* the `Slot >= self->slot_count` refusal at `:1941`.
  On a PC device older than `ID3D12Device8` the proxy commits a page for a call it then refuses with
  `E_NOTIMPL`. Move the refusal above the backing (and above any unwrapping).

---

## 6. Safelist — what does not need re-deriving

* Every `real->` site in §2: safe COM dispatch; no data-first `real` exists; the fixed slot-26 member call
  (`:1923`) is right for the console order.
* Every raw forward in §3 except the console-order cases A1/A2, slot 70's semantically-wrong forward (§5) and the
  two unmeasured `GetDesc` cases in C3: PC order, `this` first, no by-value struct return among the slots they
  forward.
* Slot 43 `GetAdapterLuid`: 8-byte RAX return, already tested through the proxy.
* The heap handle conversion (`:562-582`): PC `(this, hidden ptr)` call plus RAX tolerance is correct for both
  the host PC heap and the Vulkan-layer heap, and `port/test/device_proxy_test.cpp:263-280` pins it.
* The list proxy forwarder (`:1145`): no mapped PC command-list slot returns a struct by value, so the raw
  forward is order-safe; the only adjustments it makes are pointer-array unwrapping (`:1135-1144`).
* The queue/fence/allocator forwarder (`:1283-1284`) for every slot except the two `GetDesc` cases in C3.
* The console-heap backing (`d3d12_vulkan_device.cpp:1701-1726`) is bounded to the descriptor bytes rounded to
  the console page size and is what makes `GetCPUDescriptorHandleForHeapStart() == address` hold
  (`:1785-1798`); the write fault that remains in the layer is the `*out` write of §5, not the descriptor writes.

## 7. Cheap tests that would guard the findings (no game data)

1. `xs_proxy_mock_test`: call slot 26 **PC order** on a mock proxy (mock PC method returns properties) and
   assert the return buffer is filled and the call returns without falling through (A3).
2. `xs_proxy_mock_test`: call slot 25 **console order** (`(buffer, proxy, mask, count, descs)`) and assert the
   buffer gets the 16-byte allocation info and the device proxy is untouched (A1).
3. `d3d12_vulkan_device_test` (or a small layer test): call `CreatePlacedResource1` with `heap` set to a
   console-like value that is not a live `Heap*` and assert the call returns (E_INVALIDARG/E_NOTIMPL) instead of
   faulting (B1) — this is the shape of the current retail stop. The GPU-free `xs_proxy_mock_test` can at least
   assert slot 70 forwards its words unchanged and never dereferences `a1`.
4. `xs_proxy_mock_test`: `readable_range` (or its write twin) on a `PAGE_GUARD` page must return false (B3).
5. `d3d12_fence_timing_test` already covers A3 (it is the test that currently fails); keep it in the ctest set.
