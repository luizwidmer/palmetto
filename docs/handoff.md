# Pause and resume handoff

This page is the starting point after an indefinite pause. The working tree is clean, no background agent or build is
expected to be running, and the boot lock should be free. Everything below is measured from this checkout, not from
memory: re-check `git status`, `git log -1`, the boot lock and running processes first.

## Current state (round-21 landing)

| item | value |
|---|---|
| repository | `D:\leonida` on `main`, no PRs for our own fork |
| round-21 landing | code + docs followed by one final docs commit (baseline round-20 `d95a159ce`/`58091833d`); tree clean and pushed at this page's commit |
| last green gate | `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` on the final binary (SHA-256 `5C30DE03...200451A`) |
| latest test evidence | round-21 CTest suite **35/35 passed** (9.4 s): strict `loader_tls_race` (F4/F7 fixed), `title_va` with the validation layer on, `d3d12_title_va_production` (P-45 production pixel), `opt_in_switches`, `indirect_graphics_replay_readiness`, `root_sig`, `xmem`, plus all round-20 targets; known group `d3d12_draw`/`d3d12_set0_compute`/`d3d12_bindless_churn[_validated]` still fails at the documented queue-slot-14 Signal refusal (layer owner) |
| docs | all `tests/test_docs_*.py` green, 22 tests; the docs owners' extended suites report 38 passed / 215 subtests (clear inherited `GIT_CONFIG_*` first, below) |
| agents | none running; the 8 GPT-6 Luna (max effort) + 6 DeepSeek round-21 teammates finished, and the lead committed their slices |
| boot lock | free; no `palmetto`/`cl`/`ninja`/`ctest` process should remain |

## Round 21 landed (2026-11-28)

Fourteen teammates (8 GPT-6 Luna at max effort + 6 DeepSeek) over round plan tasks 253-266 equivalents plus follow-ups; the
lead integrated, gated, committed and updated the board. The round fixes the open loader bugs, makes the Title-VA validation
gate honest, reduces the suite's known failures, ports/validates the next peer and frontier slices, and audits every opt-in
switch.

### What landed

- **Loader F4/F7 fixed + census wired** (the round-20 open items). F4 now records the helper-installed thread block so
  `load()`'s snapshot skips a served thread; F7 stops running mapped TLS callbacks under the shared TLS lock, registers a
  callback flight under the lock so an image cannot be unmapped mid-callback, preserves per-slot attach ascending / detach
  reverse-slot callback+cleanup order, and adds a callback-free sweep for blocks loaded during detach. `loader_tls_race`
  runs as a strict CTest (no `--expect-faults`): 9 checks, 0 faults, F4 seam/control each one detach, F7 child exit 0.
  `PALMETTO_LOADER_CENSUS=1` is now wired into `pe_loader.cpp` with the 47-check formatter test and switch-off
  byte-identical stdout/stderr proofs (SHA-256 recorded), so P-57 is complete.
- **Title-VA validation gate is honest and green**. The failing validation count was three real empty pre-draw capture
  errors (`VkBufferCreateInfo-size-00912`, `VkMemoryAllocateInfo-allocationSize-07897/-07899`): the title-import pre-capture
  created a buffer/allocation with no sampled textures. The runtime now skips that capture when `pre_count`/`total` are
  zero (non-zero path unchanged); the test replays its captured stderr on failure and returns its render target to
  `RENDER_TARGET` before the automatic capture (removing two layout/sync artifacts). `d3d12_title_va` now runs with
  `PALMETTO_VK_VALIDATION=1` and passes 3/3 on the RTX 4090 with the known sRGB counter at 0.
- **P-45 production pixel acceptance**: new `d3d12_title_va_production` GPU CTest registers the production outside-arena
  BDA window (candidate `0x17490000`, `0x90000` above arena end `0x17400000`), pulls the raw synthetic span twice
  (2.2 MiB), samples the pixel through the real PC queue/fence (no Xbox slot-14 proxy) and verifies `35 ff c1 ff`, then
  `00 00 00 ff` after moving the source offscreen; named CPU refusals for below-heap, overlap and out-of-bounds windows.
  This closes the P-45 pixel falsifier for the synthetic production path - the title-boot pixel acceptance stays the
  frontier item.
