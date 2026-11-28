# Pause and resume handoff

This page is the starting point after an indefinite pause. The working tree is clean, no background agent or build is
expected to be running, and the boot lock should be free. Everything below is measured from this checkout, not from
memory: re-check `git status`, `git log -1`, the boot lock and running processes first.

## Current state (round-20 landing)

| item | value |
|---|---|
| repository | `D:\leonida` on `main`, no PRs for our own fork |
| round-20 landing | code `d95a159ce`, docs `58091833d`; this handoff update is the next commit (the tree is clean and pushed) |
| last green gate | `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` |
| latest test evidence | round-20 CTest subset **29/29 passed** (10.8 s), including the new `pe_mapper_parity`, `pe_loader_shadow_wiring`, `loader_census`, `loader_tls_race`, `ds_publication`, `query_heap_readiness`, `d3d12_residency_budget`, `ui_input_script`, `d3d12_title_va`; `d3d12_viewport_array` still 1/1; loader subset 29/29 |
| docs | all `tests/test_docs_*.py` green, 22 tests (clear inherited `GIT_CONFIG_*` first, below) |
| agents | none running; the 8 GPT-6 Luna (max effort) + 6 DeepSeek round-20 teammates finished, and the lead committed their slices |
| boot lock | free; no `palmetto`/`cl`/`ninja`/`ctest` process should remain |

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

## Open work, in rough priority order

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

## Round 20 landed (2026-11-27)

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
