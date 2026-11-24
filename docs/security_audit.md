# Security audit

**Scope:** the runtime's C++ (`runtime/src`, `runtime/shims`), the Python toolchain (`tools/`, `make_game.py`),
and the repo's build/fetch paths. Read-only review with targeted dynamic checks on this host.

**Date:** 2026-11-23. **Tree:** `main` at the commit this file was added with.

## 1. Threat model (state it, or every finding is noise)

This program loads a retail console game image into its own process and runs it.

- **Game code executing arbitrary code is by design.** It is not a finding, and no sandbox is claimed.
- **Single-user desktop.** There is no multi-tenant or server boundary to cross.
- **The attacker considered here** is a third party who can (a) modify or substitute the game package, (b) place a
  file next to `palmetto.exe` or in the working directory, (c) set environment variables for the process, or
  (d) hand the user a crafted archive/save/index to process.
- **Out of scope by construction:** memory corruption caused by the title's own code, exotic instruction sequences,
  and anything requiring the attacker to already have the user's token.

Under this model the interesting questions are: can attacker-controlled *data* corrupt our memory, can attacker-
controlled *placement or environment* get code into our process, and can the runtime reach beyond what the user
asked it to touch.

## 2. Findings

Severity is calibrated to this threat model: **no finding gives elevation.** The binary is `asInvoker` and the
shipped code enables no privilege (2.1 below), so the ceiling everywhere is *the invoking user's own token*. That is
what keeps the loader-related issues at Medium rather than High.

| # | Finding | Severity | Verified by |
|---|---|---|---|
| S1 | `dwmapi.dll` is loaded by bare name from an ordinary DXGI call: fixed name, app directory searched before System32, and `dwmapi.dll` is **not** a KnownDLL | **Medium** | read + `KnownDLLs` registry check |
| S2 | Run-time `LoadLibrary*` from mapped game code falls through to the **default** DLL search order (app dir, System32, Windows, cwd, PATH) | **Medium** | read |
| S3 | `vulkan-1.dll` loaded by bare name in the Vulkan backend | Low | read |
| S4 | Console descriptor-heap address is only alignment-checked; the fallback `VirtualAlloc(MEM_COMMIT, PAGE_READWRITE)` **rewrites the protection of any committed read-only page** at that address | Low | dynamic probe |
| S5 | Import/forwarder DLL names are joined to `game_dir_` without containment: absolute, `..` and UNC names escape the package directory (UNC means the runtime authenticates to a remote host) | Low | read |
| S6 | TLS-host helper clone is created under `%TEMP%` with a predictable name and loaded by path; the guard handle shares `FILE_SHARE_DELETE`, leaving a rename/replace window before the load | Low | read (reasoned, not raced) |
| S7 | `XBCH` declared size from a shader container is used directly as the inflate ceiling: up to 4 GiB from a few MiB, and `bad_alloc` escapes a COM method that returns `HRESULT` elsewhere | Low | read |
| S8 | Save blobs are read whole with no ceiling (`istreambuf_iterator`) while the *write* path enforces a 1 GiB quota; a sparse file turns into a huge allocation | Low | read |
| S9 | `MicrosoftGame.Config` and save `.display` files are read whole with no size limit (the locale path keeps two copies) | Low | read |
| S10 | `tools/arc9_carve.py` inflates a whole zlib stream with no `max_length` and no running cap, unlike every sibling tool | Low | read |
| S11 | `index.json` entry names are concatenated into a filesystem path unvalidated (producer sanitizes today; a third-party index would not) | Info | read |
| S12 | `LoadLibraryEx` flags supplied by the title are forwarded verbatim, including `LOAD_IGNORE_CODE_AUTHZ_LEVEL` (a code-integrity bypass on a policy-managed machine) | Info | read |
| S13 | `PALMETTO_COMPAT_DIR` selects native DLLs from an environment variable, consulted before every shim, with no integrity check | Info (by design) | read |
| S14 | RWX thunk pools: executable memory that only holds runtime-generated constants (W^X weakness, not an injection path) | Info | read |

### S1 — `dwmapi.dll` bare-name load (Medium)

`runtime/shims/dxgi_com.cpp:889-898`

```cpp
void wait_for_vblank() {
    static const DwmFlushFn dwm_flush = []() -> DwmFlushFn {
        const HMODULE mod = LoadLibraryW(L"dwmapi.dll");          // bare name, default search order
        return mod ? reinterpret_cast<DwmFlushFn>(GetProcAddress(mod, "DwmFlush")) : nullptr;
    }();
```

`WaitForVBlank` is an ordinary DXGI call reached during normal rendering, the DLL name is fixed, and the application
directory is searched **before** System32. Checked on this host: 38 KnownDLLs, `gdi32.dll` present,
**`dwmapi.dll` absent** — so a `dwmapi.dll` placed beside `palmetto.exe` is loaded instead of the system copy, and its
`DllMain` runs in the runtime process.