- **C-06 multi-count indirect readiness**: with exact `PALMETTO_D3D12X_STRICT_VIEWPORTS=1`, shared replay preflight
  refuses multi-count graphics `ExecuteIndirect` before resource resolution/emission with the named reason
  `indirect_multi_count_graphics_state_not_captured`; dispatch, counts 0/1 and the switch-off path are unchanged.
  Honest limit: full call-site graphics state is not captured, so this is a strict refusal/readiness contract, not replay
  parity.
- **Movie second-plane identity/order contract**: a pure helper requires confirmed same-PresentX identities for plane 0 and
  plane 1, classifies plane 1 as movie/UI/other overlay, and fails closed on unknown identity; the existing
  `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY` gate and switch semantics are unchanged. The measured sample has only
  one-plane PresentX calls, so this is contract-only pending measured plane evidence.
- **Query-heap readiness wired + opt-in switch audit**: `PALMETTO_D3D12X_QUERY_HEAP_READINESS=1` now runs at the layer's
  real `CreateQueryHeap` (named `E_INVALIDARG`/`E_NOTIMPL` refusals before allocation, byte-identical when unset). The
  audit registry (`port/src/opt_in_switches.h`, 294 switches, 3,010-check test, `docs/opt_in_switches.md`) re-derives the
  switch set and call-site reachability from source and records four named findings (dead wiring now down to two
  contract-only headers, `any_set` empty-value semantics, spelling, default-on switches); `loader_census` and the
  query-heap switch are now wired.
- **Suite failures fixed down to the documented queue-slot-14 group**: `root_sig`'s two checks were a real shim defect
  (the all-zero-checksum branch ran after the unconditional hash verify; fixed in `d3d12_xs_rootsig.cpp` and validated
  against the host d3d12.dll); `xmem`'s working-set check is now per-page deterministic (545/545, 3/3 runs); the bindless
  precondition failures were a test-side stack-description ABI bug fixed with a zero-tailed `PcHeapDesc`; `d3d12_draw`'s
  stale 512 MiB heap expectation now uses the measured 1024 MiB arena. All four now reach the same known
  `queue-slot-14 Signal` refusal (4 targets, layer owner), which is the only remaining known group in this set.
- **Translator next refusal class**: one exact reachable `s_clause 3` gap rule with a synthetic fixture; 491/668
  translated and 177 refused unchanged, 3 words recovered, 0 modules closed, all 491 previous SPIR-V modules
  byte-identical. The two pre-existing semantic-suite failures (`test_exec_computed`, `test_image_atomics_guard`) are
  unchanged and documented.
- **Frontier F-03 recheck**: the type-0x24 producer is not unreferenced - `0x2218180` is reached through registered
  app-state-change callbacks (`0x1D10A30` -> LEAs `0x2217C70/0x2217C90` -> tail jump), with a four-site next-arm recipe
  and the measured boot still showing 0 callback/producer hits and 0 key hits. `decomp_first_ui.md` section 11 is
  corrected (no reachability overclaim).
- **Docs**: `pe_loader_rewrite` (F4/F7/census/validation), `testing` (round-21 final gate), `boot_smoke` (round-21 arms),
  `opt_in_switches` (new page), `vertex_buffer_va` (P-45 + P-48 validation), `d3d12_vulkan` (C-06/query heap),
  `peer_port_movie`/`movie_overlay_readiness`, `peer_port_first_ui` (C-08 verification), `decomp_first_ui` (F-03),
  `translator_fixes`/`design`, `cleanroom_peers`/`remaining_mechanisms`/`README` all refreshed; the docs checks stay green.

### Boot arms on the final binary (`5C30DE03...200451A`)

Three valid light arms under the boot lock, 88 s timeout, captures/readbacks empty:

