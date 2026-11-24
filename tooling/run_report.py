"""Summarise one boot run from the files boot_report.py already leaves behind.

    python -I tooling/run_report.py <boot-dir> [--json OUT] [--top N]

Reads <boot-dir>/stderr.log (the runtime's own lines and the d3d12 trace) and <boot-dir>/boot.jsonl (the structured
events), and prints one screen that answers the questions every debugging round starts with:

    where did it stop, how far did it get, what did the title ask for that we refused, what did we stand in for,
    which imports are unimplemented, which asserts were skipped, and which slots were hit most.

Only counts, slot numbers, error classes and the project's own wording are printed. The input logs can quote the game's
strings; nothing from them is copied into the report except the module+RVA the runtime itself printed, so a report can be
pasted into an issue without carrying game data.

Exit status: 0 when a report was produced (whatever the boot did), 2 when the directory has no log to read.
"""
import argparse
import collections
import json
import os
import re
import sys

CALL_RE = re.compile(r"^\[d3d12-trace\] #(\d+) (.+)$")
FIRST_CALL_RE = re.compile(r"^\[rt\] d3d12_xs: (device|queue|fence|allocator|list|heap) slot (\d+)(?: \(([^)]*)\))? first call")
REFUSED_RE = re.compile(r"^\[rt\] d3d12_xs: .*slot (\d+) is beyond the PC .* returning E_NOTIMPL")
IMPORT_RE = re.compile(r"^\[rt\] (?:shim|import|unimplemented)[^:]*: ([^!\s]+!\S+)")
STANDIN_RE = re.compile(r"STAND-IN|stand-in")
STOP_RE = re.compile(r"^VERDICT: STOP (\S+)")
SHADERS_RE = re.compile(r"^\[rt\] d3d12: (\d+) translated shaders")
NO_SHADERS_RE = re.compile(r"^\[rt\] d3d12: (cannot open .*index\.json|no translated shaders.*)")


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read().splitlines()
    except FileNotFoundError:
        return []


