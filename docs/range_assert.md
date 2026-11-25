# The display-surface window: the title's last range assert (`game+0x247af2d`)

**Status: measured, decompiled, implemented, tested, measured again.** The `<= 2 MiB - 1` range check inside
`game+0x247ad10` was the last title `int3` fired at boot: [issue1_baseline.md](issue1_baseline.md) §6 and
[command_signature.md](command_signature.md) §5 both report it as the one assert left after the earlier slices, fired at
about +4.5 s every boot and stepped over by `PALMETTO_SKIP_INT3=1`. This page decompiles the producer from the image,
measures it at runtime, fixes the producer in the device proxy, and measures the assert set empty: the 130 s boot now
runs with **no title `int3`, no exception, the loader/streaming activity the last slice measured, and the frame loop
still black**. The next frontier is named in section 6.

This slice follows [upstream_port.md](upstream_port.md): the public GPL-3.0 fork is an evidence source, not a code
source. Everything here comes from our own decompilation, our own boot and our own tests.

## 1. Why this page exists

`game+0x247ad10` is the renderer function that creates the title's two display surfaces; it runs once from the renderer
device setup `game+0x2478440` at `game+0x247942b` (and from the teardown/rebuild path `game+0x248d810`). It registers a
memory window, creates the surfaces through console device slot 127, and then asserts that the window is nearly used up.
The assert was the only title stop left, so it is the last slice between the port and a run with no title assert at all.
This page answers three questions the earlier pages left open: what the window is, why the two stack fields stay at
`{0, 64 MiB}`, and what makes the value exceed 2 MiB in our runtime.

## 2. Measured shapes

All RVAs are relative to the fixed image base `0x140000000`; the decompile is Ghidra on the retail 1.0.2.1 image, no
analysis pass, the six functions below only.

### 2.1 The check and its window (`game+0x247ad10`, `.pdata` `0x247ad10..0x247af58`)

| site | instruction / call | meaning |
|---|---|---|
| `game+0x247ad2a` | `call game+0x144de8620` | the thread's graphics context (TLS getter; `GS:[0x58]` object `+0x410`) |
| `game+0x247ad2f` | `lea rdi,[rsp+0x70]` | the window pair, `{base/cursor, size}` |
| `game+0x247ad37` | `mov qword [rdi],0` | cursor 0 |
| `game+0x247ad3e` | `call game+0x140075140` | a cached boolean: `game+0x140075140` maps a registry value to a device class and stores `class == 6`; this host reads it **false** |
| `game+0x247ad45..5d` | `cmovne rcx,rax` with `0x4000000` / `0x5200000`; `mov [rdi+8],rcx` | window size 64 MiB (false) or 82 MiB (true) |
| `game+0x247ad64` | `call game+0x145c8f990` | pushes `{handler = game+0x56a2a60, pair = &[rsp+0x70], ctx}` onto the allocation-hook chain (see 2.3) |
| `game+0x247ad69` | `call game+0x145eb2590` | the second pass of the same surface-shaped work (count `image+0xa94390c`) |
| `game+0x247ad6e..af10` | `FUN_14501f4a0` twice + the loop over `image+0xa94390c` | builds the 16-byte surface description from `image+0x75e6fe0/fe4/fe8` and creates that many surfaces through `FUN_140cc2780` -> `game+0x14034beb0` -> `game+0x14193de10` -> device slot 127 |
| `game+0x247af14` | `mov rax,[rsp+0x78]`, `sub rax,[rsp+0x70]` | `size - cursor` |
| `game+0x247af1e` | `add rax,[rip+0x516c0cb]` = `qword[image+0x75e6ff0]` | `+ window base` (the global is set by the handler, 0 before) |
| `game+0x247af25` | `cmp rax,0x1fffff; jbe over` | the invariant: the window's **remaining** bytes must be `< 2 MiB` |
| `game+0x247af2d` | `int3` | the assert this page removes |
| `game+0x247af38` | `call game+0x145c8fa70` | pops the chain node (the scope ends here) |

The count `image+0xa94390c` is `(byte [display settings + 0x20c]) | 2` from the display-mode apply function
`game+0x22434a0`, i.e. 2 or 3 on this build; the measured host creates exactly **two** surfaces. The description's
dimensions come from `game+0x5eb24f0`, a small copy of the current width/height and a byte into
`image+0x75e6fe0/fe4/fe8`; the runtime probe logged `0xF00 x 0x870` (3840 x 2160), format `0x1c` (R8G8B8A8_UNORM),
mips 1, array 1 for both surfaces (creation registry, slot 127).

### 2.2 What the scope handler does (`game+0x56a2a60`, registered by `game+0x145c8f990`)

