"""Tests for tooling/bcpack_predictor.py: the local BCPack predictor generator.

All inputs are synthetic: the mixed BC6H/BC7 TTPF pools are built in a temp
directory from scratch with the committed rANS encoder helpers in
tests/test_bcpack_history_calib.py, so no game file, game-derived table, or
package path is used. Writes go to temp directories outside the repository; the
one test that names a repository path asserts the tool refuses it.

    python -I tests/test_bcpack_predictor.py
"""
import io
import json
import os
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tooling")
TESTS = os.path.join(ROOT, "tests")
for path in (TOOLS, TESTS):
    if path not in sys.path:
        sys.path.insert(0, path)

import bcpack  # noqa: E402
import bcpack_history_calib as calib  # noqa: E402
import bcpack_predictor as predictor  # noqa: E402
import bcpack_tables_bptc as bptc  # noqa: E402
import test_bcpack_history_calib as history_tests  # noqa: E402
import ttpf_mips as ttpf  # noqa: E402


# ---------------------------------------------------------------------------
# Synthetic package
# ---------------------------------------------------------------------------

def _write_bc7_pool(path):
    """One 64x64 two-mip BC7 texture: mip 0 is a BCPack chunk referencing history entry 91, mip 1 is zlib."""
    width, height, mips_n = 64, 64, 2
    layout = ttpf.surface_layout(width, height, 98, mips_n, ttpf.tile_bytes(4))
    linear = []
    for m, mip in enumerate(layout["mips"]):
        buffer = bytearray()
        scale = 1 if m == 0 else 2
        half = 0 if m == 0 else 3
        for by in range(mip["blocks_h"]):
            for bx in range(mip["blocks_w"]):
                red = min(254, (32 + bx * 6 * scale + half) & 0xFE)
                green = min(254, (80 + by * 6 * scale + half) & 0xFE)
                blue = 140
                acc = 1 << 6
                position = 7
                for value in (red >> 1, red >> 1, green >> 1, green >> 1, blue >> 1, blue >> 1, 127, 127):
                    acc |= (value & 0x7F) << position
                    position += 7
                acc |= (red & 1) << position
                acc |= (red & 1) << (position + 1)
                buffer += acc.to_bytes(16, "little")
        linear.append(bytes(buffer))
    tail = calib.retile_mip(linear[1], layout, layout["mips"][1])
    top = calib.retile_mip(linear[0], layout, layout["mips"][0])
    secret_endpoints = tuple([0x40, 0x60, 0x90, 0xFE, 0x40, 0x60, 0x90, 0xFE] * 3)
    raw_top = history_tests._encode_bc7_mode6_tiled(
        top, secret_entry=91, secret_ep=secret_endpoints, secret_ix=tuple([0] * 32), ref_count=8
    )
    base = ttpf.align(16 + 24 + 32, 4096)
    history_tests._write_custom_rds5_pool(
        path,
        [history_tests._pack_etpf(width, height, 1, 1, 98, base, mode=4)],
        [(8, layout["mips"][1]["region"], zlib.compress(tail, 9)), (7, layout["mips"][0]["region"], raw_top)],
    )


def _write_bc6h_pool(path):
    """One 64x64 two-mip BC6H texture: mip 0 is a BCPack chunk referencing history entry 55, mip 1 is zlib."""
    width, height, mips_n = 64, 64, 2
    layout = ttpf.surface_layout(width, height, 95, mips_n, ttpf.tile_bytes(4))
    linear = []
    for m, mip in enumerate(layout["mips"]):
        buffer = bytearray()
        scale = 1 if m == 0 else 2
        half = 0 if m == 0 else 8
        for by in range(mip["blocks_h"]):
            for bx in range(mip["blocks_w"]):
                red = 0x3800 + bx * 16 * scale + half
                green = 0x3A00 + by * 16 * scale + half
                blue = 0x3C00
                fields = [[value, 0, 0, 0] for value in (red, green, blue)]
                plan, _ = bcpack._bc6h_plan(0x0F)
                acc = 0x0F
                for channel, component, bit, position in plan:
                    if channel >= 0 and ((fields[channel][component] >> bit) & 1):
                        acc |= 1 << position
                buffer += acc.to_bytes(16, "little")
        linear.append(bytes(buffer))
    tail = calib.retile_mip(linear[1], layout, layout["mips"][1])
    top = calib.retile_mip(linear[0], layout, layout["mips"][0])
    secret_endpoints = (
        (0x1800, 0x200, 0x20, 0x20),
        (0x2800, 0x200, 0x20, 0x20),
        (0x3000, 0x200, 0x20, 0x20),
    )
    raw_top = history_tests._encode_bc6h_mode14_tiled(
        top, secret_entry=55, secret_eh=secret_endpoints, secret_ih=(0,) * 16, ref_count=8
    )
    base = ttpf.align(16 + 24 + 32, 4096)
    history_tests._write_custom_rds5_pool(
        path,
        [history_tests._pack_etpf(width, height, 1, 1, 95, base, mode=4)],
        [(8, layout["mips"][1]["region"], zlib.compress(tail, 9)), (6, layout["mips"][0]["region"], raw_top)],
    )


