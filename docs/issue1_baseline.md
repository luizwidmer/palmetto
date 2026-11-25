# Tracker issue #1 re-baselined: the title's init state and three absent shim answers

**Status: measured, implemented, tested, measured again.** Our tracker's only issue ([issue #1](https://github.com/luizwidmer/palmetto/issues/1), a finding by the issue author, not a fix) was measured at commit `f52762a8c`, before the `ReadFile` first-touch repair (`aaa803f86`) and before the DirectStorage first-touch repair (`8142fd7f9`). This page re-measures its probe at HEAD `8142fd7f9`, lands the three shim answers the issue reports as absent at HEAD, and says what each one changed. **The read-file and DirectStorage repairs between the issue and HEAD moved the boot past the issue's old stop:** the boot no longer ends at the post-repair null read at `game+0x259393a`; it runs the whole 130 s measurement window with 0 exceptions, the loader completes 228/228 begin/end pairs, the frame loop runs (10,764 submits and 5,381 presents at +120 s in the baseline arm) and the screen is still black. The issue's state words still hold: the title's game-state word is **2** and its session word is **0** for the whole window, and it makes no `XUserAddAsync`/`AddResult`/`GetId`/`gamertag` call. The three deltas remove exactly the two `int3` asserts the issue measured them to remove; the two asserts the issue left are attributed below with their producer.

This slice follows [upstream_port.md](upstream_port.md): the issue's diff is an evidence source, not a code source. Every line here is our own, written from the calls and the measured behaviour, in our structure and tests; where our measurement differs from the issue's, the measurement wins and the difference is recorded (section 4).

## 1. Why this page exists

The issue reported four shim answers that differ between the console and a PC runtime and measured each on our tree at `f52762a8c`:

1. `XSpeechSynthesizerCreate` handing out a silent synthesizer instead of `E_FAIL`;
2. device slot 25 sizing a depth-stencil description that also allows unordered access by re-querying without that flag;
3. `texture_format()` accepting sampled `A8_UNORM` for 1D and 3D textures (render targets stay 2D);
4. device slot 74 answering `S_OK` to the title's three-word console call instead of forwarding it to PC `ShaderCacheControl`.

At HEAD `8142fd7f9`, the first is already implemented behind `PALMETTO_SILENT_SPEECH` (commit `f71c7ed4d`; not enabled by this slice), the `build_runtime.ps1` path fix is already in history (commit `5414fdb5e`), and deltas 2-4 are absent: the slot-25 answer is forwarded to `GetResourceAllocationInfo` unchanged (`port/shims/d3d12_xs_device.cpp`), the A8 gate is still TEXTURE2D-only (`port/shims/d3d12_vulkan_device.cpp`, `texture_format`), and no `Slot == 74` branch exists.

The issue's measurements were taken on a tree whose loader still idled on a retried `ReadFile`, so the finding needed a re-baseline before its numbers could be trusted: the `ReadFile` repair and the DirectStorage repair changed the boot state under it. The three remaining deltas are the part of the issue that is still actionable in this tree.

## 2. Measured shapes

### 2.1 The re-baseline probe (arm A, pristine HEAD)

The issue's probe reads three words of the title image from outside the process (`ReadProcessMemory` at image base + RVA) every 10 s. The port maps the title at the fixed base `0x140000000` with `relocated: false` ([console_slots.md](console_slots.md)), so the probe reads at that fixed base in each `palmetto.exe` process and keeps the process whose read succeeds; it samples every 5 s. The boot is a standard 130 s measurement boot (`tooling/boot_report.py`, boot lock, one at a time) with `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, plus `PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_USER_TRACE=1`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_FILE_TRACE=1` and presenter captures (every 2,000 frames, at most 8).

| RVA | issue at `f52762a8c`, 30-230 s | HEAD `8142fd7f9`, 5-20 s | HEAD, 25-130 s | verdict |
|---|---|---|---|---|
| `0x7446510` (game state) | 2 for the whole run | 1 | **2** | holds: the loop is still init state 2; the word never reaches 3 |
| `0x98f828c`, low dword (session) | 0 | 0 | **0** | holds: no session 0 -> 1 step |
| `0x7453ed0`, low byte (loading) | 1 | 0 | **1** | holds; the probe adds the earlier 0 phase the issue did not sample |
| `XUserAddAsync` / `AddResult` / `GetId` / `GetGamertag` | never called | - | **0 calls** | holds: the platform trace records 37 other user-service calls, none of these four |

