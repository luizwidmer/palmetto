# Ghidra console-vtable workflow

These scripts answer one question cheaply: *what does the function behind this measured
console-vtable call site do?* The workflow was built while mapping the Xbox D3D12 console
interfaces (`docs/console_slots.md`), but nothing in it is specific to one build: the image,
project, Ghidra install and output directory are arguments, and every printed value is an RVA.

Ghidra is **not** redistributed here. Install Ghidra 11 or newer (the workflow was run with
12.1.4 PUBLIC) and a JDK 17+ (21 was used), then point the environment at them:

```powershell
$env:PALMETTO_GHIDRA_HOME = "<ghidra-install>"   # or GHIDRA_INSTALL_DIR
$env:JAVA_HOME           = "<jdk>"
```

## What each file is

| File | What it does |
|---|---|
| `rva2func.py` | maps an image RVA to its containing function using the PE exception directory (`.pdata`) - no analysis pass needed |
| `slot_census.py` | reads a boot stderr log with `PALMETTO_XS_CALLERS` lines into a per-slot caller census (CSV or Markdown: object, slot, caller RVA, function range, argument words, call count) |
| `decompile_callers.py` | the one command: census (or RVAs) -> function ranges -> analyzeHeadless import/decompile -> one `.c` file per function |
| `DecompileRanges.java` | the Ghidra headless script the wrapper runs: disassemble a range, create its function, print its decompiled C between `=== RVA ... ===` markers |
| `split_decomp.py` | splits one headless log into one `.c` file per range |

## The one command

```powershell
python -I tooling/ghidra/slot_census.py "<boot>/stderr.log" --image "<title>.exe" `
    --title-base 0x140000000 --size 0xb06d000 --out census.csv

python -I tooling/ghidra/decompile_callers.py --census census.csv --image "<title>.exe" `
    --ghidra "$env:PALMETTO_GHIDRA_HOME" --project "<project-dir>" --out decompiled/
```

The first run imports the title with `-noanalysis` (a couple of minutes for a large image) and
saves the project; every later run opens that program, so a slot decompile is seconds. Use
`--import` to re-import after the image changed and `--dry-run` to print the exact analyzeHeadless
command line without running it. `--rva 0x...,0x...` works instead of `--census` when the call
sites are already known.

The wrapper prints the analyzeHeadless log to `<out>/decompile.log`, so a later question can be
answered by grepping the log or the `.c` files, and `split_decomp.py` alone can re-split an old
log.

## Reading a decompiled call

The title is compiled for the console ABI, so a vtable call in the decompiled C is the slot the
census reported:

```c
(**(code **)(*list + 0x178))(list, 0);   // slot 0x178/8 = 47, one NULL argument
```

`tooling/d3d12_slot_table.py` prints the SDK method at each PC slot, so `slot 47` can be compared with
the PC order. A console call may pass its `this` in a different register than the PC method, and a
console-only slot has no PC method at all - that is what the census's "port handling" column in
`docs/console_slots.md` records.

## Limits

* The exception table lists functions the compiler emitted unwind info for; a leaf function that
  has none is reported as "no function in the exception table" and skipped.
* `-noanalysis` means no function starts are discovered by Ghidra; they come from `.pdata`. A
  range whose start is not a real function start decompiles garbage - check the census's
  `function_rva` column first.
* Decompiler output is C-like, not source: unknown vtable signatures lose arguments, so the
  census's measured argument words (not the decompiled call's argument list) are the shape.
* A whole-image analysis is out of scope; use Ghidra's own headless analysis if a question needs
  cross-references across the image.
