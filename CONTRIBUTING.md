# Contributing

Thanks for helping. A few rules keep the project safe to host:

- **No game data, ever.** That covers archives, the executable, carved files, shaders (DXBC, FSSM, disassembly,
  SPIR-V), textures, models, audio, videos, and reports or logs that quote them. `.gitignore` blocks the common
  types; check `git status` before you commit anyway.
- **No keys, decryption or protection workarounds**, and no links to where to get them. The tools only read files
  that are already plain on disk.
- Short excerpts needed to explain a format (a few header bytes, one instruction sequence) are fine in docs and
  comments.

## Before you start

Work is claimed on the shared task board: the pinned coordination issue
("Coordinating who works on what"), whose rules and a snapshot of the backlog live in
[docs/taskboard.md](docs/taskboard.md). Read the board and the open pull requests first, then post one claim comment:

```
claiming: <ID> - <one line on the approach and the files you expect to touch>
```

Only `P-`, `C-` and `D-` rows marked `open` are claimable; `F-` rows and rows marked `maintainer` are the maintainer's
lane, so ask in the issue before starting one. If the work has no row yet, post `finding: <one line>` with where it is and
why it matters and the maintainer adds it. Name the task in the pull request body (`Task: <ID>`), keep one task per pull
request, and when it merges post `done: <ID> -> #<PR>` - or `release: <ID>` the same day if you stop. When two pull requests
overlap anyway, the one that lands first is merged and the other is asked to rebase or close. GPU test results go in the pull
request that changes them. Work on the D3D12 -> Vulkan layer is planned and tracked in
[docs/d3d12_vulkan.md](docs/d3d12_vulkan.md): the plan, what is measured, and which D3D12 methods are still open.

[claims]: https://github.com/luizwidmer/palmetto/issues?q=is%3Aissue+is%3Aopen+in%3Atitle+%22Coordinating+who+works+on+what%22

## Working on the code

- Python tools run with `python -I` and use only the standard library unless a file says otherwise.
- A text file a tool writes (JSON, CSV, a manifest, an assembly listing) names its line ending: `open(path, "w",
  encoding="utf-8", newline="\n")` (the same for `Path.open`, `Path.write_text` and a text-mode
  `tempfile.NamedTemporaryFile`), so the output is LF on Windows too and the same input gives the same bytes on every
  platform. `tests/test_text_write_line_endings.py` fails for a writer that forgets it.
- A report writer refuses an output that names one of its inputs or another output, and asks
  `tooling/report_paths.same_report_path(a, b)` rather than deriving the answer again: it resolves both paths,
  applies the volume's case rules and falls back to `os.path.samefile`, so a new tool reuses one answer
  instead of a private copy. `tests/test_report_paths.py` covers it.
- `tooling/arc9dump` needs the submodule: `git submodule update --init`.
- A new shader-translator rule goes into the contract table in `docs/design.md`, with how it was confirmed: a GPU
  test, a Mesa/LLVM reference, or a count over the corpus.
- `python -I tests/test_<name>.py` runs one of the unit tests (standard library only; CI runs them all). The test
  files with no `__main__` block or an `import pytest` (they need the packages in `tests/requirements.txt`) run with
  `python -m pytest tests/test_<name>.py` instead, as [AGENTS.md](AGENTS.md) describes; under `python -I` they fail
  to import or run nothing.
- Runtime tests: `cmake -S port -B port/build`, `cmake --build port/build --config RelWithDebInfo`, then
  `ctest --test-dir port/build -C RelWithDebInfo --output-on-failure`. The CPU tests that need no game data are configured
  on Windows; Linux and macOS configure the portable subset (`palmetto` and the shims are Windows-only). Tests that need the retail package read `PALMETTO_GAME_DIR` (and `PALMETTO_MOVIE` for the Bink
  decode test) in place and print `SKIP` when the variable is unset; never copy or commit those files.
- Viewer streaming tests: `node --test tests/test_viewer_streaming.cjs` (Node.js 22+, CI tests 24 LTS; no packages,
  browser, network or game data). These execute the viewer streaming code with a synthetic loader and clock.
- `loader_test` writes small PE images to a `palmetto_loader_test` folder in `%TEMP%`. Antivirus that blocks newly
  written executables makes it stop with that reason; exclude the folder or set `PALMETTO_TEST_DIR` to one it skips.
- Tool checks: `python -m compileall -q build_port.py tooling tests`. The optional
  `vk_pipeline_check <spirv dir> [cache.bin] [--jobs N]` creates a module (and, for compute shaders, a
  pipeline) for every translated shader and reports failures plus the saved cache path.
- GPU tests are small hand-written RDNA2 programs in `port/test/shaders`. `build.py` turns them into SPIR-V
  using `llvm-mc` with the AMDGPU target (LLVM 21+). `check.py` runs them on your GPU and compares the results.
  A new test gets its expected values in the comment at its top and a check in `check.py`.
  [docs/testing.md](docs/testing.md) covers setup per OS, including native Windows options, and known problems.
- Say what you tested and on which GPU in the pull request.
- Run the translator tests with `python -I tooling/scripts/run_semantic_tests.py tests/semantic` (see `tests/semantic/README.md`). A new
  translator rule should come with a small hand-written `.asm` fixture and its expected output words.
