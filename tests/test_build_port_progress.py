"""Progress / ETA helpers used by build_port.py (no game data).

    python -I tests/test_make_game_progress.py
"""
import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import build_port  # noqa: E402


class FmtDurationTest(unittest.TestCase):
    def test_seconds_minutes_hours(self):
        self.assertEqual(build_port.fmt_duration(0), "0s")
        self.assertEqual(build_port.fmt_duration(9.4), "9s")
        self.assertEqual(build_port.fmt_duration(60), "1m")
        self.assertEqual(build_port.fmt_duration(75), "1m 15s")
        self.assertEqual(build_port.fmt_duration(3600), "1h 00m")
        self.assertEqual(build_port.fmt_duration(3725), "1h 02m")


class ProgressTest(unittest.TestCase):
    def test_logs_percent_and_eta(self):
        lines = []
        # __init__ reads monotonic for t0 and the first emit; each later emit reads once more.
        with mock.patch.object(build_port, "log", side_effect=lines.append), \
                mock.patch.object(build_port.time, "monotonic", side_effect=[100.0, 100.0, 120.0, 160.0]):
            p = build_port.Progress("step", 4, every_s=0.0)
            p.tick(force=True)
            p.set(4, force=True)
        self.assertIn("step: 0/4 (0%)", lines[0])
        self.assertTrue(any("1/4 (25%)" in line and "left" in line for line in lines), lines)
        self.assertTrue(any("4/4 (100%)" in line and "elapsed" in line for line in lines), lines)

    def test_empty_total(self):
        lines = []
        with mock.patch.object(build_port, "log", side_effect=lines.append):
            build_port.Progress("empty", 0)
        self.assertEqual(lines, ["empty: nothing to do"])


class StageBoardTest(unittest.TestCase):
    def test_weighted_percent_and_remaining(self):
        lines = []
        with mock.patch.object(build_port, "log", side_effect=lines.append), \
                mock.patch.object(build_port.time, "monotonic", side_effect=[0.0, 10.0, 20.0]):
            board = build_port.StageBoard(["shaders", "assets", "runtime"])
            board.complete("runtime")
            board.complete("shaders")
        self.assertIn("build: 0%", lines[0])
        # runtime is weight 2 of 40+45+2
        self.assertTrue(any("runtime done" in line and "still" in line for line in lines))
        self.assertTrue(any("shaders done" in line for line in lines))

    def test_complete_is_idempotent(self):
        lines = []
        with mock.patch.object(build_port, "log", side_effect=lines.append), \
                mock.patch.object(build_port.time, "monotonic", return_value=1.0):
            board = build_port.StageBoard(["runtime"])
            board.complete("runtime")
            before = len(lines)
            board.complete("runtime")
        self.assertEqual(len(lines), before)


if __name__ == "__main__":
    unittest.main()
