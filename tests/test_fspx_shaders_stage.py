"""build_port.py's FSPX step (fspx_shaders) and the index key the runtime finds FSPX programs by (fspx_code_sha256).

    python -I tests/test_fspx_shaders_stage.py

Zero game data: the "DLL" is synthetic bytes around hand-built containers (tests/test_fspx_unpack.py builds them), and
the stage tests mock the tools the way tests/test_shader_stage_resume.py does.
"""
import contextlib
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tooling"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import build_port  # noqa: E402
import fspx_unpack  # noqa: E402
import shader_meta  # noqa: E402
from test_fspx_unpack import kind0, make_dxbc, make_fspx, program, psv0  # noqa: E402

UNPACK = os.path.join(ROOT, "tooling", "fspx_unpack.py")


def container(code, state=None, **fspx):
    payload = make_fspx([(0, kind0() if state is None else state), (1, b"\1" * 20), (6, code), (7, b"\7" * 24)],
                        **fspx)
    return make_dxbc([(b"PSV0", psv0()), (b"HASH", b"\x5a" * 20), (b"ACCC", payload)])


def dll_like(*containers):
    """Containers between unrelated bytes, as a DLL holds them."""
    return b"MZ" + b"\0" * 126 + b"".join(c + b"\xcc" * 12 for c in containers)


def runtime_key(bytecode):
    """What port/src/shader_library.h's compute_program() hashes: the kind-6 bytes of a container's ACCC payload,
    or a bare program as it is."""
    if bytecode[:4] == b"DXBC":
        status, entries = fspx_unpack.parse_fspx(fspx_unpack.dxbc_parts(bytecode).get(b"ACCC", b""))
        if status:
            return None
        off, size = entries[6]
        bytecode = fspx_unpack.dxbc_parts(bytecode)[b"ACCC"][off:off + size]
    return hashlib.sha256(bytecode).hexdigest()


