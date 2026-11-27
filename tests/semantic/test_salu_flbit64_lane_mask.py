"""Semantic fixture for RDNA2 s_flbit_i32_b64 with a whole-wave lane-mask source.

A proven lane mask is one bit per lane, not the current invocation's 0 / all-ones word. The unsigned b64 operation
counts zeros from bit 63 down (0xFFFFFFFF for an empty mask). The fixture moves a VCC mask through an SGPR pair,
then checks single bits in both wave halves, an empty mask, a full VCC mask, and EXEC. All 64 invocations must get the
same scalar result. The fixture is synthetic and runs only in the CPU SPIR-V interpreter.
"""
import struct
import unittest

import support
import spirv_mini_interp as si

VSHARP = [0, 0, 0x1000, 0x31016FAC]
M32 = 0xFFFFFFFF
MASKS = (1 << 63, 1 << 40, 1 << 31, 1, 0, (1 << 64) - 1, (1 << 64) - 1)


def fssm():
    header = bytearray(0x148)
    struct.pack_into("<4I", header, 0x138, 0x8088, 64, 1, 1)
    return bytes(header)


def run_fixture():
    spv = support.translate_fixture("cs_s_flbit64_lane_mask", fssm=fssm())
    return si.run_workgroup(
        spv, 64, 64,
        inputs_for=lambda lane: {"%wg_id": (0, 0, 0), "%local_id": (lane, 0, 0), "%sg_lane": lane},
        push_constants=VSHARP + [0] * 28, memory={})


class Flbit64LaneMaskTests(unittest.TestCase):
    def test_unsigned_b64_counts_the_whole_wave_mask(self):
        memory = run_fixture()
        for index, mask in enumerate(MASKS):
            expected = M32 if mask == 0 else 64 - mask.bit_length()
            with self.subTest(mask=hex(mask), expected=hex(expected)):
                for lane in range(64):
                    self.assertEqual(memory[index * 256 + lane * 4], expected,
                                     f"result {index}, lane {lane}")


if __name__ == "__main__":
    unittest.main()
