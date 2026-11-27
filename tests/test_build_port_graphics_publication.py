"""build_port.py: graphics-library publication is PS/VS-default and MS opt-in by selection.

The fixtures model only synthetic index rows and modules; no package, extracted shader, or game data is read.
Run with: python -I tests/test_build_port_graphics_publication.py
"""

import hashlib
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import build_port  # noqa: E402


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(value, f, sort_keys=True)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def synthetic_key(label):
    return hashlib.sha256(("synthetic-" + label).encode("ascii")).hexdigest()


class GraphicsPublicationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        self.game = os.path.join(self.root, "game")
        self.work = os.path.join(self.root, "work")
        os.makedirs(self.game)
        os.makedirs(os.path.join(self.work, "shaders", "spirv", "fspx"), exist_ok=True)
        self.arc = os.path.join(self.game, build_port.LIBRARY_ARC)
        with open(self.arc, "wb") as f:
            f.write(b"synthetic-package-placeholder")
        self.main_index = os.path.join(self.work, "shaders", "spirv", "index.json")
        write_json(self.main_index, {})
        self.wanted = os.path.join(self.root, "wanted.json")
        self.fspx = os.path.join(self.work, "shaders", "spirv", "fspx")
        self.extracted = os.path.join(self.work, "shaders", "pipeline_graphics")
        self.staged = os.path.join(self.work, "shaders", "spirv", "pipeline_graphics")
        self.names = {
            "ps": "lib_ps_00001",
            "vs": "lib_vs_00002",
            "ms": "lib_ms_00003",
            "cs": "lib_cs_00004",
            "old_ms": "lib_ms_legacy",
        }
        self.hashes = {stage: synthetic_key(stage) for stage in ("ps", "vs", "ms", "cs", "old_ms")}

    def write_selection(self, stages):
        # The hashes are synthetic. Non-list notes have the same behavior as the tool's wanted-list parser.
        write_json(self.wanted, {**{stage: [self.hashes[stage]] for stage in stages}, "note": "synthetic"})

    def seed_published(self, include_ms=True):
        entries = {
            self.names["cs"]: {"stage": "cs", "fspx_code_sha256": self.hashes["cs"]},
            self.names["ps"]: {"stage": "ps", "fspx_code_sha256": self.hashes["ps"]},
            self.names["vs"]: {"stage": "vs", "fspx_code_sha256": self.hashes["vs"]},
        }
        if include_ms:
            entries[self.names["old_ms"]] = {"stage": "ms", "fspx_code_sha256": self.hashes["old_ms"]}
        write_json(os.path.join(self.fspx, "index.json"), entries)
        for name in entries:
            with open(os.path.join(self.fspx, name + ".spv"), "wb") as f:
                f.write(("published-" + name).encode("ascii"))

    def seed_cached_selection(self, stages):
        """Create synthetic extraction/translation cache entries so graphics_shaders exercises publication only."""
        names = [self.names[stage] for stage in stages]
        os.makedirs(os.path.join(self.extracted, "fspx"), exist_ok=True)
        os.makedirs(os.path.join(self.extracted, "disasm"), exist_ok=True)
        write_json(os.path.join(self.extracted, "index.json"), [{"file": name + ".dxbc"} for name in names])
        wanted_counts = {stage: 1 for stage in stages}
        write_json(os.path.join(self.extracted, "wanted.json"), {
            "wanted": wanted_counts,
            "found": wanted_counts,
            "missing": {stage: [] for stage in stages},
            "other_stage": {stage: [] for stage in stages},
        })
        for name in names:
            with open(os.path.join(self.extracted, "fspx", name + ".code"), "wb") as f:
                f.write(b"synthetic-code-only")
            write_json(os.path.join(self.extracted, "fspx", name + ".json"), {})
            with open(os.path.join(self.extracted, "disasm", name + ".s"), "w", encoding="utf-8", newline="\n") as f:
                f.write("synthetic listing\n")

        package = build_port.file_sha256(self.arc)
        stage_text = ",".join(stages)
        extraction_settings = build_port.graphics_extraction_settings(package, "llvm-test", self.wanted, stage_text)
        with open(os.path.join(self.extracted, build_port.GRAPHICS_EXTRACTED_RECORD), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write(extraction_settings + "\n")

        os.makedirs(self.staged, exist_ok=True)
        translated = {
            self.names[stage]: {"stage": stage, "fspx_code_sha256": self.hashes[stage]}
            for stage in stages
        }
        write_json(os.path.join(self.staged, "index.json"), translated)
        for name in names:
            with open(os.path.join(self.staged, name + ".spv"), "wb") as f:
                f.write(("synthetic-spirv-" + name).encode("ascii"))
        settings = build_port.graphics_settings(package, "llvm-test", False, (), self.wanted, stage_text)
        write_json(os.path.join(self.staged, build_port.GRAPHICS_BUILD_RECORD), {"settings": settings, "refused": []})

    def run_cached_publisher(self):
        with mock.patch.dict(os.environ, {build_port.GRAPHICS_PROGRAMS_ENV: self.wanted}), \
                mock.patch.object(build_port, "log"):
            build_port.graphics_shaders(self.game, self.work, "llvm-test")

    def test_ps_vs_selection_is_byte_identical_and_does_not_prune_existing_ms(self):
        self.write_selection(("ps", "vs"))
        self.seed_published(include_ms=True)
        self.seed_cached_selection(("ps", "vs"))
        index_path = os.path.join(self.fspx, "index.json")
        before_index = read_bytes(index_path)
        before_modules = {
            name: read_bytes(os.path.join(self.fspx, name + ".spv"))
            for name in (self.names["cs"], self.names["ps"], self.names["vs"], self.names["old_ms"])
        }

        self.assertEqual(build_port.graphics_requested_stages(self.wanted), "ps,vs")
        self.assertEqual(build_port.graphics_owned_for("ps,vs"), ("lib_ps_", "lib_vs_"))
        self.assertEqual(json.loads(build_port.graphics_extraction_settings(
            "package", "llvm-test", self.wanted))["stages"], "ps,vs")
        self.run_cached_publisher()

        self.assertEqual(read_bytes(index_path), before_index)
        for name, content in before_modules.items():
            self.assertEqual(read_bytes(os.path.join(self.fspx, name + ".spv")), content)
        with open(index_path, encoding="utf-8") as f:
            published = json.load(f)
        self.assertNotIn(self.names["ms"], published)
        self.assertIn(self.names["old_ms"], published)

    def test_explicit_ms_selection_publishes_under_stage_and_code_hash(self):
        self.write_selection(("ps", "vs", "ms"))
        self.seed_published(include_ms=False)
        self.seed_cached_selection(("ps", "vs", "ms"))

        self.assertEqual(build_port.graphics_requested_stages(self.wanted), "ps,vs,ms")
        self.assertEqual(build_port.graphics_owned_for("ps,vs,ms"), ("lib_ps_", "lib_vs_", "lib_ms_"))
        self.run_cached_publisher()

        index_path = os.path.join(self.fspx, "index.json")
        with open(index_path, encoding="utf-8") as f:
            published = json.load(f)
        self.assertEqual(published[self.names["ms"]], {
            "stage": "ms", "fspx_code_sha256": self.hashes["ms"]})
        with open(os.path.join(self.fspx, self.names["ms"] + ".spv"), "rb") as f:
            self.assertEqual(f.read(), ("synthetic-spirv-" + self.names["ms"]).encode("ascii"))
        self.assertEqual(published[self.names["cs"]], {
            "stage": "cs", "fspx_code_sha256": self.hashes["cs"]})
        self.assertIn(self.names["ps"], published)
        self.assertIn(self.names["vs"], published)

    def test_missing_or_ps_vs_only_selection_keeps_ms_opt_in_off(self):
        self.assertEqual(build_port.graphics_requested_stages(os.path.join(self.root, "missing.json")), "ps,vs")
        self.write_selection(("ps", "vs"))
        self.assertEqual(build_port.graphics_requested_stages(self.wanted), "ps,vs")


if __name__ == "__main__":
    unittest.main()