class IndexKey(unittest.TestCase):
    """fspx_code_sha256 is the SHA-256 of the kind-6 bytes fspx_unpack.py writes, the key the runtime computes."""

    def test_key_from_the_unpacked_program_matches_both_runtime_inputs(self):
        codes = [program(), program() + b"\0\0\x80\xbf"]       # two different kind-6 programs
        with tempfile.TemporaryDirectory() as t:
            dll = os.path.join(t, "upscaler.dll")
            boxes = [container(c) for c in codes]
            with open(dll, "wb") as f:
                f.write(dll_like(*boxes))
            out = os.path.join(t, "out")
            r = subprocess.run([sys.executable, "-I", UNPACK, dll, "--out", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            os.makedirs(os.path.join(out, "disasm"))
            for n, (code, box) in enumerate(zip(codes, boxes)):
                name = f"upscaler_{n:02d}"
                disasm = os.path.join(out, "disasm", name + ".s")
                key = shader_meta.fspx_code_sha256_file(disasm, name)
                self.assertEqual(key, hashlib.sha256(code).hexdigest())
                self.assertEqual(key, runtime_key(box))           # slot 62: the ACCC container
                self.assertEqual(key, runtime_key(code))          # slot 67: the bare kind-6 program
                meta = shader_meta.collect(disasm, name, [[]], [[]])
                self.assertEqual(meta.get("fspx_code_sha256"), key)
                self.assertNotIn("fssm_sha256", meta)

    def test_same_program_with_two_dispatch_states_gets_one_code_key_and_two_state_keys(self):
        """The review case on #1911: two containers with the same kind 6 and kind-0 entries for 32x1x1 and 64x1x1."""
        code = program()
        states = [kind0(threads=(32, 1, 1)), kind0(threads=(64, 1, 1))]
        with tempfile.TemporaryDirectory() as t:
            dll = os.path.join(t, "pair.dll")
            with open(dll, "wb") as f:
                f.write(dll_like(*(container(code, state) for state in states)))
            out = os.path.join(t, "out")
            r = subprocess.run([sys.executable, "-I", UNPACK, dll, "--out", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            metas = [shader_meta.collect(os.path.join(out, "disasm", f"pair_{n:02d}.s"), f"pair_{n:02d}", [[]], [[]])
                     for n in range(2)]
            self.assertEqual({m["fspx_code_sha256"] for m in metas}, {hashlib.sha256(code).hexdigest()})
            self.assertEqual([m["fspx_state_sha256"] for m in metas], [hashlib.sha256(s).hexdigest() for s in states])

    def test_an_fssm_blob_keeps_its_key_and_gets_no_fspx_key(self):
        with tempfile.TemporaryDirectory() as t:
            for sub, data in (("fssm", b"FSSM blob"), ("fspx", program())):
                os.makedirs(os.path.join(t, sub))
                ext = ".fssm" if sub == "fssm" else ".code"
                with open(os.path.join(t, sub, "s" + ext), "wb") as f:
                    f.write(data)
            disasm = os.path.join(t, "disasm", "s.s")
            self.assertIsNone(shader_meta.fspx_code_sha256_file(disasm, "s"))
            meta = shader_meta.collect(disasm, "s", [[]], [[]])
            self.assertEqual(meta["fssm_sha256"], hashlib.sha256(b"FSSM blob").hexdigest())
            self.assertNotIn("fspx_code_sha256", meta)

    def test_no_program_no_key(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertIsNone(shader_meta.fspx_code_sha256_file(os.path.join(t, "disasm", "x.s"), "x"))
            self.assertNotIn("fspx_code_sha256", shader_meta.collect(os.path.join(t, "disasm", "x.s"), "x", [[]], [[]]))

    def test_malformed_directory_is_not_unpacked(self):
        """A kind-6 entry past the payload is skipped, as the DLL and the runtime skip it: no program, no key."""
        code = program()
        bad = bytearray(container(code))
        accc = bytes(bad).find(b"FSPX")
        for k in range(4):                                     # the kind-6 entry's size, made too large
            at = accc + 16 + 16 * k
            if struct.unpack_from("<I", bad, at)[0] == 6:
                struct.pack_into("<I", bad, at + 8, 0xFFFFFF00)
        self.assertIsNone(runtime_key(bytes(bad)))
        with tempfile.TemporaryDirectory() as t:
            dll = os.path.join(t, "bad.dll")
            with open(dll, "wb") as f:
                f.write(dll_like(bytes(bad)))
            r = subprocess.run([sys.executable, "-I", UNPACK, dll, "--out", os.path.join(t, "out")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("FSPX status 5", r.stderr)
            self.assertFalse(os.listdir(os.path.join(t, "out", "fspx")))


class FspxStage(unittest.TestCase):
    """Which tools fspx_shaders() runs. Each mock writes what the real tool writes last."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = os.path.join(self.tmp.name, "work")
        self.game = os.path.join(self.tmp.name, "game")
        os.makedirs(self.game)
        self.dll = os.path.join(self.game, build_port.FSPX_DLL)
        self.write_dll(program())
        self.unpacked = os.path.join(self.work, "shaders", "fspx")
        self.spirv = os.path.join(self.work, "shaders", "spirv", "fspx")
        self.calls = []
        # A translation with programs publishes a non-empty index. Default the mock to one so the reuse tests keep
        # their meaning; a test that needs the empty case sets self.translations = {} itself.
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"synthetic module")}

    def write_dll(self, code):
        with open(self.dll, "wb") as f:
            f.write(dll_like(container(code)))

    def unpack(self):
        """What tooling/fspx_unpack.py writes last: its index plus one disassembled program and its code blob.

        fspx_shaders() now reads the unpack's index back (fspx_program_names) and repairs every listing before
        translating, so a mock that writes only a listing makes the step fail with "wrote no programs"; the
        unpack's program names must be the ones the fake translator publishes."""
        names = sorted(getattr(self, "translations", {}) or ["upscaler_00"])
        os.makedirs(os.path.join(self.unpacked, "disasm"), exist_ok=True)
        os.makedirs(os.path.join(self.unpacked, "fspx"), exist_ok=True)
        with open(os.path.join(self.unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump([{"name": name, "file": name + ".dxbc"} for name in names], f)
        for name in names:
            with open(os.path.join(self.unpacked, "disasm", name + ".s"), "w", encoding="utf-8") as f:
                f.write("\ts_endpgm\n")
            with open(os.path.join(self.unpacked, "fspx", name + ".code"), "wb") as f:
                f.write(bytes(4))

    def fake_repair(self, disasm, code, repaired):
        """Stand in for build_port.repair_fspx_listings (it starts a real llvm-mc); copy the listings through."""
        os.makedirs(repaired, exist_ok=True)
        for name in sorted(os.listdir(disasm)):
            shutil.copyfile(os.path.join(disasm, name), os.path.join(repaired, name))
        return 0, 0

    def fake_py(self, tool, *args):
        self.calls.append((tool, args))
        if tool == "fspx_unpack.py":
            self.unpack()
        elif tool == "rdna2spirv.py":
            os.makedirs(self.spirv, exist_ok=True)
            index = {}
            for name, (code, state, module) in getattr(self, "translations", {}).items():
                index[name] = {"stage": "cs", "fspx_code_sha256": code, "fspx_state_sha256": state}
                with open(os.path.join(self.spirv, name + ".spv"), "wb") as f:
                    f.write(module)
            with open(os.path.join(self.spirv, "index.json"), "w") as f:
                json.dump(index, f)

    def run_step(self, signature="llvm-mc 23", emulate=False, fallbacks=()):
        self.calls = []
        with mock.patch.object(build_port, "py", self.fake_py), \
                mock.patch.object(build_port, "repair_fspx_listings", side_effect=self.fake_repair), \
                mock.patch.object(build_port, "log", lambda *a: None):
            build_port.fspx_shaders(self.game, self.work, signature, emulate=emulate, fallbacks=fallbacks)
        return [tool for tool, _ in self.calls]

    def test_runs_once_then_nothing(self):
        self.assertEqual(self.run_step(), ["fspx_unpack.py", "rdna2spirv.py"])
        (_, unpack), (_, translate) = self.calls
        self.assertEqual(unpack, (self.dll, "--out", self.unpacked, "--disasm"))
        # the translator reads the listings the repair step wrote, not the unpack's own (docs/fspx.md)
        self.assertEqual(translate, (os.path.join(self.unpacked, "disasm_repaired"), "--out", self.spirv,
                                     "--no-asm", "--refusals",
                                     os.path.join(self.spirv, build_port.FSPX_REFUSALS)))
        self.assertTrue(os.path.isfile(os.path.join(self.spirv, "fspx_build.json")))
        self.assertEqual(self.run_step(), [])

    def test_a_refused_program_is_named_in_coverage_before_the_step_fails(self):
        """The translator's --refusals file and the coverage report name the program the whole-set check refuses."""
        def fake_py(tool, *args):
            if tool == "fspx_unpack.py":
                os.makedirs(os.path.join(self.unpacked, "disasm"), exist_ok=True)
                os.makedirs(os.path.join(self.unpacked, "fspx"), exist_ok=True)
                with open(os.path.join(self.unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                    json.dump([{"name": "upscaler_00", "file": "upscaler_00.dxbc"},
                               {"name": "upscaler_01", "file": "upscaler_01.dxbc"}], f)
                for name in ("upscaler_00", "upscaler_01"):
                    with open(os.path.join(self.unpacked, "disasm", name + ".s"), "w", encoding="utf-8") as f:
                        f.write("\ts_endpgm\n")
                    with open(os.path.join(self.unpacked, "fspx", name + ".code"), "wb") as f:
                        f.write(bytes(4))
            elif tool == "rdna2spirv.py":
                out = args[args.index("--out") + 1]
                os.makedirs(out, exist_ok=True)
                with open(os.path.join(out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                    json.dump({"upscaler_00": {"stage": "cs", "fspx_code_sha256": "c" * 64}}, f)
                with open(os.path.join(out, "upscaler_00.spv"), "wb") as f:
                    f.write(b"synthetic module")
                with open(args[args.index("--refusals") + 1], "w", encoding="utf-8", newline="\n") as f:
                    json.dump({"upscaler_01": "fall-through into undecodable words"}, f)

        def run_with(fake):
            with mock.patch.object(build_port, "py", fake), \
                    mock.patch.object(build_port, "repair_fspx_listings", side_effect=self.fake_repair), \
                    mock.patch.object(build_port, "log", lambda *a: None):
                build_port.fspx_shaders(self.game, self.work, "llvm-mc 23")

        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()) as err:
            run_with(fake_py)
        self.assertIn(build_port.FSPX_COVERAGE, err.getvalue())
        with open(os.path.join(self.unpacked, build_port.FSPX_COVERAGE), encoding="utf-8") as f:
            coverage = json.load(f)
        self.assertEqual(coverage["summary"],
                         {"programs": 2, "translated": 1, "untranslated": 1,
                          "reasons": {"fall-through into undecodable words": 1}})
        self.assertEqual(coverage["programs"]["upscaler_01"]["status"], "refused")
        self.assertIn("fall-through", coverage["programs"]["upscaler_01"]["reason"])
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))   # the partial set is not published

    def test_empty_translation_fails_the_step_and_is_retried(self):
        """A translation that produced no modules fails the step: the whole FSPX set is required (docs/fspx.md), so
        the stage must not latch an empty index as done (the port would silently ship with zero FSPX programs)."""
        self.translations = {}
        with self.assertRaises(SystemExit), mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.run_step()
        self.assertIn("translated after the listing repair", err.getvalue())
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "fspx_build.json")))
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"synthetic module")}
        self.assertEqual(self.run_step(), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(), [])

    def test_empty_translation_drops_stale_modules_from_the_previous_run(self):
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"stale module")}
        self.run_step()
        stale = os.path.join(self.spirv, "upscaler_00.spv")
        self.assertTrue(os.path.isfile(stale))
        # Simulate a translation that produced no modules over an existing cache: the marker still matches and the
        # old module is on disk. The step must delete the module and refuse the empty set, never ship it.
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write("{}")
        self.translations = {}
        with self.assertRaises(SystemExit), mock.patch("sys.stderr", new_callable=io.StringIO):
            self.run_step()
        self.assertFalse(os.path.exists(stale))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))

    def test_failed_disassembly_leaves_no_marker_and_is_retried(self):
        mc = os.path.join(self.tmp.name, "fake_mc.py")
        with open(mc, "w", encoding="utf-8", newline="\n") as f:
            f.write("import sys\nsys.stderr.write('error: disassembly stopped\\n')\nsys.exit(23)\n")
        real_py = build_port.py

        def run_tool(tool, *args):
            if tool == "fspx_unpack.py":
                self.calls.append((tool, args))
                real_py(tool, *args)
            elif tool == "rdna2spirv.py":
                # The real unpack names the programs it found; the fake translator must publish exactly those or
                # the step refuses the partial set.
                self.calls.append((tool, args))
                out = args[args.index("--out") + 1]
                os.makedirs(out, exist_ok=True)
                index = {name: {"stage": "cs", "fspx_code_sha256": "e" * 64}
                         for name in (build_port.fspx_program_names(self.unpacked) or set())}
                with open(os.path.join(out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                    json.dump(index, f)
                for name in index:
                    with open(os.path.join(out, name + ".spv"), "wb") as f:
                        f.write(b"synthetic module")
            else:
                self.fake_py(tool, *args)

        env = dict(LLVM_MC=f'"{sys.executable}" "{mc}"')
        with mock.patch.dict(os.environ, env), mock.patch.object(build_port, "py", run_tool), \
                mock.patch.object(build_port, "repair_fspx_listings", side_effect=self.fake_repair), \
                mock.patch.object(build_port, "log", lambda *a: None):
            with self.assertRaises(SystemExit) as error, contextlib.redirect_stderr(io.StringIO()):
                build_port.fspx_shaders(self.game, self.work, "llvm-mc 23")
            self.assertEqual(error.exception.code, 1)
            self.assertEqual([tool for tool, _ in self.calls], ["fspx_unpack.py"])
            self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
            self.assertFalse(os.path.exists(os.path.join(self.spirv, "fspx_build.json")))
            self.assertEqual(os.listdir(os.path.join(self.unpacked, "disasm")), [])
            with open(mc, "w", encoding="utf-8", newline="\n") as f:
                f.write("print('\\ts_endpgm')\n")
            self.calls = []
            build_port.fspx_shaders(self.game, self.work, "llvm-mc 23")
            self.assertEqual([tool for tool, _ in self.calls], ["fspx_unpack.py", "rdna2spirv.py"])
            self.assertTrue(os.path.isfile(os.path.join(self.spirv, "index.json")))
            self.assertTrue(os.path.isfile(os.path.join(self.spirv, "fspx_build.json")))
            self.calls = []
            build_port.fspx_shaders(self.game, self.work, "llvm-mc 23")
            self.assertEqual(self.calls, [])

    def test_missing_cached_module_is_rebuilt(self) -> None:
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"synthetic module")}
        self.assertEqual(self.run_step(), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(), [])
        module = os.path.join(self.spirv, "upscaler_00.spv")
        os.remove(module)

        self.assertEqual(self.run_step(), ["fspx_unpack.py", "rdna2spirv.py"])
        with open(module, "rb") as f:
            self.assertEqual(f.read(), b"synthetic module")
        self.assertEqual(self.run_step(), [])

    def test_no_dll_no_step(self):
        os.remove(self.dll)
        self.assertEqual(self.run_step(), [])
        self.assertFalse(os.path.exists(self.spirv))

    def test_each_input_change_runs_it_again(self):
        self.run_step()
        self.assertEqual(self.run_step(signature="llvm-mc 24"), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertIn("--wave64-emulate", self.calls[1][1])
        flags = ("--no-float-controls",)
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags),
                         ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertIn("--no-float-controls", self.calls[1][1])
        self.write_dll(program() + b"\0\0\x80\xbf")
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags),
                         ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags), [])
        with mock.patch.object(build_port, "translation_sources_digest", return_value="another translator"):
            self.assertEqual(self.run_step(signature="llvm-mc 24", emulate=True, fallbacks=flags),
                             ["fspx_unpack.py", "rdna2spirv.py"])

    def test_same_keys_with_different_modules_fail_the_step(self):
        """Same kind 6 and same kind 0 must translate to one module; otherwise the runtime could not choose."""
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"module A"),
                             "upscaler_01": ("c" * 64, "s" * 64, b"module B")}
        with self.assertRaises(SystemExit), mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.run_step()
        self.assertIn("upscaler_00", err.getvalue())
        self.assertIn("upscaler_01", err.getvalue())
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))   # nothing for the runtime to load
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "fspx_build.json")))

    def test_same_code_key_is_allowed_with_other_state_or_the_same_module(self):
        self.translations = {"upscaler_00": ("c" * 64, "s" * 64, b"module A"),
                             "upscaler_01": ("c" * 64, "t" * 64, b"module B"),     # Vonsek's case: other kind 0
                             "upscaler_02": ("c" * 64, "s" * 64, b"module A")}     # a duplicate of 00
        self.assertEqual(self.run_step(), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertTrue(os.path.isfile(os.path.join(self.spirv, "fspx_build.json")))

    def test_a_stopped_translation_leaves_no_marker_and_no_old_modules(self):
        self.run_step()
        open(os.path.join(self.spirv, "notes.txt"), "w").close()

        def stop(tool, *args):
            if tool == "rdna2spirv.py":
                raise KeyboardInterrupt
            self.fake_py(tool, *args)
        with mock.patch.object(build_port, "py", stop), \
                mock.patch.object(build_port, "repair_fspx_listings", side_effect=self.fake_repair), \
                mock.patch.object(build_port, "log", lambda *a: None):
            with self.assertRaises(KeyboardInterrupt):
                build_port.fspx_shaders(self.game, self.work, "llvm-mc 24")
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "index.json")))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "fspx_build.json")))
        self.assertFalse(os.path.exists(os.path.join(self.spirv, "upscaler.spv")))   # from the run before
        self.assertTrue(os.path.isfile(os.path.join(self.spirv, "notes.txt")))
        self.assertEqual(self.run_step(signature="llvm-mc 24"), ["fspx_unpack.py", "rdna2spirv.py"])
        self.assertEqual(self.run_step(signature="llvm-mc 24"), [])


