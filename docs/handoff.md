# Pause and resume handoff

This page is the starting point after an indefinite pause. The working tree is clean, no background agent or build is
expected to be running, and the boot lock should be free. Everything below is measured from this checkout, not from
memory: re-check `git status`, `git log -1`, the boot lock and running processes first.

## Current state at the pause

| item | value |
|---|---|
| repository | `D:\leonida` on `main`, no PRs for our own fork |
| paused HEAD | `51dd01de8` (this page's newest commit; `git log -1` is authoritative) |
| last green gate | `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT` |
| latest test evidence | `d3d12_viewport_array` CTest registered and passing (1/1, 1.6 s, validation layer on); loader subset 18/18; new loader-audit and rewrite targets pass |
| docs | all `tests/test_docs_*.py` green (clear inherited `GIT_CONFIG_*` first, below) |
| agents | none running at pause; the 8 GPT-6 Luna + 6 DeepSeek round-18 teammates are inactive |
| boot lock | free; no `palmetto`/`cl`/`ninja`/`ctest` process should remain |

## What was just landed

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

## Handoff to the next thread (round 20, huge push)

The previous thread reached its context limit while preparing a multi-agent push. **No round-20 subagents were launched.**
The working tree is clean and pushed at the commit that adds this section; the plan below is ready to execute as-is.

### Start the next thread with this state

- repo `D:\leonida`, branch `main`, no open PRs of ours; tree clean; boot lock free; no `palmetto`/`cl`/`ninja`/`ctest`
  process.
- last green gates: `PALMETTO_OK`, `TESTS_BUILT`; loader subset 18/18; `d3d12_viewport_array` CTest passing (1/1);
  docs checks 22 + subtests green.
- the live board (GitHub issue #4) and `docs/taskboard.md` already carry the planned rows **P-56..P-62**, P-48/P-50 and the
  opened frame frontier (F-03..F-07 claimable one at a time; F-01/F-02 blocked). Do not re-publish them.
- the team task board is at its **256-task limit**. Delete stale completed tasks with
  `team_task_update action=delete` before creating more. `task-243` and `task-250` were deleted as superseded by the
  landed shadow/base-reservation work.

### Pending tasks created but never spawned

| task | run as | subject | notes |
|---|---|---|---|
| `task-253` | Luna | PE mapper parity, old loader vs `pe_mapper` on synthetic fixtures | phase-1 falsifier; new target `pe_mapper_parity_test` |
| `task-254` | Luna | PE loader V2 shadow wiring + TLS F1 named refusal | **sole owner of `port/src/pe_loader.cpp` this round** |
| `task-255` | Luna | next measured D3D12X replay/memory gap | must not edit `d3d12_com.cpp`/`pe_loader.cpp` |
| `task-256` | Luna | next peer movie/overlay or first-run/UI slice | pure or narrow opt-in wiring, no boot |
| `task-257` | Luna | translator next measured refusal class | GPU-free Python |
| `task-258` | Luna | P-48 dedicated Title-VA GPU CTest (`d3d12_title_va_test`) | report CMake stanza; lead registers |
| `task-259` | DeepSeek | full-suite inventory refresh after loader fixes/new targets | edit `docs/testing.md` only |
| `task-260` | DeepSeek | docs parity/loader refresh for the round | docs only |
| `task-261` | Luna | P-02 first-run screen-state input script | pure/opt-in |
| `task-262` | Luna | D3D12X GPU memory residency/budget slice | pure/opt-in |
| `task-263` | DeepSeek | loader observability census (pure formatter + exact hunk) | does **not** edit `pe_loader.cpp` |
| `task-264` | DeepSeek | TLS audit F4/F7 contract tests + fix proposals | does **not** edit `pe_loader.cpp` |
| `task-265` | DeepSeek | DirectStorage ordered-publication integration behind `PALMETTO_DS_REAP_ORDER=1` | may edit `port/shims/dstorage.cpp` |
| `task-266` | DeepSeek | boot arm series after loader fixes (baseline TLS trace, shadow arm if wired, class-0xa content arm) | owns the boot lock; at most 3 arms, stop after 2 losses |

Suggested split for the push: **8 GPT-6 Luna max + 7 DeepSeek** (the 8-Luna cap keeps under the token-rate limit). Spawn
all tasks in one message with `subagent` (provider `openai`, model `gpt-6-luna`, reasoning `max` for Luna; default model for
DeepSeek), then do the lead work below while they run. A CI/contributor triage task was not created because of the task
limit; create it after deleting a completed task, or ask the maintainer directly.

### Lead work while agents run (definition of done for the push)

1. Enforce the shared-file owners: only `task-254` touches `pe_loader.cpp`; `task-265` owns `dshims/dstorage.cpp`;
   CMakeLists is lead-only (agents report stanzas).
2. Register new targets in `port/CMakeLists.txt` as their sources land (`pe_mapper_parity`, `d3d12_title_va`,
   `pe_loader_shadow_wiring`, `loader_census`, `loader_tls_race`, and any peer slice target), then reconfigure/build.
3. Run `build_gta6rt.cmd` -> `PALMETTO_OK` and `build_tests.cmd` -> `TESTS_BUILT`; run the loader subset plus the new
   CTest targets; run all docs checks with `GIT_CONFIG_*` cleared.
4. Commit directly on `main` in coherent slices (loader fixes/rewrite, peer slices, docs), push, update issue #4 rows and
   post one summary comment; finally refresh this handoff page and the board mirror.
5. Only run a boot arm after the binary is fixed and the boot lock is free; stop after two consecutive device losses.

### Constraints to carry over

- no game data or package bytes in the repo, logs, tests or task text; no keys/decryption; peer clones are read-only
  evidence and every mechanism gets a NOTICE/docs credit.
- opt-in until measured: default path byte-identical, named refusals, synthetic tests and a boot/GPU falsifier where
  applicable.
- one build at a time against `port\build`; never relink `palmetto` while an arm holds the exe; `LNK1168` means an arm
  still holds it.
- `d3d12_draw` remains the known-flaky shared GPU fixture; use `d3d12_viewport_array` for the viewport case.
- reports/evidence under `D:\leonida-work` and `reports/` stay outside the repo.
