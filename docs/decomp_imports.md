# Runtime imports: the call-site map, the shim answer and the first-visible gate list

**Status: static, from the file and from our own tree; no boot was run for this page.** The retail image is a PE32+
x86-64 executable of 120,181,760 bytes on disk, image base `0x140000000`, size of image 184,995,840, entry RVA
`0x56A1EC8`, 299,257 `.pdata` functions. Only sizes, RVAs, structure offsets, import names and counts appear here; no
game strings, asset bytes, package paths or hashes.

This page answers one question for the next slice: **of everything the title imports, what does our runtime already
answer, what is a stand-in or a named refusal, and which of those calls sit on the stage-3, first-run, UI or movie path
that has to produce the first visible content frame.** It is the groundwork map; it does not implement or boot anything.

## 1. Method, and what "measured" means here

| what was measured | from | how it is marked below |
|---|---|---|
| the 587 import entries of the retail image, in 68 DLLs | PE import directory, `tooling/import_call_sites.py` (`import_entries`) | measured |
| the code stubs and every call site that reaches them | byte scan of the executable sections: `FF 15`/`FF 25` RIP-relative references to an IAT slot, and `E8`/`E9 rel32` to an incremental-link `jmp [IAT]` stub | measured |
| the GDK runtime surface: 17 classes, 19 interface entries, 161 statically linked thunks | `tooling/gdk_runtime_calls.py` on the same image, names from `tooling/gdk_runtime_classes.json`; the helper is at `0x55DE048` | measured |
| which subsystem each call site belongs to | the containing `.pdata` function of the call site, matched against function RVAs named by the repository's decomp and boot pages | **inferred** (phase attribution) |
| the shim answer for an import | the `SHIM_TABLE` entries in `port/shims/*.cpp` | measured from our tree |
| the behaviour of an answer | the existing measured pages cited per row | measured elsewhere, cited |

**Measured vs inferred, precisely.** The import entries, stub counts, call-site counts and the shim tables are counted
from bytes. The **phase column is inferred**: a call site is put in a phase only when its containing function is one
that a decomp or boot page already names (the RVAs are cited in sections 3 and 5). A call site in a function no page
has named stays *unattributed*, and the page says so rather than guessing. For the seven console DLLs 85 of 97 sites
are attributed; for the GDK thunks 200 of 358 sites are; for the other import families most sites are unattributed
because no decomp slice has named their callers yet.

**Limits of the count.** A call through a register after a `mov reg,[IAT]` is not seen, so the site counts are a lower
bound for indirect uses and an upper bound for direct ones (a stray `E8`/`E9` byte that lands on a stub would be
counted). 60 of the 587 imports have **no static call site at all**; they are listed as unreferenced in section 4.8, not
as missing answers. The call graph is not a reachability proof: a site is code that *can* call, and the boot pages say
which ones a boot actually reached.

**Ghidra.** The analysed project at `D:\leonida-work\classa\ghidra-proj.rep` was left untouched; the counts here were
re-derived with the repository's own PE tools so that anyone can reproduce them from the file with the two commands
below. The decomp pages this page cross-references are the ones that used the project read-only.

```powershell
python -I tooling/import_call_sites.py <image> --json
python -I tooling/gdk_runtime_calls.py <image> --json --out <dir>
```

**Cross-references used.** `docs/decomp_first_ui.md` (first-run, UI, movie path), `docs/decomp_renderer.md` (renderer
command graph and present path), `docs/decomp_jobs.md` (job/thread timing) and `docs/decomp_memory.md` (title heap,
XMem arena and graphics VA model) landed while this page was written and are used in sections 4, 5 and 6.
`docs/decomp_stage.md` was **not present** at the time of writing, so the stage-machine half of the cross-reference is
marked pending for that page.

## 2. The imported surface (measured)

All 587 import entries bind at the dry run's mapping stage (the per-image table is
the local, gitignored `reports/pe/game_scarlett_master.exe.md` (not part of the repository)); this page is about the code
calls into them, not about binding.

| count | value | note |
|---|---:|---|
| import entries | 587 | in 68 DLLs (68 import descriptors) |
| IAT code stubs (`jmp [IAT]`) | 378 | incremental-link stubs; the other entries are called straight through `FF 15` |
| direct `call [IAT]` sites | 3,444 | `FF 15` in the executable sections |
| calls to a stub | 35,060 | `E8 rel32` to one of the 378 stubs |
| tail jumps to a stub | 512 | `E9 rel32`; these replace a call plus return |
| **all import call sites** | **39,016** | 587 entries, 68 DLLs |
| imports with no static call site | 60 | dead imports the linker kept, listed in section 4.8 |
| imports named in one of our `SHIM_TABLE`s | 95 | 37 console runtime + 15 WinHttp + 7 COM + 3 bcrypt + 1 GameInput + the file/loader/process/memory names in 4.7 |
| imports bound to a bundled package image | 209 | the four package libraries and the three bundled CRT DLLs |
| imports bound to the host | 283 | 341 host-resolved entries minus the 58 host entries our shims intercept by name |

The dry run's own split ([`docs/runtime_imports.md`](runtime_imports.md), "The retail executable") is bundled 209,
console-shim 37, host 341, and it reports 0 unimplemented imports on this tree. This page's 95 is larger than the dry
run's 37 because the dry run calls "shim" only the unhosted console DLLs, while our tables also replace host names
(`WinHttp*`, the file paths, COM apartments, the CRT argv/exit family, `VirtualAlloc` for title memory, `GameInputCreate`,
`BCrypt*`).

**The GDK surface is not an import.** The executable imports no `xgameruntime.dll` function. Its 161 `xgameruntime.lib`
thunks call the shared helper at `0x55DE048`, which asks `QueryApiImpl` for a class/interface and then calls one vtable
slot. The 17 classes and their 161 thunks are the runtime's class surface and are counted separately in section 3.2;
`port/shims/xgameruntime.cpp` and its four sibling files serve them.

## 3. Subsystem and phase map

### 3.1 Import families

Call sites attributed to a phase by the containing-function method of section 1; **unattr** is the remainder. The
phase names are this campaign's: `boot`, `loader`, `stage 3`, `frame`, `UI/movie`, `audio`, `save/store`, `other`.