The app directory is already a code-loading directory by design (`palmetto_tls_host.dll` is loaded from it by full
path), so an attacker who can write there already wins. It still deserves fixing because it needs **no package
modification and no unusual behaviour** — just a dropped file and a call the title always makes.

Fix: `LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`.

### S2 — Run-time `LoadLibrary` fallback uses the default search order (Medium)

`runtime/shims/library_loader.cpp:141-200`. Bare names not served by a shim or by `<game_dir>\<basename>` reach
`LoadLibraryA(name)` / `LoadLibraryW(name)` with **no flags**, and no `SetDefaultDllDirectories`,
`SetDllDirectory` or `AddDllDirectory` call exists anywhere in the tree. The search order therefore includes the
current directory and PATH for any name absent from System32.

Note the asymmetry: **static imports are handled correctly** (`pe_loader.cpp:1766-1767` uses
`LOAD_LIBRARY_SEARCH_SYSTEM32`, and the compat path uses a full path with restricted search). Only the run-time path
the title chooses is exposed.

Fix: resolve bare names with `LOAD_LIBRARY_SEARCH_SYSTEM32` (the package directory is already tried explicitly), and
optionally `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` once at startup.

### S4 — Protection rewrite at a title-chosen address (Low)

`runtime/shims/d3d12_vulkan_device.cpp:1712-1734`. The console heap address is accepted when it is non-zero,
64 KiB-aligned and at least one page. For a committed page that is neither writable nor guard/no-access (the
runtime's own `.rdata`, `.text`, IAT), `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` fails because the range is not a
placeholder, and the fallback runs `VirtualAlloc(target, page, MEM_COMMIT, PAGE_READWRITE)`.

Verified on this host that this call **succeeds on an already-committed `PAGE_READONLY` page and leaves it
`PAGE_READWRITE`**: allocate, `VirtualProtect` to `0x02`, then `VirtualAlloc(MEM_COMMIT, 0x04)` → returns the
address, protection becomes `0x04`.

What it cannot do: map pagefile memory over the runtime's code or heap. `MEM_REPLACE_PLACEHOLDER` only works on
placeholders and the runtime creates none; XMem's `isolate_placeholder` requires `MEM_RESERVE`
(`xmem.cpp:318-326`), so a committed runtime page can never be replaced. The outcome is an in-process write the
title did not have, not a boundary crossing.

Fix: only touch the range when it is `MEM_FREE`/`MEM_RESERVE`, or already committed-and-writable; never call
`VirtualAlloc` on a committed page with different protection.

### S7 — `XBCH` size drives the inflate ceiling (Low)

`runtime/src/shader_library.h:628-631`

```cpp
std::memcpy(&unpacked, blob.data() + 16, 4);                       // declared size, from the file
std::vector<std::uint8_t> out;
if (!zlib_inflate(blob.subspan(24), out, unpacked) || ...) return {};   // used as the limit
```

DEFLATE reaches ~1032:1, so a few MiB of compressed zeros inside one shader entry becomes a multi-GiB allocation,
and `std::bad_alloc` escapes `Create*PipelineState` — a COM method that returns `E_OUTOFMEMORY` for allocation
failure elsewhere (`d3d12_vulkan_device.cpp:4058`, `:4173`).

The same field is capped by the tool that reads it: `tools/fssm_unpack.py:45` `MAX_FSSM_BYTES = 64 << 20`, rejected
at `:102-103`. The runtime is 64x more permissive than the project's own contract.

Fix: `if (unpacked > (64u << 20)) return {};`

### S8 — Save blobs read without the quota the write path enforces (Low)

`runtime/shims/xgameruntime_gamesave.cpp:365-370` reads each blob with `istreambuf_iterator` into a vector with no
size check, while `commit()` enforces `kQuotaBytes = 1 GiB` on write (`:386`). A large or NTFS-sparse blob inside a
save container that a user was handed becomes a huge allocation instead of a clean `E_GS_*`. Same path truncates
`info.size` to `uint32_t` at `:347` for blobs ≥ 4 GiB.

Fix: `fs::file_size` check against the quota before allocating; clamp or refuse oversized blobs.

### S6 — TLS helper clone (Low, reasoned)

`runtime/src/pe_loader.cpp:290-336` stages `%TEMP%\palmetto_tls_host_<pid>_<slot>.dll.new`, opens a guard on it with
`GENERIC_READ` and `FILE_SHARE_READ | FILE_SHARE_DELETE`, renames it to the final name, then loads it by path. The
shared `FILE_SHARE_DELETE` permits another process of the same user to open the clone with `DELETE` access, rename it
away and place its own file at the path before `LoadLibraryExW`. The window is small but the name is predictable.
Only same-user local access, and it matters most if the runtime runs elevated for other reasons — which today it
does not need to.

Fix: hold the guard without `FILE_SHARE_DELETE` across the load where possible, randomize the name, and verify the
loaded module's path (`GetModuleFileName`) after load.

### S10 — Unbounded inflate in the ARC9 carver (Low)

`tools/arc9_carve.py:218-231` calls `d.decompress(chunk)` with no `max_length` and joins every piece. Every sibling
tool caps inflation (`navmesh_decode.py:68` 256 MiB, `ped_catalogues.py:102` 64 MiB, `restofworld_index.py:43`
512 MiB, `arc9_names.py:140` passes `DECOMPRESS_MAX`). A crafted archive can exhaust memory in the carve process.

Fix: a running cap with `decompress(chunk, remaining)` and a skip once exceeded, matching the siblings.

## 2.1 What protects the code today (verified, not assumed)

These are the reasons several whole classes of bug are *not* findings, and they are worth keeping:

- **No privilege is enabled anywhere.** The only token call in shipped code is a read-only `PrivilegeCheck`
  (`d3d12_vulkan_device.cpp:10071-10094`); there is no `AdjustTokenPrivileges`, no `SeDebugPrivilege`, no realtime
  thread priority, no service creation. Global-realtime queues are *refused* unless the token already holds the
  privilege. Binary manifest is `asInvoker`.
- **Static imports never reach a bare-name load.** `Loader::resolve` uses shims, then an explicit package path, then
  `LOAD_LIBRARY_SEARCH_SYSTEM32`, with the game folder deliberately excluded (`pe_loader.cpp:1766-1767`). The compat
  loader rejects `/ \ :` in the name and loads by full path with restricted dependency search (`:1714`, `:1724`).
- **The PE and container parsers are bounds-checked throughout** — per-field range checks, `size_t` promotion, and
  explicit caps in DXBC/FSSM/FSPX/RTS0/`index.json`/shader-cache/PE mapping/TLS/export/import parsing. The review
  found **no memory-corruption primitive reachable from a malformed package**. See `docs/boot_audit.md` for the
  adjacent dispatch audit.
- **XMem confines the title's memory requests** to its own recorded placeholder windows (`xmem.cpp:307-314`,
  `318-326`, `682-723`).