def summarise(boot_dir):
    # boot_report.py prints its VERDICT line on stdout and the runtime logs on stderr; read both.
    stderr = read_lines(os.path.join(boot_dir, "stdout.log")) + read_lines(os.path.join(boot_dir, "stderr.log"))
    events = []
    jsonl = os.path.join(boot_dir, "boot.jsonl")
    if os.path.isfile(jsonl):
        with open(jsonl, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    calls = 0
    last_call = ""
    per_slot = collections.Counter()
    first_calls = collections.OrderedDict()
    refused_beyond = collections.Counter()
    standins = 0
    unimplemented = collections.Counter()
    shader_library = None
    shader_problem = None
    stop = None
    other = []

    for line in stderr:
        match = CALL_RE.match(line)
        if match:
            calls += 1
            last_call = match.group(2)
            per_slot[match.group(2).split(" ")[0]] += 1
            continue
        match = STOP_RE.match(line)
        if match:
            stop = match.group(1)
            continue
        match = FIRST_CALL_RE.match(line)
        if match and match.group(1) not in first_calls:
            first_calls[match.group(1)] = (match.group(2), match.group(3) or "past the public chain")
            continue
        match = REFUSED_RE.match(line)
        if match:
            refused_beyond[match.group(1)] += 1
            continue
        if STANDIN_RE.search(line):
            standins += 1
            continue
        match = IMPORT_RE.match(line)
        if match:
            unimplemented[match.group(1)] += 1
            continue
        match = SHADERS_RE.match(line)
        if match:
            shader_library = int(match.group(1))
            continue
        match = NO_SHADERS_RE.match(line)
        if match:
            shader_problem = match.group(1)
            continue
        if "failed with" in line or "refused" in line or "unsupported" in line:
            other.append(line)

    exceptions = [e for e in events if e.get("kind") == "exception"]
    skipped = [e for e in events if e.get("kind") == "int3_skipped"]
    unimplemented_events = [e for e in events if e.get("kind") == "unimplemented_call"]

    # boot_report.py keeps its verdict in summary.txt: the exit code, the wall time and whether the timeout hit.
    exit_code = wall_time = None
    timed_out = None
    for line in read_lines(os.path.join(boot_dir, "summary.txt")):
        if line.startswith("exit code:"):
            exit_code = line.split(":", 1)[1].strip()
        elif line.startswith("wall time:"):
            wall_time = line.split(":", 1)[1].strip()
        elif line.startswith("timeout:"):
            timed_out = line.split(":", 1)[1].strip()
    if stop is None:
        if exceptions:
            raw_code = str(exceptions[-1].get("code") or "0")
            try:
                code_int = int(raw_code, 16) if raw_code.lower().startswith("0x") else int(raw_code)
            except ValueError:
                code_int = 0
            stop = f"{exceptions[-1].get('at')} (0x{code_int:08x})"
        elif exit_code is not None:
            stop = f"no exception; process exit {exit_code}"

    return {
        "boot_dir": os.path.basename(boot_dir.rstrip("\\/")),
        "exit_code": exit_code,
        "wall_time": wall_time,
        "timed_out": timed_out,
        "stop": stop,
        "d3d12_calls": calls,
        "last_traced_call": last_call,
        "shader_library_entries": shader_library,
        "shader_problem": shader_problem,
        "standins": standins,
        "refusals_and_failures": len(other),
        "first_calls": dict(first_calls),
        "refused_beyond_table": dict(refused_beyond),
        "unimplemented_imports": dict(unimplemented),
        "unimplemented_events": len(unimplemented_events),
        "int3_skipped": [e.get("at") for e in skipped],
        "exceptions": [
            {"at": e.get("at"), "code": e.get("code"), "access": e.get("access"), "address": e.get("address")}
            for e in exceptions[:6]
        ],
        "slot_hits": dict(per_slot.most_common(12)),
        "failure_lines": other[-8:],
    }


def render(summary):
    out = []
    out.append(f"# boot {summary['boot_dir']}")
    out.append("")
    out.append(f"- stop: **{summary['stop'] or 'no verdict recorded'}**")
    out.append(f"- D3D12 calls: **{summary['d3d12_calls']}** (last traced: {summary['last_traced_call'] or '-'})")
    timing = ", ".join(x for x in (summary.get("wall_time"), summary.get("exit_code")) if x)
    if timing:
        out.append(f"- run: {timing}" + (f" (timeout: {summary['timed_out']})" if summary.get("timed_out") else ""))
    if summary["shader_library_entries"] is not None:
        out.append(f"- translated shaders loaded: {summary['shader_library_entries']}")
    if summary["shader_problem"]:
        out.append(f"- **shader library problem: {summary['shader_problem']}**")
    out.append(f"- refusals/failures logged: {summary['refusals_and_failures']}, stand-ins: {summary['standins']}")
    out.append(f"- unimplemented calls (events): {summary['unimplemented_events']}")
    if summary["exceptions"]:
        out.append("")
        out.append("## exceptions")
        for e in summary["exceptions"]:
            out.append(f"- {e['at']} code {e['code']} {e.get('access') or ''} {e.get('address') or ''}".rstrip())
    if summary["int3_skipped"]:
        out.append("")
        out.append("## int3 stepped over (soft asserts)")
        for at in summary["int3_skipped"][:8]:
            out.append(f"- {at}")
    shown = 0
    for kind, (slot, name) in summary["first_calls"].items():
        if shown == 0:
            out.append("")
            out.append("## slots the title reached for the first time")
        shown += 1
        out.append(f"- {kind} slot {slot}: {name}")
    if summary["slot_hits"]:
        out.append("")
        out.append("## most-hit traced calls")
        for name, count in summary["slot_hits"].items():
            out.append(f"- {name}: {count}")
    if summary["refused_beyond_table"]:
        out.append("")
        out.append("## refusals past a proxy table")
        for slot, count in sorted(summary["refused_beyond_table"].items(), key=lambda kv: -kv[1]):
            out.append(f"- slot {slot}: {count}")
    if summary["failure_lines"]:
        out.append("")
        out.append("## last refusal/failure lines")
        for line in summary["failure_lines"]:
            out.append(f"- {line[:200]}")
    out.append("")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description="Summarise one boot run from its logs.")
    parser.add_argument("boot_dir", help="a palmetto-boot-* directory")
    parser.add_argument("--json", help="also write the summary as JSON to this path")
    parser.add_argument("--top", type=int, default=12, help="how many most-hit calls to list")
    args = parser.parse_args()

    if not os.path.isdir(args.boot_dir):
        print(f"not a directory: {args.boot_dir}", file=sys.stderr)
        return 2
    summary = summarise(args.boot_dir)
    if summary["d3d12_calls"] == 0 and summary["stop"] is None and not summary["exceptions"]:
        print(f"no boot log found in {args.boot_dir}", file=sys.stderr)
        return 2
    print(render(summary))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=2)
        print(f"(json written to {args.json})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