| subsystem | imports | IAT stubs | call sites | boot | loader | stage 3 | frame | UI/movie | audio | save/store | other | unattr |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| console runtime DLLs (7) | 37 | 37 | 97 | 5 | 37 | 0 | 12 | 1 | 30 | 0 | 0 | 12 |
| Bink2Scarlett.dll (movie) | 23 | 1 | 33 | 0 | 0 | 0 | 0 | 32 | 0 | 0 | 0 | 1 |
| libHttpClient.GDK.dll (HTTP) | 38 | 38 | 72 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 72 |
| XboxBvhBuilder.dll (ray tracing) | 7 | 0 | 25 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 24 |
| amd_fidelityfx_loader_xbox.dll (upscale loader) | 4 | 4 | 6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 6 |
| MSVCP140 / VCRUNTIME140* (bundled VC runtime) | 137 | 90 | 27,665 | 0 | 15 | 0 | 5 | 40 | 9 | 11 | 5 | 27,580 |
| WINHTTP.dll (offline shim) | 15 | 0 | 38 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 36 |
| WS2_32.dll (host sockets) | 25 | 4 | 87 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 87 |
| file API set | 23 | 0 | 41 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 38 |
| library-loader API set | 9 | 2 | 24 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 24 |
| COM API set | 7 | 0 | 13 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 11 |
| process/thread/memory/synch core sets | 77 | 35 | 258 | 5 | 5 | 0 | 5 | 0 | 2 | 0 | 0 | 241 |
| window messages (ext-ms ntuser) | 7 | 1 | 3 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 |
| CRT API sets | 162 | 155 | 10,373 | 3 | 0 | 0 | 10 | 7 | 0 | 7 | 5 | 10,341 |
| bcrypt.dll | 3 | 3 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 |
| ADVAPI32.dll (registry, host) | 3 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 |
| MFPlat/MFReadWrite (host media) | 7 | 7 | 8 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 8 |
| GameInput.dll | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| api-ms-win-core-profile (host) | 2 | 0 | 266 | 0 | 0 | 1 | 1 | 0 | 0 | 0 | 0 | 264 |

The console DLLs and the GDK thunks are the only families where the phase column says much: they are the ones the
existing decomp pages name. The VC runtime and CRT rows are enormous because every function in the image calls the CRT;
their attributed sites are the same caller functions as the rows above and are not a second copy of the work.

### 3.2 The GDK runtime classes

`slots` counts the slots the image calls (a slot the image has a thunk for but calls from nowhere is included in
`thunks`); `sites` is the direct call sites to the class's thunks. "Served by" is the port/shims file that registers the
class GUID; `xgameruntime.cpp` is the common file, the `_user`/`_gamesave`/`_store`/`_offline` files are its faces.

| class | interfaces | slots | thunks | sites | boot | loader | frame | UI/movie | save/store | other | unattr | served by |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| XThreadingImpl | 1 | 18 | 18 | 134 | 5 | 0 | 1 | 3 | 6 | 5 | 114 | xgameruntime.cpp |
| XStoreImpl | 2 | 44 | 44 | 74 | 0 | 0 | 0 | 0 | 66 | 0 | 8 | xgameruntime.cpp, xgameruntime_store.cpp |
| XUserImpl | 2 | 21 | 21 | 39 | 1 | 0 | 4 | 1 | 0 | 16 | 17 | xgameruntime.cpp, xgameruntime_user.cpp |
| XPackageImpl | 1 | 18 | 18 | 31 | 6 | 0 | 0 | 0 | 23 | 0 | 2 | xgameruntime.cpp |
| XGameSaveImpl | 1 | 20 | 20 | 22 | 0 | 0 | 0 | 0 | 19 | 0 | 3 | xgameruntime.cpp, xgameruntime_gamesave.cpp |
| XNetworkingImpl | 1 | 10 | 10 | 12 | 5 | 0 | 0 | 0 | 0 | 7 | 0 | xgameruntime.cpp |
| XAccessibilityImpl | 1 | 10 | 10 | 10 | 0 | 0 | 0 | 8 | 0 | 0 | 2 | xgameruntime.cpp, xgameruntime_offline.cpp |
| XGameUiImpl | 1 | 5 | 5 | 8 | 0 | 0 | 0 | 8 | 0 | 0 | 0 | xgameruntime.cpp, xgameruntime_offline.cpp |
| XSystemImpl | 1 | 3 | 3 | 8 | 2 | 0 | 0 | 0 | 0 | 3 | 3 | xgameruntime.cpp |
| XGameImpl | 1 | 2 | 2 | 6 | 1 | 0 | 0 | 0 | 0 | 1 | 4 | xgameruntime.cpp |
| XLauncherImpl | 1 | 3 | 3 | 3 | 0 | 0 | 0 | 2 | 0 | 0 | 1 | xgameruntime.cpp, xgameruntime_offline.cpp |
| XPersistentLocalStorageImpl | 1 | 2 | 2 | 3 | 0 | 0 | 0 | 0 | 2 | 0 | 1 | xgameruntime.cpp |
| XDisplayImpl | 1 | 1 | 1 | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | xgameruntime.cpp, xgameruntime_offline.cpp |
| XGameRuntimeFeatureImpl | 1 | 1 | 1 | 2 | 1 | 0 | 0 | 0 | 0 | 0 | 1 | xgameruntime.cpp |
| XSystemAnalyticsImpl | 1 | 1 | 1 | 2 | 0 | 0 | 1 | 0 | 0 | 0 | 1 | xgameruntime.cpp |
| XAppCaptureImpl | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | xgameruntime.cpp (known, **not served**) |
| XGameEventImpl | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | xgameruntime.cpp, xgameruntime_offline.cpp |

The 161 thunks produce 358 direct call sites. The largest unattributed block is `XThreadingImpl` (114 sites): most of
its callers are the statically linked runtime library's own block around the thunks (`0x55D..`-`0x563..`), i.e. the
library calling itself (async blocks, queue wrappers), not title game code. The class shape convention is
`Xxx(args);` → `QueryApiImpl(class, interface)`, then `slot(this, args...)`, then `Release`. The argument count after
`this` is estimated per slot by `tooling/gdk_runtime_calls.py`; the full per-slot table with argument estimates is that
tool's own text output, reproducible with the command in section 1 (for example `XUserRegisterForChangeEvent` args~4,
`XDspConnectWithMaximumStreamLimit` is exported as a DLL function rather than a class slot).

### 3.3 The 37 console runtime imports, one row each

`stubs` is the incremental-link stub count; the call form column shows how the sites reach the import. Phases are the
same attribution as 3.1; `-` means no site on a named path. No console import has a site in the stage-3 handlers
themselves — the stage-3 work is done by renderer and loader functions, which is why stage 3 appears in the frame and
loader columns rather than its own.

