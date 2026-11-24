#!/usr/bin/env python3
"""Run one `palmetto` boot attempt and print a compact, comparable summary.

Usage:
    python -I tools/boot_report.py --exe <palmetto.exe> --image <game exe>
        --game-dir <package> --overlay <dir> [--timeout S] [--trace <jsonl>]
        [--out <dir>] [--env NAME=VALUE ...]

The harness starts the runtime itself (`palmetto --run <image> --game-dir ... --overlay ... --trace-d3d12
--trace <jsonl>`), captures its stdout and stderr to files under `--out` (default
`%TEMP%\\palmetto-boot-<timestamp>`), and prints:

  * the exit code, the wall time, and whether the timeout had to kill the process;
  * the number of `[d3d12-trace] #N` lines the runtime wrote to stderr and the largest N;
  * the last five distinct `[rt]` diagnostic lines, with filesystem paths, shader hashes,
    pointer-shaped values and data-dump payloads redacted;
  * the first `exception` event of the JSONL startup trace: kind, code, `at`, `access`, `address`;
  * a one-line VERDICT: `STOP <at>` when an exception was recorded, else `ALIVE <seconds>`.

The same text is written to `<out>/summary.txt`. The exit code is 0 whenever the runtime was launched and
parsed, even when the game itself crashed; a non-zero exit means the harness could not run (bad arguments,
a missing path, an existing `--trace` file, or a capture directory inside the repository).

The harness inherits the environment; `--env NAME=VALUE` overrides one variable per use. Set
`PALMETTO_D3D12=vulkan` (and `PALMETTO_SHADERS`) to select the backend that writes the `[d3d12-trace]` lines,
or pass them with `--env`. The summary only aggregates lines the runtime itself prints: no game file,
executable bytes, package address, shader data or extracted asset is read or emitted. The captured logs
and the trace stay under `--out` and are never written into the repository.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RT_TAIL = 5
RT_WIDTH = 200

D3D12_LINE = re.compile(r"^\[d3d12-trace\] #(\d+) ")
RT_LINE = re.compile(r"^\[rt\] ")
HASH = re.compile(r"\b[0-9A-Fa-f]{32,}\b")
ADDR = re.compile(r"0x[0-9A-Fa-f]{10,}")
PTR = re.compile(r"\b[0-9A-Fa-f]{16}\b")
PARAMS = re.compile(r"(params\[24\]\s*=).*$")
DIALOG = re.compile(r"(game dialog:).*$")


class HarnessError(RuntimeError):
    """The harness could not run or parse the attempt (not a game crash)."""


def _decode(data: bytes) -> str:
    """Decode captured bytes: UTF-8 normally, UTF-16 when a byte-order mark says so."""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16", "replace")
    return data.decode("utf-8-sig", "replace")


def _path_pattern(paths) -> re.Pattern[str] | None:
    """A pattern that redacts the absolute paths this attempt was given."""
    values = sorted({str(p) for p in paths if p}, key=len, reverse=True)
    if not values:
        return None
    return re.compile("(" + "|".join(re.escape(v) for v in values) + ")", re.IGNORECASE)


def _sanitize(line: str, paths: re.Pattern[str] | None) -> str:
    """Redact paths, hashes, addresses and data-dump payloads from one runtime diagnostic."""
    text = line.rstrip("\r\n")
    if paths is not None:
        text = paths.sub("<path>", text)
    text = HASH.sub("<hash>", text)
    text = PARAMS.sub(r"\1 <24 words redacted>", text)
    text = DIALOG.sub(r"\1 <text redacted>", text)
    text = ADDR.sub("0x<addr>", text)
    text = PTR.sub("<ptr>", text)
    if len(text) > RT_WIDTH:
        text = text[:RT_WIDTH] + " ..."
    return text


def _iter_lines(path: Path):
    try:
        with open(path, "r", encoding="utf-8-sig", errors="replace", newline="") as fh:
            for line in fh:
                yield line
    except OSError as e:
        raise HarnessError(f"cannot read {path}: {e.strerror or e}") from e


def parse_stderr(path: Path, paths: re.Pattern[str] | None) -> dict:
    """Count the D3D12 calls and keep the last distinct `[rt]` lines of the stderr capture."""
    calls = 0
    max_seq = 0
    rt_lines: list[str] = []
    seen: set[str] = set()
    for line in _iter_lines(path):
        match = D3D12_LINE.match(line)
        if match:
            calls += 1
            max_seq = max(max_seq, int(match.group(1)))
            continue
        if RT_LINE.match(line):
            text = _sanitize(line, paths)
            if text and text not in seen:
                seen.add(text)
                rt_lines.append(text)
    return {"calls": calls, "max_seq": max_seq, "rt_lines": rt_lines}


def _read_trace_bytes(path: Path) -> bytes:
    """Read the trace, retrying briefly while a just-killed process releases the file handle."""
    deadline = time.monotonic() + 3.0
    while True:
        try:
            return path.read_bytes()
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def parse_trace(path: Path) -> dict:
    """Read the startup JSONL trace: event count, exception count, first exception."""
    info = {"available": False, "path": path, "error": None, "events": 0, "exceptions": 0,
            "bad_lines": 0, "first": None}
    try:
        data = _read_trace_bytes(path)
    except OSError as e:
        info["error"] = f"not readable ({e.strerror or e})"
        return info
    if not data.strip():
        info["error"] = "empty (the runtime did not write any event)"
        return info
    info["available"] = True
    text = _decode(data)
    events = 0
    exceptions = 0
    bad = 0
    first = None
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            bad += 1
            continue
        if not isinstance(event, dict) or "kind" not in event:
            bad += 1
            continue
        events += 1
        if event.get("kind") == "exception":
            exceptions += 1
            if first is None:
                first = event
    info.update(events=events, exceptions=exceptions, bad_lines=bad, first=first)
    return info


def _exit_code(code: int | None) -> str:
    if code is None:
        return "unknown"
    return f"{code} (0x{code & 0xFFFFFFFF:08X})"


def _exception_line(event: dict) -> str:
    code = event.get("code")
    at = event.get("at")
    parts = ["kind exception"]
    parts.append("code ?" if code is None else f"code {code}")
    parts.append("at <unknown>" if not at else f"at {at}")
    access = event.get("access")
    if access is not None:
        parts.append(f"access {access}")
    address = event.get("address")
    if address is not None:
        parts.append(f"address {address}")
    return ", ".join(parts)


def _stop_at(event: dict) -> str:
    at = event.get("at")
    if at:
        return str(at)
    address = event.get("address")
    if address is not None:
        return f"address {address}"
    code = event.get("code")
    return f"code {code}" if code is not None else "unknown"


def run(args: argparse.Namespace) -> int:
    exe = Path(args.exe).expanduser().resolve()
    image = Path(args.image).expanduser().resolve()
    game_dir = Path(args.game_dir).expanduser().resolve()
    overlay = Path(args.overlay).expanduser().resolve()

    for name, path, want in (
        ("--exe", exe, "file"),
        ("--image", image, "file"),
        ("--game-dir", game_dir, "folder"),
    ):
        if want == "file" and not path.is_file():
            raise HarnessError(f"{name} is not a file: {path}")
        if want == "folder" and not path.is_dir():
            raise HarnessError(f"{name} is not a folder: {path}")
    if overlay.exists() and not overlay.is_dir():
        raise HarnessError(f"--overlay is not a folder: {overlay}")
    if args.timeout <= 0:
        raise HarnessError("--timeout must be positive")

    repo = Path(__file__).resolve().parent.parent

    def inside_repo(path: Path) -> bool:
        try:
            path.relative_to(repo)
            return True
        except ValueError:
            return False

    if args.out:
        out_dir = Path(args.out).expanduser().resolve()
    else:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        out_dir = Path(tempfile.gettempdir()) / f"palmetto-boot-{stamp}"
    if inside_repo(out_dir):
        raise HarnessError(f"--out is inside the repository ({repo}); choose a folder outside it")
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.trace:
        trace = Path(args.trace).expanduser().resolve()
        if inside_repo(trace):
            raise HarnessError(f"--trace is inside the repository ({repo}); keep traces outside it")
        if trace.exists():
            raise HarnessError(
                f"--trace already exists: {trace}; the runtime only creates a new trace file, so remove it"
            )
        trace.parent.mkdir(parents=True, exist_ok=True)
    else:
        trace = out_dir / "boot.jsonl"
        index = 1
        while trace.exists():
            trace = out_dir / f"boot-{index}.jsonl"
            index += 1

    overlay.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    for item in args.env:
        name, sep, value = item.partition("=")
        if not sep or not name:
            raise HarnessError(f"--env wants NAME=VALUE, got {item!r}")
        env[name] = value

    command = [
        str(exe),
        "--run", str(image),
        "--game-dir", str(game_dir),
        "--overlay", str(overlay),
        "--trace", str(trace),
        "--trace-d3d12",
    ]

    stdout_path = out_dir / "stdout.log"
    stderr_path = out_dir / "stderr.log"
    started = time.monotonic()
    timed_out = False
    code: int | None = None
    with open(stdout_path, "wb") as out_fh, open(stderr_path, "wb") as err_fh:
        try:
            process = subprocess.Popen(
                command,
                cwd=str(out_dir),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=out_fh,
                stderr=err_fh,
            )
        except OSError as e:
            raise HarnessError(f"cannot start {exe}: {e.strerror or e}") from e
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            # The runtime can relaunch itself to reserve an image range; end that child too.
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=30,
                    check=False,
                )
            except (OSError, subprocess.SubprocessError):
                pass
            try:
                process.kill()
            except OSError:
                pass
            code = process.wait()
    elapsed = time.monotonic() - started

    paths = _path_pattern([
        str(exe), str(image), str(game_dir), str(overlay), str(trace), str(out_dir),
        os.environ.get("PALMETTO_SHADERS", ""),
    ])
    try:
        stderr_info = parse_stderr(stderr_path, paths)
    except HarnessError as e:
        raise HarnessError(f"cannot parse the captured stderr: {e}") from e
    trace_info = parse_trace(trace)

    lines = [f"boot report: {exe}", f"out: {out_dir}"]
    if trace_info["available"]:
        lines.append(
            f"trace: {trace_info['path']} ({trace_info['events']} event(s), "
            f"{trace_info['exceptions']} exception(s))"
        )
    else:
        lines.append(f"trace: {trace_info['path']} (unavailable: {trace_info['error']})")
    lines.append(f"stdout: {stdout_path}")
    lines.append(f"stderr: {stderr_path}")
    lines.append(f"exit code: {_exit_code(code)}")
    wall = f"wall time: {elapsed:.1f} s"
    if timed_out:
        wall += f" (killed by the {args.timeout:g} s timeout)"
    lines.append(wall)
    lines.append(f"timeout: {'killed after ' + format(args.timeout, 'g') + ' s' if timed_out else 'no (limit ' + format(args.timeout, 'g') + ' s)'}")
    if stderr_info["calls"]:
        lines.append(
            f"d3d12: {stderr_info['calls']} call(s) recorded, max #{stderr_info['max_seq']}"
        )
    else:
        lines.append(
            "d3d12: no [d3d12-trace] line recorded (the Vulkan backend writes them: "
            "PALMETTO_D3D12=vulkan; was the device reached?)"
        )
    tail = stderr_info["rt_lines"][-RT_TAIL:]
    if tail:
        lines.append(f"last [rt] lines ({len(tail)} distinct, redacted):")
        lines.extend(f"  {line}" for line in tail)
    else:
        lines.append("last [rt] lines: none")
    first = trace_info["first"]
    if first is not None:
        lines.append(f"first exception: {_exception_line(first)}")
        verdict = f"STOP {_stop_at(first)}"
    else:
        note = "none recorded"
        if not trace_info["available"]:
            note = "none recorded (trace unavailable)"
        lines.append(f"first exception: {note}")
        verdict = f"ALIVE {elapsed:.1f} s"
    lines.append(f"VERDICT: {verdict}")

    report = "\n".join(lines)
    print(report)
    try:
        with open(out_dir / "summary.txt", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(report + "\n")
    except OSError as e:
        raise HarnessError(f"cannot write the summary: {e.strerror or e}") from e
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Run one palmetto boot attempt and print a compact summary."
    )
    parser.add_argument("--exe", required=True, help="path to palmetto.exe")
    parser.add_argument("--image", required=True, help="the game executable to run")
    parser.add_argument("--game-dir", required=True, help="the package folder passed as --game-dir")
    parser.add_argument("--overlay", required=True, help="the write overlay folder")
    parser.add_argument("--timeout", type=float, default=300.0, help="seconds before the attempt is killed")
    parser.add_argument("--trace", help="JSONL trace path (default: <out>/boot.jsonl; must not exist)")
    parser.add_argument("--out", help="capture folder (default: %%TEMP%%\\palmetto-boot-<timestamp>)")
    parser.add_argument(
        "--env", action="append", default=[], metavar="NAME=VALUE",
        help="set one environment variable for the runtime (repeatable)",
    )
    args = parser.parse_args(argv)
    try:
        return run(args)
    except HarnessError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I
        sys.stdout.reconfigure(errors="replace")
        sys.stderr.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
