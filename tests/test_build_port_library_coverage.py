"""build_port.py: the pipeline library's compute coverage report and its wanted compute program list.

The compute library step (library_shaders) publishes the title's precompiled compute pipelines by the SHA-256 of
their kind-6 bytes. This covers what it reports about them: which library programs translated, which the translator
refused and why, whether each one is published in the shader corpus the runtime reads, and that a wanted program
list is part of what the extraction and translation depend on. No game data: the fixtures are empty sidecars.

    python -I tests/test_build_port_library_coverage.py
"""

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
import build_port  # noqa: E402


def write_json(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f)


class LibraryCoverageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = self.tmp.name
        self.extracted = os.path.join(self.work, "pipeline_library")
        self.staged = os.path.join(self.work, "spirv", "pipeline_library")
        self.fspx = os.path.join(self.work, "spirv", "fspx")
        for path in (self.extracted, self.staged, self.fspx):
            os.makedirs(path)
        # The extraction's index names every program it wrote; the translator's index names the ones it translated.
        write_json(os.path.join(self.extracted, "index.json"),
                   [{"file": name + ".dxbc"} for name in ("lib_cs_00000", "lib_cs_00001", "lib_cs_00002")])
        write_json(os.path.join(self.staged, "index.json"),
                   {"lib_cs_00000": {"stage": "cs"}, "lib_cs_00002": {"stage": "cs"}})
        for name in ("lib_cs_00000", "lib_cs_00002"):
            with open(os.path.join(self.staged, name + ".spv"), "wb") as f:
                f.write(b"\x03\x02\x23\x07")
        write_json(os.path.join(self.staged, build_port.LIBRARY_REFUSALS),
                   {"lib_cs_00001": "fall-through into undecodable words"})
        # Only one of the two translated programs is published in the corpus the runtime reads.
        write_json(os.path.join(self.fspx, "index.json"), {"lib_cs_00000": {"stage": "cs"}})
        with open(os.path.join(self.fspx, "lib_cs_00000.spv"), "wb") as f:
            f.write(b"\x03\x02\x23\x07")

    def test_report_names_each_program_status_and_writes_coverage_json(self):
        report = build_port.library_coverage(self.extracted, self.staged, self.fspx)
        self.assertIsNotNone(report)
        summary = report["summary"]
        self.assertEqual(summary["extracted"], 3)
        self.assertEqual(summary["translated"], 2)
        self.assertEqual(summary["untranslated"], 1)
        self.assertEqual(summary["published"], 1)
        self.assertEqual(summary["reasons"], {"fall-through into undecodable words": 1})
        programs = report["programs"]
        self.assertEqual(programs["lib_cs_00000"], {"status": "translated", "reason": "", "published": True})
        self.assertFalse(programs["lib_cs_00002"]["published"])
        self.assertEqual(programs["lib_cs_00001"]["status"], "refused")
        self.assertIn("fall-through", programs["lib_cs_00001"]["reason"])
        on_disk_path = os.path.join(self.extracted, build_port.LIBRARY_COVERAGE)
        with open(on_disk_path, encoding="utf-8") as f:
            on_disk = json.load(f)
        self.assertEqual(on_disk["summary"], summary)
        self.assertEqual(on_disk["programs"], programs)

    def test_a_program_with_no_refusal_reason_is_reported_untranslated(self):
        os.remove(os.path.join(self.staged, build_port.LIBRARY_REFUSALS))
        report = build_port.library_coverage(self.extracted, self.staged)
        self.assertEqual(report["programs"]["lib_cs_00001"]["status"], "untranslated")
        self.assertEqual(report["programs"]["lib_cs_00001"]["reason"], "reason not recorded")
        self.assertFalse(report["programs"]["lib_cs_00001"]["published"])
        self.assertEqual(report["summary"]["published"], 0)

    def test_a_wanted_extraction_reports_its_requested_programs(self):
        write_json(os.path.join(self.extracted, "wanted.json"), {"wanted": {"cs": 3}, "found": {"cs": 3}})
        with unittest.mock.patch("build_port.log") as log:
            report = build_port.library_coverage(self.extracted, self.staged, self.fspx)
        self.assertTrue(report["summary"]["wanted_list"])
        lines = [str(call.args[0]) for call in log.call_args_list if call.args]
        self.assertTrue(any("refused by name: lib_cs_00001" in line for line in lines), lines)

    def test_no_extraction_index_is_not_a_report(self):
        os.remove(os.path.join(self.extracted, "index.json"))
        self.assertIsNone(build_port.library_coverage(self.extracted, self.staged, self.fspx))


class LibraryWantedListTests(unittest.TestCase):
    def test_a_build_without_a_list_keeps_its_recorded_settings(self):
        # Adding the option must not invalidate the marker of a work folder built before it existed.
        plain = build_port.library_extraction_settings("package", "llvm")
        self.assertNotIn("wanted", json.loads(plain))
        self.assertEqual(plain, build_port.library_extraction_settings("package", "llvm", None))
        selected = build_port.library_extraction_settings("package", "llvm", "digest")
        self.assertEqual(json.loads(selected)["wanted"], "digest")
        self.assertNotIn("wanted", json.loads(build_port.library_settings("package", "llvm", False, ())))
        self.assertEqual(json.loads(build_port.library_settings("package", "llvm", False, (), "digest"))["wanted"],
                         "digest")

    def test_the_wanted_list_path_is_environment_overridable(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(build_port.compute_programs_path(tmp), os.path.join(tmp, build_port.COMPUTE_PROGRAMS_FILE))
            named = os.path.join(tmp, "measured.json")
            previous = os.environ.get(build_port.COMPUTE_PROGRAMS_ENV)
            os.environ[build_port.COMPUTE_PROGRAMS_ENV] = named
            try:
                self.assertEqual(build_port.compute_programs_path(tmp), os.path.abspath(named))
            finally:
                if previous is None:
                    del os.environ[build_port.COMPUTE_PROGRAMS_ENV]
                else:
                    os.environ[build_port.COMPUTE_PROGRAMS_ENV] = previous

    def test_the_digest_is_none_without_a_list_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = os.path.join(tmp, "missing.json")
            self.assertIsNone(build_port.wanted_list_digest(missing))
            self.assertIsNone(build_port.wanted_list_digest(None))
            empty = os.path.join(tmp, "empty.json")
            write_json(empty, {"cs": []})
            digest = build_port.wanted_list_digest(empty)
            self.assertRegex(digest, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