| import | stubs | sites | call form | phase attribution |
|---|---:|---:|---|---|
| `xmem.dll!XMemAlloc` | 1 | 3 | 3 thunk calls | frame 1, UI/movie 1, audio 1 |
| `xmem.dll!XMemAllocDefault` | 1 | 1 | 1 thunk tail | - (tail site has no containing function) |
| `xmem.dll!XMemAllocatePhysicalPages` | 1 | 4 | 4 thunk calls | loader 2, audio 1, - 1 |
| `xmem.dll!XMemFree` | 1 | 3 | 2 thunk calls, 1 thunk tail | frame 1, audio 1, - 1 |
| `xmem.dll!XMemFreeDefault` | 1 | 1 | 1 thunk tail | - |
| `xmem.dll!XMemFreePhysicalPages` | 1 | 3 | 3 thunk calls | loader 2, frame 1 |
| `xmem.dll!XMemGetWorkingSetStatistics` | 1 | 3 | 3 thunk calls | boot 2, frame 1 |
| `xmem.dll!XMemIsToolsMemAvailable` | 1 | 1 | 1 thunk call | boot 1 |
| `xmem.dll!XMemMapPhysicalPages` | 1 | 12 | 12 thunk calls | loader 6, frame 5, - 1 |
| `xmem.dll!XMemMapPhysicalPagesScatter` | 1 | 2 | 2 thunk calls | - 2 |
| `xmem.dll!XMemTransferMemory` | 1 | 1 | 1 thunk call | boot 1 |
| `xmem.dll!XMemVirtualAlloc` | 1 | 23 | 23 thunk calls | loader 19, audio 2, boot 1, frame 1 |
| `acphal.dll!AcpHalCreate` | 1 | 1 | 1 thunk call | audio 1 |
| `acphal.dll!AcpHalAllocateShapeContexts` | 1 | 1 | 1 thunk call | audio 1 |
| `acphal.dll!AcpHalReleaseShapeContexts` | 1 | 1 | 1 thunk call | audio 1 |
| `acphal.dll!ApuCreateHeap` | 1 | 1 | 1 thunk call | audio 1 |
| `acphal.dll!ApuAlloc` | 1 | 1 | 1 thunk call | audio 1 |
| `acphal.dll!ApuFree` | 1 | 1 | 1 thunk tail | - (leaf wrapper) |
| `acphal.dll!ApuIsVirtualAddressValid` | 1 | 4 | 4 thunk calls | audio 4 |
| `acphal.dll!ApuMapVirtualAddress` | 1 | 4 | 4 thunk calls | audio 4 |
| `acphal.dll!ApuMapApuAddress` | 1 | 2 | 2 thunk calls | audio 2 |
| `xapu.dll!XApuConnect` | 1 | 1 | 1 thunk call | audio 1 |
| `xapu.dll!XApuDisconnect` | 1 | 1 | 1 thunk call | - 1 |
| `xapu.dll!XApuEnqueueCommand` | 1 | 4 | 4 thunk calls | audio 4 |
| `xapu.dll!XApuGetResultCount` | 1 | 1 | 1 thunk call | audio 1 |
| `xapu.dll!XApuDequeueResult` | 1 | 1 | 1 thunk call | audio 1 |
| `chad.dll!XDspConnectWithMaximumStreamLimit` | 1 | 1 | 1 thunk call | audio 1 |
| `chad.dll!XDspActivate` | 1 | 1 | 1 thunk call | audio 1 |
| `chad.dll!XDspSubmitCommand` | 1 | 1 | 1 thunk call | audio 1 |
| `chad.dll!XDspDeactivate` | 1 | 1 | 1 thunk tail | - (leaf wrapper) |
| `chad.dll!XDspDisconnect` | 1 | 1 | 1 thunk tail | - (leaf wrapper) |
| `d3d12_xs.dll!D3D12XboxCreateDevice` | 1 | 1 | 1 thunk call | frame 1 |
| `d3d12_xs.dll!D3D12SerializeVersionedRootSignature` | 1 | 2 | 2 thunk calls | frame 1, - 1 |
| `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` | 1 | 1 | 1 thunk call | loader 1 |
| `xg_xs.dll!XGSuggestSwizzleMode` | 1 | 2 | 2 thunk calls | loader 2 |
| `xg_xs.dll!XGCreateTextureComputer` | 1 | 4 | 4 thunk calls | loader 4 |
| `xcrdapi.dll!DStorageGetFactory` | 1 | 1 | 1 thunk call | loader 1 |

The loader sites of `XMemVirtualAlloc` are the XMem arena/page-window setup: the one-shot initialiser `0x1E1CF60`
(16/24 GiB placeholders and the `D3D12ConfigureVirtualMemoryX` call), the windowed page-pool forms `0x1E86440` and
`0x1E6C110`, and the placed-texture fill `0x193DE10`; the four physical-page imports map 64 KiB views out of those
windows (`XMemAllocatePhysicalPages` at `0x1E6C22D`/`0x1E866E4`, `XMemMapPhysicalPages` at
`0x1E6C295`/`0x1E86738`, `XMemFreePhysicalPages` at `0x1E6C32D`/`0x1E868E2`), which is the heap-arena chain of
[`docs/decomp_memory.md`](decomp_memory.md) section 6. Its `frame` site is the renderer device setup (`0x2478440`) and
its `boot` site is the startup-memory cluster (`0x2216390`); the three `XMemGetWorkingSetStatistics` sites are outside
the heap/arena chain. The two "frame" physical-page calls are `0x2473090`/`0x2474660`, in the same renderer cluster as
`0x2478440`.

## 4. Shapes, shim tables and status, group by group

Status vocabulary, used exactly: **answered** = the shim returns the console meaning we have measured; **stand-in** =
the answer keeps the title running but substitutes a host meaning (offline user, dropped audio command, linear texture
layout); **refused by name** = the shim returns a named failure (`E_NOTIMPL`, `E_NOINTERFACE`, `E_FAIL`) with a log
line; **missing** = no binding or no slot; **host** = bound to the desktop DLL unmodified; **module** = bound to a
bundled package image.

### 4.1 `xmem.dll` — 12 imports, 57 sites

Shapes are our measured declarations in `port/shims/xmem.h`/`xmem.cpp` and the constants read at the title's sites
([`docs/xmem_exe_calls.md`](xmem_exe_calls.md), [`docs/runtime_imports.md`](runtime_imports.md)):

