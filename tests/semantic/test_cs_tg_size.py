"""TG_SIZE_EN: the compute work-group-info SGPR (COMPUTE_PGM_RSRC2 bit 10), written for every read of it.

COMPUTE_PGM_RSRC2 bit 10 makes the hardware fill a system SGPR right after the enabled workgroup-id SGPRs. Its value
is the workgroup's wave count in bits [5:0] and this wave's index in [11:6] (Mesa,
src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c, GFX6-10). The translator writes that value in the compute
prologue: thread i is in wave i / W (W = 32 or 64, the program's own wave size), and the wave count is the group's
thread count rounded up to a whole wave.

Every read is accepted (docs/translator_fixes.md, "TG_SIZE general value"). Until this file the translator accepted
only one wave64 idiom (`s_and_b32 sN, sTG, 0xfc0` whose result feeds `v_mbcnt`) and refused everything else, and
even that acceptance never fired: the check treated the idiom register's own use by `v_mbcnt` as a second,
non-idiom use. Measured on the 668-program request union, the 25 programs the check refused split into 8 that read
the register (all of them with exactly that idiom and then `v_mbcnt`, the local invocation index) and 17 that
overwrite the register before reading it, so they never consume the hardware value. All 25 translate now.

Each case translates a hand-written listing with tooling/rdna2spirv.py and runs every invocation of the workgroup in
spirv_mini_interp; each invocation stores the value it computed at output word LocalInvocationIndex. A wave64 program
is forced with the `s_mov_b64 exec, -1` marker (its absence leaves the code wave32).
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("PALMETTO_TOOLS_DIR") or os.path.join(ROOT, "tooling")

sys.dont_write_bytecode = True
for _path in (HERE, TOOLS_DIR):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import spirv_mini_interp as si  # noqa: E402
import support  # noqa: E402

VSHARP = [0, 0, 0x4000, 0x31016FAC]        # raw buffer V# for the output, number of bytes set per run
WAVE64 = "s_mov_b64 exec, -1"              # the strict marker that makes the listing wave64 code
TG_SIZE_EN = 1 << 10
# 4 user SGPRs, workgroup id x in s4, thread ids x/y/z in v0/v1/v2: the work-group-info SGPR is s5.
RSRC2 = 0x1088 | TG_SIZE_EN
# The same with all three workgroup ids enabled: they are s4/s5/s6 and the work-group-info SGPR is s7.
RSRC2_TGID_XYZ = 0x388 | 2 << 11 | TG_SIZE_EN


def fssm(rsrc2, size):
    blob = bytearray(0x148)
    struct.pack_into("<4I", blob, 0x138, rsrc2, *size)
    return bytes(blob)


def translate(body, size, rsrc2=RSRC2):
    return support.translate_lines("cs_tg_size", list(body) + ["s_endpgm"], fssm=fssm(rsrc2, size))


def run(body, size, wave64, rsrc2=RSRC2):
    """Translate `body` plus a per-lane store of v5 at word LocalInvocationIndex, then run every invocation.

    v5 is the value the case under test computes; the tail computes the flat local invocation index from the
    thread ids (v3 = x + y * sx + z * sx * sy) and stores v5 there, so the returned list is in invocation order.
    """
    sx, sy, sz = size
    lines = ([WAVE64] if wave64 else []) + list(body) + [
        f"v_mad_u32_u24 v3, v1, {sx}, v0", f"v_mad_u32_u24 v3, v2, {sx * sy}, v3",
        "v_lshlrev_b32 v3, 2, v3", "buffer_store_dword v5, v3, s[0:3], 0 offen"]
    module = translate(lines, size, rsrc2)
    invocations = sx * sy * sz
    wave = 64 if wave64 else 32
    memory = si.run_workgroup(
        module, invocations, wave,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i % sx, (i // sx) % sy, i // (sx * sy)),
                              "%sg_lane": i % wave},
        push_constants=VSHARP + [0] * 28, memory={})
    return [memory.get(4 * i) for i in range(invocations)]


LOCAL_INDEX_WAVE64 = ["s_and_b32 s6, s5, 0xfc0", "v_mbcnt_lo_u32_b32 v5, -1, s6",
                      "v_mbcnt_hi_u32_b32 v5, -1, v5"]
# bits [11:6] are the wave index, so the wave's base lane is (value & 0xfc0) >> 1 in a 32-wide wave.
LOCAL_INDEX_WAVE32 = ["s_and_b32 s6, s5, 0xfc0", "s_lshr_b32 s6, s6, 1", "v_mbcnt_lo_u32_b32 v5, -1, s6"]


class TgSizeTests(unittest.TestCase):
    def test_local_invocation_index_wave64(self):
        # Mesa's wave64 LocalInvocationIndex: mbcnt(~0, tg_size & 0xfc0).
        for size in ((256, 1, 1), (16, 16, 1), (8, 8, 4)):
            with self.subTest(size=size):
                self.assertEqual(run(LOCAL_INDEX_WAVE64, size, True), list(range(size[0] * size[1] * size[2])))

    def test_local_invocation_index_wave32(self):
        for size in ((128, 1, 1), (8, 8, 1), (4, 4, 4)):
            with self.subTest(size=size):
                self.assertEqual(run(LOCAL_INDEX_WAVE32, size, False), list(range(size[0] * size[1] * size[2])))

    def test_wave_count_is_bits_5_to_0(self):
        # Rounded up to a whole wave: a partial last wave still counts.
        for wave64, size, waves in ((True, (256, 1, 1), 4), (False, (256, 1, 1), 8), (True, (100, 1, 1), 2),
                                    (False, (6, 6, 1), 2), (True, (1, 1, 1), 1), (False, (32, 32, 1), 32)):
            with self.subTest(wave64=wave64, size=size):
                got = run(["s_and_b32 s6, s5, 0x3f", "v_mov_b32 v5, s6"], size, wave64)
                self.assertEqual(set(got), {waves})

    def test_whole_value_wave64(self):
        # 192 threads: 3 waves of 64; wave index in [11:6], wave count in [5:0]. This is the whole value the
        # translator writes, so it is also the pin for "nothing else is set in it".
        got = run(["v_mov_b32 v5, s5"], (192, 1, 1), True)
        self.assertEqual(got, [((i // 64) << 6) | 3 for i in range(192)])

    def test_a_single_wave_writes_count_one(self):
        # A one-wave workgroup's bits [5:0] are 1, not 0.
        for wave64, size in ((True, (64, 1, 1)), (True, (32, 1, 1)), (False, (32, 1, 1)), (True, (1, 1, 1))):
            with self.subTest(wave64=wave64, size=size):
                self.assertEqual(set(run(["v_mov_b32 v5, s5"], size, wave64)), {1})

    def test_follows_the_workgroup_ids(self):
        # TGID_X/Y/Z enabled: the ids are s4/s5/s6, so the work-group-info SGPR is s7.
        body = ["s_and_b32 s8, s7, 0xfc0", "v_mbcnt_lo_u32_b32 v5, -1, s8", "v_mbcnt_hi_u32_b32 v5, -1, v5"]
        got = run(body, (128, 1, 1), True, RSRC2_TGID_XYZ)
        self.assertEqual(got, list(range(128)))

    def test_not_written_without_the_bit(self):
        # RSRC2 bit 10 clear: no work-group-info value, and the SGPR after the ids stays user data rather than a
        # value the translator invented.
        module = translate(["v_mov_b32 v5, s5"], (64, 1, 1), rsrc2=0x1088)
        self.assertNotIn("%tgs_", module)

    def test_general_read_forms_translate(self):
        # Every one of these was refused before the general value: the copy and the wave32 cases by the idiom
        # check, the in-place idiom by the check treating v_mbcnt's use as a second, non-idiom use of the register.
        self.assertEqual(run(["s_mov_b32 s6, s5", "v_mov_b32 v5, s6"], (128, 1, 1), True),
                         [((i // 64) << 6) | 2 for i in range(128)])
        self.assertEqual(run(["s_mov_b32 s6, s5", "v_mov_b32 v5, s6"], (128, 1, 1), False),
                         [((i // 32) << 6) | 4 for i in range(128)])
        # The in-place form is the one the measured corpus programs use.
        in_place = ["s_and_b32 s5, s5, 0xfc0", "v_mbcnt_lo_u32_b32 v5, -1, s5",
                    "v_mbcnt_hi_u32_b32 v5, -1, v5"]
        self.assertEqual(run(in_place, (128, 1, 1), True), list(range(128)))
        # A read of the wave count alone (mask 0x3f, not the old 0xfc0) in wave32 code.
        self.assertEqual(set(run(["s_and_b32 s6, s5, 0x3f", "v_mov_b32 v5, s6"], (128, 1, 1), False)), {4})


if __name__ == "__main__":
    unittest.main()
