"""Recover only a measured `s_waitcnt lgkmcnt(0)` in an undecodable listing gap.

The current 668-program request-union scan found 30 reachable one-dword gaps whose complete RDNA2 decode is exactly
`s_waitcnt lgkmcnt(0)`. The translator's existing `NOOPS` contract drops this instruction; the recovery only restores
that exact word and adds no wait/barrier behavior. Other wait counts, wider gaps and undecodable words stay refused. The
fixture is synthetic and runs through the CPU SPIR-V interpreter; no GPU, game program or game bytes are used.

    python -I tests/semantic/test_s_waitcnt_lgkm0_gap.py -v
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import mrt0_words, rdna2spirv, translate_fixture  # noqa: E402

CODE_START = 0x200
NAME = "ps_s_waitcnt_lgkm0_gap"
WAIT_LGKM0 = bytes.fromhex("7f c0 8c bf")
WAIT_VMCNT0 = bytes.fromhex("70 3f 8c bf")
WAIT_LGKM1 = bytes.fromhex("7f c1 8c bf")


def fssm(wait_word=WAIT_LGKM0):
    """Synthetic wave32 FSSM containing one hidden waitcnt word after the listed s_nop."""
    blob = bytearray(CODE_START)
    struct.pack_into("<I", blob, 0xD4, 0x20)
    struct.pack_into("<4I", blob, 0x138, 0x40010, 1, 1, 1)
    blob.extend(bytes.fromhex("00 00 80 bf"))  # listed s_nop
    blob.extend(wait_word)
    blob.extend(bytes.fromhex("04 02 00 7e"))  # listed v_mov_b32_e32 v0, s4
    blob.extend(bytes.fromhex("0f 18 00 f8 00 00 00 00"))  # export mrt0
    blob.extend(bytes.fromhex("00 00 81 bf"))  # s_endpgm
    return bytes(blob)


class WaitcntGapTest(unittest.TestCase):
    def test_exact_lgkmcnt_zero_wait_recovers_and_executes(self):
        asm = translate_fixture(NAME, fssm=fssm())
        push = [0, 0, 0, 0, 0x12345678, 0xDEADBEEF, 0, 0]
        self.assertEqual(mrt0_words(asm, push_constants=push), [0x12345678] * 4)

    def test_other_wait_counts_remain_refused(self):
        for wait_word in (WAIT_VMCNT0, WAIT_LGKM1):
            with self.subTest(wait_word=wait_word.hex()), self.assertRaisesRegex(
                    rdna2spirv.Unsupported, "fall-through into undecodable words"):
                translate_fixture(NAME, fssm=fssm(wait_word))


if __name__ == "__main__":
    unittest.main()
