"""A translated loop is bounded by tooling/spirv_loop_guard.py at LOOP_LIMIT header visits per entry.

Four scenarios, each translated from a hand-written listing by tooling/rdna2spirv.py and executed on the CPU by
spirv_mini_interp.run_workgroup:

  * a loop whose exit becomes reachable keeps exactly the values it had (the guard is invisible to a loop that
    finishes inside its budget);
  * a loop whose exit cannot become reachable (an even step against an odd stop value) runs exactly the budget and
    then leaves through its merge block, with the last values it managed to store;
  * a nested loop gets a fresh budget at every entry to its outer loop, so a limit of 3 is 9 visits, not 3;
  * the dispatch-loop form (--dispatch-loop) is bounded too, because it is an ordinary loop around a switch.

A budget of 0 disables the pass: the module is the unguarded one and carries no guard ids.

The expected values come from the ISA semantics written in the fixture's own comments, not from the translator.
The budget's meaning is deliberate: the loop guard trades exactness at the limit for termination, so "stops after
the budget" is the assertion, not "computes the right answer for a nonterminating program".
"""
import os
import sys
import unittest
from unittest.mock import patch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("PALMETTO_TOOLS_DIR") or os.path.join(ROOT, "tooling")

sys.dont_write_bytecode = True
for _path in (HERE, TOOLS_DIR):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402
import test_loop_ballot_barrier as lbb  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "loop_iteration_guard.asm")
VSHARP = [0, 0, 0x4000, 0x31016FAC]      # raw buffer V#: base 0, the number of bytes is set per run


def listing():
    """The fixture's instruction lines, and the label for the loop head (the fixture writes it as `again:`)."""
    with open(FIXTURE, encoding="utf-8") as f:
        return [line for line in f.read().splitlines() if line and not line.startswith(";")]


def translate(lanes, limit, structured=True):
    """The translated module for `lanes` invocations, with the loop guard at `limit` visits."""
    body = ["s_mov_b64 exec, -1" if lanes == 64 else "s_mov_b32 exec_lo, -1"] + listing()
    with patch.object(rdna2spirv, "LOOP_LIMIT", limit):
        text, gen = lbb._translate_cs("cs_loop_guard", body, threads=lanes, emulate=lanes == 64,
                                      structured=structured)
    assert gen.wave64 == (lanes == 64) and gen.emulate == (lanes == 64)
    return text


def execute(text, lanes, stop):
    """Run the translated module with `lanes` invocations; return each invocation's output word."""
    memory = si.run_workgroup(
        text, lanes, 32,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i % 32, "%local_index": i},
        push_constants=[0, 0, lanes * 4, 0x31016FAC, stop] + [0] * 27, memory={})
    return [memory.get(4 * lane, 0) for lane in range(lanes)]


class LoopGuardTests(unittest.TestCase):
    def test_reachable_exit_keeps_its_values(self):
        for lanes in (32, 64):
            with self.subTest(lanes=lanes):
                self.assertEqual(execute(translate(lanes, 16), lanes, 6), [6] * lanes)

    def test_unreachable_exit_stops_at_the_budget(self):
        for lanes in (32, 64):
            with self.subTest(lanes=lanes):
                # The step is 2 and the stop value is odd, so the comparison never matches: without the guard this
                # loop would not terminate. limit 16 leaves after 16 visits, the last storing 32; limit 3 after 3,
                # the last storing 6.
                self.assertEqual(execute(translate(lanes, 16), lanes, 1), [32] * lanes)
                self.assertEqual(execute(translate(lanes, 3), lanes, 1), [6] * lanes)

    def test_zero_limit_disables_the_pass(self):
        text = translate(32, 0)
        self.assertNotIn("%loop_guard_", text)
        self.assertEqual(execute(text, 32, 6), [6] * 32)

    def test_nested_budget_resets_at_each_entry(self):
        lines = ["s_mov_b32 s9, 0", "s_mov_b32 s10, 0", "outer:", "s_mov_b32 s11, 0",
                 "inner:", "s_add_u32 s10, s10, 1", "s_add_u32 s11, s11, 1", "s_cmp_eq_u32 s11, 3",
                 "s_cbranch_scc0 inner", "s_add_u32 s9, s9, 1", "s_cmp_eq_u32 s9, 3", "s_cbranch_scc0 outer",
                 "v_mov_b32_e32 v1, s10", "v_lshlrev_b32_e32 v2, 2, v0",
                 "buffer_store_dword v1, v2, s[0:3], 0 offen", "s_endpgm"]
        with patch.object(rdna2spirv, "LOOP_LIMIT", 3):
            text, _ = lbb._translate_cs("cs_loop_guard_nested", lines, threads=32)
        # 3 outer entries x 3 inner visits; a budget shared between entries would stop at 3.
        self.assertEqual(execute(text, 32, 0), [9] * 32)

    def test_dispatch_loop_form_is_bounded_too(self):
        for lanes in (32, 64):
            with self.subTest(lanes=lanes):
                text = translate(lanes, 16, structured=False)
                self.assertIn("OpSwitch", text)
                words = execute(text, lanes, 1)
                self.assertTrue(all(0 < value <= 32 and value % 2 == 0 for value in words), words[:8])


if __name__ == "__main__":
    unittest.main()
