#!/usr/bin/env python3
r"""Run XCL's synthetic test title under palmetto and under XCL, and compare the results.

XCL (https://github.com/ivall/xcl) is an target console compatibility layer licensed GPL-2.0-only,
which is not compatible with this repository's licence. This tool contains no code from it: it only
starts programs built from a separate XCL checkout that you point it at, reads what they print, and
writes a report. Nothing from that checkout is copied into this repository.

The test title (xcl/tests/testtitle) is a fake console executable. It imports kernelx, xgameruntime,
d3d12_xs and xmem, and prints one "[PASS] <check>" or "[FAIL] <check>: <reason>" line per check.
A title built with "[RUN] <check>" lines (printed before each check) lets this tool show which check
is in progress and which one hangs; without them, the check in progress is inferred from XCL's order.
"[SKIP] <check>" lines are honoured, and XCL_TESTTITLE_SKIP (';'-separated name fragments) is passed
to a title that supports it.

A check that prints nothing for --timeout seconds is a hang. The tool records the stacks of the
title's threads (with cdb, when the Windows debugging tools are installed), stops the process, and
starts the title again with that check skipped, so one hang does not hide every later check.

Usage:
    python -I tools/xcl_compare.py --xcl D:\src\xcl [--palmetto runtime\build\RelWithDebInfo\palmetto.exe]
        [--work DIR] [--timeout 15] [--skip NAME] [--out report.json] [--gui]

No game files are used or needed.
"""

import argparse
import json
import math
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CDB_DEFAULT = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
STATUS_ORDER = ("PASS", "FAIL", "SKIP", "HANG", "NOT RUN")


def find_palmetto():
    for rel in ("runtime/build/RelWithDebInfo/palmetto.exe", "runtime/build/Release/palmetto.exe",
                "runtime/build/Debug/palmetto.exe", "runtime/build/palmetto.exe"):
        p = REPO / rel
        if p.is_file():
            return p
    return None


def find_xcl_binaries(xcl_dir):
    for sub in ("target/release", "target/debug"):
        d = Path(xcl_dir) / sub
        names = [d / "xcl.exe", d / "xcl_testtitle.exe", d / "testgame_mw.dll"]
        if all(p.is_file() for p in names):
            return names
    return None


def prepare_game(xcl_dir, bins, dest):
    """A fresh copy of XCL's synthetic game folder with the test title and its DLL."""
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(Path(xcl_dir) / "tests" / "data" / "testgame", dest)
    shutil.copy2(bins[1], dest / bins[1].name)
    shutil.copy2(bins[2], dest / bins[2].name)


class Run:
    """One runtime's results: check name -> {status, reason, unimplemented, stack, seconds}."""

    def __init__(self, label):
        self.label = label
        self.order = []
        self.checks = {}
        self.exit_code = None

    def set(self, name, **fields):
        if name not in self.checks:
            self.order.append(name)
            self.checks[name] = {"status": "NOT RUN", "reason": "", "unimplemented": [], "stack": [], "seconds": None}
        self.checks[name].update(fields)


def parse_result(line, current, known):
    """Return (status, name, reason) for a [PASS]/[FAIL]/[SKIP] line, else None."""
    for status in ("PASS", "FAIL", "SKIP"):
        tag = "[" + status + "] "
        if not line.startswith(tag):
            continue
        body = line[len(tag):]
        names = ([current] if current else []) + sorted(known, key=len, reverse=True)
        for name in names:
            if body == name:
                return status, name, ""
            if body.startswith(name + ": "):
                return status, name, body[len(name) + 2:]
        name, _, reason = body.partition(": ") if status == "FAIL" else (body, "", "")
        return status, name, reason
    return None


