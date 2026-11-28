"""Recover only the measured `s_clause 3` instruction in a decode gap.

The current 668-program request-union census found three reachable four-byte gaps whose exact RDNA2 decode is one
unmodified SOPP `s_clause 3`. The translator already treats `s_clause` as a no-op; this rule restores only that exact
word. Other immediates, widths, instructions, and gaps remain refused. The fixture is synthetic and runs through the CPU
SPIR-V interpreter; no GPU, game program, or game bytes are used.

    python -I tests/semantic/test_s_clause_gap.py -v
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
NAME = "ps_s_clause_gap"
CLAUSE3 = bytes.fromhex("03 00 a1 bf")
CLAUSE1 = bytes.fromhex("01 00 a1 bf")


def fssm(clause_word=CLAUSE3):
    """Synthetic wave32 FSSM with an unlisted SOPP word between listed instructions."""
    blob = bytearray(CODE_START)
    struct.pack_into("<I", blob, 0xD4, 0x20)
    struct.pack_into("<4I", blob, 0x138, 0x40010, 1, 1, 1)
    blob.extend(bytes.fromhex("00 00 80 bf"))  # listed s_nop
    blob.extend(clause_word)                   # hidden s_clause
    blob.extend(bytes.fromhex("04 02 00 7e"))  # listed v_mov_b32 v0, s4
    blob.extend(bytes.fromhex("0f 18 00 f8 00 00 00 00"))  # export mrt0
    blob.extend(bytes.fromhex("00 00 81 bf"))  # s_endpgm
    return bytes(blob)


class SClauseGapTest(unittest.TestCase):
    def test_exact_clause_recovers_and_preserves_export(self):
        asm = translate_fixture(NAME, fssm=fssm())
        push = [0, 0, 0, 0, 0x12345678, 0, 0, 0]
        self.assertEqual(mrt0_words(asm, push_constants=push), [0x12345678] * 4)

    def test_other_clause_immediate_remains_refused(self):
        with self.assertRaisesRegex(rdna2spirv.Unsupported, "fall-through into undecodable words"):
            translate_fixture(NAME, fssm=fssm(CLAUSE1))


if __name__ == "__main__":
    unittest.main()