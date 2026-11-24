"""Unit tests for build_port.py --pick: the folder dialog, with a stand-in tkinter and stand-in kdialog/zenity
runs (no display needed).

    python -I tests/test_make_game_pick.py
"""
import io
import os
import sys
import types
import unittest
from contextlib import redirect_stderr
from unittest.mock import MagicMock, patch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import build_port


def fake_tkinter(answers, tk_error=None):
    """A tkinter whose folder dialog returns `answers` in turn ("" is a cancel)."""
    tk = types.ModuleType("tkinter")
    root = MagicMock()
    if tk_error:
        tk.Tk = MagicMock(side_effect=tk_error)
    else:
        tk.Tk = MagicMock(return_value=root)
    filedialog = types.ModuleType("tkinter.filedialog")
    filedialog.askdirectory = MagicMock(side_effect=list(answers))
    messagebox = types.ModuleType("tkinter.messagebox")
    messagebox.showwarning = MagicMock()
    tk.filedialog, tk.messagebox = filedialog, messagebox
    modules = {"tkinter": tk, "tkinter.filedialog": filedialog, "tkinter.messagebox": messagebox}
    return modules, root, filedialog, messagebox


VERSION = {"title": "Test", "version": "1.0.2.1"}


class PickGameTest(unittest.TestCase):
    """The Tk dialog: Windows, macOS, and Linux without kdialog or zenity."""

    def setUp(self):
        p = patch("build_port.native_picker", return_value=None)
        p.start()
        self.addCleanup(p.stop)

    def test_returns_a_supported_folder(self):
        modules, root, dialog, warn = fake_tkinter([os.path.join("D:", "game")])
        with patch.dict(sys.modules, modules), patch("build_port.match_version", return_value=VERSION), \
                patch("build_port.log"):
            self.assertEqual(build_port.pick_game(), os.path.normpath(os.path.join("D:", "game")))
        self.assertEqual(dialog.askdirectory.call_count, 1)
        warn.showwarning.assert_not_called()
        root.destroy.assert_called_once()

    def test_asks_again_after_an_unsupported_folder(self):
        modules, root, dialog, warn = fake_tkinter(["wrong", "right"])
        with patch.dict(sys.modules, modules), \
                patch("build_port.match_version", side_effect=lambda d: VERSION if d.endswith("right") else None), \
                patch("build_port.log"):
            self.assertTrue(build_port.pick_game().endswith("right"))
        self.assertEqual(dialog.askdirectory.call_count, 2)
        warn.showwarning.assert_called_once()
        root.destroy.assert_called_once()

    def test_cancel_stops_with_a_pointer_to_game(self):
        modules, root, _, _ = fake_tkinter([""])
        err = io.StringIO()
        with patch.dict(sys.modules, modules), redirect_stderr(err), self.assertRaises(SystemExit) as stop:
            build_port.pick_game()
        self.assertEqual(stop.exception.code, 1)
        self.assertIn("--game", err.getvalue())
        root.destroy.assert_called_once()  # the hidden window goes even when the build stops

    def test_no_display_stops_with_a_pointer_to_game(self):
        modules, _, dialog, _ = fake_tkinter([], tk_error=RuntimeError("no display name"))
        err = io.StringIO()
        with patch.dict(sys.modules, modules), redirect_stderr(err), self.assertRaises(SystemExit) as stop:
            build_port.pick_game()
        self.assertEqual(stop.exception.code, 1)
        self.assertIn("--game", err.getvalue())
        self.assertIn("no display name", err.getvalue())
        dialog.askdirectory.assert_not_called()


def runs(*results):
    """A subprocess.run whose calls return (returncode, stdout) from `results` in turn."""
    return MagicMock(side_effect=[types.SimpleNamespace(returncode=c, stdout=o) for c, o in results])


