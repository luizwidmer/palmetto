"""Turn a PALMETTO_XS_CALLERS boot log into a per-slot caller census.

The runtime's caller probe (`PALMETTO_XS_CALLERS=1`) prints one line per console vtable call
site: the object kind, the absolute slot, the caller's return address and the first four argument
words. `PALMETTO_XS_CALLERS_PROFILE=1` adds the per-site call counts. This reads a boot's stderr
log and emits the census as CSV or Markdown, normalising the caller address to an RVA of the title
image (the title is mapped at a fixed base, so the arithmetic is exact) and optionally adding the
containing function's range from the PE exception table (rva2func.Image).

    python -I tooling/ghidra/slot_census.py boot/stderr.log --image <title.exe> --out census.csv
    python -I tooling/ghidra/slot_census.py boot/stderr.log --title-base 0x140000000 --size 0xb06d000 --format markdown

`--title-base` defaults to 0x140000000 and `--size` to 0 when no image is given: a caller below
the base or at or above base+size is reported as external (a shipped DLL or the runtime's own
module) rather than as a title RVA. The boot log itself is not committed; the census rows are
RVAs, slot numbers and argument words only. Standard library only.
"""
import argparse
import csv
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # sibling tools, also under `python -I`
from rva2func import Image

CALLER = re.compile(
    r"d3d12_xs: (list|device|queue|fence|allocator) slot (\d+) caller (0x[0-9a-fA-F]+) \(args ([0-9a-fx ]*)\)")
PROFILE = re.compile(
    r"d3d12_xs:\s+(\d+)\s+(list|device|queue|fence|allocator) slot (\d+) caller (0x[0-9a-fA-F]+)")
FIRST = re.compile(
    r"d3d12_xs: (list|device|queue|fence|allocator) slot (\d+) first call \(args ([0-9a-fx ]*)\)")
KINDS = ("device", "queue", "allocator", "fence", "list")


def read_census(log_path):
    """{(kind, slot, caller): {"args": [...], "calls": n}} from an stderr log."""
    rows = {}
    with io.open(log_path, encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            match = PROFILE.search(line)
            if match:
                key = (match.group(2), int(match.group(3)), int(match.group(4), 16))
                rows.setdefault(key, {"args": [], "calls": 0})
                rows[key]["calls"] = max(rows[key]["calls"], int(match.group(1)))
                continue
            match = CALLER.search(line)
            if match:
                key = (match.group(1), int(match.group(2)), int(match.group(3), 16))
                rows.setdefault(key, {"args": [], "calls": 0})
                if not rows[key]["args"]:
                    rows[key]["args"] = match.group(4).split()
                continue
            match = FIRST.search(line)
            if match:
                key = (match.group(1), int(match.group(2)), None)  # no caller: first-call shape only
                rows.setdefault(key, {"args": match.group(3).split(), "calls": 0})
    return rows


def census_rows(rows, title_base, title_size, image):
    out = []
    for (kind, slot, caller), data in rows.items():
        inside = caller is not None and title_base <= caller < title_base + title_size
        rva = caller - title_base if inside else None
        function = image.containing(rva) if (image is not None and inside) else None
        args = data["args"]
        out.append({
            "kind": kind,
            "slot": slot,
            "caller": "" if caller is None else "0x%x" % caller,
            "caller_rva": "" if rva is None else "0x%x" % rva,
            "function_rva": "" if function is None else "0x%x-0x%x" % (function[0], function[1]),
            "argc": len(args),
            "a1": args[0] if len(args) > 0 else "",
            "a2": args[1] if len(args) > 1 else "",
            "a3": args[2] if len(args) > 2 else "",
            "a4": args[3] if len(args) > 3 else "",
            "calls": data["calls"],
        })
    order = {kind: i for i, kind in enumerate(KINDS)}
    out.sort(key=lambda row: (order.get(row["kind"], len(order)), row["slot"], row["caller_rva"]))
    return out


COLUMNS = ("kind", "slot", "caller", "caller_rva", "function_rva", "argc", "a1", "a2", "a3", "a4", "calls")


def emit_csv(rows):
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=COLUMNS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return buffer.getvalue()


def emit_markdown(rows):
    lines = ["| object | slot | caller RVA | function RVA | calls | args |",
             "|---|---:|---|---|---:|---|"]
    for row in rows:
        args = " ".join(row["a%d" % i] for i in range(1, row["argc"] + 1)) or "(none)"
        caller = row["caller_rva"] or row["caller"] or "(first-call line only)"
        function = row["function_rva"] or ""
        lines.append("| %s | %d | %s | %s | %d | %s |"
                     % (row["kind"], row["slot"], caller, function, row["calls"], args))
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("log", help="a boot stderr log with PALMETTO_XS_CALLERS lines")
    parser.add_argument("--out", help="write to this file instead of stdout")
    parser.add_argument("--format", choices=("csv", "markdown"), default="csv",
                        help="output format (default: csv)")
    parser.add_argument("--image", help="the title PE, to add each caller's function range")
    parser.add_argument("--title-base", default="0x140000000",
                        help="the title image's load base (default: 0x140000000)")
    parser.add_argument("--size", default="0",
                        help="the title image's mapped size; 0 means no image was given (default: 0)")
    args = parser.parse_args(argv)
    image = None
    if args.image:
        try:
            image = Image(args.image)
        except (OSError, ValueError) as error:
            parser.error(str(error))
    rows = census_rows(read_census(args.log), int(args.title_base, 0), int(args.size, 0), image)
    text = emit_markdown(rows) if args.format == "markdown" else emit_csv(rows)
    if args.out:
        with io.open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("slot_census: %d row(s) to %s" % (len(rows), args.out))
    else:
        sys.stdout.write(text)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
