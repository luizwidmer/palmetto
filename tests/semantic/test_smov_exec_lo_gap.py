'''Recover only a measured `s_mov_b32 exec_lo, sN` in an undecodable listing gap.

The current 668-program request-union census found 23 reachable four-byte gaps that decode as one exact, unmodified SOP1
`s_mov_b32 exec_lo, sN` with a one-dword SGPR source. The translator already models this write; this rule only restores the
measured word. `exec_hi`, special/immediate sources, wider words and other gaps stay refused. The fixture is synthetic and
runs through the CPU SPIR-V interpreter; no GPU, game program or game bytes are used.

    python -I tests/semantic/test_smov_exec_lo_gap.py -v
'''
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from support import rdna2spirv, run_fragment, translate_fixture  # noqa: E402

CODE_START = 0x200
NAME = 'ps_smov_exec_lo_gap'
EXEC_LO = 126
EXEC_HI = 127
VCC_LO = 106
INLINE_ONE = 129


def fssm(destination=EXEC_LO, source=4):
    '''Synthetic wave32 FSSM with one generated SOP1 word in the listing gap.'''
    blob = bytearray(CODE_START)
    struct.pack_into('<I', blob, 0xD4, 0x20)
    struct.pack_into('<4I', blob, 0x138, 0x40010, 1, 1, 1)
    word = 0xBE800000 | (3 << 8) | (destination << 16) | source
    blob.extend(bytes.fromhex('00 00 80 bf'))  # listed s_nop
    blob.extend(struct.pack('<I', word))       # hidden s_mov_b32
    blob.extend(bytes.fromhex('05 02 00 7e'))  # listed v_mov_b32 v0, s5
    blob.extend(bytes.fromhex('0f 18 00 f8 00 00 00 00'))  # export mrt0
    blob.extend(bytes.fromhex('00 00 81 bf'))  # s_endpgm
    return bytes(blob)


class ExecLowMoveGapTest(unittest.TestCase):
    def test_exact_exec_low_move_updates_active_lanes(self):
        asm = translate_fixture(NAME, fssm=fssm())
        value = 0x12345678
        for mask in (1, 0):
            with self.subTest(mask=mask):
                push = [0, 0, 0, 0, mask, value, 0, 0]
                outputs = run_fragment(asm, push_constants=push)
                if mask:
                    self.assertEqual(list(outputs['%out_mrt0']), [value] * 4)
                else:
                    self.assertNotIn('%out_mrt0', outputs)

    def test_exec_hi_and_other_exec_low_sources_remain_refused(self):
        for destination, source in ((EXEC_HI, 4), (EXEC_LO, VCC_LO), (EXEC_LO, INLINE_ONE)):
            with self.subTest(destination=destination, source=source), self.assertRaisesRegex(
                    rdna2spirv.Unsupported, 'fall-through into undecodable words'):
                translate_fixture(NAME, fssm=fssm(destination, source))


if __name__ == '__main__':
    unittest.main()