class NativePickerTest(unittest.TestCase):
    def env(self, **values):
        return patch.dict(os.environ, values, clear=True)

    def test_kde_prefers_kdialog(self):
        with self.env(DISPLAY=":0", XDG_CURRENT_DESKTOP="KDE"), patch.object(build_port.os, "name", "posix"), \
                patch.object(build_port.sys, "platform", "linux"), \
                patch("build_port.shutil.which", side_effect=lambda t: "/usr/bin/" + t):
            self.assertEqual(build_port.native_picker(), ("kdialog", "/usr/bin/kdialog"))

    def test_other_desktops_prefer_zenity(self):
        with self.env(WAYLAND_DISPLAY="wayland-0", XDG_CURRENT_DESKTOP="GNOME"), \
                patch.object(build_port.os, "name", "posix"), patch.object(build_port.sys, "platform", "linux"), \
                patch("build_port.shutil.which", side_effect=lambda t: "/usr/bin/" + t):
            self.assertEqual(build_port.native_picker(), ("zenity", "/usr/bin/zenity"))

    def test_takes_whichever_is_installed(self):
        with self.env(DISPLAY=":0", XDG_CURRENT_DESKTOP="KDE"), patch.object(build_port.os, "name", "posix"), \
                patch.object(build_port.sys, "platform", "linux"), \
                patch("build_port.shutil.which", side_effect=lambda t: "/usr/bin/zenity" if t == "zenity" else None):
            self.assertEqual(build_port.native_picker(), ("zenity", "/usr/bin/zenity"))

    def test_none_without_a_display_or_on_windows_and_macos(self):
        which = patch("build_port.shutil.which", return_value="/usr/bin/x")
        with self.env(), patch.object(build_port.os, "name", "posix"), \
                patch.object(build_port.sys, "platform", "linux"), which:
            self.assertIsNone(build_port.native_picker())
        for name, platform in (("nt", "win32"), ("posix", "darwin")):
            with self.env(DISPLAY=":0"), patch.object(build_port.os, "name", name), \
                    patch.object(build_port.sys, "platform", platform), which:
                self.assertIsNone(build_port.native_picker())


class PickNativeTest(unittest.TestCase):
    """The desktop's dialog on Linux, run through kdialog or zenity."""

    def pick(self, tool, run, version=lambda d: VERSION):
        with patch("build_port.native_picker", return_value=(tool, "/usr/bin/" + tool)), \
                patch("build_port.subprocess.run", run), patch("build_port.match_version", side_effect=version), \
                patch("build_port.log"):
            return build_port.pick_game()

    def test_kdialog_returns_a_supported_folder(self):
        run = runs((0, "/games/palmetto\n"))
        self.assertEqual(self.pick("kdialog", run), os.path.normpath("/games/palmetto"))
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], "/usr/bin/kdialog")
        self.assertIn("--getexistingdirectory", cmd)

    def test_zenity_asks_again_from_the_last_folder_after_a_warning(self):
        run = runs((0, "/games/wrong\n"), (0, ""), (0, "/games/right\n"))
        got = self.pick("zenity", run, version=lambda d: VERSION if d.endswith("right") else None)
        self.assertTrue(got.endswith("right"))
        ask1, warn, ask2 = (c[0][0] for c in run.call_args_list)
        self.assertIn("--directory", ask1)
        self.assertIn("--warning", warn)
        self.assertIn("--filename=" + os.path.join(os.path.normpath("/games/wrong"), ""), ask2)

    def test_cancel_stops_with_a_pointer_to_game(self):
        err = io.StringIO()
        with redirect_stderr(err), self.assertRaises(SystemExit) as stop:
            self.pick("kdialog", runs((1, "")))
        self.assertEqual(stop.exception.code, 1)
        self.assertIn("--game", err.getvalue())

    def test_a_broken_tool_falls_back_to_tk(self):
        modules, root, dialog, _ = fake_tkinter(["/games/palmetto"])
        with patch.dict(sys.modules, modules):
            self.assertEqual(self.pick("zenity", runs((255, ""))), os.path.normpath("/games/palmetto"))
        dialog.askdirectory.assert_called_once()
        root.destroy.assert_called_once()


class PickArgumentsTest(unittest.TestCase):
    def test_pick_and_game_are_exclusive(self):
        err = io.StringIO()
        with patch.object(sys, "argv", ["build_port.py", "--pick", "--game", "D:/game"]), \
                patch("build_port.pick_game") as pick, redirect_stderr(err), self.assertRaises(SystemExit) as stop:
            build_port.main()
        self.assertEqual(stop.exception.code, 2)
        self.assertIn("--pick and --game", err.getvalue())
        pick.assert_not_called()

    def test_pick_replaces_the_search(self):
        chosen = os.path.abspath("picked")
        with patch.object(sys, "argv", ["build_port.py", "--pick", "--check"]), \
                patch("build_port.below_normal"), \
                patch("build_port.pick_game", return_value=chosen) as pick, \
                patch("build_port.choose_game") as search, \
                patch("build_port.check", return_value={"expected_exe": "game.exe"}) as check:
            self.assertEqual(build_port.main(), 0)
        pick.assert_called_once()
        search.assert_not_called()
        self.assertEqual(check.call_args[0][0], chosen)


if __name__ == "__main__":
    unittest.main()