| import | shape |
|---|---|
| `XMemAlloc`, `XMemAllocDefault` | `void* XMemAlloc(size_t size, uint64_t attributes)` |
| `XMemFree`, `XMemFreeDefault` | `void XMemFree(void* p, uint64_t attributes)` |
| `XMemVirtualAlloc` | `void* XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocationType, ULONGLONG attributes, DWORD protect)` |
| `XMemAllocatePhysicalPages` | `BOOL XMemAllocatePhysicalPages(uint64_t attributes, uint64_t* pageCount, uint64_t* pageArray)` |
| `XMemFreePhysicalPages` | `BOOL XMemFreePhysicalPages(uint64_t pageCount, const uint64_t* pageArray)` |
| `XMemMapPhysicalPages` | `void* XMemMapPhysicalPages(void* address, uint64_t pageCount, const uint64_t* pageArray)` |
| `XMemMapPhysicalPagesScatter` | `BOOL XMemMapPhysicalPagesScatter(void* const* addresses, uint32_t pageCount, const uint64_t* pageArray)` |
| `XMemGetWorkingSetStatistics` | `BOOL XMemGetWorkingSetStatistics(uint32_t kind, void* stats)` |
| `XMemIsToolsMemAvailable` | `BOOL XMemIsToolsMemAvailable(void)` |
| `XMemTransferMemory` | `BOOL XMemTransferMemory(direction, size_t size)` |

Shim: `port/shims/xmem.cpp`, `SHIM_TABLE(xmem)`. Status: **answered** for ten (allocation, free, virtual alloc, the
physical-page family, working-set statistics, the two default aliases); **stand-in** for `XMemTransferMemory`
(returns TRUE, records direction/size; a PC has no title/system memory partition) and `XMemIsToolsMemAvailable` (FALSE;
the title asks only on an out-of-memory path). A refused `XMemVirtualAlloc` is an `int3` assert in the title's XMem
initialiser (`0x1E1D3A7`, `0x1E1CFE9`, `0x1E1D574`), so every attribute word the title passes has to be accepted; the
shim serves them all ([`docs/decomp_memory.md`](decomp_memory.md) sections 6-7). The table also serves
`XMemVirtualQuery` and `XMemGetWriteWatch`/`XMemResetWriteWatch`, which the executable does not import but
`XboxBvhBuilder.dll` does.

### 4.2 `d3d12_xs.dll` — 3 imports, 4 sites

Shapes from [`docs/d3d12_xs_surface.md`](d3d12_xs_surface.md) and `port/shims/d3d12_xs.cpp`:

| import | shape | status |
|---|---|---|
| `D3D12XboxCreateDevice` | `HRESULT (IUnknown* adapter, const void* params24, const IID* riid, void** out)`, the console device factory | answered: wraps the PC `ID3D12Device` in the Xbox-ABI proxy ([`docs/d3d12_vulkan.md`](d3d12_vulkan.md)) |
| `D3D12SerializeVersionedRootSignature` | forwarded to the host, flags cleared on a copy | answered |
| `D3D12ConfigureVirtualMemoryX` | `HRESULT (void* block32)` with the count at `+4`; the title tests only the result | answered (`S_OK`; PC/Vulkan owns VM policy) |

Shim: `port/shims/d3d12_xs.cpp` registers the three names beside the console device table; the device class itself is
in `port/shims/d3d12_xs_device.cpp` and `port/shims/d3d12_vulkan_device.cpp`. The call sites on named paths are
`0x2478440` (device setup: `D3D12XboxCreateDevice`), `0x247FC40` (a serialized root signature) and `0x1E1CF60`
(virtual memory). No console import in this row is stand-in or missing.

### 4.3 `xg_xs.dll` and `XCRDAPI.dll` — texture layout and DirectStorage

| import | shape | shim and status |
|---|---|---|
| `XGSuggestSwizzleMode` | `HRESULT (const void* desc, uint32_t flags, uint32_t* mode)` | `port/shims/xg_xs.cpp`; answered with the explicit linear mode for a committed row-major image |
| `XGCreateTextureComputer` | `HRESULT (const void* desc, void** out)` | answered (a computer for a description whose layout can be filled honestly; slot 2 fills the 0x1B40-byte record, slot 8 copies linear data, slot 10 `GetResourceTiling` is **refused by name** because the runtime creates no 64 KiB-tiled resource; slot 10 has 0 measured calls) |
| `DStorageGetFactory` | `HRESULT DStorageGetFactory(REFIID, void**)` | `port/shims/dstorage.cpp`; answered (the factory is registered for `xcrdapi.dll`), zlib request shapes are served, BCPack/swizzle/page-list shapes are **refused by name** behind the opt-in switches of [`docs/directstorage_advanced.md`](directstorage_advanced.md) |

The measured content path ([`docs/texture_layout.md`](texture_layout.md)): the call sites are the description helper
`0x4ECC940` (suggest) and the placed-resource fill `0x16F7100` with the layout cache `0x979C40`/`0x5121980`
(create); two more create sites (`0x3441F0` and the `0xC4B030` suggest caller) are not on a named path. All six
`xg_xs` sites are loader-phase and answered.

### 4.4 Audio: `AcpHal.dll`, `xapu.dll`, `chad.dll` — 19 imports, 29 sites

Shapes are the measured declarations in `port/shims/acphal.cpp` and `port/shims/xapu_chad.cpp`; the argument/constant
reads per site are in [`docs/audio_hal_exe_calls.md`](audio_hal_exe_calls.md).

| group | shapes | shim | status |
|---|---|---|---|
| `AcpHalCreate`, `AcpHalAllocateShapeContexts`, `AcpHalReleaseShapeContexts`, `ApuCreateHeap`, `ApuAlloc`, `ApuFree`, `ApuIsVirtualAddressValid`, `ApuMapVirtualAddress`, `ApuMapApuAddress` | measured out-parameter and handle shapes; `(void*, uint32_t alignment)` for the validate/map pair, `(uint32_t apu)` for the reverse map | `port/shims/acphal.cpp` | answered (nine) |
| `XApuConnect`, `XApuDisconnect`, `XApuEnqueueCommand`, `XApuGetResultCount`, `XApuDequeueResult` | measured queue handle + 0x50-byte command/result blocks | `port/shims/xapu_chad.cpp` | stand-in (connect returns a real handle, commands accepted and dropped, nothing advances) |
| `XDspConnectWithMaximumStreamLimit`, `XDspActivate`, `XDspSubmitCommand`, `XDspDeactivate`, `XDspDisconnect` | measured handle + stream-count (`0x80` streams) + swapped-output wrappers | `port/shims/xapu_chad.cpp` | stand-in (same rule) |

The 29 sites are attributed to audio except the four unattributed ones (`ApuFree`, `XApuDisconnect`, `XDspDeactivate`
and `XDspDisconnect`, all tail jumps from leaf wrappers). The measured boot reaches the two connects and the APU result
poll at about +25.42 s and stops there; no command is enqueued ([`docs/audio_shims.md`](audio_shims.md) section 5).

