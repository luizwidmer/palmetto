# The title heap, the XMem arena and the memory-model contract: a static map

**Status: static decompilation (no boot).** This page reads the retail image's allocator and memory-service
code out of the reused Ghidra project (opened read-only), maps the heap object and its inner allocator, the
XMem/GDK calls that give the heap its backing memory, the graphics VA/page model the same service layer
creates, and the zero-byte class-0xa allocation path end to end. It then ranks the memory-model hypotheses a
shim change could test. Every row says whether it is **M** (measured statically: the decompiled or
disassembled instruction, the function RVA, the structure offset, or a value quoted from a named prior boot on
this tree) or **I** (inferred: a reading of the code's shape that no probe has confirmed). No boot was run for
this slice; no runtime code, test, switch or default path changed; no git write was made. RVAs are relative to
the image's preferred base `0x140000000`. No strings, asset bytes or hashes of game data appear here.

The three questions the page answers: what the title heap *is* (object, bins, rounding, counters, the
address-keyed registry) and which invariants the zero-byte block violates; how the pages the heap lives on are
obtained from the services our shims answer, and which fields of our answer the title consumes; and which
shim-side lever can still change the class-0xa overrun's outcome, with a bounded falsifier each.

## 1. Evidence and method

| item | value |
|---|---|
| image | the retail title image the campaign decompiles, kept outside the repository (RVAs only in this page) |
| project | `D:\leonida-work\classa\ghidra-proj.rep`, opened read-only (`-readOnly`); no import, no analysis pass, no save |
| scripts | `D:\leonida-work\rea-tooling\ghidra_scripts` (`DecompileRanges`, `DisassembleRange`) driven by `D:\leonida-work\decomp-mem\aa.ps1`; dumps under `D:\leonida-work\decomp-mem\out-*` |
| static scans | PE `.pdata` function map (`rva2func.py`), direct `E8`/`E9` caller scan, RIP-relative xref scan filtered to `lea`/`mov` opcode prefixes, import-thunk map via `tooling/import_call_sites.py`, file-backed qword dump |
| prior evidence used as context (not as this page's evidence) | [vertex_buffer_va.md](vertex_buffer_va.md) sections 14-19 (the 0x1000-byte copy, the zero-byte block, the class 0xa chain, the four placement arms), [page_address_space.md](page_address_space.md) (title VAs, rebase design, boot measurement), [title_memory_model.md](title_memory_model.md) (every title-memory consumer and its cost), [card_copies.md](card_copies.md) (guard-page write tracker; `GetWriteWatch` refused on pagefile views), [memory_growth.md](memory_growth.md) (XMemVirtualAlloc/XMemAlloc committed counters), [xmem_exe_calls.md](xmem_exe_calls.md) (the 12-import surface and its 23 `XMemVirtualAlloc` sites), [runtime_imports.md](runtime_imports.md), [replay_resolution.md](replay_resolution.md) (the placed range not importable) |
| work notes | `D:\leonida-work\decomp\memory.md` (function map, open questions) |
| not used | no boot for this page; no runtime answer treated as static evidence; no game data |

## 2. The heap singleton and its vtable

The title's default allocator is a **pointer cell in `.bss`**, not an object image: RVA `0xABCC310` holds the
heap-object address, and the constant-upload helper's default path reads it directly (`0x45594F0`: with a null
allocator argument it loads `_DAT_14ABCC310` and calls its vtable slot 0x50).

| fact | value | source |
|---|---|---|
| heap singleton cell | RVA `0xABCC310`, 8 bytes, uninitialised image (`.bss`) | M, file-backed check |
| writer of the cell | one `mov [rip+disp],rax` site, instruction RVA `0x5B661A5`, inside the getter `0x5B66140-0x5B661CC` | M, filtered xref scan |
| readers of the cell | 4,103 RIP-relative references (3,975 `mov rcx,[…]`, the rest other registers/`lea` forms) across `.text` | M, scan; a lower bound in practice because the filter accepts real `lea`/`mov` prefixes only |
| heap object vtable | RVA `0x663BDE0`, 24 qwords, installed by `0x17243B0` | M, vtable dump + `0x17243B0` decompile |
| base-class vtable | RVA `0x66395F0`, installed by the base constructor `0x56AD480` (zeroes 0xB8 bytes) then overwritten by `0x17243B0` | M |
| singleton getter | `0x5B66140`: if the global source object `0x149E306E8` is live, lock, and when the cell is null create the heap with `0x17242B0(source, name, 0, descriptor)` | M |
| heap-object name | a compile-time string in `.rdata` at RVA `0x6780461` (the getter's argument; content not quoted) | M |

The vtable is the heap's public interface. Roles below are read from each slot's body (decompile), so a role
that needs more than its own body carries **I**.

| slot | function RVA | role | evidence |
|---:|---|---|---|
| 0 | `0x56AD480` | base constructor (sets `0x66395F0`, zeroes 0xB8) | M |
| 1, 2 | `0x4380` | `ret` stub | M |
| 3 | `0x4870` | `mov al,1; ret` stub | M |
| 4 (`0x20`) | `0x1724220` | register this heap in the global address-keyed tree, link into the parent's child list, AddRef | M |
| 5 (`0x28`) | `0x1724170` | install the heap's grower: `inner+0x248 = arg` | M (inner+0x248 is the grower read by the grow path) |
| 6 (`0x30`) | `0x7128D0` | set the heap's limit: `inner+0x240` and heap `+0x48`, floored at the current footprint | M |
| 7 (`0x38`) | `0x45D9730` | AddRef (refcount `+0x20`) | M |
| 8 (`0x40`) | `0x288900` | Release: refcount `+0x20`; at zero unlink from the parent and free the object through `0x1456AD430`; also calls slot `0x100` of the class above | M |
| 9 (`0x48`) | `0x5159FC0` | large/alignment allocation in this heap's inner allocator (`0x515A040`), lock-aware | M |
| 10 (`0x50`) | `0x538D930` | **plain allocation**: lock, `0x538D220(inner, size, out, 0)` | M |
| 11 (`0x58`) | `0x599720` | **realloc**: resolve owner heap by pointer, `0x599F30`/`0x599900` | I (call shape `0x538DFF0` passes `(cell, ptr, n*0x30)`) |
| 12 (`0x60`) | `0x515ABB0` | **free**: resolve owner heap by pointer, free the block | M |
| 13 (`0x68`) | `0x45D9760` | allocate size+granularity in the heap owning an address | M |
| 14 (`0x70`) | `0x538D040` | **allocate in the heap that owns a key address** | M (call site `0x184EB0` passes the creator object as key) |
| 15 (`0x78`) | `0x54C8AF0` | heap owning an address (returns the heap object) | M |
| 16 (`0x80`) | `0x45D9930` | heap lookup by address (same tree walk) | M |
| 17 (`0x88`) | `0x45D9A40` | allocate a registry node (source heap, size+8) | M |
| 18 (`0x90`) | `0x45D9A90` | free a registry node | M |
| 19 (`0x98`) | `0x45D9AE0` | sweep every child heap (calls child slot `0xA8`) | M |
| 20 (`0xA0`) | `0x712890` | own footprint (`inner+0x230`) | M |
| 21 (`0xA8`) | `0x45D9B30` | own + children footprint | M |
| 22 (`0xB0`) | `0x45D9BA0` | own allocated bytes (`inner+0x238`) | M |
| 23 (`0xB8`) | `0x45D9BE0` | own + children allocated bytes (recursive) | M |

## 3. The heap object's layout and creation

`0x17242B0` creates one heap object: `size = 0x320 + (strlen(name) & ~0xF)`; it allocates that from the
global source object's vtable slot 0x18 (`0x140703440`-style page source), installs the derived vtable
(`0x17243B0`), initialises the embedded inner allocator at `+0xB8` (`0x17243E0`) and copies the name to the
tail. `0x17243E0` is the only place the inner allocator's first fields are set, so it fixes the layout.

| offset | width | meaning | source |
|---:|---:|---|---|
| `+0x00` | 8 | vtable `0x663BDE0` | M, `0x17243B0` |
| `+0x08`, `+0x10` | 8 each | parent-list forward/back links (unlinked by Release) | M, `0x288900` |
| `+0x0C` | 4 | word tested for zero by Release before the unlink | M, `0x288900` |
| `+0x18` | 8 | total object size (set at creation) | M, `0x17242B0` |
| `+0x20` | 4 | refcount, starts at 1 | M |
| `+0x30` | 8 | bootstrap marker: freeing this exact pointer through slot 0x60 releases the heap object (slot 0x40) | M, `0x515ABB0` tail |
| `+0x38`..`+0x60` | 6 qwords | creation descriptor: flags, two fields, parent heap pointer at `+0x60` | M, `0x17242B0` |
| `+0x48` | 8 | limit mirror (slot 6 writes it with `inner+0x240`) | M |
| `+0x68` | 8 | child-heap list sentinel (`+0x310` of the object) | M |
| `+0x80` | mutex | the heap lock; `+0x88`/`+0x90` its waiter counters | M |
| `+0xA0` | 1 | lock-enable flag: `~(descriptor flags) & 1`; both the alloc and free paths branch on it | M |
| `+0xA1` | 1 | descriptor flag `0x10` inverted | M |
| `+0xA8` | 8 | pointer to the embedded inner allocator = `object + 0xB8` | M |
| `+0xB8` | 0x258 | the inner allocator (`0x538D220`'s `this`) | M, `0x17243E0` |
| `+0x310` | name | the compile-time name (tail) | M |

## 4. The inner allocator (`0x538D220`): bins, rounding, growth, free

The inner allocator is a dlmalloc-shaped mspace: 16-byte chunks with boundary tags, a 64-bin free-list array
with a bitmap, address-ordered segment growth in 0x1000-byte pages, and segment-relative 2-bit boundary maps.

| offset (inner) | width | meaning | source |
|---:|---:|---|---|
| `+0x00` | 8 | source object: the grow path calls `[source_vtable+0x18](source, bytes, 8)` for a 0x1000 page | M, `0x17243E0`, `0x703440` |
| `+0x08` | 8 | this heap object (the region key handed to the page source) | M, `0x17243E0` |
| `+0x10` | 8 | granule = `max(descriptor granule, 8)`; the measured 16-byte class rounding is the `0x10` branch | M |
| `+0x18` | 8 | 64-bit bin-present bitmap | M, `0x538D6C0` |
| `+0x20` | 8 x 64 | bin head pointers, index 0..0x3F | M, `0x538D6C0` (`this[index+1]` from `inner+0x18`) |
| `+0x220`, `+0x228` | 8 each | segment list head/tail (grow appends 0x1000-byte segments) | M, `0x7032F0`, `0x705350` |
| `+0x230` | 8 | footprint (bytes handed out to segments; grows by 0x1000) | M |
| `+0x238` | 8 | allocated bytes counter (added on every small allocation) | M, `0x538D220` |
| `+0x240` | 8 | limit: when footprint + 0x1000 would exceed it, the grow path asks the grower instead | M |
| `+0x248` | 8 | grower object (vtable slot +8 is its "provide bytes" call) | M, slot 5 writes it |
| `+0x250` | 8 | allocation count (incremented on every small allocation) | M |

**Allocation paths, as the code branches.**

| request | path | result | source |
|---|---|---|---|
| `size < 0x201`, granule `0x10` | round to `(size+0xf) & 0x7f0`; search the bin for that class (`0x538D410`); if empty, grow one 0x1000 page (`0x7032F0`/`0x705350`) and retry | a chunk whose declared class is the rounded size; out[2] = rounded size; `+0x250 += 1`, `+0x238 += rounded` | M |
| `size >= 0x201`, granule `0x10` | `0x538DE00`: `rounded = (size+7) & ~7`; total chunk = `0x20 + rounded + (granule>0x10 ? 8 : 0)`; request it from the owning heap's grow path through `[heap_vtable+0x18]` when the limit allows, else the page source; insert the tail into a bin | large chunk; out[2] = rounded | M |
| granule `>= 0x11` | `0x515A040`: same two paths with a different bin helper (`0x515A240`) and a weaker split | same shape | M |
| `size == 0` | rounds to `0`, class `0`, bin search index clamps to the **highest** bin 0x3F | a real zero-size chunk (see section 9) | M |

**Free** (`0x515ABB0`, slot 0x60): null is a no-op; otherwise the owner heap is resolved by the pointer (fast
lookup `0x515ADD0`, else the global address tree under the source lock), the region descriptor is the tree
node's `+0x18 & ~3`, then — in the region's lock order (region `+0x80`, then source `+8`) — the block goes to
`0x284000` (the free-block core; `0x283FF0` is a thunk to it), which coalesces with its neighbours through the
boundary tags and inserts the chunk into its bin (`0x515B1D0` for the small bins, tree insert otherwise).
Freeing the pointer equal to the heap's `+0x30` marker releases the heap object itself (`0x288900`). The
measured 0x515ACF5 crash instruction is this function's first read of the resolved node (`node+0x18`), so a
corrupted registry/tree turns any later free into the same fault.

**Invariants the allocator relies on (and the overrun breaks).**

1. Every chunk starts on a 16-byte boundary and its class nibbles in the header plus the segment's 2-bit
   boundary maps (`0x538D410` writes both) agree; a chunk is at least `0x10` bytes.
2. The bins are doubly linked through the free chunks and the bitmap at `inner+0x18` marks exactly the
   non-empty heads; a bin head read must not be a user block.
3. A freed chunk's header and link words stay intact between the free and its next reuse; the class-0
   allocation returns a chunk whose *start is the same address as the free chunk it came from* (section 9),
   so the copy that follows is writing over live allocator metadata by construction.
4. The address-keyed registry (section 5) maps every heap range to its heap object; a write over a registry
   node or its payload makes the next free/classed allocation fault at `0x515ACF5`/`0x538D0E6`.
5. Counters `+0x238`/`+0x250` are never rolled back by free, so they measure cumulative allocation, which is
   why they are a usable probe for "how many small allocations happened before the zero-size request".

## 5. The address-keyed heap registry

The allocator supports many heaps, and the entry points that receive a bare pointer must find the owning one.
They share one global structure set:

| item | RVA | role | source |
|---|---:|---|---|
| tree root | `0x9E30730` (`.bss`) | address-ordered tree of registered heaps; nodes are `{+0x08 left, +0x10 right, +0x18 payload}` with the payload's low two bits as flags | M |
| source object | `0x9E306E8` (`.bss`) | lock at `+8`; children/free-list counters; source heap pointer `0x9E306F0` | M, `0x288900`, `0x7032F0` |
| fast lookup | `0x515ADD0` | pointer → tree node, before taking the lock | M (every entry point calls it first) |
| registration | `0x1724220` → `0x17242B0` | insert a heap into the tree and the parent's child list | M |
| node alloc/free | `0x45D9A40` / `0x45D9A90` | registry nodes come from the source heap | M |

The registry is the reason `free(ptr)` and `allocate-near(key)` work across heaps: slot 0x70's key is an
*address*, and the allocator picks the registered heap whose base is the greatest one `<= key` (the tree walk
in `0x538D040`/`0x515ABB0`/`0x54C8AF0`). Measured consumer: the renderer's job allocation at `0x184EB0` asks
slot 0x70 for 0xA0 bytes with the creator object as the key, so the job comes from the same heap as that
object; the constant-table block then comes from the default heap through slot 0x50. That is why the job and
the zero-byte block sit in one arena.

## 6. The XMem/GDK service layer: where the heap's pages come from

The image imports 12 `xmem.dll` functions through jump thunks (call-site census in
[xmem_exe_calls.md](xmem_exe_calls.md)); the thunk RVAs and this page's additions:

| import | thunk RVA | where it is called |
|---|---:|---|
| `XMemVirtualAlloc` | `0x5F3FCC0` | 23 sites; the ones this page reads are below |
| `XMemAllocatePhysicalPages` | `0x5F3FCD0` | `0x1E6C22D`, `0x1E866E4`, plus two other subsystems |
| `XMemMapPhysicalPages` | `0x5F3FCE0` | 12 sites; `0x1E6C295`, `0x1E86738..D1` map 64 KiB views into the windows |
| `XMemFreePhysicalPages` | `0x5F3FD10` | `0x1E6C32D`, `0x1E868E2` |
| `XMemGetWorkingSetStatistics` | `0x5F3FCF0` | three sites (`0x2216008`, `0x2216042`, `0x247454E`); none of them is in the heap/arena chain this page maps |
| `XMemAlloc`/`XMemFree`/`XMemAllocDefault`/`XMemFreeDefault`/`XMemTransferMemory`/`XMemIsToolsMemAvailable`/`XMemMapPhysicalPagesScatter` | other thunks | not in the heap/arena chain this page maps |

**The XMem initialiser `0x1E1CF60`.** It runs once (guarded by `.bss` flag `0x1471BF9F8`), asserts on the
host processor population, and build the VA space and the pools. The structure (static reading):

| # | operation | request / value | source |
|---:|---|---|---|
| 1 | VA window size | `((capability XOR 1) OR 2) << 33` = 16 GiB or 24 GiB | M |
| 2 | placeholder 1 | `XMemVirtualAlloc(0, size, 0x20402000, 0x21, 0x4)`; answer stored `0x1471BF970`, size `0x1471BF980` | M; boot measured 0x600000000 = 24 GiB ([vertex_buffer_va.md](vertex_buffer_va.md) section 19.2 row 1) |
| 3 | placeholder 2 | `XMemVirtualAlloc(0, size, 0x20402000, 0x3, 0x801)`; answer `0x1471BF978` | M; boot row 2 |
| 4 | VA-space configuration | the initialiser zeroes a 32-byte stack descriptor, fills its `+4` dword from the capability computation, and passes it to the console D3D12 import `D3D12ConfigureVirtualMemoryX` (thunk `0x5F3FD60`, `d3d12_xs.dll`) | M |
| 5 | 9-slot pool loop | for loop index 0..8 reads size `[0x1471BF9B0 + i*8]`; cases 0, 2, 4, 5 allocate a large-page pool and register it; cases 1, 3 and 6-8 allocate nothing | M |
| 6 | case 0 pool | `XMemVirtualAlloc(0, size, 0x20003000, 0x183, 0x22004)` at call site `0x1E1D181`; the answer goes into the **first free pool slot** (each allocating case scans the slot table for it, and asserts if all are full), then `0x14221A4F0(base, size)` registers it and pool object `0x1471C1560` is the slot's object | M; boot row 3 = `0x49400000` |
| 7 | case 2 pool (**the heap arena**) | same call shape at call site `0x1E1D288`, registered with `0x145B1A7D0(0x1471C2808, size, base, 0x20000, 0x1471C28B0)` — 128 KiB granularity | M; boot row 6 = `0x25400000`, the answer the shim registry attributes the heap to |
| 8 | case 4 pool | call site `0x1E1D369`, registered with `0x141D14170(0x147444B30, base, size)` | M; boot row 5 = `0x23300000` |
| 9 | case 5 pool | call site `0x1E1D4D6`, registered with `0x141D14170(0x1474438B0, base, size)` | M; boot row 4 = `0x89700000` |
| 10 | finalise | pool-object table `0x14AAD45B8 + i*8`; state word `0x1471BFA00`, table base `0x14AAD45B0`; a slot-3 special case copies `0x14AAD45C0` to `0x14AAD45D0` | M |

Two consequences are measurable and matter below. First, the heap arena request is a **large-page commit**
(`0x20003000` = `MEM_COMMIT|MEM_RESERVE` plus the 2 MiB page mode, attributes `0x183`, protect `0x22004`),
so the arena is ordinary committed private memory, not a placeholder, not a mapped physical-page window and
not an imported range. Second, a **null answer is an `int3`** in the initialiser (`0x1E1D3A7`, `0x1E1CFE9`,
`0x1E1D574`): an `XMemVirtualAlloc` refusal is a title-side assert, not a graceful path.

**The physical-page and window producers.** `0x1E6C110` is the page-pool initialiser: it asks
`XMemAllocatePhysicalPages(0x380, pages, array)` for `size >> 16` pages, then reserves its VA window with two
`0x20402000` placeholders (attrs `0x21`/`0x3`, protect `4`/`0x801`) — or **reuses the initialiser's
placeholders** (`0x1471BF970`/`978`/`980`) when they exist — and falls back to one committed
`0x20402000`/`0x23`/`0x2004` region when a GPU memory-manager object (`0x148759D00`) is present, mapping the
pages into it with `XMemMapPhysicalPages`. Its page-number tables pack **21-bit page numbers, three per 64-bit
word** (`0x193DE10` builds the same 3-per-word bitmap); page number = 4 KiB frame number of a 64 KiB page,
which agrees with the shim's AWE emulation. `0x1E86440` is the windowed form: reserve `0x42000` bytes
(`0x20402000`/`0x7`/`1`), `VirtualFree(...,0x8002)` the pieces, then five `0x20406000`/`0x27` window carves
at call sites `0x1E865A3`, `0x1E865D5`, `0x1E86605`, `0x1E86635`, `0x1E86671` (the five window rows of the
section-19.2 boot log), map the physical pages into them, and submit a work item through
`[object+0x428]`. Plain committed ranges come from `0x5BA6C10` (`0x1400000`, `0x20403000`/`0x181`/`4`),
`0x1D6E620` (caller-sized, same shape) and `0x193E220`.

| allocation type | page mode | host meaning (shim) | protect values seen | uses here |
|---:|---|---|---|---|
| `0x20003000` | 2 MiB large pages | `VirtualAlloc2` commit+reserve, 2 MiB alignment | `0x22004`, `0x2404` | the four heap pools (the arena is the case-2 pool) |
| `0x20402000` | 64 KiB pages | placeholder reserve (`MEM_RESERVE_PLACEHOLDER`) | `0x4`, `0x801`, `0x2004` | VA windows for physical pages; giant VA spans |
| `0x20403000` | 64 KiB | plain commit (`VirtualAlloc`/`VirtualAlloc2`) | `0x4` | standalone committed ranges |
| `0x20406000` | 64 KiB | reserve-only window carved from a placeholder | `0x1004`, `0x1404`, `0x8404`, `0x2404`, `0x2004` | the five page-map windows |
| `0x42000` | 64 KiB | standalone placeholder | `0x1` | the window-set reservation above |

The protect words are the console's `PAGE_GRAPHICS_*` bits over `PAGE_READWRITE`
(`0x2004` = RW + graphics read/write, `0x22004` adds coherent, `0x2404` adds nocache); that reading is the
prior page's inference from the constants, restated here only because the pools choose those words.

## 7. Where our answers are consumed

| our answer | recorded / consumed at | consumer effect | source |
|---|---|---|---|
| `XMemVirtualAlloc` placeholder answers (16/24 GiB each) | `0x1471BF970`, `0x1471BF978`, size `0x1471BF980` | the page-pool initialiser reuses them instead of reserving its own window (`0x1E6C110`), and one branch releases them with `VirtualFree(...,0x8000)` before committing its own region | M |
| `XMemVirtualAlloc` large-page answer for the arena (`0x25400000` at caller `0x1E1D28D`) | case-2 pool registry `0x1471C2808`; the heap anchored to it | the title heap object sits at a constant `0x3473020` from the returned pointer in all four measured placement arms ([vertex_buffer_va.md](vertex_buffer_va.md) section 19.3) | M (static call site + boot rows) |
| the other three large-page answers (`0x49400000`, `0x23300000`, `0x89700000`) | pools 0, 4, 5 with their own registries (`0x1471C1560`, `0x147444B30`, `0x1474438B0`) | serve pages to other subsystems; the job/zero-block chain only uses the case-2 arena | M (static) / I (which subsystem each pool serves) |
| requested size | pool size table read at `[0x1471BF9B0 + i*8]`; slot `+0x8` of each pool entry holds the served size | the pool uses the whole served size as its range; the committed subrange the title reports at the crash (`0x212C8000`-`0x212C9000` of `0x25400000`) is title-side bookkeeping inside our answer | M (call shape) / boot rows |
| alignment | shim's 2 MiB-granularity answer (`page_mode == large` sets page and base granularity to 2 MiB, `VirtualAlloc2` with `MEM_ADDRESS_REQUIREMENTS`) | the title treats the answer as its pool base; the alignment arm (`align:0x1000000`) was measured to move the pointer and the heap with it, not to survive | M (shim code) + boot rows |
| commit state and protection | `VirtualQuery` at the block/allocator (`0x212C8000` committed under an `0x25400000` answer); page windows' protections are applied per page by the map path | the title reads allocation base/region/state; the windows receive their protection at map time (the shim records it and applies it through the placeholder APIs) | M (shim code; boot rows for Query) |
| the page windows (`0x20406000` answers) | immediately passed to `XMemMapPhysicalPages` and recorded in title bitmaps | a window answer is the destination address for 64 KiB section views; the page numbers are the title's own 21-bit entries | M |
| a refused `XMemVirtualAlloc` | `int3` at the initialiser sites | a refusal is a crash: no fallback path in this layer | M |
| `XMemGetWorkingSetStatistics` answer | not in this chain (its three sites are outside the heap/arena code) | nothing in the heap contract depends on it | M (call-site census) |
| VA-space descriptor | `D3D12ConfigureVirtualMemoryX` import (thunk `0x5F3FD60`) in the initialiser, fed the 16/24 GiB window size and the computed pool count | the console graphics runtime is configured with the same VA window the XMem placeholders reserve: the heap arena and the graphics VA model are two users of one configured address space | M |

## 8. The graphics VA/page model, statically

The same service layer creates both the title's VA space and its GPU-visible pages, which is why the graphics
ranges and the heap are one XMem family and still different memory kinds:

| element | static evidence | matches |
|---|---|---|
| VA space configured with the graphics runtime | the initialiser passes its 32-byte descriptor (window size and pool count) to `D3D12ConfigureVirtualMemoryX` (`0x5F3FD60`) before the pool loop | M (static call site; the import name from the import table) |
| two 16/24 GiB placeholder VA spans | initialiser steps 2-3 | the measured 0x600000000 answers (section 19.2 rows 1-2) |
| physical-page windows: 64 KiB pages, page numbers = 21-bit entries, 3 per word, first-touch commit | `0x1E6C110`, `0x193DE10`, `0x1E86440` | the shim's AWE emulation (64 KiB pagefile sections, page number = slot*16, `0x10000` page, 4 KiB first-touch commit) |
| placed buffers / console descriptor heaps are title VAs inside one XMem region, overlapping | not read here as code; the runtime pages measured them | [page_address_space.md](page_address_space.md) sections 2 and 4 (placed buffer 182,452,224 B at title VA `0x1CD10B00000`, descriptor heap `0x2600000` B at `0x1CD10000000`, overlap 27 MiB) |
| the heap arena is **not** in that model | large-page commit, `MEM_PRIVATE`, `VirtualQuery` state committed | [vertex_buffer_va.md](vertex_buffer_va.md) sections 15.2/19.2 (block, job, allocator all in one `MEM_PRIVATE` region) |
| placed-resource import is refused, stand-in used | no import call appears in the heap chain; the Shader-visible VA is aliased/rebase | [replay_resolution.md](replay_resolution.md), [page_address_space.md](page_address_space.md) section 1, [title_memory_model.md](title_memory_model.md) section 3.1 |
| title pages are copied into the arena per draw where aliasing is impossible | replay-side, not title code | [page_content.md](page_content.md), [title_memory_model.md](title_memory_model.md) section 2.1 |

The static reading adds one boundary: the heap arena's pages are **ours in the sense that our answer created
the region**, but every range decision inside it (segment, chunk, bin, registry node) is title code over those
pages, and the graphics VA map (console VAs, Vk device addresses, arena offsets) is a different namespace from
the title's heap addresses.

## 9. The class-0xa zero-size path, end to end

Static chain, with the runtime steps already measured in [vertex_buffer_va.md](vertex_buffer_va.md) sections
14-16 cited as such:

| # | step | what the code does | source |
|---:|---|---|---|
| 1 | the class is a compile-time constant | the image's format table `0x658DC80` has a 16-byte record keyed `0xA`; the command object's slot 0x20 is `0x23E550` whose body is `mov eax,0xa; ret` | M (static) + measured boot (sections 16.2-16.3) |
| 2 | the format object is cached | `0x1B37440` allocates a 0x20-byte object with vtable `0x6597C60`, fills the record's words, appends to the owner's `+0x98` array; seeded from `0x1B373F0` | M |
| 3 | job creation | `0x184EB0` allocates the 0xA0 job through slot 0x70 with the flush object as key, then `0x185570` stores the command object at `job+0x30` and the format object at `job+0x58` | M |
| 4 | class getter | `job+0x28` = `0x6FECC0` (`mov rcx,[rcx+0x58]; jmp [rax+8]`); the format object's slot 8 is `0x6FECE0` (`return *(u32*)(this+8)`) → 0xA | M |
| 5 | helper entry | `0x45594F0(class 0xA, planes 1, dims 256x1)`, reached from the executor at `0x1335D30` | M + measured (section 16.2 step 8) |
| 6 | size computation | `0x455A610` sums `0x1555B60(class, width, height)` = `height * 0x1555C10(class, width)`; `0x1555C10` has no case for 0xA → `0x100 * 0 = 0` | M (static decompile) + measured (section 16.3) |
| 7 | block request | slot 0x50 asks the default heap's inner allocator for `0x0` bytes; `(0+0xf)&0x7f0 = 0`; class 0; bin search index clamps to 0x3F (`0x538D6C0`) | M |
| 8 | entry written | the helper stores `{width 0x100, height 1, stride 0, size 0, block}` (entry written at `0x4559684`) | M + measured bytes (section 15.4) |
| 9 | copy | `0x550E2D0` copies `*(command+0x38) << 4` = `0x100` positions x 16 bytes = `0x1000` bytes to `entry+0x18` | M (copy loop) + measured range (section 14.3) |

**The exact heap state this produces.** At the block request the inner allocator sees `size = 0`; the rounded
class is 0 and the bin lookup uses index 0x3F. If that bin holds a free class-0 chunk, the request takes it;
if it does not, the grow path supplies a fresh 0x1000-byte segment and the chunk is split. In both cases the
split for a **zero-byte** request degenerates: the remainder starts at the *same address* as the returned
block (`0x538D410`: `remainder = chunk + rounded_size` with `rounded_size = 0`), so

| moment | expected heap state |
|---|---|
| before the call | bin 0x3F may hold N class-0 chunks (the "zero-class free tree" of the measured boots); counters `+0x250 = C`, `+0x238 = B` |
| at return | returned pointer = `P`; a zero-size chunk is marked allocated at `P`; the remainder chunk is inserted into a bin *at the same address* `P`, so the region's first 0x10..0x20 bytes carry both the allocated zero chunk's marks and the free chunk's header/link words; `+0x250 = C+1`, `+0x238 = B+0` |
| after the copy | the 0x1000 bytes at `P` overwrite (a) the live values of whatever chunk started at `P`, and (b) beyond it, the next bytes in the segment; the allocator's next bin read, free, or classed-tree lookup reads a corrupted node — the measured stops `0x538D0E6` (classed tree read, section 15's third catch), `0x515ACF5` (free-path node read), `0x13360A3`/`0x1335DCB` (executor reads over the corrupted command object) |
| victim election | if the class-0 chunk came from a previously freed region the block sits below the job (`job-0xEF0`, prefix boot) and the copy covers the live job; if it came from the segment bump the block sits above the job (`job+0xA0`/`+0x110`) and the copy runs into free metadata. The measured four placement arms reproduce both shapes with our switches off, so the loading history, not our answer, elects the victim ([vertex_buffer_va.md](vertex_buffer_va.md) section 19.6) |

That is the whole mechanism at the allocator: the title asks its own allocator for zero usable bytes, the
allocator honestly returns a zero-size chunk, and the copy's 16-bytes-per-element arithmetic writes a 0x1000-byte
payload into a chunk that has no payload. Our shims are outside it; the next section ranks what a shim could
still change.

## 10. A ranked list of memory-model hypotheses

Ranking: cheap to falsify and close to a *measurable attribution* first; pure placement levers last, because
[vertex_buffer_va.md](vertex_buffer_va.md) section 19 already measured three of them negative. "Our file" is
the file in this repository a test or probe would touch; none of these is implemented by this slice.

| rank | hypothesis (class) | our file(s) | bounded falsifier | expected if true |
|---:|---|---|---|---|
| 1 | **H1 Write-watch on the arena (write-watch).** The arena is plain committed private memory, so unlike the pagefile-section views that refused `GetWriteWatch`, our shim could create it with `MEM_WRITE_WATCH` and read the dirty-page list after the copy. | `port/shims/xmem.cpp` (large-pages branch: add `MEM_WRITE_WATCH` to the host type for the measured arena caller), `port/src/main.cpp` (one alloc-watch payload reading `GetWriteWatch` at the block) | one stage-3 boot with the arena created write-watch and a `GetWriteWatch` after the block allocation: expect dirty pages inside `[block, block+0x1000)` | `GetWriteWatch` succeeds and names the overrun's pages: the corruption becomes a measured address set (attribution); `ERROR_INVALID_PARAMETER` because the 2 MiB-granularity reservation cannot be watched closes the class |
| 2 | **H2 Guard-page watch on the arena (write-watch).** The landed guard-page tracker arms committed writable pages and reports the first write exception; arm it on the arena range before stage 3. | `port/shims/xmem.cpp` (`page_watch_arm`/`page_watch_poll` already exist), consumer in `port/src/vulkan_backend.h`, arming from the existing stage-3 poll in `port/src/main.cpp` | one stage-3 boot with the tracker armed on the heap arena: expect a guarded write whose faulting address lies in `[block, block+0x1000)` and is not a read | a recorded guard hit names the overrun page (attribution, same family as H1); `unavailable`/no hit means the copy's destination was not the guessed range, which itself falsifies the placement assumption |
| 3 | **H3 Fully committed answer (page commit).** Our answer is committed in the host sense, but the title's own bookkeeping marks only `0x212C8xxx` of the `0x25400000` served as committed; pre-touching every page before returning could change what its grow/limit code sees. | `port/shims/xmem.cpp` (new opt-in arm: touch one byte per 2 MiB page of the large-page answer before returning it) | one stage-3 boot; read `VirtualQuery` at the block and the job plus the block↔job offset: expect the committed subrange to grow or the block band to move | a measured change says the title consumes our commit state for its pool bookkeeping; no change says it only consumes the base and size, and the commit lever is closed |
| 4 | **H4 Larger served size (layout/size).** Only the front/alignment arms were measured; a **tail** extension of the same answer (same base, larger served range) is untested and could move the segment growth/free-tree history. | `port/shims/xmem.cpp` (new `XmemLayoutMode::extend` or an argument to `prefix`-style placement) | one stage-3 boot with the arena answer extended by e.g. 64 MiB at the tail; compare block−job and the stop family with the control | different block−job offset or a surviving pass says the size is a lever; identical layout says the arena's tail is outside every decision the heap makes (consistent with section 19.6's "window/prefix/guard moved nothing structural") |
| 5 | **H5 Host page granularity (page commit).** Serve the 2 MiB-mode request as 4 KiB/64 KiB host commits (same answer and size) so `VirtualQuery` granularity and the title's page arithmetic see different regions. | `port/shims/xmem.cpp` (`page_mode == kXmemLargePages` branch: keep guest rounding, drop the host 2 MiB requirement under a switch) | one stage-3 boot; `VirtualQuery` region size at the block and the block−job offset against the control | a different region shape with an unchanged block band closes the granularity lever; a moved victim would make the commit granularity a measured input |
| 6 | **H6 Heap limit from the served size (layout/commit).** The heap's limit (`inner+0x240`, set through vtable slot 6 `0x7128D0`, floored at the footprint) gates when the grow path asks the grower instead of the page source; if it derives from the served arena size, a different answer changes the allocation history. | `port/shims/xmem.cpp` (any layout/size arm) plus the existing alloc-watch payload in `port/src/main.cpp` (read `allocator+0xA8 → +0x240` and `+0x238`/`+0x250` at the block) | one stage-3 boot per arm; print the limit and the two counters at the block allocation | equal limit/counters across arms says the limit is a title constant; a different limit says the arena contract feeds the allocator's slow path and H4 should be re-ranked above it |
| 7 | **H7 Arena headroom for staged content (layout/content).** The replay's bounded title-span staging refuses when the arena has no room (`title span staging: refused no arena room`); a larger served arena is the lever for the content-pass vertex stream. | `port/shims/xmem.cpp` (size arm), `port/src/vulkan_replay.h` (`stage_title_span`, eight 4 MiB ranges) | one content-window boot with `PALMETTO_GRAPHICS_CONSOLE_ROOT=1` (the section-18.5 acceptance) and a larger arena answer: expect the staging refusal line to disappear and draw B's driver block to be fed | the refusal disappears and the content acceptance is reached in that arm: unblocks a later content step; unchanged refusal says arena room is not the constraint |
| 8 | **H8 The producer, not the shim (title-side).** The only fix that sizes the block is the title's own row-stride switch (`0x1555C10` has no case for class 0xA; a case returning `0x100` makes block and copy agree) or a different upload path. | none in this repository (no shim answer participates); a probe in `port/src/main.cpp` can only measure it | a patched title image whose `0x1555C10` returns `0x100` for 0xA, one boot: expect the entry `{256,1,0x100,0x1000,block}` and the allocator stops to disappear | confirms sections 15-16 and closes the shim question: the overrun is title-side and no layer answer sizes the block |

The honest verdict of the ranking: every cheap shim lever changes *where or when* the overrun surfaces
(attribution) rather than whether it happens; the placement family is measured negative, and the only
sizing lever is the title's own table. H1/H2 are ranked first because they turn the silent corruption into a
measured address/page set at the cost of one probe payload, which is the smallest next measurement.

## 11. Limits

- **Pool sizes are not decoded.** The 9-slot size table at `0x1471BF9B0` is `.bss` and is written before
  `0x1E1CF60` by a producer this slice did not find; only the four measured requests are claimed.
- **The heap descriptor argument is not decoded.** The 4th argument of `0x17242B0` (flags/granule/limit) is
  recovered only as field copies (`+0x38`..`+0x60`, `+0xA0`, `+0xA1`); its producer is upstream of the getter.
- **Which code calls slot 4 with the region base is not decoded.** Registration in the address-keyed tree is
  certain from the insert code and from the lookup callers; the caller that binds a heap to its arena range is
  inferred (the pool managers are the only code that knows the base).
- **The bin-index formula is read, not derived.** `0x515B1D0` computes the small-bin index from the chunk's own
  class fields with a clamp; the exact mapping class→bin and the meaning of bin 0x3F for class 0 are read from
  the code shape, not measured against a boot.
- **The tree walk's key is an address.** Every consumer (`0x538D040`, `0x515ABB0`, `0x54C8AF0`, `0x45D9930`,
  `0x45D9760`) walks the same ordered tree comparing a pointer value; the payload is `node+0x18 & ~3`. The
  earlier description of slot 0x70 as "classed" is refined here: the key is an *owner address*, and the
  allocation happens in that key's heap.