def build_synthetic_package(root):
    """A package-shaped folder whose data/ttdb holds the two synthetic pools."""
    folder = os.path.join(root, "data", "ttdb", "synthetic")
    os.makedirs(folder, exist_ok=True)
    _write_bc6h_pool(os.path.join(folder, "mixed_bc6h.ttpf"))
    _write_bc7_pool(os.path.join(folder, "mixed_bc7.ttpf"))
    return root


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run_cli(argv):
    """Run the tool's CLI, returning (exit status, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        result = predictor.main(argv)
    return result, out.getvalue(), err.getvalue()


def _read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def _write_json(path, document):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(document, handle)


def _synthetic_file(tmp, name="predictor.json"):
    """Write the deterministic synthetic fixture the way the self-test does; return its path."""
    tables = predictor.self_test_tables()
    data, _checksum = predictor.encode_predictor(tables)
    path = os.path.join(tmp, name)
    with open(path, "wb") as handle:
        handle.write(data)
    return path


class SelfTestTest(unittest.TestCase):
    def test_fixture_is_deterministic_and_round_trips(self):
        first = predictor.encode_predictor(predictor.self_test_tables())
        second = predictor.encode_predictor(predictor.self_test_tables())
        self.assertEqual(first, second, "the synthetic fixture must be byte-identical between runs")
        data, checksum = first
        self.assertTrue(data.endswith(b"\n"))
        document = json.loads(data)
        self.assertEqual(document["format"], predictor.FORMAT_ID)
        self.assertEqual(document["formatVersion"], predictor.FORMAT_VERSION)
        self.assertEqual(document["checksum"], {"algorithm": "sha256", "value": checksum})
        self.assertEqual(predictor.predictor_checksum(predictor._table_members(document)), checksum)

    def test_self_test_cli_writes_a_valid_fixture_outside_the_repository(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, "first.json")
            second = os.path.join(tmp, "second.json")
            status, out, err = _run_cli(["self-test", "--out", first])
            self.assertEqual(status, 0, err)
            self.assertIn("Self-test predictor", out)
            self.assertEqual(err, "")
            status, _out, err = _run_cli(["self-test", "--out", second, "--quiet"])
            self.assertEqual(status, 0, err)
            self.assertEqual(_read_bytes(first), _read_bytes(second), "two self-tests must give the same bytes")
            report = predictor.validate_predictor(first)
            self.assertFalse(report["legacy"])
            self.assertEqual(report["format"], predictor.FORMAT_ID)
            self.assertEqual(report["checksum"], predictor.predictor_checksum(
                predictor._table_members(json.loads(_read_bytes(first)))))

    def test_self_test_uses_a_sparse_fixture(self):
        report = predictor.validate_document(predictor.self_test_tables())[1]
        self.assertLess(report["bc6h"]["endpointHistory_rows"], 256)
        self.assertLess(report["bc7"]["historyEndpoints_rows"], 256)
        self.assertGreater(report["bc7"]["indexKnownSlots"], 0)


class GenerateTest(unittest.TestCase):
    def test_generate_from_a_synthetic_package_is_deterministic_and_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = build_synthetic_package(os.path.join(tmp, "package"))
            first = os.path.join(tmp, "predictor-a.json")
            second = os.path.join(tmp, "predictor-b.json")
            status, out, err = _run_cli(
                ["generate", package, "--out", first, "--limit", "5", "--json"]
            )
            self.assertEqual(status, 0, err)
            report = json.loads(out)
            self.assertEqual(report["mode"], "generate")
            self.assertTrue(report["wrote"])
            self.assertEqual(report["format"], predictor.FORMAT_ID)
            self.assertEqual(report["pools"], 2)
            status, _out, err = _run_cli(
                ["generate", package, "--out", second, "--limit", "5", "--quiet"]
            )
            self.assertEqual(status, 0, err)
            self.assertEqual(_read_bytes(first), _read_bytes(second), "the same inputs must give the same bytes")

            document = json.loads(_read_bytes(first))
            self.assertEqual(set(document), {"format", "formatVersion", "checksum", "bc6h", "bc7"})
            for codec in ("bc6h", "bc7"):
                self.assertEqual(len(document[codec]["endpointHistory" if codec == "bc6h" else "historyEndpoints"]), 256)

            # The file is consumable by the committed offline decoder, not just by this tool's validator.
            endpoints = bptc.load_bc6h(first, with_confirmed_bits=True)
            self.assertEqual(len(endpoints[0]), 256)
            loaded = bptc.load_bc7(first)
            self.assertEqual(len(loaded["history_endpoints"]), 256)

            validated = predictor.validate_predictor(first)
            self.assertFalse(validated["legacy"])
            self.assertEqual(validated["files"], 1)
            coverage = validated["tables"]
            self.assertEqual(coverage["bc6h"]["endpointHistory_rows"], 256)
            self.assertEqual(coverage["bc7"]["historyEndpoints_rows"], 256)
            # Real calibration happened: the synthetic chunk's referenced rows carry a nonzero class.
            self.assertGreater(
                coverage["bc6h"]["endpointClass_provisional"] + coverage["bc6h"]["endpointClass_last_resort"], 0
            )
            self.assertGreater(
                coverage["bc7"]["indexKnownSlots"] + coverage["bc6h"]["indexConfirmedSlots"], 0
            )
            # No package path leaks into the predictor.
            self.assertNotIn(os.path.abspath(package).encode(), _read_bytes(first))

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = build_synthetic_package(os.path.join(tmp, "package"))
            out = os.path.join(tmp, "dry", "predictor.json")
            status, stdout, err = _run_cli(
                ["generate", package, "--out", out, "--limit", "5", "--dry-run"]
            )
            self.assertEqual(status, 0, err)
            self.assertIn("nothing was written", stdout)
            self.assertFalse(os.path.exists(out))
            self.assertFalse(os.path.exists(os.path.dirname(out)), "--dry-run must not create the parent folder")

    def test_generate_refuses_to_write_inside_the_repository(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = build_synthetic_package(os.path.join(tmp, "package"))
            for name in ("predictor-in-repo.json", os.path.join("tooling", "predictor-in-repo.json")):
                out = os.path.join(ROOT, name)
                status, _out, err = _run_cli(
                    ["generate", package, "--out", out, "--limit", "1"]
                )
                self.assertEqual(status, 2)
                lines = err.splitlines()
                self.assertEqual(len(lines), 1, err)
                self.assertTrue(lines[0].startswith("ERROR: "), err)
                self.assertIn("outside the repository", lines[0])
                self.assertFalse(os.path.exists(out))
                self.assertNotIn("Traceback", err)

    def test_generate_refuses_a_missing_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            status, _out, err = _run_cli(
                ["generate", os.path.join(tmp, "absent"), "--out", os.path.join(tmp, "p.json")]
            )
            self.assertEqual(status, 2)
            self.assertEqual(len(err.splitlines()), 1, err)
            self.assertIn("absent", err)

    def test_generate_requires_an_output_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                predictor.main(["generate", tmp])
            self.assertEqual(raised.exception.code, 2)


class ValidateTest(unittest.TestCase):
    def test_validate_accepts_the_self_test_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = _synthetic_file(tmp)
            status, out, err = _run_cli(["validate", path, "--json"])
            self.assertEqual(status, 0, err)
            report = json.loads(out)
            self.assertTrue(report["valid"])
            self.assertFalse(report["legacy"])
            self.assertEqual(report["checksum"], json.loads(_read_bytes(path))["checksum"]["value"])

    def test_validate_accepts_legacy_combined_and_directory_forms(self):
        tables = predictor.self_test_tables()
        with tempfile.TemporaryDirectory() as tmp:
            combined = os.path.join(tmp, "legacy.json")
            _write_json(combined, tables)
            report = predictor.validate_predictor(combined)
            self.assertTrue(report["legacy"])
            self.assertIsNone(report["format"])

            folder = os.path.join(tmp, "dir")
            os.makedirs(folder)
            for codec in ("bc6h", "bc7"):
                _write_json(os.path.join(folder, f"{codec}.json"), tables[codec])
            report = predictor.validate_predictor(folder)
            self.assertTrue(report["legacy"])
            self.assertEqual(report["files"], 2)
            self.assertIn("bc6h", report["tables"])
            self.assertIn("bc7", report["tables"])

    def test_validate_accepts_a_single_codec_file_at_the_root(self):
        tables = predictor.self_test_tables()
        for codec in ("bc6h", "bc7"):
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, f"{codec}.json")
                _write_json(path, tables[codec])
                report = predictor.validate_predictor(path)
                self.assertTrue(report["legacy"])
                self.assertIn(codec, report["tables"])
                self.assertNotIn("bc7" if codec == "bc6h" else "bc6h", report["tables"])

    def test_validate_never_rewrites_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = _synthetic_file(tmp)
            before = _read_bytes(path)
            status, _out, err = _run_cli(["validate", path, "--quiet"])
            self.assertEqual(status, 0, err)
            self.assertEqual(_read_bytes(path), before)

    def test_validate_rejects_a_version_mismatch(self):
        tables = predictor.self_test_tables()
        data, _checksum = predictor.encode_predictor(tables)
        document = json.loads(data)
        document["formatVersion"] = predictor.FORMAT_VERSION + 1
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "v2.json")
            _write_json(path, document)
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("unsupported_version", err)
            self.assertEqual(len(err.splitlines()), 1, err)

    def test_validate_rejects_a_wrong_format_id(self):
        document = json.loads(predictor.encode_predictor(predictor.self_test_tables())[0])
        document["format"] = "someone-elses-predictor"
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "other.json")
            _write_json(path, document)
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("unsupported_format", err)

    def test_validate_rejects_a_tampered_table_by_checksum(self):
        document = json.loads(predictor.encode_predictor(predictor.self_test_tables())[0])
        document["bc7"]["historyEndpoints"]["0"][0] ^= 1
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "tampered.json")
            _write_json(path, document)
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("checksum_mismatch", err)

    def test_validate_rejects_truncated_and_non_json_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            data, _checksum = predictor.encode_predictor(predictor.self_test_tables())
            cases = (("truncated.json", data[:len(data) // 2]), ("junk.json", b"{not json"),
                     ("utf16.json", data.decode("utf-8").encode("utf-16")))
            for name, payload in cases:
                path = os.path.join(tmp, name)
                with open(path, "wb") as handle:
                    handle.write(payload)
                status, _out, err = _run_cli(["validate", path])
                self.assertEqual(status, 2, name)
                self.assertIn("malformed", err)
                self.assertEqual(len(err.splitlines()), 1, err)
                self.assertNotIn("Traceback", err)

    def test_validate_rejects_out_of_range_values(self):
        cases = {
            "endpoint": lambda doc: doc["bc6h"]["endpointHistory"]["0"][1].__setitem__(1, 5000),
            "depth": lambda doc: doc["bc6h"]["endpointConfirmedBits"]["0"][0].__setitem__(0, 17),
            "class": lambda doc: doc["bc6h"]["endpointClass"].__setitem__("7", 4),
            "last_resort": lambda doc: doc["bc7"]["historyEndpointLastResort"][0].__setitem__(0, 300),
        }
        for name, tamper in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp:
                document = json.loads(predictor.encode_predictor(predictor.self_test_tables())[0])
                tamper(document)
                path = os.path.join(tmp, "bad.json")
                _write_json(path, document)
                status, _out, err = _run_cli(["validate", path])
                self.assertEqual(status, 2)
                self.assertIn("out_of_range", err)

    def test_validate_rejects_bad_row_lengths(self):
        document = json.loads(predictor.encode_predictor(predictor.self_test_tables())[0])
        document["bc6h"]["indexHistory"]["0"] = [0] * 15
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "short.json")
            _write_json(path, document)
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("bad_length", err)

    def test_validate_rejects_a_confirmed_depth_above_the_recorded_depth(self):
        document = json.loads(predictor.encode_predictor(predictor.self_test_tables())[0])
        document["bc7"]["historyEndpointBits"]["0"][0] = 2
        document["bc7"]["historyEndpointConfirmedBits"]["0"][0] = 8
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "depth.json")
            _write_json(path, document)
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("out_of_range", err)
            self.assertIn("exceeds", err)

    def test_validate_rejects_an_oversize_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "huge.json")
            with open(path, "wb") as handle:
                handle.seek(predictor.MAX_FILE_BYTES)
                handle.write(b" ")
            status, _out, err = _run_cli(["validate", path])
            self.assertEqual(status, 2)
            self.assertIn("too_large", err)

    def test_validate_reports_a_missing_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            status, _out, err = _run_cli(["validate", os.path.join(tmp, "absent.json")])
            self.assertEqual(status, 2)
            self.assertIn("missing", err)
            self.assertEqual(len(err.splitlines()), 1, err)


class PolicyGuardTest(unittest.TestCase):
    def test_output_may_not_overwrite_an_input_pool(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pool.ttpf")
            with open(path, "wb") as handle:
                handle.write(b"TTPF")
            with self.assertRaises(predictor.PredictorError) as raised:
                predictor.check_output_path(path, [path])
            self.assertEqual(raised.exception.reason, "usage")

    def test_output_may_not_be_a_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(predictor.PredictorError) as raised:
                predictor.check_output_path(tmp, [])
            self.assertEqual(raised.exception.reason, "usage")

    def test_tool_source_contains_no_package_path_or_derived_table(self):
        # The module may name only its own files; a committed game path or a prefilled table would be a policy break.
        with open(predictor.__file__, encoding="utf-8") as handle:
            source = handle.read()
        self.assertNotIn("GTA6", source)
        self.assertNotIn("CYBERLEEK", source)
        self.assertNotIn("Torrent", source)


if __name__ == "__main__":
    unittest.main()
