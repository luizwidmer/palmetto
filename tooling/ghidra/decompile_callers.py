"""Decompile the title functions behind measured console-vtable call sites, in one command.

This is the wrapper that makes the next slot question cheap. It composes the three pieces:

1. `slot_census.py` output (or an explicit RVA list) gives the caller return addresses measured
   during a boot;
2. `rva2func.py` maps each to the function that contains it, using the PE's exception table - no
   analysis pass needed;
3. one analyzeHeadless run imports the title (once, with -noanalysis) and decompiles exactly
   those functions, and `split_decomp.py` writes one `.c` file per function.

    python -I tooling/ghidra/decompile_callers.py --census census.csv --image <title.exe> \
        --ghidra <ghidra-install> --project <project-dir> --out decompiled/
    python -I tooling/ghidra/decompile_callers.py --rva 0x325c0,0x328be --image <title.exe> \
        --ghidra <ghidra-install> --project <project-dir> --out decompiled/ --dry-run

The Ghidra install can come from `PALMETTO_GHIDRA_HOME` or `GHIDRA_INSTALL_DIR`; a JDK from
`JAVA_HOME` (Ghidra 11+ needs JDK 17 or newer). The first run imports the title into the project
directory and asks for no analysis, so it costs a couple of minutes; later runs open the saved
program, so every further slot decompile is seconds. Nothing here is specific to one game build:
the image, project and output paths are arguments and every printed value is an RVA.

Standard library only. Ghidra itself is not redistributed; see docs/dev-tools.md.
"""
import argparse
import csv
import io
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # sibling tools, also under `python -I`
from rva2func import Image
from split_decomp import split


def find_analyze_headless(ghidra):
    name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
    path = os.path.join(ghidra, "support", name)
    return path if os.path.isfile(path) else None


def find_java(java_home):
    if not java_home:
        return None
    name = "java.exe" if os.name == "nt" else "java"
    path = os.path.join(java_home, "bin", name)
    return path if os.path.isfile(path) else None


def project_exists(project_dir, project_name):
    return os.path.isfile(os.path.join(project_dir, project_name + ".gpr"))


def read_rvas(census_path):
    """Caller RVAs from a slot_census.py CSV: one integer per row (empty ones skipped)."""
    out = []
    with io.open(census_path, encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            text = (row.get("caller_rva") or "").strip()
            if text:
                out.append(int(text, 16))
    return out


def unique_functions(image, rvas):
    """[(begin, end)] of the containing functions, deduplicated and sorted."""
    seen = {}
    for rva in rvas:
        found = image.containing(rva)
        if found:
            seen[found[0]] = found[1]
        else:
            print("decompile_callers: 0x%x has no function in the exception table; skipped" % rva,
                  file=sys.stderr)
    return sorted(seen.items())


def run(command, log_path, what):
    """Run a Ghidra command, appending combined output to `log_path`. Returns True on exit 0."""
    with io.open(log_path, "a", encoding="utf-8", errors="replace") as log:
        log.write("\n### %s\n$ %s\n" % (what, " ".join(command)))
        log.flush()
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, shell=False)
        except OSError as error:
            log.write("[could not start: %s]\n" % error)
            print("decompile_callers: could not start analyzeHeadless: %s" % error, file=sys.stderr)
            return False
    return result.returncode == 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--census", help="a slot_census.py CSV (its caller_rva column is used)")
    source.add_argument("--rva", help="comma-separated caller RVAs, hex, 0x optional")
    parser.add_argument("--image", required=True, help="the title PE")
    parser.add_argument("--ghidra", default=os.environ.get("PALMETTO_GHIDRA_HOME")
                        or os.environ.get("GHIDRA_INSTALL_DIR"),
                        help="Ghidra install directory (default: $PALMETTO_GHIDRA_HOME or $GHIDRA_INSTALL_DIR)")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"),
                        help="JDK directory to run Ghidra with (default: $JAVA_HOME)")
    parser.add_argument("--project", default=os.environ.get("PALMETTO_GHIDRA_PROJECT"),
                        help="Ghidra project directory (default: $PALMETTO_GHIDRA_PROJECT)")
    parser.add_argument("--project-name", default="console-slots", help="Ghidra project name")
    parser.add_argument("--out", required=True, help="output directory for the log and .c files")
    parser.add_argument("--import", dest="do_import", action="store_true",
                        help="import the image even when the project already exists")
    parser.add_argument("--read-only", action="store_true", default=True,
                        help="open the program read-only (default)")
    parser.add_argument("--dry-run", action="store_true", help="print the commands and ranges only")
    args = parser.parse_args(argv)
    if not args.ghidra:
        parser.error("--ghidra is required (or set PALMETTO_GHIDRA_HOME / GHIDRA_INSTALL_DIR)")
    if not args.project:
        parser.error("--project is required (or set PALMETTO_GHIDRA_PROJECT)")
    analyze = find_analyze_headless(args.ghidra)
    if not analyze:
        parser.error("no analyzeHeadless under %s/support (is --ghidra a Ghidra install?)" % args.ghidra)
    try:
        image = Image(args.image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.census:
        rvas = read_rvas(args.census)
    else:
        try:
            rvas = [int(part, 16) for part in args.rva.split(",") if part.strip()]
        except ValueError:
            parser.error("--rva must be a comma-separated list of hex RVAs")
    functions = unique_functions(image, rvas)
    if not functions:
        parser.error("no function range for any of the %d RVA(s)" % len(rvas))
    ranges = ",".join("%x-%x" % (begin, end) for begin, end in functions)
    os.makedirs(args.out, exist_ok=True)
    log_path = os.path.join(args.out, "decompile.log")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    program = os.path.basename(args.image)
    environment = dict(os.environ)
    java = find_java(args.java_home)
    if java:
        environment["JAVA_HOME"] = args.java_home
    if os.name == "nt":
        environment.setdefault("GHIDRA_INSTALL_DIR", args.ghidra)

    print("decompile_callers: %d caller RVA(s) -> %d function(s), %d byte(s) of code"
          % (len(rvas), len(functions), sum(end - begin for begin, end in functions)))
    import_cmd = [analyze, args.project, args.project_name, "-import", args.image, "-noanalysis"]
    process_cmd = [analyze, args.project, args.project_name, "-process", program, "-noanalysis"]
    if args.read_only:
        process_cmd.append("-readOnly")
    process_cmd += ["-scriptPath", script_dir, "-postScript", "DecompileRanges.java", ranges]
    need_import = args.do_import or not project_exists(args.project, args.project_name)
    if args.dry_run:
        if need_import:
            print("would import: " + " ".join(import_cmd))
        print("would decompile: " + " ".join(process_cmd))
        return 0
    if need_import:
        print("decompile_callers: importing %s into %s (no analysis; the first run costs a few minutes)"
              % (program, args.project))
        if not run(import_cmd, log_path, "import"):
            parser.error("the import failed; see %s" % log_path)
    print("decompile_callers: decompiling %d function(s) ..." % len(functions))
    if not run(process_cmd, log_path, "decompile"):
        parser.error("the decompile run failed; see %s" % log_path)
    with io.open(log_path, encoding="utf-8", errors="ignore") as handle:
        files = split(handle.read())
    if not files:
        parser.error("the log has no decompiled blocks; see %s" % log_path)
    for name, content in files.items():
        with io.open(os.path.join(args.out, name + ".c"), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content + "\n")
    print("decompile_callers: %d file(s) in %s (log: %s)" % (len(files), args.out, log_path))
    print("decompile_callers: grep the .c files for the call you measured; a console slot is "
          "`[object + slot*8]` in the decompiled call, or the slot number in the file's name.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