def thread_stacks(cdb, pid):
    """Stacks of every thread of a live process (non-invasive attach), as a list of strings."""
    if not cdb or not Path(cdb).is_file():
        return ["(no cdb.exe: install the Windows SDK debugging tools for stacks)"]
    try:
        out = subprocess.run([str(cdb), "-pv", "-p", str(pid), "-c", "~*kc 30;qd"], capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return ["(cdb failed: %s)" % e]
    stacks, cur = [], []
    for line in out.splitlines():
        s = line.strip()
        if " Id: " in s and "Teb:" in s:
            if cur:
                stacks.append("\n".join(cur))
            cur = [s]
        elif cur and s and not s.startswith(("Call Site", "***", "quit:", "NatVis")):
            cur.append(s)
    if cur:
        stacks.append("\n".join(cur))
    # Threads that are inside the title or a runtime shim say the most; idle pool threads are dropped.
    useful = [t for t in stacks if any(k in t for k in ("xcl_testtitle", "palmetto!", "xcl!"))]
    return useful or stacks


def kill_tree(proc):
    """Stop the title and every process it started (taskkill /T on Windows, the process alone elsewhere)."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
    else:
        proc.kill()


def run_title(cmd, env, cwd, run, known, timeout, cdb, events, skip, transcript=None):
    """Run one title process; returns the name of a check that hung, or None.

    The title's output goes to the events as it arrives and, when `transcript` (a path) is given, is appended to
    that file; it is not kept in memory."""
    env = dict(env)
    if skip:
        env["XCL_TESTTITLE_SKIP"] = ";".join(skip)
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    lines = queue.Queue()

    def pump():
        with proc.stdout:
            for line in proc.stdout:
                lines.put(line.rstrip("\r\n"))
        lines.put(None)

    threading.Thread(target=pump, daemon=True).start()
    out = open(transcript, "a", encoding="utf-8", newline="\n") if transcript else None
    if out:
        out.write("--- %s\n" % " ".join(str(c) for c in cmd))
    try:
        return _follow(proc, lines, run, known, timeout, cdb, events, out, cmd)
    finally:
        if out:
            out.close()


def _follow(proc, lines, run, known, timeout, cdb, events, out, cmd):
    current, started, last = None, time.monotonic(), time.monotonic()
    done_names = []
    events.put(("start", run.label, " ".join(str(c) for c in cmd)))
    eof = False
    while True:
        try:
            if eof:
                # EOF ends the output stream. The title can still be running.
                remaining = timeout - (time.monotonic() - last)
                proc.wait(timeout=max(0.0, min(0.25, remaining)))
                break
            line = lines.get(timeout=0.25)
        except (queue.Empty, subprocess.TimeoutExpired):
            if time.monotonic() - last > timeout:
                hung = current or next((n for n in known if n not in done_names), "(before the first check)")
                stacks = thread_stacks(cdb, proc.pid)
                kill_tree(proc)
                proc.wait()
                run.set(hung, status="HANG", reason="no output for %d s" % timeout, stack=stacks,
                        seconds=round(time.monotonic() - started, 2))
                events.put(("check", run.label, hung))
                return hung
            events.put(("tick", run.label, current, time.monotonic() - started))
            continue
        if line is None:
            eof = True
            continue
        last = time.monotonic()
        if out:
            out.write(line + "\n")
        events.put(("line", run.label, line))
        if line.startswith("[RUN] "):
            current, started = line[6:], time.monotonic()
            if current not in run.checks or run.checks[current]["status"] == "NOT RUN":
                run.set(current, status="RUNNING")
            events.put(("check", run.label, current))
            continue
        if line.startswith("[rt] unimplemented: ") and current:
            if current not in run.checks:
                run.set(current)
            imp = line.split(": ", 1)[1].strip()
            if imp not in run.checks[current]["unimplemented"]:
                run.checks[current]["unimplemented"].append(imp)
            continue
        res = parse_result(line, current, known)
        if res:
            status, name, reason = res
            if run.checks.get(name, {}).get("status") != "HANG":
                run.set(name, status=status, reason=reason, seconds=round(time.monotonic() - started, 2))
            done_names.append(name)
            events.put(("check", run.label, name))
            current, started = None, time.monotonic()
    run.exit_code = proc.returncode
    if current and run.checks.get(current, {}).get("status") == "RUNNING":
        run.set(current, status="FAIL", reason="the title exited during this check (exit code %s)" % proc.returncode)
        events.put(("check", run.label, current))
    return None


def run_all(args, events):
    xcl_dir = Path(args.xcl).resolve()
    bins = find_xcl_binaries(xcl_dir)
    if not bins:
        raise SystemExit("no built XCL in %s: run 'cargo build --release --workspace' there first" % xcl_dir)
    palmetto = Path(args.palmetto).resolve() if args.palmetto else find_palmetto()
    if not palmetto or not palmetto.is_file():
        raise SystemExit("palmetto.exe not found: build the runtime first (scripts/build_runtime.ps1)")
    work = Path(args.work).resolve() if args.work else Path(tempfile.gettempdir()) / "palmetto_xcl_compare"
    if work.drive.upper() == "D:":
        # XCL maps the console drive D: to the game folder, so a real D:\ path is not reachable inside it.
        raise SystemExit("--work is on D:, which XCL maps to the game folder; use a folder on another drive")
    work.mkdir(parents=True, exist_ok=True)
    cdb = Path(args.cdb) if args.cdb else (CDB_DEFAULT if CDB_DEFAULT.is_file() else None)

    xcl, leo = Run("XCL"), Run("Palmetto")
    events.put(("runs", xcl, leo))
    transcripts = {"xcl": work / "xcl_output.txt", "palmetto": work / "palmetto_output.txt"}
    for path in transcripts.values():
        open(path, "w", encoding="utf-8", newline="\n").close()

    # 1. XCL: the reference run, which also gives the check order.
    if (work / "xcl").exists():
        shutil.rmtree(work / "xcl")
    prepare_game(xcl_dir, bins, work / "xcl" / "game")
    env = dict(os.environ, XCL_DATA_ROOT=str(work / "xcl" / "data"))
    run_title([str(bins[0]), "run", str(work / "xcl" / "game"), "--", "--hello"], env, str(work / "xcl"), xcl,
              [], args.timeout, cdb, events, list(args.skip), transcripts["xcl"])
    known = list(xcl.order)

    # 2. palmetto, restarted with a hung check skipped, up to --max-restarts times.
    # Wipe stale output from an earlier run once; per-attempt restarts reset only game/ and
    # overlay/ so earlier trace_<attempt>.jsonl files (including the hanging attempt's) survive.
    palmetto_dir = work / "palmetto"
    if palmetto_dir.exists():
        shutil.rmtree(palmetto_dir)
    palmetto_dir.mkdir(parents=True, exist_ok=True)

    def palmetto_cmd(attempt):
        game = palmetto_dir / "game"
        overlay = palmetto_dir / "overlay"
        if overlay.exists():
            shutil.rmtree(overlay)
        prepare_game(xcl_dir, bins, game)
        trace = palmetto_dir / ("trace_%d.jsonl" % attempt)
        return [str(palmetto), "--run", str(game / bins[1].name), "--game-dir", str(game),
                "--overlay", str(overlay), "--trace-d3d12", "--trace", str(trace), "--", "--hello"]

    run_with_restarts(palmetto_cmd, dict(os.environ), str(palmetto_dir), leo, known, args.timeout, cdb, events,
                      list(args.skip), args.max_restarts, transcripts["palmetto"])
    for name in known:
        if name not in leo.checks:
            leo.set(name)
    report = make_report(xcl, leo, known, palmetto, xcl_dir)
    report["transcripts"] = {k: str(v) for k, v in transcripts.items()}
    out = Path(args.out) if args.out else work / "report.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
        f.write("\n")
    events.put(("done", report, str(out)))
    return report, out


def run_with_restarts(cmd_for, env, cwd, run, known, timeout, cdb, events, skip, max_restarts, transcript=None):
    """Run the title, and after a hang run it again with the hung check in XCL_TESTTITLE_SKIP.

    cmd_for(attempt) returns the command line for each attempt (it may also reset the work folder). Stops when a run
    ends without a hang, after max_restarts restarts, or when a skipped check hangs again. Returns the skip list."""
    skip = list(skip)
    for attempt in range(max_restarts + 1):
        hung = run_title(cmd_for(attempt), env, cwd, run, known, timeout, cdb, events, skip, transcript)
        if not hung:
            break
        if hung in skip:
            # The title ignored XCL_TESTTITLE_SKIP (it was built without that local change), so a restart would
            # only hang at the same check again.
            events.put(("line", run.label, "--- %s hung again although skipped: the title does not support "
                        "XCL_TESTTITLE_SKIP, so the checks after it are not run ---" % hung))
            break
        skip.append(hung)
        events.put(("line", run.label, "--- %s hung; restarting with it skipped ---" % hung))
    return skip


def git_head(path):
    try:
        return subprocess.run(["git", "-C", str(path), "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
    except OSError:
        return ""


def make_report(xcl, leo, known, palmetto, xcl_dir):
    rows = []
    for name in known + [n for n in leo.order if n not in known]:
        x = xcl.checks.get(name, {"status": "NOT RUN", "reason": ""})
        l = leo.checks.get(name, {"status": "NOT RUN", "reason": "", "unimplemented": [], "stack": []})
        rows.append({"check": name, "xcl": x["status"], "xcl_reason": x["reason"], "palmetto": l["status"],
                     "palmetto_reason": l["reason"], "unimplemented": l.get("unimplemented", []),
                     "stack": l.get("stack", []),
                     # A check that fails in both runs for different reasons still differs.
                     "same": (x["status"], x["reason"]) == (l["status"], l["reason"])})
    count = lambda run, s: sum(1 for r in rows if r[run] == s)
    return {
        "palmetto_commit": git_head(REPO), "xcl_commit": git_head(xcl_dir), "palmetto": str(palmetto),
        "summary": {run: {s: count(run, s) for s in STATUS_ORDER} for run in ("xcl", "palmetto")},
        "differences": sum(1 for r in rows if not r["same"]),
        "xcl_exit_code": xcl.exit_code, "palmetto_exit_code": leo.exit_code, "checks": rows,
    }


def print_report(report, out):
    w = max(len(r["check"]) for r in report["checks"]) if report["checks"] else 10
    w = min(w, 70)
    for r in report["checks"]:
        mark = " " if r["same"] else "*"
        line = "%s %-*s  XCL %-7s  Palmetto %-7s" % (mark, w, r["check"][:w], r["xcl"], r["palmetto"])
        if not r["same"] and r["xcl"] == r["palmetto"]:
            line += "  XCL: %s / Palmetto: %s" % (r["xcl_reason"][:60], r["palmetto_reason"][:60])
        elif r["palmetto_reason"] and not r["same"]:
            line += "  " + r["palmetto_reason"][:120]
        print(line)
        for imp in r["unimplemented"]:
            print("      unimplemented: " + imp)
        for st in r["stack"][:2]:
            print("      " + st.replace("\n", "\n      "))
    s = report["summary"]
    print("\nXCL:     " + ", ".join("%s %d" % (k, v) for k, v in s["xcl"].items() if v))
    print("Palmetto: " + ", ".join("%s %d" % (k, v) for k, v in s["palmetto"].items() if v))
    print("%d check(s) differ; report: %s" % (report["differences"], out))


def gui(args):
    import tkinter as tk
    from tkinter import ttk

    events = queue.Queue()
    root = tk.Tk()
    root.title("XCL test title: XCL vs Palmetto (palmetto)")
    root.geometry("1400x850")
    status = tk.StringVar(value="starting")
    ttk.Label(root, textvariable=status, anchor="w", font=("Segoe UI", 11, "bold")).pack(fill="x", padx=8, pady=4)
    pane = ttk.PanedWindow(root, orient="vertical")
    pane.pack(fill="both", expand=True)
    cols = ("xcl", "palmetto", "detail")
    tree = ttk.Treeview(pane, columns=cols, show="tree headings")
    tree.heading("#0", text="Check")
    tree.heading("xcl", text="XCL")
    tree.heading("palmetto", text="Palmetto")
    tree.heading("detail", text="Palmetto detail (reason, unimplemented imports, hang)")
    tree.column("#0", width=520)
    tree.column("xcl", width=80, anchor="center")
    tree.column("palmetto", width=90, anchor="center")
    tree.column("detail", width=680)
    colours = {"PASS": "#d8f5d8", "FAIL": "#f8d0d0", "HANG": "#f5c27a", "SKIP": "#e4e4e4", "RUNNING": "#cfe3ff",
               "NOT RUN": "#ffffff"}
    for s, c in colours.items():
        tree.tag_configure(s, background=c)
    pane.add(tree, weight=3)
    bottom = ttk.PanedWindow(pane, orient="horizontal")
    logs = {}
    for label in ("XCL", "Palmetto"):
        frame = ttk.Labelframe(bottom, text=label + " output")
        txt = tk.Text(frame, height=12, font=("Consolas", 9), wrap="none")
        txt.pack(fill="both", expand=True)
        logs[label] = txt
        bottom.add(frame, weight=1)
    pane.add(bottom, weight=2)
    detail = tk.Text(root, height=8, font=("Consolas", 9))
    detail.pack(fill="x")
    state = {"runs": None, "items": {}}

    def row(name):
        xcl, leo = state["runs"]
        x = xcl.checks.get(name, {}).get("status", "")
        l = leo.checks.get(name, {})
        text = l.get("reason", "")
        if l.get("unimplemented"):
            text += ("  " if text else "") + "unimplemented: " + ", ".join(l["unimplemented"])
        tag = l.get("status") or x or "NOT RUN"
        if name not in state["items"]:
            state["items"][name] = tree.insert("", "end", text=name)
        tree.item(state["items"][name], values=(x, l.get("status", ""), text), tags=(tag,))
        tree.see(state["items"][name])

    def on_select(_):
        sel = tree.selection()
        if not sel or not state["runs"]:
            return
        name = tree.item(sel[0], "text")
        l = state["runs"][1].checks.get(name, {})
        x = state["runs"][0].checks.get(name, {})
        detail.delete("1.0", "end")
        detail.insert("end", "%s\nXCL: %s %s\nPalmetto: %s %s\n" % (name, x.get("status"), x.get("reason", ""),
                                                                    l.get("status"), l.get("reason", "")))
        for st in l.get("stack", []):
            detail.insert("end", "\n" + st + "\n")

    tree.bind("<<TreeviewSelect>>", on_select)

    def poll():
        try:
            while True:
                ev = events.get_nowait()
                kind = ev[0]
                if kind == "runs":
                    state["runs"] = (ev[1], ev[2])
                elif kind == "start":
                    status.set("%s: starting %s" % (ev[1], ev[2]))
                elif kind == "line":
                    logs[ev[1]].insert("end", ev[2] + "\n")
                    logs[ev[1]].see("end")
                elif kind == "check":
                    row(ev[2])
                elif kind == "tick" and ev[2]:
                    status.set("%s: running '%s' (%.1f s)" % (ev[1], ev[2], ev[3]))
                elif kind == "done":
                    s = ev[1]["summary"]
                    status.set("done. XCL %d pass / Palmetto %d pass, %d fail, %d hang; %d differ. Report: %s" % (
                        s["xcl"]["PASS"], s["palmetto"]["PASS"], s["palmetto"]["FAIL"], s["palmetto"]["HANG"],
                        ev[1]["differences"], ev[2]))
                elif kind == "error":
                    status.set("error: " + ev[1])
        except queue.Empty:
            pass
        root.after(100, poll)

    def worker():
        try:
            run_all(args, events)
        except SystemExit as e:
            events.put(("error", str(e)))
        except Exception as e:  # shown in the window instead of a lost traceback
            events.put(("error", repr(e)))

    threading.Thread(target=worker, daemon=True).start()
    root.after(100, poll)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--xcl", required=True, help="an XCL checkout with 'cargo build --release --workspace' done")
    ap.add_argument("--palmetto", help="palmetto.exe (default: the one under runtime/build)")
    ap.add_argument("--work", help="work folder, not on D: (default: %%TEMP%%\\palmetto_xcl_compare)")
    ap.add_argument("--timeout", type=float, default=15.0, help="seconds without output that count as a hang")
    ap.add_argument("--max-restarts", type=int, default=8, help="restarts after hangs")
    ap.add_argument("--skip", action="append", default=[], help="skip checks whose name contains this")
    ap.add_argument("--cdb", help="cdb.exe for stacks of a hung title")
    ap.add_argument("--out", help="report path (default: <work>/report.json)")
    ap.add_argument("--gui", action="store_true", help="show the checks live in a window")
    args = ap.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        ap.error("--timeout must be finite and greater than zero")
    if args.max_restarts < 0:
        ap.error("--max-restarts must be non-negative")
    if sys.platform != "win32":
        raise SystemExit("Windows only: both runtimes run Windows executables")
    if args.gui:
        gui(args)
        return 0
    events = queue.Queue()

    def drain():
        while True:
            ev = events.get()
            if ev[0] == "line":
                print("[%s] %s" % (ev[1], ev[2]), flush=True)
            elif ev[0] == "done":
                return

    t = threading.Thread(target=drain, daemon=True)
    t.start()
    report, out = run_all(args, events)
    t.join(timeout=2)
    print()
    print_report(report, out)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    sys.exit(main())