### 4.5 GDK platform classes

| class group | status | notes |
|---|---|---|
| `XThreadingImpl` | answered | the runtime is Microsoft's MIT `vendor/xtaskqueue` behind the shim; parity measured in [`docs/task_queue_parity.md`](task_queue_parity.md) |
| `XUserImpl`, `XUserGamertagImpl` | stand-in | one offline signed-in user; the change event is delivered only under the persistent-store arm, not by default ([`docs/user_events.md`](user_events.md), [`docs/persistent_store.md`](persistent_store.md)) |
| `XAccessibilityImpl` | refused by name (default), stand-in (opt-in) | `XSpeechSynthesizerCreate` returns `E_FAIL` and a null handle by default; `PALMETTO_SILENT_SPEECH=1` serves one silent 44-byte WAVE stream ([`docs/first_run_path.md`](first_run_path.md) 3.2) |
| `XDisplayImpl` | answered | HDR probe returns `S_OK` with HDR disabled |
| `XNetworkingImpl` | stand-in | offline hint is the default; an internet arm is opt-in ([`docs/networking_gate.md`](networking_gate.md)) |
| `XPackageImpl` | stand-in + refused by name | `IXPackageImpl4` is `E_NOINTERFACE` by default; slots 5/6/9/39 are `E_NOTIMPL` by default; both are opt-in switches ([`docs/loading_gate.md`](loading_gate.md) sections 4-5) |
| `XGameSaveImpl`, `XStoreImpl`, `XPersistentLocalStorageImpl` | stand-in | offline provider and store, overlay-backed persistent path ([`docs/persistent_store.md`](persistent_store.md)) |
| `XGameUiImpl`, `XLauncherImpl` | stand-in | answered `S_OK` when asked, but every call site has 0 measured calls |
| `XGameImpl`, `XSystemImpl`, `XSystemAnalyticsImpl`, `XGameRuntimeFeatureImpl`, `XGameEventImpl` | stand-in | fixed console identity values |
| `XAppCaptureImpl` | missing | the GUID is in `xgameruntime.cpp`'s known-but-not-served registry; the class is never requested ([`docs/loading_gate.md`](loading_gate.md) section 2) |

### 4.6 Bundled package modules

| module | imports/sites | status | what the call sites are |
|---|---:|---|---|
| `Bink2Scarlett.dll` | 23 / 33 | module | 18 imports are referenced from the movie wrapper cluster (`0x442EA70` metadata open, `0x442EFE0` playback open, `0x442F760` frame-buffer registration, `0x2EF0E0` per-frame decode, `0x442FD10` close, `0x15E3C10` memory/OS-file callbacks) and the 2D draw (`0x2349D80`); five imports (`BinkGetPlatformInfo`, `BinkGoto`, `BinkSetSoundSystem`, `BinkSetVolume`, `BinkStartAsyncThread`) have no static call site. The module's own imports bind through `xmem.cpp`, `file_system.cpp` (`CreateFile2`) and `com_shim.cpp` ([`docs/decomp_first_ui.md`](decomp_first_ui.md) section 5; [`docs/movie_path.md`](movie_path.md)) |
| `libHttpClient.GDK.dll` | 38 / 72 | module | the HTTP call surface (`HCHttpCall*`), called from the title's HTTP layer around `0x562F..`-`0x563..`; the module's own network goes through `WinHttp*`, which our offline shim owns. No HTTP call in the measured boot ([`docs/networking_gate.md`](networking_gate.md)) |
| `XboxBvhBuilder.dll` | 7 / 25 | module | BVH build/rehydrate/prebuild-info/copy entry points; one site is the renderer device setup `0x2478440` constructing the builder, the rest are ray-tracing callers. Not reached in the measured boots |
| `amd_fidelityfx_loader_xbox.dll` | 4 / 6 | module | `ffxCreateContext`, `ffxQuery`, `ffxDestroyContext`, `ffxDispatch` around `0x28EF..`; the console upscaler programs are not requested in the measured window ([`docs/upscale_standin.md`](upscale_standin.md)) |
| `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` | 137 / 27,665 | module | the bundled VC runtime images; `VCRUNTIME140.dll` alone carries 26,697 calls (overload thunks and EH). Phase attribution is meaningless at this granularity and is reported only as a count |

### 4.7 Host services our shims intercept

| DLL and names | shim and status |
|---|---|
| `WINHTTP.dll`, all 15 imports | `port/shims/winhttp_offline.cpp`; stand-in: every session/request returns an offline answer and opens no network. `WinHttpSetStatusCallback` and `WinHttpCrackUrl` are included |
| file API set, 13 of 23 imports (`CreateFileW`, `ReadFile`, `FindFirstFile*`, `GetFileAttributes*`, `DeleteFileW`, `CreateDirectoryW`, `MoveFileExW`, and the rest of the `FILE_API` list) | `port/shims/file_system.cpp`; answered: `GAME:`/`G:\` map under `--game-dir`, `T:\` is scratch, writes go to the overlay. The other ten file imports stay host ([`docs/runtime_imports.md`](runtime_imports.md), "Console paths and the write overlay") |
| `CreateFile2` | `port/shims/file_system.cpp`; answered, and the movie library imports it |
| library loader, 7 of 9 imports (`LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress`, `FreeLibrary`) | `port/shims/library_loader.cpp`; answered for bundled modules (a `LoadLibrary` of a package DLL maps it through the loader) |
| process/argv; `VirtualAlloc` under the memory set; `GetCommandLineA/W`, `ExitProcess`, `abort`, `exit` and the UCRT argv family | `port/shims/process.cpp`; `VirtualAlloc` also `port/shims/xmem.cpp`; answered (the guest-visible argv/exit semantics are measured in `port/test/` and [`docs/runtime_imports.md`](runtime_imports.md)) |
| COM, all 7 imports (`CoInitializeEx`, `CoUninitialize`, `CoCreateInstance`, `CoCreateGuid`, `CoTaskMemFree`, `PropVariantClear`, `StringFromGUID2`) | `port/shims/com_shim.cpp`; answered (per-thread apartment depth and host-init ownership tracked) |
| `GameInput.dll`, 1 import (ordinal `#1` = `GameInputCreate`) | `port/shims/gameinput.cpp`; stand-in: own XInput/keyboard backend, a scripted pad behind a switch, `GetCurrentReading` is `GAMEINPUT_E_READING_NOT_FOUND` while no device is paired ([`docs/input_backend.md`](input_backend.md)) |
| `bcrypt.dll`, 3 imports (`BCryptOpenAlgorithmProvider`, `BCryptGenRandom`, `BCryptCloseAlgorithmProvider`) | `port/shims/bcrypt_rng.cpp`; answered with the host RNG behind the measured call shape |