The handler receives `(size, flags, &pair, next)`, where `next` is the rest of the hook chain:

- `if ((flags & 0xc000) != 0x8000)`: tail-call the next engine's handler with the same size and flags; the chain's bottom
  is the imported default allocator (`XMemAllocDefault`, IAT slot `image+0x6793a08`), so an ordinary allocation is
  passed through untouched.
- otherwise: `image+0x75e6ff0` is the window's base. The first such call reserves `pair[1]` bytes (64/82 MiB) through the
  chain bottom, then sets `pair[0] = image+0x75e6ff0 = base`. Every later call carves: `pair[0] += round_up_64KiB(size)`
  and returns the carved address, `int3`-ing if it would leave the window. So `pair` is `{cursor, window}` and the check
  in 2.1 is `window - carved + base = window - carved`.

Two scope chains exist: `game+0x145c8f990` pushes the node into bucket `0x80` of the XMemAlloc hook table
(`image+0x6843370 + 0x80*8 = image+0x6843770`), and `game+0x145c8fa00` into bucket `0x80` of the XMemFree hook table
(`image+0x6843b80 + 0x400 = image+0x6843f80`); both share the manager/lock at `image+0x71bf988`.

### 2.3 The exported allocation hooks (`image+0x6844380`)

The image's export directory has exactly one export: `__XMemAllocationHooks__`, RVA `0x684380`, a table of function
pointers (`0x144deb420`, `0x144e38ec0`, ...). The two hooks are the console XMem runtime's allocation and free
dispatchers:

- `game+0x4deb430` (alloc): locks `image+0x71bf988`, selects bucket `(attributes >> 16) & 0xFF` from the table at
  `image+0x6843370`, copies every chain node whose context is the thread's or `-1`, then calls the first node's handler
  with `(size, attributes, node.pair, node.next)`.
- `game+0x4e38ed0` (free): the same over the table at `image+0x6843b80` and the bottom `XMemFreeDefault`
  (`IAT image+0x6793a20`).

Each table's sentinel node is `{handler = the IAT thunk to the default allocator, arg = 0, ctx = -1, next = 0}`
(`image+0x61889b0` / `0x61889d0`), so a chain that no title scope matches calls the engine allocator directly.

On the console the **runtime** calls these hooks when it allocates a resource's memory; the title installs its scope
around the two display-surface allocations to carve them from its window. Our device proxy replaced the console runtime
for slot 127, and it created a host committed resource instead — so no allocation ever entered the title's scope.

### 2.4 What the runtime did before the fix (diagnostic boot, `PALMETTO_POKE_LOG`)

