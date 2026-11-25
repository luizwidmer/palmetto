# Development tools

From the Palmetto checkout root, use Python 3 to list the development tools:

```powershell
python -I tooling/scripts/dev.py --help
python -I tooling/scripts/dev.py discover
python -I tooling/scripts/dev.py discover --json
```

From another directory, pass the path to `tooling/scripts/dev.py`, quoting it if needed.
The inventory always describes the checkout containing that script.

`boot-trace` points to the analyzer for startup JSONL and D3D12 stderr logs.
`guest-scene` points to the headless synthetic guest runner. Run a listed help
command from the checkout root to see the producer's options. The text
inventory displays commands for PowerShell on Windows and POSIX shells
on other platforms. The inventory
checks for each producer script and lists its prerequisites without checking
those prerequisites or running the producer. Live attach, pause, step, input,
and screenshots are unavailable through this entry point.

## JSON output

`discover --json` writes one object to stdout:

- `schema_version` identifies the format. The current version is `1`.
- `tools` lists entries by `id`. Each `path` uses forward slashes and is
  relative to the checkout root. `present` indicates whether the script is
  a file. `help_argv` is a list of arguments for a command run from the checkout
  root. Its first argument is the Python executable running discovery. It
  is `null` when the script is missing. `requirements` lists prerequisites
  for using the producer.
- `live_controls` contains an `available` boolean and a `reason`.

Exit code `0` indicates that the inventory was emitted, including when a
producer script is missing. Invalid CLI arguments exit with code `2`.

## Analyze a boot trace

Use the existing analyzer through the development CLI:

```powershell
python -I tooling/scripts/dev.py boot-trace --help
python -I tooling/scripts/dev.py boot-trace "logs/startup trace.jsonl"
python -I tooling/scripts/dev.py boot-trace "logs/startup trace.jsonl" --d3d12 "logs/d3d12 stderr.log" --json
```

Pass a startup JSONL from `palmetto --trace`, a D3D12 stderr log from
`palmetto --trace-d3d12`, or both. Relative input paths are interpreted from
the directory where you run the CLI. From outside the checkout, give the
path to `tooling/scripts/dev.py` and keep the trace paths relative to your directory.

The analyzer supplies the help, output, diagnostics and exit status. If one
input fails but the other can be read, `--json` can still emit a partial
summary on stdout with a diagnostic on stderr and exit code `2`. If neither
input can be analyzed, there is no JSON summary. The summary describes the
recorded events, and the exit status reports the analysis result.

You can also run `python -I tooling/boot_trace.py` with the same arguments.

## Run a synthetic guest scene

Use the existing headless runner with a loader and guest built on Windows
with Vulkan, static SDL3 and `PALMETTO_GUEST_SCENE=ON`. Supply the synthetic
M1.5 shader fixture root containing `spirv/`. See the
[runtime instructions](../port/README.md#a-600-frame-headless-guest-scene)
for the build, fixture and Wine setup.

```powershell
python -I tooling/scripts/dev.py guest-scene --help
python -I tooling/scripts/dev.py guest-scene --loader "build/palmetto.exe" --guest "build/tiny_guest_draw.exe" --shaders "m15 shaders" --out "scene run" --negative-controls
```

Pass `--out PATH` or `--out=PATH` with a new or empty directory. Relative
paths are interpreted from the directory where you run the CLI, including
when you invoke `tooling/scripts/dev.py` by its full path from another directory.

The CLI forwards the runner's exit code and diagnostics. Exit code `0`
means the scene checks passed. Exit code `1` can indicate a scene check
failure or a failure before the runner can write a report, such as an
output directory it cannot create. Exit code `2` indicates a usage or
prerequisite error.

When the runner writes `report.json`, it prints the path. Open that file
for the scene result and per-run details. For example, if the full run
starts and the loader exits with an error, `runs.full.returncode` records
the loader's exit code while the CLI returns the runner's exit code.
The output directory also holds any logs, frame records and PPM images
the runner produced. If the runner rejects an occupied directory, any
report already there belongs to an earlier run.

You can also invoke `python -I tooling/scripts/run_guest_scene.py` directly.

## Console vtable slot maps with Ghidra

The console D3D12 objects are not in the Windows SDK order, so a slot number in a trace does not
name a method by itself. The map of what every console object's slots are is
[console_slots.md](console_slots.md); this section is how to extend it. The tools are in
`tooling/ghidra/`, and their own page is `tooling/ghidra/README.md`.

Ghidra and a JDK are not bundled. Install Ghidra 11 or newer (12.1.4 PUBLIC was used together with
JDK 21) and point the environment at them; the scripts read the paths from arguments or from
these variables:

```powershell
$env:PALMETTO_GHIDRA_HOME = "<ghidra-install>"   # or GHIDRA_INSTALL_DIR
$env:JAVA_HOME           = "<jdk>"
```

The workflow starts from a boot with the runtime's caller probe on
(`PALMETTO_XS_CALLERS=1`, `PALMETTO_XS_CALLERS_PROFILE=1`). Its stderr log becomes a per-slot
census of the console calls: the object, the absolute slot, the caller's return address (a title
RVA; the port maps the title at 0x140000000), the containing function's range from the PE
exception directory, the first four argument words, and the per-site call count.

```powershell
python -I tooling/ghidra/slot_census.py "<boot>/stderr.log" --image "<title>.exe" --title-base 0x140000000 --size 0xb06d000 --out census.csv
```

One command then decompiles the functions behind all those call sites. The first run imports the
title into a Ghidra project with `-noanalysis` and takes a couple of minutes for a large image; it
saves the project, so every later run needs seconds to decompile more ranges.

```powershell
python -I tooling/ghidra/decompile_callers.py --census census.csv --image "<title>.exe" --ghidra "$env:PALMETTO_GHIDRA_HOME" --project "<project-dir>" --out decompiled/
```

The output directory gets the analyzeHeadless log plus one `.c` file per decompiled function. A
console call appears in that C as an indirect call through the object's vtable, so the slot is
immediately visible; the census's measured argument words, not the decompiled call's argument
list, are the argument shape (the decompiler does not know the console method signatures).

The individual pieces can be used on their own: `rva2func.py` maps any RVA to its containing
function from the PE exception table, `split_decomp.py` re-splits an old headless log, and
`DecompileRanges.java` is the Ghidra script the wrapper runs. `--rva` takes call sites directly
when they are already known, and `--dry-run` prints the exact analyzeHeadless command without
running it.

The PC method at a slot is in the build's SDK header, including inherited bases, with:

```powershell
python -I tooling/d3d12_slot_table.py --header "<sdk>/um/d3d12.h" --interface ID3D12GraphicsCommandList
```

That prints absolute slots, so a console slot and the PC slot at the same index can be compared
side by side. `--selftest` checks the parser on a synthetic header, `--list` lists the interfaces
the header declares and `--json` prints the same table for a script.

Limits: the exception table lists the functions the compiler emitted unwind info for, so a leaf
function with none is skipped; a range must start at a real function start or it decompiles
garbage; and a whole-image analysis is deliberately out of scope. The census records the run, not
every possible run: a slot absent from it was simply not called, and a row marked unmodelled in
[console_slots.md](console_slots.md) is an observation, not an identification.