### 4.8 Host-bound services and unreferenced imports

| group | why it matters | status |
|---|---|---|
| `WS2_32.dll` (25 imports, 87 sites) | a `--run` can open real sockets; [`docs/runtime_imports.md`](runtime_imports.md) says run the title with the network off | host, no shim |
| `MFPlat.DLL` / `MFReadWrite.dll` (7 imports, 8 sites, callers around `0x33A4xxx`) | Media Foundation media-type/source-reader setup; the boot measured the movie module never enters its decode path, so this path is not measured either | host, no shim |
| `ADVAPI32.dll` (3 imports: `RegOpenKeyW`, `RegQueryValueExW`, `RegCloseKey`, one caller in the static runtime block) | reads the host registry; the console would read its own settings service | host, no shim |
| CRT API sets (162 imports, 10,373 sites) and the kernel core sets (77 imports, 258 sites) | ordinary runtime/platform calls; no console-only meaning | host, with the process/argv/COM/memory exceptions above |
| window messages (ext-ms ntuser: `PeekMessageA`, `DispatchMessageA`, `TranslateMessage`, `DefWindowProcA` and the other three names) | three sites, in frame-path functions; the message pump | host, no shim |

**Unreferenced imports (60 of 587).** 29 are C++ library names (26 `MSVCP140.dll`, 3 `VCRUNTIME140.dll`), 5 are Bink
imports (`BinkGetPlatformInfo`, `BinkGoto`, `BinkSetSoundSystem`, `BinkSetVolume`, `BinkStartAsyncThread`), 9 are
process/thread/synch/sysinfo names (`SuspendThread`, `OpenThread`, `TerminateProcess`, `GetThreadPriority`,
`GetCurrentThreadStackLimits`, `CreateMutexA`, `ReleaseMutex`, `Sleep`, `GetTickCount64`), 4 are window messages
(`DispatchMessageA`, `PeekMessageA`, `TranslateMessage`, `DefWindowProcA`), 10 are CRT names (string, math, filesystem,
heap and time), 2 are core-set names (`LocalFree`, `InitializeSListHead`) and 1 is a `ws2_32` ordinal (`#18`). A dead
import is not a missing answer: the linker kept it, nothing calls it.

## 5. Cross-reference: the stage-3, first-run, UI and movie calls that are still stand-in or refused

The four decomp slices that landed give the RVAs below; the phase attribution of section 3 comes from them and from
the earlier pages. The rule for this list: a call is a **candidate gate for first visible content** only if it is
(a) on one of the named paths and (b) answered by a stand-in, a named refusal or a missing binding. "Static" means the
call site exists; "measured" means an existing boot page recorded it.