| arm | switches | wall | result | device loss |
|---|---|---|---|---|
| a1 | `PALMETTO_TRACE_TLS=1 PALMETTO_TEB_CHECK=1` + state trace | 88.4 s (killed) | ALIVE, stage 2 at 86 s | 0 |
| a2 | +`PALMETTO_LOADER_CENSUS=1 PALMETTO_PE_LOADER_SHADOW_PARSE=1` | 82.1 s | 15 census lines, **0 shadow divergence lines**, stage 2 at 78 s; then two refused `ExecuteCommandLists` -> `present failed (VkResult -4)` -> post-loss AV at `game+0x86F97E` (single arm, not attributable) | 1 |
| a3 | +`PALMETTO_DS_REAP_ORDER=1 PALMETTO_TITLE_CLASS0A_SIZE=1` | 88.2 s (killed) | ALIVE, reached stage 3 at 86 s, class-0xa mitigation line plus a `0x1000`-sized block with no overlap, one `reap-order-on` line (2 lanes, buffer 64) with no refusal | 0 |

Loader surface identical in all arms: preferred `0x140000000` (not relocated), size 184,995,840, entry `+0x56a1ec8`,
binds 95/420/72/0, five modules, `/GA` TLS 53,246 reads at index 6, TEB slot-8 marker matching. Losses 0/1/0, so the
two-consecutive-loss stop rule never fired. Arms `a1`/`a2` of an earlier pass on the intermediate binary were invalidated
by the lead attaching a debugger (NTSTATUS 0xC0000354) and are recorded as INVALID, not as runtime findings.

### Gates measured at the landing

- `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`.
- round-21 CTest suite: **35/35 passed** (9.4 s), including the strict loader race gate, validation-on title-VA, the P-45
  production GPU test, the switch audit, the indirect readiness contract, `root_sig` and `xmem`.
- known group still failing as documented: `d3d12_draw`, `d3d12_set0_compute`, `d3d12_bindless_churn`,
  `d3d12_bindless_churn_validated` - all at the queue-slot-14 `Signal` refusal, not a test defect.
