"""Recover only a measured scalar-register move hidden in an undecodable listing gap.

The request-union scan found 12 fall-through programs whose first reachable gap is one valid 4-byte `s_mov_b32` from an ordinary
SGPR to another ordinary SGPR. The translator already has exact semantics for that move; the rule only restores this
word from the native bytes, not special-register, immediate, wider, or other gaps. The fixture is synthetic and is run
through the CPU SPIR-V interpreter; no GPU, game program, or game bytes are used.

    python -I tests/semantic/test_smov_sgpr_gap.py -v
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
NAME = "ps_smov_sgpr_gap"


def fssm(source=5):
    """Synthetic wave32 FSSM: eight user SGPRs, with a generated SOP1 word at code start."""
    blob = bytearray(CODE_START)
    struct.pack_into("<I", blob, 0xD4, 0x20)
    struct.pack_into("<4I", blob, 0x138, 0x40010, 1, 1, 1)
    # SOP1 s_mov_b32 s4, s5 for source=5. Other test sources are inline/special, not in the accepted class.
    word = 0xBE800000 | (3 << 8) | (4 << 16) | source
    blob.extend(bytes.fromhex("00 00 80 bf"))  # listed s_nop: make the gap follow reachable code
    blob.extend(struct.pack("<I", word))
    blob.extend(bytes.fromhex("04 02 00 7e 0f 18 00 f8 00 00 00 00 00 00 81 bf"))
    return bytes(blob)


class SgprMoveGapTest(unittest.TestCase):
    def test_plain_sgpr_move_is_recovered_and_runs(self):
        asm = translate_fixture(NAME, fssm=fssm())
        push = [0, 0, 0, 0, 0xDEADBEEF, 0x12345678, 0, 0]
        self.assertEqual(mrt0_words(asm, push_constants=push), [0x12345678] * 4)

    def test_other_smov_operand_classes_remain_refused(self):
        for source in (106, 129):  # VCC_LO and inline integer 1, respectively
            with self.subTest(source_kind=source), self.assertRaisesRegex(
                    rdna2spirv.Unsupported, "fall-through into undecodable words"):
                translate_fixture(NAME, fssm=fssm(source))


if __name__ == "__main__":
    unittest.main()