| path | call (RVA) | our answer | flag | source |
|---|---|---|---|---|
| stage 3 / renderer init | `d3d12_xs.dll!D3D12XboxCreateDevice` at `0x2478440` | proxy device | answered | [`docs/decomp_renderer.md`](decomp_renderer.md) 1, [`docs/render_loop.md`](render_loop.md) |
| stage 3 / renderer init | `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` at `0x1E1CF60`, `D3D12SerializeVersionedRootSignature` at `0x247FC40` | `S_OK`, forwarded | answered | [`docs/d3d12_xs_surface.md`](d3d12_xs_surface.md) |
| stage 3 / renderer memory | `xmem.dll!XMemVirtualAlloc` at `0x2478440`, `XMemMapPhysicalPages` at `0x2473090`/`0x2474660` | host pages/windows | answered | [`docs/xmem_exe_calls.md`](xmem_exe_calls.md) |
| stage 3 / XMem arena | `XMemVirtualAlloc` at `0x1E1D181`/`0x1E1D288`/`0x1E1D3A7` (arena and pool placeholders) and `D3D12ConfigureVirtualMemoryX` at `0x1E1CF60` | placeholders/large pages accepted; a refusal is an `int3` in the title | answered | [`docs/decomp_memory.md`](decomp_memory.md) 6-7 |
| stage 3 / content fill | `xg_xs.dll!XGCreateTextureComputer` at `0x16F7100`/`0x979C40`/`0x5121980`/`0x3441F0`, `XGSuggestSwizzleMode` at `0x4ECC940`/`0xC4B030` | linear layout, record fill, copy | answered; computer slot 10 `GetResourceTiling` refused by name, 0 measured calls | [`docs/texture_layout.md`](texture_layout.md) |
| first-run / user | `XUserImpl` `XUserAddAsync` at `0x5BB5390`, `XUserGetState` poll wrapper at `0x2211400` | offline user | stand-in | [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4, [`docs/persistent_store.md`](persistent_store.md) |
| first-run / user event | `XUserRegisterForChangeEvent` at `0x59257E0` (measured +1.98 s) | `S_OK`; callback never fires by default | stand-in | [`docs/user_events.md`](user_events.md) |
| first-run / accessibility | `XAccessibilityImpl` `XSpeechSynthesizerCreate` at `0x5A75F80` (measured +25.7..29.4 s) | `E_FAIL` + null handle by default; one silent voice under `PALMETTO_SILENT_SPEECH` | **refused by name** (default), stand-in (opt-in) | [`docs/first_run_path.md`](first_run_path.md) 3.2, [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4 |
| first-run / speech narration | `XSpeechSynthesizerEnumerateInstalledVoices` at `0x5A76030`, narration wrappers `0x3E40E80`/`0x3E41A30`/`0x3E41A50`/`0x3E41A70`/`0x3E41AB0`/`0x3E40C60` | as above | refused by name (default) | [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4 |
| first-run / input | `GameInput.dll!#1` (`GameInputCreate`) at `0x59258A0`; `IGameInput::GetCurrentReading` at `0x2211B40`, `GetGamepadState` at `0x6AECE0` (measured +25.44 s with a pad paired) | own backend; no reading without a device | stand-in | [`docs/input_backend.md`](input_backend.md) 5, [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4 |
| first-run / dialogs | `XGameUiImpl` text entry `0x43A2800`/`0x3DAEAA0`, result `0x3DAE860`/`0x43A2A80`; profile card `0x3DAF310`/`0x3DAF210`; launcher `0x3DAED40` | answered `S_OK` if asked | stand-in; **0 measured calls** | [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4, 6 |
| first-run / settings | `XGameSaveEnumerateContainerInfo` at `0x28D12B0`/`0x28D12E0`, `XGameSaveReadBlobData` at `0x28D1480`, `XPersistentLocalStorageGetPath` at `0x28D0E00` (measured +2.0..3.9 s) | offline store, overlay path | stand-in | [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4, [`docs/persistent_store.md`](persistent_store.md) |
| first-run / package | `XPackageImpl` wrapper `0x1CE6E20` (measured +2.93 s) | `E_NOINTERFACE`/`E_NOTIMPL` by default | refused by name (default); **measured refuted as the gate** | [`docs/loading_gate.md`](loading_gate.md) 5-7 |
| first-run / display | `XDisplayTryEnableHdrMode` at `0x2481CA0` (measured +3.0..21.6 s) | `S_OK`, HDR disabled | answered | [`docs/loading_gate.md`](loading_gate.md) 2 |
| first-run / capture | `XAppCaptureImpl` `XAppCaptureTakeScreenshot` at `0x223C9B0` | class not served | **missing**; **0 measured calls** | [`docs/loading_gate.md`](loading_gate.md) 2, [`docs/decomp_first_ui.md`](decomp_first_ui.md) 4 |
| UI / movie draw | `Bink2Scarlett.dll!BinkOpenWithOptions` at `0x442EBE6` (probe) and `0x442F056` (playback), `BinkRegisterFrameBuffers` at `0x442F9B8`, `BinkDoFrameAsync*`/`BinkNextFrame` in `0x2EF0E0`, draw at `0x2349D80` | the bundled decoder is mapped and bound; its 23 imports answer through xmem/file/com shims | module, **not entered** (state gate) | [`docs/movie_path.md`](movie_path.md) 2.3-2.5, [`docs/decomp_first_ui.md`](decomp_first_ui.md) 5 |
| UI / movie audio | `chad.dll!XDspConnectWithMaximumStreamLimit` at `0x5B52600`, `xapu.dll!XApuConnect` at `0x43AEE50` | handles, commands dropped | stand-in | [`docs/audio_shims.md`](audio_shims.md) 5 |
| audio | `XApuEnqueueCommand` at `0x48390`/`0x4EE9920`/`0x4F20900`/`0x4F20AD0`; `XApuDequeueResult` at `0x4EAA840`; `XDspActivate` at `0x11D4750`; `XDspSubmitCommand` at `0x4E5B200` | accepted and dropped | stand-in | [`docs/audio_hal_exe_calls.md`](audio_hal_exe_calls.md), [`docs/audio_shims.md`](audio_shims.md) 5 |
| loader | `ReadFile` retry at `0x314275`, `CreateFileW` archive open at `0xF43645`, `DStorageGetFactory` at `0x1D8E4D0`, DirectStorage enqueue callers `0x28C9xxx` | path mapping + overlay, zlib inflate, zlib requests only | answered; BCPack/swizzle request shapes refused by name (0 measured) | [`docs/readfile_first_touch.md`](readfile_first_touch.md), [`docs/directstorage_advanced.md`](directstorage_advanced.md) |

**What still stands in or is refused on these paths.** In order of the measured reach: the speech synthesizer
(`XAccessibilityImpl`, refused by name by default, reached at +25.7..29.4 s), the offline user event and the offline
user/save store (stand-ins, the event reached only in the persistent-store arms), the input reading (stand-in, reached
at +25.44 s with a paired pad), the audio connects and command queues (stand-ins, reached at +25.42 s), the Bink movie
path (module bound, not entered), and the package readiness surface (refused by name by default, measured refuted as a
gate). Everything on the stage-3 renderer and loader paths is answered; the stage-3 walls that remain are layer
vtable/state issues named by [`docs/decomp_renderer.md`](decomp_renderer.md) and [`docs/decomp_jobs.md`](decomp_jobs.md),
and the memory-model wall (the class-0xa zero-size block and its 0x1000-byte copy) is named by
[`docs/decomp_memory.md`](decomp_memory.md) sections 9-10. None of those is an import answer.

The cross-reference for the stage machine itself is **pending** `docs/decomp_stage.md`; the function RVAs this page
already attributes to stage 3 (`0x81B200`, `0x81B270`, `0x812620`, `0x81AFB0`, `0x21B8960`, `0x21B7070`, `0x53EBE50`/
`0x53EBEA0`/`0x53EBFE0`/`0x53EC0C0`, `0x21B68D0`, `0x21B6910`) come from [`docs/state_machine.md`](state_machine.md)
and [`docs/decomp_first_ui.md`](decomp_first_ui.md) section 2. No console import has a call site inside those handlers,
so a stage-machine cross-reference is unlikely to change the gate list.

## 6. Ranked implementation list

Ranked by whether the answer stands between the current tree and the first visible content frame. Effort is S (< half
a day), M (a slice), L (multi-slice). Every row names the call sites that justify it and a falsifier that retires it.
Items already measured as not on the path are excluded in the table after this one.

| rank | item | why it is on this list | effort | falsifier |
|---:|---|---|---|---|
| 1 | **Finish the speech surface and decide its default** (`PALMETTO_SILENT_SPEECH`): make `XSpeechSynthesizerCreate` succeed by default, or keep the refusal only if the first-run flow can never reach narration | the only **refused-by-name** answer on a **measured** first-run call site (`0x5A75F80`, +25.7..29.4 s) | S | a boot in which create succeeds and the next narration slots (`0x3E41A30`/`0x3E41A70`/`0x3E41AB0`) run; if the screen owner then advances (a new owner executes, a present changes) the item was a gate, if nothing follows it was not (the existing A/B says exactly this, so the open half is the default, not the code) |
| 2 | **Deliver the offline sign-in event and the settings/save read by default** (`port/shims/xgameruntime_user.cpp`, `xgameruntime_gamesave.cpp`, `persistent_store.h`) | the first-run user step (`0x5BB5390` → `XUserAddAsync`), the state poll (`0x539470` → `0x2211400`) and the settings read (`0x28D12B0`/`0x28D1480`/`0x28D1A30`) are on the named first-run path; the event is currently opt-in | S/M | a boot with the event and the store on by default: if the save subsystem initialises at ~23-24 s but no first-run owner (`0x2234A40`, `0x3DAF310`, `0x442EA70`) executes or no present changes, the item is not the visible-frame gate (the persistent-store arms already measured the provider init and no stage move) |
| 3 | **Make the input backend always-built and decide the scripted-pad default** (`port/shims/gameinput.cpp`) | the first-run/UI phase reads a paired pad at +25.44 s (`0x2211B40`, `0x6AECE0`); without a device it reads nothing, and a first-run screen conventionally waits on a press | M | a boot with the scripted pad on where more than the current two reading calls happen and a first-run screen owner or a content present changes; the measured arms have reads with no following save/stage move, so the falsifier is "more reads, still nothing follows" |
| 4 | **Prove the movie trigger and its frame-buffer mapping** (not an import change: `Bink2Scarlett.dll` and its imports are already bound) | the whole Bink path has named call sites (`0x442EA70`, `0x442EFE0`, `0x442F760`, `0x2EF0E0`, `0x2349D80`) and no measured execution; the mapping goes through title code into our D3D12 layer | M | one boot in which `0x442EA70` executes or `player+0x1C4` reaches 9; if the module runs and still no frame is drawn, the gap is the mapping/draw (the movie page's static negative says the producer is above the frame loop) |
| 5 | **Give the audio stand-in a real data path** (`port/shims/xapu_chad.cpp`, `port/xma_decoder/`) | the audio bring-up and the APU poll are reached at +25.42 s but every command is dropped; audio does not gate the visible frame, so it is ranked by value not by the gate | L | a boot where one of the four `XApuEnqueueCommand` sites executes and audible output is produced; the visual state and the stage words stay unchanged by construction |
| 6 | **Check the task-queue dispatch path that drives the boot-media tick** (`XTaskQueueDispatch` sites at `0x63AD90`) | `decomp_first_ui.md` hypothesis H1: the frame tick is driven by GDK dispatch, so our dispatch cadence could decide whether the boot-media frame body runs; the answer itself is already "answered" (vendor task queue) | S | a boot where `XTaskQueueDispatch` returns success but the tick entry `0x313560` or its call site `0x63AD78` never executes; the dispatch is then not the driver and H1 is retired |

### Excluded: already measured as not this gate

| item | the measurement that retires it |
|---|---|
| `XPackageImpl` readiness (`PALMETTO_PACKAGE_V4`/`PALMETTO_PACKAGE_READY`) | the 300 s acceptance boot: the chain completes, the callback runs, and the pipeline population, draw loop and black readbacks are unchanged ([`docs/loading_gate.md`](loading_gate.md) 5-7) |
| `XNetworkingImpl` connectivity hint | the internet/offline A/B: the title branches (the preferred UDP port query runs) and the stage words do not move ([`docs/networking_gate.md`](networking_gate.md) 5) |
| silent speech as the sole gate | the silent-speech A/B: create and enumerate advance one call and the stage does not move ([`docs/first_run_path.md`](first_run_path.md) 5); it stays rank 1 as a completion item |
| `XGameUiImpl` / `XLauncherImpl` real dialogs | 0 measured calls in 300 s; served stand-ins if the page flow ever asks ([`docs/loading_gate.md`](loading_gate.md) 2) |
| `XAppCaptureImpl` (missing class) | 0 measured calls; the class is never requested ([`docs/loading_gate.md`](loading_gate.md) 2) |
| `xg_xs` computer slot 10 `GetResourceTiling` (refused by name) | 0 measured calls; the runtime creates no tiled resource ([`docs/texture_layout.md`](texture_layout.md)) |
| DirectStorage BCPack/swizzle/page-list request shapes | 0 measured requests; the 130 s boot streams zlib file-to-memory only ([`docs/directstorage_advanced.md`](directstorage_advanced.md) 1) |
| `XStoreImpl` / `XGameSaveImpl` container and licence calls | 0 measured `XGameSave`/`XStore` calls in the 130 s and 300 s windows ([`docs/persistent_store.md`](persistent_store.md) 1); the static sites exist for the settings path, which the measured boot does not reach |
| `XMemIsToolsMemAvailable` (FALSE stand-in) | the one site is on an out-of-memory path ([`docs/runtime_imports.md`](runtime_imports.md)) |
| stage-3 renderer overlays/overruns (type-5 zero-byte block, root arguments, vertex feed, colour path, upscaler kind) | not import answers; the ranked conversion hypotheses are in [`docs/decomp_renderer.md`](decomp_renderer.md) 8 and the timing ones in [`docs/decomp_jobs.md`](decomp_jobs.md) 8 |
| memory-model hypotheses (heap bins/growth, the XMem arena placement, the class-0xa zero-size path) | not import answers: the XMem answers are all served and the hypotheses are about placement and the copy that follows, ranked in [`docs/decomp_memory.md`](decomp_memory.md) 10 |

## 7. Limits

- **Static only, and not a trace.** Nothing here was run; the call-site counts are a byte-pattern census, the phase
  attribution is by containing function, and section 5's "reached" flags quote other pages' boots.
- **The census is a lower bound for indirect calls.** A call through a register after an IAT load is not counted; a
  stray `E8`/`E9` byte could add one site. The 60 unreferenced imports are "no static site", not "never called".
- **Phase attribution is incomplete by design.** 85 of 97 console-DLL sites and 200 of 358 GDK sites are attributed;
  the rest are reported as unattributed rather than guessed. The 114 unattributed `XThreadingImpl` sites are mostly the
  statically linked runtime library calling itself.
- **The GDK class table is class-level.** A class reads "served" when its GUID is in a shim; an individual slot can
  still be a named refusal (the XPackage slots, the speech create). The per-slot answers are in the shim sources and the
  pages cited in 4.5.
- **Cross-reference pending** for `docs/decomp_stage.md`, which was not present when this page was written. Sections 3
  and 5 use the four slices that landed plus the earlier boot/decomp pages.
- **No game data.** Import names, RVAs, counts, slot numbers, argument counts and HRESULTs only.

## 8. Files, evidence and uncommitted changes

- **This page** is the only repository file this slice writes: `docs/decomp_imports.md`, new and untracked. No other
  page, `port/` file, test or build file was edited; no git command that writes was run.
- **Work notes and scratch evidence** are outside the repository, under `D:\leonida-work\decomp\`: `imports.md` (the
  notes), `imports/import_call_sites.json` (the 587-entry census), `imports/import_sites.csv` (every call site with its
  containing function), `imports/gdk_thunk_sites.csv` (the 161 thunks with their call sites), and the scripts
  `census.py`, `classify.py`, `summary.py`, `final_tables.py`, `page_tables.py` that derive the tables above.
- **Commands**: the two in section 1 reproduce the census; the phase labels are the RVA lists in `classify.py`, each
  entry tagged with the page it came from.
- **Uncommitted tree at the time of writing** (not this slice's): `port/src/vulkan_replay.h`,
  `port/test/frame_dump_test.cpp`, and the parallel slices' `docs/decomp_first_ui.md`, `docs/decomp_jobs.md`,
  `docs/decomp_memory.md`, `docs/decomp_renderer.md`. This page does not touch them.