class ShadersStageWithTheDll(unittest.TestCase):
    """The whole shaders() stage with the DLL in the game folder: the FSPX step comes after the game's shaders and
    never makes them run again."""

    def test_fspx_runs_after_the_corpus_and_alone_when_only_its_inputs_change(self):
        with tempfile.TemporaryDirectory() as t:
            work, game = os.path.join(t, "work"), os.path.join(t, "game")
            os.makedirs(game)
            dll = os.path.join(game, build_port.FSPX_DLL)
            with open(dll, "wb") as f:
                f.write(dll_like(container(program())))
            ran = []
            dxbc = os.path.join(work, "shaders", "dxbc")

            def touch(*parts):
                os.makedirs(os.path.dirname(os.path.join(*parts)), exist_ok=True)
                open(os.path.join(*parts), "w").close()

            def fake_py(tool, *args):
                ran.append(tool)
                if tool == "shader_carve.py":
                    touch(dxbc, "index.json")
                    with open(os.path.join(dxbc, "index.json"), "w") as f:
                        f.write('[{"file": "shader.dxbc"}]')
                elif tool == "fssm_unpack.py":
                    touch(dxbc, "fssm_index.json")
                    with open(os.path.join(dxbc, "fssm_index.json"), "w") as f:
                        f.write('[{"name": "shader"}]')
                elif tool == "shader_disasm.py":
                    os.makedirs(os.path.join(dxbc, "disasm"), exist_ok=True)
                    # build_port checks that every unpacked shader got a non-empty listing
                    with open(os.path.join(dxbc, "disasm", "shader.s"), "w") as f:
                        f.write("s_endpgm\n")
                elif tool == "fspx_unpack.py":
                    # The step reads the unpack's index back and repairs its listings before translating them.
                    unpacked = os.path.join(work, "shaders", "fspx")
                    os.makedirs(os.path.join(unpacked, "disasm"), exist_ok=True)
                    os.makedirs(os.path.join(unpacked, "fspx"), exist_ok=True)
                    with open(os.path.join(unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                        json.dump([{"name": "upscaler_00", "file": "upscaler_00.dxbc"}], f)
                    with open(os.path.join(unpacked, "disasm", "upscaler_00.s"), "w", encoding="utf-8") as f:
                        f.write("\ts_endpgm\n")
                    with open(os.path.join(unpacked, "fspx", "upscaler_00.code"), "wb") as f:
                        f.write(bytes(4))
                elif tool == "rdna2spirv.py":
                    out = args[args.index("--out") + 1]
                    touch(out, "index.json")
                    # Publish a runtime-addressable module for the cache reuse check.
                    with open(os.path.join(out, "index.json"), "w", encoding="utf-8", newline="\n") as f:
                        json.dump({"upscaler_00": {"stage": "cs", "fspx_code_sha256": "e" * 64}}, f)
                    with open(os.path.join(out, "upscaler_00.spv"), "wb") as f:
                        f.write(b"synthetic module")

            def fake_translate(disasm, spirv, jobs, emulate=False, fallbacks=()):
                ran.append("translate")
                touch(spirv, "index.json")

            def fake_repair(disasm, code, repaired):
                os.makedirs(repaired, exist_ok=True)
                for name in sorted(os.listdir(disasm)):
                    shutil.copyfile(os.path.join(disasm, name), os.path.join(repaired, name))
                return 0, 0

            def stage():
                ran.clear()
                with mock.patch.object(build_port, "py", fake_py), \
                        mock.patch.object(build_port, "need", lambda *a: None), \
                        mock.patch.object(build_port, "translate", fake_translate), \
                        mock.patch.object(build_port, "repair_fspx_listings", side_effect=fake_repair), \
                        mock.patch.object(build_port, "llvm_mc_signature", return_value="llvm-mc 23"), \
                        mock.patch.object(build_port, "log", lambda *a: None):
                    build_port.shaders(game, work, 2)
                return list(ran)

            self.assertEqual(stage(), ["shader_carve.py", "fssm_unpack.py", "shader_disasm.py", "translate",
                                       "fspx_unpack.py", "rdna2spirv.py"])
            self.assertEqual(stage(), [])
            with open(dll, "wb") as f:
                f.write(dll_like(container(program() + b"\0\0\x80\xbf")))
            self.assertEqual(stage(), ["fspx_unpack.py", "rdna2spirv.py"])
            self.assertTrue(os.path.isfile(os.path.join(work, "shaders", "spirv", "index.json")))
            self.assertTrue(os.path.isfile(os.path.join(work, "shaders", "spirv", "fspx", "index.json")))


class FspxCoverage(unittest.TestCase):
    """fspx_coverage(): every unpacked upscaler program's translation status, written beside the unpack.

    The FSPX step keeps the set whole or not at all, so the coverage report is what names the program it refused and
    the reason before the stage stops, and what a build's 29-of-29 result can be read from (docs/fsr_route.md). Zero
    game data: the "index" lists names and each module is four bytes."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.unpacked = os.path.join(self.tmp.name, "shaders", "fspx")
        self.spirv = os.path.join(self.tmp.name, "shaders", "spirv", "fspx")
        for path in (self.unpacked, self.spirv):
            os.makedirs(path)
        self.names = [f"upscaler_{n:02d}" for n in range(3)]
        with open(os.path.join(self.unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump([{"name": name, "file": name + ".dxbc"} for name in self.names], f)

    def write_index(self, names):
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({name: {"stage": "cs"} for name in names}, f)
        for name in names:
            with open(os.path.join(self.spirv, name + ".spv"), "wb") as f:
                f.write(b"\x03\x02\x23\x07")

    def write_refusals(self, reasons):
        with open(os.path.join(self.spirv, build_port.FSPX_REFUSALS), "w", encoding="utf-8", newline="\n") as f:
            json.dump(reasons, f)

    def test_report_names_each_program_status_and_writes_coverage_json(self):
        self.write_index(self.names[:2])
        self.write_refusals({self.names[2]: "fall-through into undecodable words"})
        report = build_port.fspx_coverage(self.unpacked, self.spirv)
        summary = report["summary"]
        self.assertEqual(summary["programs"], 3)
        self.assertEqual(summary["translated"], 2)
        self.assertEqual(summary["untranslated"], 1)
        self.assertEqual(summary["reasons"], {"fall-through into undecodable words": 1})
        self.assertEqual(report["programs"][self.names[0]], {"status": "translated", "reason": ""})
        self.assertEqual(report["programs"][self.names[2]]["status"], "refused")
        self.assertIn("fall-through", report["programs"][self.names[2]]["reason"])
        with open(os.path.join(self.unpacked, build_port.FSPX_COVERAGE), encoding="utf-8") as f:
            on_disk = json.load(f)
        self.assertEqual(on_disk["summary"], summary)
        self.assertEqual(on_disk["programs"], report["programs"])

    def test_an_index_entry_without_its_module_is_not_translated(self):
        """The runtime looks the module up beside the index; an entry without one creates no pipeline."""
        self.write_index(self.names[:2])
        os.remove(os.path.join(self.spirv, self.names[1] + ".spv"))
        report = build_port.fspx_coverage(self.unpacked, self.spirv)
        self.assertEqual(report["programs"][self.names[1]]["status"], "untranslated")
        self.assertEqual(report["programs"][self.names[1]]["reason"], "reason not recorded")
        self.assertEqual(report["summary"]["translated"], 1)
        self.assertEqual(report["summary"]["untranslated"], 2)

    def test_no_unpack_index_is_not_a_report(self):
        os.remove(os.path.join(self.unpacked, "index.json"))
        self.assertIsNone(build_port.fspx_coverage(self.unpacked, self.spirv))


class FspxCacheComplete(unittest.TestCase):
    """Pin fspx_cache_complete(), the predicate that decides whether the FSPX outputs can be reused."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.spirv = os.path.join(self.tmp.name, "shaders", "spirv", "fspx")
        os.makedirs(self.spirv)
        # fspx_cache_complete() now takes the unpack folder as well as the translation folder: reuse requires
        # every program the unpack listed to be in the index with its module (docs/fspx.md).
        self.unpacked = os.path.join(self.tmp.name, "shaders", "fspx")
        os.makedirs(self.unpacked)
        with open(os.path.join(self.unpacked, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump([{"name": "upscaler_00", "file": "upscaler_00.dxbc"}], f)

    def write_index(self, text):
        with open(os.path.join(self.spirv, "index.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def complete(self):
        return build_port.fspx_cache_complete(self.unpacked, self.spirv)

    def test_empty_index_is_not_complete(self):
        self.write_index("{}")
        self.assertFalse(self.complete())

    def test_missing_index_is_not_complete(self):
        self.assertFalse(self.complete())

    def test_malformed_index_is_not_complete(self):
        self.write_index("{not json")
        self.assertFalse(self.complete())

    def test_non_mapping_indexes_are_not_complete(self):
        for text in ("[]", "null"):
            with self.subTest(text=text):
                self.write_index(text)
                self.assertFalse(self.complete())

    def test_one_entry_with_its_module_is_complete(self):
        self.write_index(json.dumps({"upscaler_00": {"stage": "cs", "fspx_code_sha256": "e" * 64}}))
        with open(os.path.join(self.spirv, "upscaler_00.spv"), "wb") as f:
            f.write(b"synthetic module")
        self.assertTrue(self.complete())

    def test_one_entry_without_its_module_is_not_complete(self):
        self.write_index(json.dumps({"upscaler_00": {"stage": "cs", "fspx_code_sha256": "e" * 64}}))
        self.assertFalse(self.complete())

    def test_existing_module_without_runtime_lookup_metadata_is_not_complete(self):
        with open(os.path.join(self.spirv, "upscaler_00.spv"), "wb") as f:
            f.write(b"synthetic module")
        for metadata in ({"error": "synthetic metadata failure"},
                         {"stage": "cs", "fspx_code_sha256": "abbreviated"},
                         {"stage": "ps", "fspx_code_sha256": "e" * 64}):
            with self.subTest(metadata=metadata):
                self.write_index(json.dumps({"upscaler_00": metadata}))
                self.assertFalse(self.complete())


if __name__ == "__main__":
    unittest.main()
