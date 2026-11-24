"""build_port.py rejects a --out/--work that is a file (or sits under one) before doing any work, instead of a
traceback from os.makedirs or a run that reports success without creating the destination.

    python -I tests/test_make_game_destinations.py
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import build_port


class DestinationTests(unittest.TestCase):
    def run_main(self, *argv):
        """main() over the given arguments, with the game check stubbed out. Returns (exit code, stderr)."""
        err = io.StringIO()
        with patch.object(sys, "argv", ["build_port.py", *argv]), \
             patch.object(build_port, "below_normal"), \
             patch.object(build_port, "log"), \
             patch.object(build_port, "check", return_value={"title": "test", "version": "1.0"}), \
             patch.object(build_port, "preflight", side_effect=AssertionError("preflight was reached")), \
             contextlib.redirect_stderr(err):
            with self.assertRaises(SystemExit) as raised:
                build_port.main()
        return raised.exception.code, err.getvalue()

    def test_out_is_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game")
            os.makedirs(game)
            out = os.path.join(tmp, "out.txt")
            open(out, "w").close()
            code, err = self.run_main("--game", game, "--out", out, "--work", os.path.join(tmp, "work"))
            self.assertEqual(code, 1, err)
            self.assertIn("--out is not a folder", err)

    def test_work_is_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game")
            os.makedirs(game)
            work = os.path.join(tmp, "work.txt")
            open(work, "w").close()
            code, err = self.run_main("--game", game, "--out", os.path.join(tmp, "out"), "--work", work)
            self.assertEqual(code, 1, err)
            self.assertIn("--work is not a folder", err)
            self.assertFalse(os.path.exists(os.path.join(tmp, "out")), "the build created --out before rejecting --work")

    def test_out_under_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, "game")
            os.makedirs(game)
            blocker = os.path.join(tmp, "blocker.txt")
            open(blocker, "w").close()
            code, err = self.run_main("--game", game, "--out", os.path.join(blocker, "out"),
                                      "--work", os.path.join(tmp, "work"))
            self.assertEqual(code, 1, err)
            self.assertIn("--out is not a folder", err)

    def test_missing_destination_is_accepted(self):
        # A destination that does not exist yet is fine as long as its nearest existing parent is a folder.
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(sys, "argv", ["build_port.py", "--game", os.path.join(tmp, "game"), "--check",
                                        "--out", os.path.join(tmp, "new", "out"),
                                        "--work", os.path.join(tmp, "new", "work")]), \
             patch.object(build_port, "below_normal"), patch.object(build_port, "log"), \
             patch.object(build_port, "check", return_value={"title": "test", "version": "1.0"}), \
             contextlib.redirect_stderr(io.StringIO()):
            os.makedirs(os.path.join(tmp, "game"))
            self.assertEqual(build_port.main(), 0)


class DriveRootGameTests(unittest.TestCase):
    """--game at a drive root: the game folder is the dump's own top-level manifest names, so an unrelated --out on
    the same drive is allowed and the rerun that resumes a build is not rejected."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.game = self.tmp   # the drive root itself: every top-level folder here is "on the drive"
        os.makedirs(os.path.join(self.game, "Content"))
        self.version = {"title": "test", "version": "1.0", "expected_exe": "Game.exe",
                        "files": {"Content/a.bin": {}, "Game.exe": {}}}

    def main_at_drive_root(self, out, err):
        """main() with the game folder reported as a drive root. Returns nothing; it stops at preflight."""
        real_splitdrive = os.path.splitdrive
        real_game = os.path.normcase(os.path.realpath(self.game))

        def fake_splitdrive(path):
            return ("X:", "\\") if os.path.normcase(path) == real_game else real_splitdrive(path)

        with patch.object(sys, "argv", ["build_port.py", "--game", self.game, "--out", out,
                                        "--work", os.path.join(self.tmp, "work")]), \
             patch.object(build_port, "below_normal"), patch.object(build_port, "log"), \
             patch.object(build_port, "check", return_value=self.version), \
             patch.object(build_port.os.path, "splitdrive", fake_splitdrive), \
             patch.object(build_port, "preflight", side_effect=AssertionError("preflight was reached")), \
             contextlib.redirect_stderr(err):
            build_port.main()

    def test_out_on_the_same_drive_is_accepted_on_resume(self):
        # The folder is already there, as after a first run: listing the drive would count it as the game's own.
        out = os.path.join(self.tmp, "Palmetto_Port")
        os.makedirs(out)
        with self.assertRaisesRegex(AssertionError, "preflight was reached"):
            self.main_at_drive_root(out, io.StringIO())

    def test_out_inside_a_top_level_manifest_folder_is_rejected(self):
        err = io.StringIO()
        with self.assertRaises(SystemExit) as raised:
            self.main_at_drive_root(os.path.join(self.game, "Content", "out"), err)
        self.assertEqual(raised.exception.code, 1, err.getvalue())
        self.assertIn("--out and --work must be outside the game folder.", err.getvalue())


if __name__ == "__main__":
    unittest.main()