One 60 s boot at the pre-slice tree (`583b91195`) with four register probes, standard recipe
(`PALMETTO_D3D12=vulkan`, shaders, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`,
`PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_FILE_TRACE=1`):

| probe | measurement |
|---|---|
| `game+0x5eb2503` (surface size copy) | `rax=0xF00` (3840), `rdx=0x870` (2160), `rcx=0x1c` |
| `game+0x247ad64` (scope push) | `rdi = pair` -> `pair = {0, 0x4000000}`; `rdx = game+0x56a2a60` (handler); `rcx = 0xD04` (ctx) |
| `game+0x56a2a60` (handler entry) | **never fired** — the title's scope handler never ran during the boot |
| `game+0x247af25` (the check) | `rax = 0x4000000` — the whole 64 MiB window untouched; the assert fires |

Both display surfaces were created (`slot 127`, 3840 x 2160 RGBA8 = 0x1FA4000 bytes each) between the push and the
check, and the check value stayed 0x4000000. So the producer is not address rebasing and not a size that leaks a
rebased VA: the window is never carved because the allocation path that would carve it is missing in our runtime.

### 2.5 The console arithmetic the check expects

Two 4K RGBA8 display surfaces are 2 x 0x1FA4000 bytes. The handler rounds each carve up to 64 KiB, so
`2 x 0x1FB0000 = 0x3F60000` is carved from a `0x4000000` window, leaving `0xA0000` (655,360) — under `0x200000`, so the
check passes on the console. With the 82 MiB window the same two carves leave `0x12A0000`; section 6 keeps that branch
as an open limit rather than claiming it.

## 3. What we implemented

One behaviour, in the device proxy's slot 127 (`port/shims/d3d12_xs_device.cpp`,
`proxy_create_committed_resource_layout`): when the created resource is a display surface (`D3D12_HEAP_FLAG_ALLOW_DISPLAY`)
with the measured surface shape (2D, single-sample, single-mip, single-plane RGBA8), the proxy calls the title's exported
XMem allocation hook (the function pointer at `image+0x6844380[0]`) with the surface's byte size and the routing word
`0x00808000`, before it creates the host resource. The hook chain then runs the title's own scope handler
`game+0x56a2a60`: the first call reserves the window through the chain bottom (`XMemAllocDefault`, our xmem shim's
allocator) and sets the base; the second carves the second surface. The host resource creation is unchanged — the
display surfaces stay host-allocated committed resources.

The routing word is built from the two measured sides rather than guessed whole: bits 23:16 = `0x80` is the bucket the
scope registration pushes to (2.3), and bits 15:14 = `10` is the branch `game+0x56a2a60` tests for reserve-and-carve
(2.2). The remaining bits do not reach a decision: the shim's default allocator ignores the attribute word, and the
handler passes it through unchanged.

Supporting pieces: `title_surface_allocate` (the default allocator) reads the mapped image base from the startup trace's
image registration, refuses a hook pointer outside the mapped image, and returns null before a title is mapped (the
GPU-free tests). `set_title_surface_allocator` / `title_surface_allocator` is the seam the proxy test uses to replace the
call with a recorder. No other file is touched: no `main.cpp`, xmem, Vulkan-layer or CMake change.

## 4. Cross-check against the reference

The public GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) was read as evidence
for the console ABI (its xmem shim and console-ABI page, upstream paths `runtime/shims/xmem.cpp` and
`d3d12_xs_console_abi.md`, at `main`). Its xmem shim maps
`XMemAlloc`/`XMemFree`/the physical-page family onto the host and its console-ABI page documents device slot 127 as a
committed-resource creation, but **neither carries the export-hook chain or the display-surface window accounting**, so
there is no upstream slice to agree or disagree with here; the mechanism on this page is our own measurement. No code
was copied. The upstream project remains the copyright of its contributors and its license continues to apply to its own
tree.

## 5. A/B measurement

One machine, the repository boot lock, one boot at a time, the standard 130 s measurement recipe; the A arm is the
diagnostic boot of 2.4 (60 s — the assert fires at +4.5 s, so the shorter window measures the same event) and B is the
fix boot.

- **A (measured, pre-slice)**: HEAD `583b91195`, `palmetto.exe` SHA-256 `1864FE17...DEE7B4`, four probes as in 2.4.
- **B (fix)**: HEAD `8daa4a6c3` plus the two uncommitted shim files (the only source change between the arms is
  `port/shims/d3d12_xs_device.*`: the commits between the two HEADs are documentation only). `palmetto.exe` SHA-256
  `45DF506F3009A76156F40D9103B07EA63F811BE3BEFC3649D4540E497B1003BF`, source
  `d3d12_xs_device.cpp` SHA-256 `F57F6B3EA6B8B179150779D83F57663B76AEB62A6802354B9DEE1B5B2CF4B5AA`; the probe log is
  `PALMETTO_POKE_LOG=56A2A60,247AF25`.

| | A: pre-slice (`583b91195`, 60 s) | B: fix (130 s) |
|---|---|---|
| boot outcome | ALIVE, killed by the limit, 0 exceptions | **ALIVE, killed by the 130 s limit, 0 exceptions** |
| title `int3` set | `0x247af2d` (the one assert) | **none — the assert set is empty** |
| frame-scope handler | never fired | **fired twice**: `rcx = 0x1FA4000` (33,177,600 bytes), `rdx = 0x808000` each |
| check value at `game+0x247af25` | `0x4000000` (64 MiB, fires) | **`0xA0000` (655,360 < 2 MiB, passes)** |
| startup trace events | — | 19,636 |
| `[d3d12-trace]` calls | — (60 s arm) | 265,725 (max #268,121) |
| loader / streaming | — | file opens 121; repaired `ReadFile` first-touch at +26.6 s (271,044,608 bytes, 4,136 pages); repaired `IDStorageQueueX::EnqueueRequest` at +27.6 s and +28.1 s |
| timeline at +120 s | — | submits 10,603 / presents 5,301; compute 1,312 asked / 749 created (563 slot-67 refusals) |
| captures | — | 3 (frames 1 / 2,001 / 4,001), 2564 x 1421, mean RGB 0.00 |
| verdict | the last title assert fires every boot | **assert set empty; no new stop; the boot runs the full window and the frame loop stays black** |

The run-to-run counters match the previous slice's fix arm within its recorded variation (`command_signature.md` §5 arm
C: 19,953 trace events, 268,172 d3d12 calls, 10,834 submits / 5,416 presents, 882 asks / 535 compute creations, 121 file
opens, the same first-touch repairs), so the slice changes the assert set and nothing measurable else.

## 6. Limits

- **The next frontier is not an assert any more.** With the set empty the boot stops only at the 130 s kill; the named
  remaining fronts are the 563 console slot-67 `DeserializeComputePipelineStateX` refusals per boot (their own slice)
  and the black frame loop (first light). This page does not claim either.
- **One machine, A 60 s at the previous HEAD, B 130 s.** The assert the arms compare fires at +4.5 s, so the shorter A
  window measures the same event; no longer control was run for this page.
- **The routing word is modelled at unmeasured bits.** Only the routing bits are measured (bucket `0x80`, the
  `0x8000`/`0x4000` mask); the console's exact attribute word is not in the title image, and no consumer below the
  routing reads the remaining bits.
- **Only the measured surface shape is accounted** (2D, single-sample, single-mip, single-plane RGBA8). A display
  surface with another shape is created without the accounting call; if a later scoped window is left unfilled the title
  will assert again, and extending the shape is the first thing that falsifier names.
- **The 82 MiB branch is not modelled.** `game+0x140075140` reads false on this host (64 MiB window). A host whose read
  selects `0x5200000` gets two `0x1FB0000` carves and 19.5 MiB spare, so this slice would not remove the assert there;
  the console must fill that window differently (an unmeasured third surface or a larger carve) and that is the open
  question, not a claim.
- **Accounting, not aliasing.** The title's hook returns a title address inside its window; the PC display surface is
  still a host committed resource at a host address (the layer cannot yet place a display surface at a title VA). The
  title's budget arithmetic is reproduced; the console's address relation is not.
- **No release path.** The window block (64 MiB) is allocated through `XMemAllocDefault` and kept for the boot; the
  console runtime would release it through the free hook when the surface dies, which this proxy does not do yet. The
  measured window is one boot, so the leak is bounded by the boot; a long-lived session should free it.
- **One allocation path.** The call is made where the console runtime makes it for slot 127 display surfaces, on the
  title thread inside that call. No other path (slot 76/133, the Vulkan layer) consumes the hooks.
- **No game data**: RVAs, interface shapes, sizes, counts and outcomes only.

## 7. Tests

[`port/test/xs_proxy_mock_test.cpp`](../port/test/xs_proxy_mock_test.cpp) extends the GPU-free proxy target (the mock
stands in for the PC device; no title, no GPU). `xs_proxy_mock_test` **PASS (9 allocations refused on purpose)**,
including the new checks in the slot-127 block:

| check | what it proves |
|---|---|
| a slot-127 creation with `D3D12_HEAP_FLAG_ALLOW_DISPLAY` asks the installed title allocator for `1920*1080*4` bytes | the surface's byte size is what the scope accounts |
| the attributes word it receives is `0x00808000` | the routing word (bucket `0x80`, reserve-and-carve bits) |
| a slot-127 creation without `ALLOW_DISPLAY` asks nothing | the accounting is limited to display surfaces, not every resource |
| the allocator seam is restored after the block | the GPU-free executable keeps the default (null-before-title) allocator |
| the window arithmetic is modelled: each carve rounds up to 64 KiB, two 3840x2160 RGBA8 carves (`0x1FB0000` each) leave `0xA0000` of a `0x4000000` window, the pre-fix value `0x4000000` fails the compare, `0x1FFFFF` passes and `0x200000` does not | the slot's call size lands on the passing side of the title's check, and the boundary of `game+0x247af25` is stated on both sides |

`_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and the GPU-free gate
targets pass: `xs_proxy_mock`, `unimplemented_policy`, `frame_dump`, `graphics_record`, `graphics_pso`,
`xbox_present`, `buffer_va_index`, `dstorage`, `xapu_chad`. The new source and page pass
`tooling/scripts/check_no_game_data.py`. The docs checks (`tests/test_docs_*.py`) pass on the tracked tree except
`test_docs_cli_flags`, whose one failure is `docs/story_load_probe.md:347` naming `--interval` (another slice's page at
HEAD `8daa4a6c3`, outside this slice); this page's backticked paths, links, tables, list items and repeated lines pass
the same checks when applied to it.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) at `main` (its xmem shim,
console-ABI page and Vulkan-layer page: upstream paths `runtime/shims/xmem.cpp`, `d3d12_xs_console_abi.md`,
`docs/d3d12_vulkan.md`), which carries the console device ABI but no display-surface window accounting; no code from
that project was copied. The upstream project remains the
copyright of its contributors and its license continues to apply to its own tree. This page adds no entry to
[NOTICE.md](../NOTICE.md) because the slice imports no upstream code; the lead's commit flow owns that file's edit if one
is wanted.