The state words do not move for the rest of the boot either: 26 samples from 5.00 s to 130.33 s, unchanged from 25 s on, and the probe sees the title process leave at 135.34 s (the harness killed it at the 130 s limit). What changed since the issue is not the state words but where the boot is: at HEAD the loader is not dormant (no 68,608-retry idle) and the frame loop is alive; the screen is black in the same three captures (frames 1, 2,001 and 4,001, mean RGB 0.00 in both arms).

### 2.2 Device slot 25, the depth-stencil + unordered-access pair (arm B measurement)

With the stand-in in place the boot prints, once:

```text
[rt] d3d12_xs: slot 25: depth-stencil texture (format 19 flags 0x80300006) sized without unordered access,
which the PC refuses with it: sized
```

This is our own measurement of the issue's shape: a `TEXTURE2D` description, format 19 (`R32_FLOAT_X8X24_TYPELESS`), flags `0x80300006` (`ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS` plus the console flag bits `0x80300000`). The first host answer on the pristine tree is `UINT64_MAX` (the stand-in only retries when `SizeInBytes == UINT64_MAX`), and clearing `ALLOW_UNORDERED_ACCESS` makes the same query answer a real size. That is the pair the console allows and the PC runtime refuses.

### 2.3 The A8 1D query

The issue measured the title sizing 128x1 1D A8 textures through device slot 25 with flags `0x300000` and layouts `0x0`/`0x100`, and the TEXTURE2D-only gate refusing them with `UINT64_MAX`. The gate is at `port/shims/d3d12_vulkan_device.cpp` `texture_format()` (`desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || ...`). Our boot does not log that query separately; the removal of the `game+0x344196` assert is measured with both this answer and the slot-25 answer landed (section 5), which is the same combined condition the issue measured (section 4).

### 2.4 Console device slot 74

The boot's own baseline forwarding measures the console shape: with no `Slot == 74` branch the proxy forwards by PC index to `ID3D12Device9::ShaderCacheControl`, and the layer's trace records `ID3D12Device9::ShaderCacheControl failed` immediately before the title's `int3` at `game+0x1e6c2c5`. With the answer the first call logs

```text
[rt] d3d12_xs: device slot 74 (console three-word call, game+0x1e6c2c1) returns S_OK:
words 0x27cc34d0000 0x185c0 0x1471bfcc8
```

which is the issue's measured three-word shape (pointer, `0x185c0`, pointer) exactly.

## 3. What we implemented

Three changes, all in the two shim files, default-on because each one is measured to remove a named title assert and neither regresses the boot (section 5). Nothing else was touched: no `CMakeLists.txt`, `main.cpp`, `xgameruntime*`, file-system/xmem/DirectStorage shim, or compute-pipeline path.