- docs checks: 22 passed (plus the docs owners' 38-test/215-subtest extended suite).
- full-suite inventory: recorded in `docs/testing.md` by the inventory owner.

### What is still open (next thread)

1. **Queue-slot-14 `Signal` group** is the only known failing group left in this set (`d3d12_draw`, `d3d12_set0_compute`,
   `d3d12_bindless_churn[_validated]`); it is the d3d12_xs queue-proxy owner's slice. The bindless slice also recorded a
   report-only robustness finding for the descriptor-tail heuristic (validate the follower size word instead of trusting
   it).
2. **F4/F7 residual coverage**: no deterministic arm proves Loader destruction re-entered from its own thread-event
   callback (self-wait, unsupported), static-TLS DLL thread-events are not covered, and the 16-slot mapped-TLS cap
   remains. The V2 adapter is still unwired.
3. **Title-boot pixel acceptance** remains the content/presentation frontier: a3's stage-3 run exercised the class-0xa
   mitigation with a correctly sized block but is a single arm, not a controlled A/B; the a2 loss chain is post-loss AV
   family, not attributable to the loader switches.
4. **F-03** next-arm recipe is published (four sites); F-01/F-02 stay blocked.
5. **Contract-only files**: `d3d12_residency_budget.h` and `ui_input_script.h` have no runtime caller by design; the movie
   plane identity helper needs measured plane evidence before wiring.
6. **CI**: the five failing jobs on `main` predate both rounds (`translator-tests`, `stdlib-tests-linux`,
   `viewer-tests`, `semantic-tests (ubuntu-latest)`, `macos-python`); P-26 branch protection remains open.

### Constraints to carry over

- no game data or package bytes in the repo, logs, tests or task text; no keys/decryption; peer clones are read-only
  evidence and every mechanism gets a NOTICE/docs credit.
- opt-in until measured: default path byte-identical, named refusals, synthetic tests and a boot/GPU falsifier where
  applicable.
- one build at a time against `port\build`; never relink `palmetto` while an arm holds the exe; `LNK1168` means an arm
  still holds it.
- `d3d12_draw` remains a known-failing shared GPU fixture in the queue-slot-14 group; use `d3d12_viewport_array` for the
  viewport case.
- reports/evidence under `D:\leonida-work` and `reports/` stay outside the repo.

## What landed in round 19 (kept for history)

- **PE-loader audit** (`5b0233a48`): core mapping, binding, TLS/thread/unwind and base-reservation faults fixed with named
  errors and five synthetic audit suites (`loader_core_audit`, `loader_bind_audit`, `loader_tls_audit`,
  `loader_selfcheck`, extended `pe_loader_test`). The measured verdict: the black frame is **not** loader-caused -
  103/103 arms map/import/initialise cleanly and the `PALMETTO_TRACE_TLS=1` arm shows per-thread /GA views installed in
  every listed thread (ALIVE 90.3 s, no exception). Independent mapped-image self-check: 203 checks, 0 faults.
- **PE-loader rewrite phases 0-2**: `pe_image` parser (29 refusal + 6 edge shapes, 48,659-case mutation test),
  `pe_mapper`, `pe_resolver`, `pe_loader_shadow`, and the old-loader/parser parity harness (0 discrepancies). The mapper
  uses the measured page-tail copy rule.
- **P-47**: `d3d12_viewport_array` CTest registered and passing on the RTX 4090 (`e55f04d33` + pause commit).
- **Peer slices from round 18**: `stream_reap` (ordered completion retirer for a future concurrent DirectStorage queue)
  and `movie_overlay_readiness` (opt-in movie-overlay evidence contract), both pure and unwired, with tests/docs.
- **Workspace cleanup**: ~93.7 GB of stale scratch/installers removed; evidence directories referenced by docs kept.

## Open work after round 20 (kept for history)

The list below was superseded by the Round 21 section above; kept for the record.

1. **P-50 / mapper parity** (task-253 stopped before start): old `Loader` vs new `pe_mapper` mapping/protection parity on
   the synthetic fixtures; fix any real mapper difference, not the old loader.
2. **Loader V2 adapter (task-254 stopped before start)**: wire `PALMETTO_PE_LOADER_SHADOW_PARSE=1` / `_STRICT=1` into
   `map_image` (hunk is in docs/pe_loader_rewrite.md and the shadow slice), and turn the TLS F1 fallback ("no TlsAlloc
   slot below 64") into a named load failure instead of logging and continuing.
3. **P-48 title-VA CTest** (task-258 stopped before start): dedicated GPU test for raw title VA -> window -> pull ->
   readback with the named refusals; the P-45 production alias was measured, the pixel assertion was blocked by the Xbox
   queue slot-14 Signal in the shared fixture.
4. **Loader observability**: additive stderr census (`mapped <name> base size relocated entry`, bind summary, TLS
   attach/detach counts, VirtualProtect failures) behind one switch, asserted by the parser/mapper tests.
5. **Peer campaign**: continue the priority subsystems - movie/Bink overlay path (wire `movie_overlay_readiness` at the
   second-plane decision behind an opt-in switch), first-run/UI presentation, streaming completion integration; keep the
   `docs/cleanroom_peers.md` parity matrix current with peer pins and NOTICE credits.
6. **F4/F7 TLS items**: duplicate-install race and thread-event `LoadLibrary` self-deadlock are analysis-only with
   falsifier recipes in the round-18 TLS audit report (gitignored under `reports/pe/tls_audit/`).
7. **Full-suite refresh**: the round-19 inventory task (task-259 stopped before start) should rerun the 302-test table
   against the new targets and update `docs/testing.md`.

## Where things live

- target package (never commit bytes): `D:\Torrent\GTA6.Xbox.v1.0.2.1-CYBERLEEK\game_scarlett_master.exe`
- boot template and evidence: `D:\leonida-work\fsrstable\boot_arm.ps1`; arm output under `D:\leonida-work\fsrstable\boot-*`
- loader audit reports/evidence: `D:\leonida-work\coreaudit`, `D:\leonida-work\baseaudit`,
  `D:\leonida-work\scratch\bindaudit`, `D:\leonida\reports\pe\tls_audit` (gitignored `reports/`)
- PE rewrite page: `docs/pe_loader_rewrite.md`; board: pinned GitHub issue #4 + `docs/taskboard.md` mirror
- peer clones (read-only evidence): `D:\leonida-work\peers\{fivestar,percy,victory,victory-fork,xcl}`

## Resume commands

Clear the inherited Git config first or the docs tests that spawn a temporary repository fail:

```powershell
Get-ChildItem Env:GIT_CONFIG_* | Remove-Item
Set-Location D:\leonida
git status --short
git log -1 --format='%h %s'
```

Build gates (run one at a time; no build while a boot arm holds `palmetto.exe`):

```powershell
cmd.exe /c D:\leonida\_setup\build_gta6rt.cmd   # expect PALMETTO_OK
cmd.exe /c D:\leonida\_setup\build_tests.cmd  # expect TESTS_BUILT
```

Loader and rewrite tests:

```powershell
$ctest = "$env:LOCALAPPDATA\Programs\DeepSeek Harness\resources\app.asar.unpacked\..."; # or the bundled
# cmake path used in the transcripts, then:
& $ctest --test-dir D:\leonida\port\build -C RelWithDebInfo `
  -R "^(loader|pe_loader|pe_loader_bounds|loader_tls|module_names|unimplemented_policy|startup_trace|pe_image|pe_image_fuzz|pe_mapper|pe_resolver|pe_loader_shadow|loader_core_audit|loader_bind_audit|loader_tls_audit|loader_selfcheck|d3d12_viewport_array)$" `
  --output-on-failure
```

Docs checks:

```powershell
$py = "C:\Users\Luiz Widmer\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
& $py -I -m pytest -q tests/test_docs_env_vars.py tests/test_docs_repo_paths.py tests/test_docs_links.py `
  tests/test_docs_table_rows.py tests/test_docs_repeated_lines.py tests/test_docs_cli_flags.py
```

Boot arm (take the lock, stop after two consecutive device losses; the host is prone to NVIDIA event 153):

```powershell
& D:\leonida-work\fsrstable\boot_arm.ps1 -Tag resume-smoke -TimeoutSec 90 -Light -Who <name>
```

## Pause risks to re-check

- No `palmetto`/`cl`/`ninja`/`ctest` process and no `D:\leonida-work\.boot-lock` file before resuming.
- The shared `port/build` is Ninja; build one target at a time, and never relink `palmetto` while an arm holds it.
- `reports/` is gitignored: the TLS/loader audit reports referenced above are evidence only and are not in the repo.
- CMakeLists is lead-owned: agents report stanzas, the lead applies them.
- `d3d12_draw` remains the known-flaky shared GPU fixture; use `d3d12_viewport_array` for the viewport case.

## Round 20 landing (kept for history)

The huge round-20 push executed: 14 teammates (8 GPT-6 Luna at max effort + 6 DeepSeek) over round plan tasks 253-266,
with the lead integrating, gating, committing and updating the board. Code is `d95a159ce`, docs `58091833d`, and this
handoff update is the next commit on `main`; everything is pushed.

### What landed

- **P-50 mapper parity**: `pe_mapper` mirrors the legacy ImageBase/FileAlignment/SectionAlignment and shared-page
  differing-protection refusals before allocation; `pe_mapper_parity` (309 checks, 0 failures) maps the same synthetic
  fixtures through the old `Loader` and the new mapper and compares base/size/entry, page-tail and raw-pointer-zero
  bytes, relocation results, protection plan vs VirtualQuery and the named malformed refusals.
- **P-56 loader V2 shadow wiring + TLS F1**: `map_image` caches only exact `PALMETTO_PE_LOADER_SHADOW_PARSE=1` /
  `PALMETTO_PE_LOADER_SHADOW_STRICT=1`, reuses the file buffer, emits bounded `[rt] loader shadow parse` diagnostics,
  throws only in strict mode and is byte-identical when off; TLS F1 now frees a >=64 `TlsAlloc` index and refuses the
  load with `GA_TLS_SLOT_UNAVAILABLE`. `pe_loader_shadow_wiring` 6/6, `loader_tls`/`loader_tls_audit` green (45 checks,
  `FIXED AUDIT-F1/F5/F6`).
- **P-57 census layer**: `port/src/loader_census.h` (header-only, `PALMETTO_LOADER_CENSUS=1`) formats bounded
  mapped/bound/tls/tls-counters/protect-failed lines; `loader_census` 47 checks; the `pe_loader.cpp` wiring hunks are
  recorded outside the repo (`D:\leonida-work\census\hunks.md`) and are **not applied**.
- **F4/F7 TLS contracts**: `loader_tls_race` deterministically reproduces the duplicate-install window (2 vs 1
  `DLL_THREAD_DETACH`) and the thread-event load self-deadlock (child watchdog) with three child arms; CTest runs
  `--expect-faults` while the fixes are open. A validated F4 patch proposal (scratch copy: seam count 1) and an F7
  analysis proposal are in `D:\leonida-work\tlsrace\report.md`; no loader fix was applied this round.
- **P-48 title-VA GPU CTest**: `d3d12_title_va` registers and passes 1/1 on the RTX 4090: raw title VA -> window
  registration -> `buffer_at` resolve -> pulled bytes sampled/read back through the real queue/fence, with named
  out-of-window and `WriteBufferImmediate` refusals. The functional gate runs without the validation layer; with
  `PALMETTO_VK_VALIDATION=1` its final counter assertion fails on a nonzero count whose messages are swallowed by the
  ExecuteCommandLists stderr capture (open item in `vertex_buffer_va.md`; `d3d12_viewport_array` stays the
  validation-clean GPU test).
- **Pure peer/contract slices**: D3D12X query-heap readiness and CPU residency budget contracts (opt-in, unwired); the
  movie-overlay readiness gate at the second-plane decision behind `PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1`
  (guard only, no playback claim); DirectStorage ordered publication behind `PALMETTO_DS_REAP_ORDER=1` with the serial
  default unchanged; the first-run screen-state input contract (`PALMETTO_UI_INPUT_SCRIPT=1`, pure/unwired); the
  translator's one exact `s_mov_b32 exec_lo` gap rule (491/668 translated, all 491 previous modules byte-identical,
  no program closure - an honest negative).
- **P-61 inventory + P-62 docs**: `docs/testing.md` refreshed against the round-20 tree; `docs/progress.json` and the
  generated README block retire the 2026-11-24 frontier as historical and point at the content/presentation frontier,
  fixing the D-04 leftover XccQ114 reported.
- **Boot arms (P-55/P-56 follow-up)**: three light arms on one fixed binary (`BFA39770...`): baseline and class-0xa
  content arms ALIVE at 88.4 s (stage 2 at 86 s); the shadow-parse arm reached stage 3 at 62 s, emitted **0**
  shadow-parse divergence lines, then hit the known class-0xa AV at `game+0x13360a3` (single arm, not attributable).
  Loader surface identical in all arms: preferred `0x140000000`, not relocated; bindings 95/420/72/0; 5 modules; the
  `/GA` TLS rewrite; TEB checks. 0 device losses in 3/3. Evidence: `D:\leonida-work\fsrstable\boot-round20-266-{a1,a2,a3}`;
  page `docs/boot_smoke.md`.
- **Test-gate fix**: `loader_test` now calls `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)`. A malformed
  compat image made Windows raise a hard-error dialog that blocked CTest with no one to answer it, while standalone
  runs returned 0; `loader` CTest is green in 1.0 s again.
- **P-60 translator rule**, **P-02 input contract**, **P-58 query/residency contracts** and **P-59 overlay/DS slices** are
  each behind an exact opt-in switch or pure/unwired, with synthetic tests and page updates; default behaviour is
  unchanged in every case.

### Gates measured at the landing

- `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`.
- round-20 CTest subset (loader/rewrite + peers + GPU): **29/29 passed** in 10.8 s, including `loader`,
  `pe_mapper_parity`, `pe_loader_shadow_wiring`, `loader_census`, `loader_tls_race` (2 expected F4/F7 faults),
  `ds_publication`, `query_heap_readiness`, `d3d12_residency_budget`, `ui_input_script`, `movie_overlay_readiness`,
  `d3d12_viewport_array` and `d3d12_title_va`.
- docs checks: **22 passed** (env_vars, repo_paths, links, table_rows, repeated_lines, cli_flags).
- full-suite refresh: 321 registered, 6 exclusions, 314 executed -> 281 pass / 11 skip / 22 fail with the grouped causes
  in `docs/testing.md`; `d3d12_draw` remains the known-flaky shared GPU fixture (use `d3d12_viewport_array`).

### What is still open (next thread)

1. **P-57 wiring**: apply the census hunks to `pe_loader.cpp` behind `PALMETTO_LOADER_CENSUS=1` (owner-only file) and
   assert the lines in the parser/mapper tests; hunks and the switch-off byte-identical proof are outside the repo.
2. **F4/F7 fixes**: the F4 proposal is validated on a scratch copy; F7 (release the TLS lock before running callbacks,
   drain in-flight callbacks on destruction) is analysis-only. Land one at a time, then drop `--expect-faults` from the
   `loader_tls_race` CTest.
3. **P-48 validation-on item**: identify the validation error whose message the ExecuteCommandLists stderr capture
   swallows, then re-enable `PALMETTO_VK_VALIDATION=1` on `d3d12_title_va`.
4. **Frame frontier** F-03..F-07 is unchanged and claimable one at a time; F-01/F-02 stay blocked. The content/presentation
   wall is the same measured wall (class-0xa stage-3 AV; no content producer/colour input).
5. **CI**: the five failing jobs on `main` predate round 20 (`translator-tests`, `stdlib-tests-linux`, `viewer-tests`,
   `semantic-tests (ubuntu-latest)`, `macos-python`); P-26 branch protection and the P-39 triage remain open.
6. The title-VA production BDA ordering pixel acceptance (P-45) and the multi-count replay gap (C-06) are the next
   renderer/memory items.

### Start the next thread with this state

- repo `D:\leonida`, branch `main`, tree clean and pushed; boot/build locks free; no `palmetto`/`cl`/`ninja`/`ctest`
  process expected.
- last green gates above; the round-20 team task board rows (task-1..task-15) are closed. The board started empty in that
  session, so the 256-task limit did not apply; the CI/contributor triage row was lead-owned as task-13.
- the live board (issue #4) and `docs/taskboard.md` carry the round-20 statuses; do not re-publish them.
- boot-arm evidence and the `D:\leonida-work\scratch\round20\issue4.md` snapshot are outside the repo.

### Constraints to carry over

- no game data or package bytes in the repo, logs, tests or task text; no keys/decryption; peer clones are read-only
  evidence and every mechanism gets a NOTICE/docs credit.
- opt-in until measured: default path byte-identical, named refusals, synthetic tests and a boot/GPU falsifier where
  applicable.
- one build at a time against `port\build`; never relink `palmetto` while an arm holds the exe; `LNK1168` means an arm
  still holds it.
- `d3d12_draw` remains the known-flaky shared GPU fixture; use `d3d12_viewport_array` for the viewport case.
- reports/evidence under `D:\leonida-work` and `reports/` stay outside the repo.

## Round 22 (2026-11-28): the session-3 completion instrument, SSE4a emulation, and the menu push

This round targeted the first menus directly. The frontier moved: **the session-3 loading wall is no longer the wall**, but
no menu pixel has been rendered yet.

### What landed

- `PALMETTO_TITLE_SESSION3_COMPLETE=1` (`port/src/title_frontier_patch.h`, wired in `port/src/main.cpp`): after the image
  map, a bounded writer waits for init state >= 2 and writes 1 into the loading record's slot-4 completion dword
  `image+0x78ACB20` (the dword the title's 33 ms poll `0x21B7070` tests through `0x41E8C0`) every 100 ms. In the first
  arm this took the title **init 2 -> 3 and session 1 -> 13 at 68.0 s** with the stage-3 pump running.
- `PALMETTO_TITLE_PRESENT_KEYS=1`: forces the two media/UI present-pass key bytes `image+0x71B90AD`/`0x71B90AC`, gated to
  **init >= 3 and session != 0** (forcing earlier stalls the boot; every weaker gate was measured).
- `PALMETTO_TITLE_OUTRECORD_STATE2=1`: the section-15.6 one-byte redirect at `image+0x402E987` (`0x74 -> 0xEB`), with
  full site-byte validation. It applies correctly but disturbs early boot when combined; dedicated-arm only.
- **Clean-room SSE4a emulation** (`port/src/sse4a_emulation.h`, called from the illegal-instruction handler in
  `port/shims/startup_trace.h`): the forced-completion arm stopped at `image+0x2E7FF1` on the AMD-only `INSERTQ`
  instruction (`f2 0f 78 c1 10 10`) inside a half-float packing routine. EXTRQ/INSERTQ (both immediate and register
  forms) are now emulated; 25 synthetic checks; the stop is removed by construction.
- Presenter-keys and out-record diagnostics are opt-in and off by default; the default path is byte-identical.
- New CTests: `title_frontier_patch` (46 checks), `sse4a_emulation` (25 checks); `code_hooks` 211 checks after the
  exact-`1` class-0xa parser hardening; `ui_draw_probe` 198 checks after the bounded caller ring; the queue-slot-14
  mapping suite is green (7/7). `tooling/scripts/frame_analysis.py` + `tests/test_frame_analysis.py` are the bounded
  capture/readback analyzer.

### What the arms measured

- Forced completion reached session 13/stage 3 at 68 s, then stopped at INSERTQ (fixed now).
- The title's own window exists (`sgaWindowClass`, 3870x2190 outer, 2580x1460 client at 150% DPI) and is visible and
  foreground; its client area is **pure black** in every window screenshot (83 samples), and presenter captures in the
  arms that reached session 1 were 19/19 mean RGB `0 0 0`.
- Every later long arm lost the Vulkan device (`present failed (VkResult -4)`) before a full stage-3/session-13 window
  with the present keys armed, so no menu frame exists yet. The second arm of the round's first batch was invalidated by
  a lead debugger attach; the loss ledger is outside the repo.

### What is still open (next thread)

1. **P-75 first menus rendering** (the round's open frontier): run one 300 s arm on a quiet host with
   `PALMETTO_TITLE_CLASS0A_SIZE=1 PALMETTO_TITLE_SESSION3_COMPLETE=1 PALMETTO_TITLE_PRESENT_KEYS=1` (no OUTRECORD),
   state trace 2 s, full readback + presenter captures + window screenshots. Success = session 13 reached, the SSE4a
   emulation line appears, the stage-3-gated present keys arm, and a non-black title frame is captured. If the SSE4a
   emulation is not the last CPU-feature gap, the next stop is named the same way (illegal instruction -> decode ->
   emulation) and the same arm is repeated.
2. The host must stay quiet for that arm (close GPU-using desktop apps; several recent losses match Chrome/desktop GPU
   activity). The two-consecutive-loss rule is recorded in `D:\leonida-work\fsrstable\r22-loss-ledger.txt`.
3. `PALMETTO_TITLE_OUTRECORD_STATE2` needs its own gated installation (only when the completion dispatch can run) before
   it can be combined with the completion/present instruments.
4. The class-0xa mitigation remains a title-side inference (not console-faithful) and the `PALMETTO_TITLE_CLASS0A_SIZE`
   parser is now exact-`1` only; a clean-host stage-3 A/B is still open.
5. The round-21 items remain: queue-slot-14 layer slice (now mapped in tests), F4/F7 residual coverage, V2 adapter,
   contract-only headers, CI/branch-protection items.

### Where the round-22 evidence lives

`D:\leonida-work\fsrstable\boot-t41-pause` (session 13, DA291AFB), `boot-t41-pause2`, `boot-r22t7-p3-2d2a`,
`boot-r22t7-combined-da541` (device loss), `boot-r22t14-windowshot{,2,3}` (window exists/client black), the loss ledger
`r22-loss-ledger.txt`, window PNGs under `D:\leonida-work\evidence\menus-r22\window*`, frame analysis under
`D:\leonida-work\evidence\menus-r22\frame-analysis`, and the marker `D:\leonida-work\.round22-patch-ready` (binary SHA
history for each retune). Page: `docs/decomp_first_ui.md` section 20; frontier paragraph in `docs/issues.md`.

### Round-3 probe (same day): the media/UI present pass runs once and freezes presentation

A 300 s arm on the bounded-burst binary (`FF7ECF51…`) repeated the stable window (session 13 from 92 s, ALIVE 300.2 s,
0 losses), and the bounded present-key burst made the media/UI body take `body-present-pass` at 92 s, but
**presentation froze** (presents/submits 1,118/2,236 held for ~200 s) and all captures stayed black. The 2D-batch probe
recorded its first **post-stage-3** hits (2 batch-build/2 batch-submit in the `>=3` bucket) but no menu draw. Next
falsifier: wait-trace/stack sample of the body during the present pass (semaphores `0x71B9078`/`0x71B9088`, worker events,
plane resource pointer) plus a bounded target-binding probe at the UI batch submit path; the wall is the media/UI plane
or worker event, not session completion. Evidence: `D:\leonida-work\fsrstable\boot-menus-r23-burst`,
`D:\leonida-work\evidence\menus-r23b`, page section 20.7.