- **The path shims confine `GAME:`/`T:` access** to the drive roots (no `..` climb, no `:` component, overlay-internal
  names refused) and classify `MAXIMUM_ALLOWED` as a write; save names are `%`-encoded so separators and a leading
  dot cannot appear.
- **Archive extraction is sanitized.** `tools/palmetto_mods.py` rejects `..`, absolute, drive-letter, ADS and device
  names, then re-checks containment with `commonpath` and rejects symlink members. Carved outputs are named by
  offset (`%012x`), and `shader_carve.py` strips `<>:"/\|?*` and reserved names.
- **The local gallery is loopback-only** with a `Host` allowlist that closes DNS rebinding, `realpath`-contained
  paths on every route, no CORS header, and no write/delete method.
- **The Python tools use no shell, no `eval`/`exec`/`pickle`/`yaml`, and no network code** beyond
  `git submodule update`; the C++ fetches that do exist are opt-in and SHA-256 pinned.

## 3. Recommended order

1. **S1 and S2** — one-line search-path fixes; they are the only findings reachable with a dropped file and normal
   rendering.
2. **S7, S10** — add the caps the project's own tools already use; both are exact one-liners and remove the
   `bad_alloc`-through-`HRESULT` class.
3. **S4** — refuse the protection rewrite; keep the alignment check as is.
4. **S5** — `std::filesystem::path(name).filename()` for import/forwarder names.
5. **S8, S9, S11, S12** — hygiene.
6. **S6** — harden when convenient; no privilege boundary depends on it today.

## 4. Method and limits

Coverage: every `LoadLibrary*`, `VirtualAlloc*`, `MapViewOfFile*`, `VirtualProtect`, token/privilege, environment
variable, registry and file-path site in `runtime/src` and `runtime/shims`; every file/stream reader that parses
package or save bytes; the Python archive→path boundary, subprocess call sites, servers and fetch paths.

Dynamic checks were limited to host-only probes (a `VirtualAlloc` protection check and a `KnownDLLs` read). The game
package was not opened for this audit and is not referenced. Not audited: the C# `arc9-explorer` submodule (the
component that actually decodes asset names), the ~90 per-opcode shader probe tools beyond sampling, and the GPU
driver's own parser surface.

This is a review, not a proof of absence: no fuzzing campaign was run against the parsers in this pass, and the
protection-rewrite finding (S4) was verified as a primitive but not weaponized into a specific game-side sequence.
