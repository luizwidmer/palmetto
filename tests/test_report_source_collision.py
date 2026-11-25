"""A report tool must never truncate the file it was asked to index.

Run with ``python -I tests/test_report_source_collision.py``. Set ``PALMETTO_TOOLS_DIR``
to compare against another revision of the tools (used as the negative control:
on main these tests fail because the source file is replaced by the report).

All inputs are built here by hand; no game file or game data is used.
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.environ.get("PALMETTO_TOOLS_DIR", os.path.join(ROOT, "tooling"))


def make_bink():
    # A minimal valid Bink 2 file: bink_index.py accepts a single named file only when it holds a parseable
    # stream (a KB2<revision> tag plus a consistent header whose declared size reaches the whole record; an
    # empty 0x2C-byte header parses as no stream and is then refused as a mislabelled file, exit 2). The earlier
    # fixture was arbitrary text, which the single-file format check now rejects; this one gives the normal-path
    # case below a file the tool indexes (one 1-frame stream).
    header = b"KB2z" + struct.pack("<10I", 0x40, 1, 0, 1, 16, 16, 30, 1, 0, 0)
    return header + bytes(0x48 - len(header))     # size8 + 8 = the whole 0x48-byte record


def make_arc(payload, head_size=0x40):
    head = bytearray(head_size)
    head[0:4] = b"9FPR"
    struct.pack_into("<I", head, 4, len(head))
    return bytes(head) + zlib.compress(payload, 9) + bytes(16)


def make_bvh_payload():
    # Minimal hand-built BVH4 stream (same layout as tests/test_bvh_index.py).
    b = bytearray(0x140)
    struct.pack_into("<I", b, 0x100, 0x81020006)
    struct.pack_into("<I", b, 0x104, 0)
    struct.pack_into("<I", b, 0x20, 0x140)
    struct.pack_into("<I", b, 0x28, 0x40)
    return bytes(b)


class ReportSourceCollisionTest(unittest.TestCase):
    def run_tool(self, name, args):
        command = [sys.executable, "-I", os.path.join(TOOLS_DIR, name)] + args
        return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)

    def assert_refused_without_touching(self, tool, args, source, contents):
        with tempfile.TemporaryDirectory() as folder:
            src = os.path.join(folder, source)
            with open(src, "wb") as fh:
                fh.write(contents)
            argv = [a.replace("{src}", src) for a in args]
            result = self.run_tool(tool, argv)
            with open(src, "rb") as fh:
                after = fh.read()
            self.assertEqual(after, contents, f"{tool} overwrote its input: {after[:60]!r}")
            self.assertNotEqual(result.returncode, 0, f"{tool} accepted source == output")
            self.assertIn("must not overwrite the source", result.stderr)

    def test_bink_index_refuses_out_equal_to_source(self):
        self.assert_refused_without_touching(
            "bink_index.py", ["{src}", "--out", "{src}"], "x.bk2", make_bink())

    def test_bvh_index_refuses_out_equal_to_source(self):
        self.assert_refused_without_touching(
            "bvh_index.py", ["{src}", "--out", "{src}"], "x.arc", make_arc(make_bvh_payload()))

    def test_bvh_index_refuses_csv_equal_to_source(self):
        self.assert_refused_without_touching(
            "bvh_index.py", ["{src}", "--csv", "{src}"], "x.arc", make_arc(make_bvh_payload()))

    def test_anim_index_refuses_out_equal_to_source(self):
        self.assert_refused_without_touching(
            "anim_index.py", ["{src}", "{src}"], "x.arc", make_arc(b"\0" * 64, 0x40))

    def test_anim_index_refuses_rigs_equal_to_source(self):
        with tempfile.TemporaryDirectory() as folder:
            src = os.path.join(folder, "x.arc")
            contents = make_arc(b"\0" * 64, 0x40)
            with open(src, "wb") as fh:
                fh.write(contents)
            out = os.path.join(folder, "out.csv")
            result = self.run_tool("anim_index.py", [src, out, "--rigs", src])
            with open(src, "rb") as fh:
                self.assertEqual(fh.read(), contents)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must not overwrite the source", result.stderr)

    def test_rbf0_index_refuses_out_equal_to_source(self):
        self.assert_refused_without_touching(
            "rbf0_index.py", ["{src}", "{src}"], "x.arc", make_arc(b"RBF0" + bytes(128), 0x100))

    def test_rbf0_index_refuses_a_different_spelling_of_the_same_path(self):
        # realpath/normcase, not string equality: .\x.arc and x.arc are one file on Windows.
        with tempfile.TemporaryDirectory() as folder:
            src = os.path.join(folder, "x.arc")
            contents = make_arc(b"RBF0" + bytes(128), 0x100)
            with open(src, "wb") as fh:
                fh.write(contents)
            other = os.path.join(folder, ".", "x.arc")
            result = self.run_tool("rbf0_index.py", [src, other])
            with open(src, "rb") as fh:
                self.assertEqual(fh.read(), contents)
            self.assertNotEqual(result.returncode, 0)

    def test_a_distinct_report_path_still_works(self):
        # The guard must not break the normal path.
        with tempfile.TemporaryDirectory() as folder:
            src = os.path.join(folder, "x.bk2")
            out = os.path.join(folder, "index.csv")
            contents = make_bink()
            with open(src, "wb") as fh:
                fh.write(contents)
            result = self.run_tool("bink_index.py", [src, "--out", out])
            self.assertEqual(result.returncode, 0, result.stderr)
            with open(src, "rb") as fh:
                self.assertEqual(fh.read(), contents)
            self.assertTrue(os.path.isfile(out))


if __name__ == "__main__":
    unittest.main()
