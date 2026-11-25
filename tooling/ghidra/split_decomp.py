"""Split a Ghidra headless log into one decompiled-C file per range.

`tooling/ghidra/DecompileRanges.java` prints one block per range:

    === RVA 0x32520..0x33cb5 ===
    <decompiled C>

This finds those markers, strips the logging decoration analyzeHeadless adds to script output, and
writes `<startRVA>.c` for each block. A run that decompiled 54 functions becomes 54 files a text
tool can search, instead of one large log.

    python -I tooling/ghidra/split_decomp.py <headless.log> --out <dir>
    python -I tooling/ghidra/split_decomp.py <headless.log> --out <dir> --prefix fn_

Reads only the log, so it never needs Ghidra, the title image or the project. Standard library only.
"""
import argparse
import io
import os
import re
import sys

MARKER = re.compile(r"===\s*RVA\s+0x([0-9a-fA-F]+)\.\.0x([0-9a-fA-F]+)\s*===")
# analyzeHeadless prefixes the script's output with the logger's level, the script class and, on
# some versions, a thread tag; strip what precedes the marker on the marker line too.
PREFIXES = ("INFO  ", "WARN  ", "(GhidraScript)  ", " (GhidraScript)")
TAGS = ("(GhidraScript)",)
NOT_A_FUNCTION = ("[decompile failed", "[error at", "[could not create")


def clean(line):
    for prefix in PREFIXES:
        if line.startswith(prefix):
            line = line[len(prefix):]
    stripped = line.rstrip()
    for tag in TAGS:
        if stripped.endswith(tag):
            stripped = stripped[: -len(tag)].rstrip()
    for tag in TAGS:
        if "> " in stripped and stripped.endswith(tag):
            stripped = stripped[: stripped.rfind(tag)].rstrip()
    # "DecompileRanges.java> " (and the equivalent for another script name) is the printf prefix.
    if re.match(r"^[A-Za-z0-9_]+\.java> ", stripped):
        stripped = stripped.split("> ", 1)[1]
    return stripped


def split(log_text, prefix=""):
    """{startRVA hex name: C text} for every marker block in `log_text`."""
    files = {}
    current = None
    body = []
    for raw in log_text.splitlines():
        match = MARKER.search(raw)
        if match:
            if current is not None:
                files[current] = "\n".join(body)
            current = match.group(1).lower()
            body = []
            continue
        if current is None:
            continue
        body.append(clean(raw))
    if current is not None:
        files[current] = "\n".join(body)
    return {prefix + name: text for name, text in files.items()}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("log", help="an analyzeHeadless log containing DecompileRanges output")
    parser.add_argument("--out", required=True, help="directory to write <startRVA>.c files into")
    parser.add_argument("--prefix", default="", help="filename prefix (default: none)")
    parser.add_argument("--quiet", action="store_true", help="print nothing when the log had no blocks")
    args = parser.parse_args(argv)
    try:
        with io.open(args.log, encoding="utf-8", errors="ignore") as handle:
            text = handle.read()
    except OSError as error:
        parser.error("cannot read %s: %s" % (args.log, error))
    files = split(text, args.prefix)
    if not files:
        if not args.quiet:
            print("split_decomp: no '=== RVA ... ===' block in %s" % args.log, file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    for name, content in files.items():
        path = os.path.join(args.out, name + ".c")
        with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content + "\n")
    real = [text for text in files.values()
            if not any(text.lstrip().startswith(marker) or marker in text for marker in NOT_A_FUNCTION)]
    print("split_decomp: wrote %d file(s) to %s (%d with decompiled C)" % (len(files), args.out, len(real)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