- **No runtime probe was added or run.** Every claim marked **M** is a static read; the boot numbers quoted
  are from the named prior pages, not from this slice.
- **The page-write instruments are not validated on this memory.** H1's `MEM_WRITE_WATCH` compatibility with a
  2 MiB-granularity `VirtualAlloc2` reservation and H2's guard arming over the arena are both untested here;
  their falsifiers are exactly those refusals.
- **One image, one build.** RVAs belong to the retail build the campaign uses; a re-import or a different
  image needs the function map re-run (`rva2func.py`).

## 12. Tests and gates

- **No test target changed.** This slice adds one documentation page; no runtime, shim, test, switch or
  default path was touched, so no test binary's behaviour can move.
- **No new switch is introduced or renamed.** The page names only switches that already exist and are already
  documented: `PALMETTO_ALLOC_WATCH`, `PALMETTO_ALLOC_WATCH_FREE`, `PALMETTO_XMEM_LAYOUT`,
  `PALMETTO_XMEM_LAYOUT_CALLER`, `PALMETTO_CARD_COPY_WATCH`, `PALMETTO_GRAPHICS_CONSOLE_ROOT`. The
  environment-variable check (`tests/test_docs_env_vars.py`) enumerates tracked files, so an untracked page is
  outside it until the lead's commit adds the page to the tree.
- **No boot lock, no boot, no GPU call** was used by this slice; the Ghidra project was opened read-only and
  no project save was made.
- **No git write.** The page is an untracked file at slice end; the lead's commit owns it.

## 13. Attribution

This page was assembled from this project's own static analysis of the user's own image: the decompiled and
disassembled functions named by RVA above (`0x1E1CF60`, `0x1E6C110`, `0x1E86440`, `0x193DE10`, `0x5BA6C10`,
`0x1D6E620`, `0x5B66140`, `0x17242B0`, `0x17243B0`, `0x17243E0`, the `0x663BDE0` vtable and its members,
`0x538D220`, `0x538D410`, `0x538D6C0`, `0x538DE00`, `0x515A040`, `0x515B130`, `0x515B1D0`, `0x7032F0`,
`0x705350`, `0x703440`, `0x704A70`, `0x284000`, `0x45594F0`, `0x550E2D0`, `0x184EB0`), the PE import table
and the RIP-relative/caller scans, with the runtime numbers quoted from the prior pages named in each row. The
title functions were decompiled with the project's Ghidra tooling from the user's own copy of the image; no
peer code was used and no game data is added. RVAs, field offsets, sizes, allocation types, protect words,
counts and outcomes only.