- **Slot 25 stand-in** (`port/shims/d3d12_xs_device.cpp`): `slot25_allocation_info()` calls the host first and does nothing when the host answered a size. When the host answered `UINT64_MAX` and at least one description is a non-buffer texture that allows **both** depth-stencil and unordered access, it re-queries the same descriptions with `ALLOW_UNORDERED_ACCESS` cleared (one retry, never a loop, descriptions copied into a bounded stack array of 64) and returns the retry's size when it is valid. A retry that is still refused is logged (one line for the first refusal, four for later ones) and the host's original invalid answer is returned, so the stand-in cannot hide a genuine refusal. Both call orders of the thunk (console order and PC order) use it. The comment block next to the code states the measured pair and the assert it removes.
- **Sampled A8 1D/3D** (`port/shims/d3d12_vulkan_device.cpp` `texture_format()`): the A8 gate now accepts `TEXTURE1D`/`TEXTURE3D` for a sampled texture while an A8 render target stays `TEXTURE2D` (the title's 3840x2160 output mask is 2D) and depth-stencil or unordered access stays refused. The existing A8 create/view paths (R8 image with the alpha swizzle, volume and line view types) already handle the other dimensions; only this gate was refusing them.
- **Device slot 74** (`port/shims/d3d12_xs_device.cpp`): a `Slot == 74` branch answers `S_OK` and prints the first call's three words once. `slot_name()` names the slot and `device_slot_overridden(74)` reports it, so the slot is a declared override rather than a forwarding entry. The method's meaning is not identified (section 6); the measured answer is what the title accepts.

The tests extend the existing GPU-free proxy target ([`port/test/xs_proxy_mock_test.cpp`](../port/test/xs_proxy_mock_test.cpp)): the mock's PC slot 25 now reproduces the host's refusal (the depth-stencil + unordered-access pair and a zero-width description) and the checks cover the sized retry, the still-refused description (the stand-in returns the host's invalid answer) and the one-query path for a description the host sizes; slot 74's `S_OK`, its hit counter and `device_slot_overridden(74)` are checked, and the "forwarded unchanged" control moved from 74 to 77.

## 4. Cross-check against the issue's diff

The issue's proposed diff is the reference for this work; there is no upstream project involved in this page. Agreement and differences:

| Topic | Issue's diff (measured at `f52762a8c`) | This implementation | Why |
|---|---|---|---|
| Slot 25 | helper around `GetResourceAllocationInfo` in the slot-25 branch, retry without the flag, refusal log | same mechanism, our own function name, comments, bounds (count <= 64 stack copy, one retry, bounded refusal log) and both call orders | **Agree on the mechanism.** Ours states the bound on the copied descriptions and keeps the host's original answer when the retry fails |
| A8 | gate replaced by 1D/2D/3D sampled acceptance with a 2D render-target exception | the same measured rule, written as a `sampled_line_or_volume` local with our comment | **Agree.** Both keep depth-stencil and unordered access refused |
| Slot 74 | branch returns `S_OK` and logs the words; also added to the override list | same answer, same one-time log, plus a `slot_name` entry | **Agree on the answer.** The console method remains unidentified in both |
| Other two deltas | silent speech behind a switch, `build_runtime.ps1` root fix | already in history at HEAD (`f71c7ed4d`, `5414fdb5e`); not re-implemented | **Agree, not part of this slice** |
| State words | unchanged with all four applied | unchanged between arm A and arm B | **Agree**, re-measured on the post-repair boot |

One deliberate difference from the issue's text: the issue reads the loop as "the gate is elsewhere"; at HEAD the loader no longer idles, so the same state words now describe a live, black frame loop that never makes the session step. The gate moved: the next non-fatal, named frontier is console compute-pipeline creation (`slot 67` `DeserializeComputePipelineStateX` refusing with `E_INVALIDARG`), which is another slice's scope and untouched here.

## 5. A/B measurement

Two 130 s boots under the boot lock, one at a time, from `D:\leonida-work\issue1\` (outside the repository), same switches and capture settings, one variable pair (the two shim files):

- **A** is the pristine tree at HEAD `8142fd7f9`, `palmetto.exe` SHA-256 `F35E6AA701973197E39BEE3573EC8B7B93EF105E5E3EECC2518096A1657E342F`.
- **B** is the same tree plus this slice's two shim files, `palmetto.exe` SHA-256 `A6B586BB350023304F967E9BB9A38B38633E92D27B88D73F75AE7D12B3FA8CD2`; the two source files at boot time have SHA-256 `C7DF131CD819E6FF24D4D0CDD373C8E38143862DF511C5972C768F0FE83EC0A0` (`d3d12_xs_device.cpp`) and `8F6AEF596A2EC165DC89E99B7473A7ED81EE16B9D0B2309ECCA66ED8AC8F3A02` (`d3d12_vulkan_device.cpp`), unchanged after the final rebuild. The executable's hash is not a stable identity across relinks (the debug directory is regenerated); the two shim source hashes are the slice's reproducible identity, and they are the ones B was booted with.

| | A: pristine HEAD `8142fd7f9` | B: HEAD + three stand-ins |
|---|---|---|
| boot outcome | ALIVE, killed by the 130 s limit, 0 exceptions | ALIVE, killed by the 130 s limit, 0 exceptions |
| startup trace | 19,753 events | 19,819 events |
| `[d3d12-trace]` calls | 269,794 | 277,273 |
| `int3_skipped` per boot | `0x247af2d` +4.78 s, `0x19f4f58` +6.75 s, `0x1e6c2c5` +7.01 s, `0x344196` +20.71 s | `0x247af2d` +4.81 s, `0x19f4f58` +6.73 s |
| state words (external probe) | 1/0/0 until 20 s, 2/0/1 from 25 s to 130 s | identical |
| `XUser*` calls of the issue | 0 | 0 |
| loader begin/end pairs | 228 / 228 | 228 / 228 |
| `ReadFile` first-touch repair | 4,136 pages, 271,044,608 of 271,044,608 bytes read | same |
| DirectStorage shapes | `file=5123 zlib=5002 refusals=0` | same |
| pipeline population at +120 s | 51,786 graphics PSOs; compute 559 asked / 315 created (244 slot-67 refusals) | 51,786 graphics; compute 639 asked / 355 created (284 refusals) |
| submits / presents at +120 s | 10,764 / 5,381 | 11,101 / 5,550 |
| captures | 3 (frames 1 / 2,001 / 4,001), mean RGB 0.00 | 3, mean RGB 0.00 |
| verdict | two named console shapes produce two fatal-in-title asserts | **deltas advance the shim, not the state words**: both named asserts gone, the state words and the black frame loop unchanged |

The compute and submit/present differences are run-to-run variation of the live frame loop (the slot-67 failures also vary with timing: 244 in A, 284 in B); neither arm shows a regression signal from the deltas, and both keep the same exception-free 130 s window.

## 6. The two remaining asserts, attributed

Both are asserted in the title but stepped over by `PALMETTO_SKIP_INT3=1` (the standard recipe), so they are non-fatal at HEAD; the attribution below names their containing functions and the exact title-side producer.

| `int3` | containing function | condition (disassembly and decompile) | producer | removed by the deltas? |
|---|---|---|---|---|
| `game+0x247af2d` | `game+0x247ad10..0x247af58` | at `game+0x247af25`: `rax = [rsp+0x78] - [rsp+0x70]; rax += qword[image+0x75e6ff0]; cmp rax, 0x1fffff; jbe over; int3`. The two stack fields start as `{0, 0x4000000 or 0x5200000}` (64 or 82 MiB, chosen by the boolean from `game+0x140075140`) and are passed to `game+0x145c8f990` before the check; the additive global is in the zero-filled tail of `.data` (no file bytes), so the check is a <= 2 MiB - 1 range invariant on that value unless code writes the global | called from the renderer device setup `game+0x2478440` at `game+0x247942b` (and from `game+0x248d810`) | **no** (still fires at +4.81 s in B) |
| `game+0x19f4f58` | `game+0x19f4df0..0x19f5297` | `if (game+0x2478440() == 0) int3` (call at `game+0x19f4f4f`, `test al, al; jne`) | `game+0x2478440` returns 0 at its first failing console **device slot 41** call. Measured with a temporary one-boot instrumentation of the proxy's slot-41 forwarding: call 1 `game+0x247954e` -> `S_OK`, call 2 `game+0x2479590` -> `S_OK`, call 3 `game+0x24795c6` (return address `game+0x24795cc`, out pointer `image+0x75e75e8`) -> `0x80070057` `E_INVALIDARG`, which makes the setup return 0 (the five call sites are `game+0x247954e`, `game+0x247958a`, `game+0x24795c6`, `game+0x2479605`, `game+0x2479646`). The proxy forwards PC index 41 to `ID3D12Device::CreateCommandSignature`, which refuses the console's third command-signature description (our layer's rule list is in `d3d12_vulkan_device.cpp`, `CreateCommandSignature`); the first two console descriptions pass its rules | **no** (still fires at +6.73 s in B) |

The two asserts the deltas removed, for the record: `game+0x1e6c2c5` is asserted in `game+0x1e6c110` after the console slot-74 call at `game+0x1e6c2c1` when the answer is negative (the baseline boot forwarded it and `ShaderCacheControl failed`); `game+0x344196` is asserted in `game+0x343e80` after the slot-25 sizing path (the baseline's depth-stencil + unordered-access query answered `UINT64_MAX`). Neither fires in arm B.

The next exact producer to model is therefore console **device slot 41** `CreateCommandSignature`: two of the title's five startup descriptions are already PC-compatible and the third is not. A later slice can dump the third description's fields (ByteStride, argument count and the argument types as the PC method reads them) and either translate the console layout or answer it honestly; it needs one instrumented boot, the same method used above.

## 7. Limits

- **One machine, one boot per arm, 130 s.** The issue's numbers were 240 s boots; the frontier measured here now ends at the 130 s kill rather than at a crash, so a longer boot is not needed to state this result but has not been run since `8142fd7f9`.
- **The A8 and slot-25 deltas are not isolated.** They landed together so arm B measures the combined effect; the issue measured the A8 change alone removing `game+0x344196`. A single-delta pair would separate them.
- **Slot 74 is answered, not identified.** The proxy returns `S_OK` to every three-word slot-74 call and logs the first one. The console method's meaning, and a different future call shape at the same slot, would need a new measurement.
- **The slot-25 retry is bounded.** It runs only when the host answered `UINT64_MAX`, only for non-buffer depth-stencil + unordered-access descriptions, with at most 64 descriptions copied, one retry, and the host's original answer returned when the retry also fails. A count above the bound is forwarded unchanged.
- **The A8 relaxation is dimension-only.** A sampled A8 texture in `TEXTURE1D`/`TEXTURE3D` is accepted; an A8 render target stays `TEXTURE2D`; depth-stencil and unordered access stay refused for A8. Actual 1D/3D A8 texture creation is not exercised by the GPU-free test target; the allocation answer is.
- **The `0x247af2d` condition is read statically.** The additive global at `image+0x75e6ff0` is not backed by file bytes; whether title code writes it in the measured window was not observed, so the page states the measured instruction sequence and the file-backed value separately.
- **The `0x19f4f58` producer was measured with a temporary build.** The slot-41 log used a bounded `fprintf` added for one short boot and was removed before the final build; the source hashes of arm B are those of the final tree.
- **The compute-pipeline refusals are not this slice's.** `slot 67` `DeserializeComputePipelineStateX` returns `E_INVALIDARG` (244-284 calls per boot); the slice that owns the compute path will measure and implement it. This page only records the count so the two arms are comparable.
- **`d3d12_fence_timing_test` (GPU-dependent, not in the standard test-target list) segfaults in `Fence::from` on queue console slot 17 `Wait`; the same binary built against HEAD's `d3d12_xs_device.cpp` segfaults identically**, so it is not a regression from this slice. The GPU-free gate targets pass.
- No game data: this page contains RVAs, interface shapes, HRESULTs, counts and outcomes only, no package path, executable byte, shader byte or extracted table.

## 8. Tests

[`port/test/xs_proxy_mock_test.cpp`](../port/test/xs_proxy_mock_test.cpp) extends the existing GPU-free proxy target: `xs_proxy_mock_test` **PASS, 0 failures**, including the new checks:

| Check | What it proves |
|---|---|
| slot 74 answers `S_OK` and the mock's PC slot 74 is never reached | the measured console three-word call is served, not forwarded |
| `device_slot_hits(74) == 1`, `device_slot_overridden(74)` | the slot is a declared override with its own hit counter |
| slot 25 sizes a depth-stencil + unordered-access texture | the stand-in re-queries with `ALLOW_UNORDERED_ACCESS` cleared and returns the retry's size |
| the re-query's flags are the original minus `ALLOW_UNORDERED_ACCESS` | what the stand-in changes about the description, measured at the mock's entry |
| a description the retry still refuses keeps `UINT64_MAX` | the stand-in cannot hide a genuine refusal |
| a plain colour texture is sized by the first query | no extra host query on a description the host accepts |

The other standard gate targets build and the GPU-independent subset runs **15/15 passed** (`xs_proxy_mock`, `unimplemented_policy`, `frame_dump`, `graphics_record`, `graphics_pso`, `xbox_present`, `buffer_va_index`, `dstorage`, `xapu_chad`); `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` and `_setup\build_tests.cmd` prints `TESTS_BUILT`; the docs checks (`tests/test_docs_*.py`) pass; `tooling/scripts/check_no_game_data.py` passes on the changed files.

## 9. Cross-check and attribution

The measured finding this page re-baselines is tracker issue #1 in `luizwidmer/palmetto`, by the issue author (`marekpastuszak-lang`), measured at our commit `f52762a8c`; the issue's diff is the reference for the three deltas. No code from it or from any other tree was copied: the three answers here are our own functions, comments, names and tests, written from the title's measured calls and our layer's measured answers. The commit that moved the boot past the issue's old frontier, and that this page merely re-measures, is `8142fd7f9` (DirectStorage first-touch repair; the `ReadFile` half is `aaa803f86`). The silent-speech answer the issue also reports is `f71c7ed4d`; the `build_runtime.ps1` path fix is `5414fdb5e`.
