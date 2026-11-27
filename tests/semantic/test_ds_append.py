"""DS_APPEND on LDS: increment by active lanes and broadcast the old counter.

Standard-library-only semantic fixture: the real translator emits the SPIR-V and the
workgroup interpreter runs every lane; no GPU, LLVM, or game data is used.

RDNA2 ISA: LDS DS_APPEND uses the aligned instruction offset, adds popcount(EXEC)
once per wave, and returns the pre-operation value to each active lane. GDS uses a
different address space and remains explicitly unsupported here.
"""
import os
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TOOLS_DIR = os.environ.get("PALMETTO_TOOLS_DIR") or str(ROOT / "tooling")
for path in (str(HERE), TOOLS_DIR):
    if path not in sys.path:
        sys.path.insert(0, path)

import rdna2spirv  # noqa: E402
import support  # noqa: E402
from test_loop_ballot_barrier import run_compute_workgroup  # noqa: E402

SEED = 0x10203040
INACTIVE = 0xDEADBEEF
OUT_VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]


def fssm(wave, threads):
    """Minimal compute FSSM header: 4 user SGPRs, 4 KiB LDS, and explicit wave size."""
    blob = bytearray(0x148)
    struct.pack_into("<I", blob, 0xD4, 0x20 if wave == 32 else 0)
    struct.pack_into("<4I", blob, 0x138, 0x40008, threads, 1, 1)
    return bytes(blob)


def translate(wave, threads, emulate=False):
    with patch.object(rdna2spirv, "WAVE64_EMULATE", emulate):
        return support.translate_fixture(f"cs_w{wave}_ds_append", fssm=fssm(wave, threads))


class TestDsAppend(unittest.TestCase):
    def test_partial_exec_count_and_old_value_broadcast(self):
        # EXEC is a proper subset, so this checks popcount(EXEC), not a hard-coded wave width.
        # The wave32 case has two waves sharing LDS; their atomic order is intentionally not prescribed.
        for wave, threads, emulate, active_count in ((32, 64, False, 13), (64, 64, False, 40),
                                                      (64, 64, True, 40)):
            with self.subTest(wave=wave, emulate=emulate):
                spvasm = translate(wave, threads, emulate)
                subgroup_size = 32 if emulate or wave == 32 else wave
                memory = run_compute_workgroup(
                    spvasm,
                    threads,
                    subgroup_size,
                    push_constants=OUT_VSHARP + [0] * 28,
                    memory={},
                )
                wave_old = []
                waves = threads // wave
                for w in range(waves):
                    start = w * wave
                    active = {memory[8 * lane] for lane in range(start, start + active_count)}
                    self.assertEqual(len(active), 1, f"wave {w} active lanes did not receive one old value")
                    wave_old.extend(active)
                    for lane in range(start, start + active_count):
                        self.assertEqual(memory[8 * lane], memory[8 * start], f"active result, lane {lane}")
                    for lane in range(start + active_count, start + wave):
                        self.assertEqual(memory[8 * lane], INACTIVE, f"inactive destination, lane {lane}")
                self.assertEqual(set(wave_old), {SEED + n * active_count for n in range(waves)})
                final = SEED + waves * active_count
                for lane in range(threads):
                    self.assertEqual(memory[8 * lane + 4], final, f"final counter, lane {lane}")

    def test_gds_misaligned_and_invalid_destination_remain_refused(self):
        header = fssm(32, 32)
        for line in (
            "ds_append v3 offset:4 gds",
            "ds_append v3 offset:2",
            "ds_append v[3:4] offset:4",
        ):
            with self.subTest(line=line), self.assertRaises(rdna2spirv.Unsupported):
                support.translate_lines("cs_ds_append_refusal", [line, "s_endpgm"], fssm=header)


if __name__ == "__main__":
    unittest.main()
