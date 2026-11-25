"""tooling/spirv_loop_guard.py: the assembly post-pass that bounds every loop of a translated module.

    python -m pytest tests/test_rdna2spirv_loop_guard.py

The modules it runs on are hand-written here (no game data, no shader bytes): a loop with the normal conditional
latch (loop or leave), one with an unconditional latch and a merge-block phi, and the three shapes the pass refuses.
Where SPIRV-Tools is installed, the guarded modules are also assembled and validated, because the pass's whole job is
to leave a module a driver still accepts; the shapes the upstream reference splits with a second latch block are
refused, and that is measured to be the validator's answer (tooling/spirv_loop_guard.py's docstring has the error).
"""
import subprocess

import pytest

import rdna2spirv
import spirv_loop_guard

from conftest import HAVE_SPIRV

HEAD = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%true = OpConstantTrue %bool
%false = OpConstantFalse %bool
%c0 = OpConstant %uint 0
%fn = OpTypeFunction %void
"""

# The normal structured loop: the latch branches to the header (repeat) or to the merge (leave).
CONDITIONAL_LATCH = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %head
%head = OpLabel
OpLoopMerge %end %cont None
OpBranch %body
%body = OpLabel
OpBranch %cont
%cont = OpLabel
OpBranchConditional %false %head %end
%end = OpLabel
%r = OpPhi %uint %c0 %cont
OpReturn
OpFunctionEnd
"""

# The latch is an unconditional branch to the header, so the merge block has no predecessor until the guard's exit
# edge is added: its phi then needs a value from that edge (the pass inserts an OpUndef).
UNCONDITIONAL_LATCH = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %head
%head = OpLabel
OpLoopMerge %end %cont None
OpBranchConditional %false %body %end
%body = OpLabel
OpBranch %cont
%cont = OpLabel
OpBranch %head
%end = OpLabel
%r = OpPhi %uint %c0 %head
OpReturn
OpFunctionEnd
"""

# Two blocks branch back to the header: not the one-back-edge shape a structured loop has.
TWO_BACK_EDGES = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %head
%head = OpLabel
OpLoopMerge %end %cont None
OpBranch %body
%body = OpLabel
OpBranch %cont
%cont = OpLabel
OpBranchConditional %false %head %end
%end = OpLabel
OpReturn
OpFunctionEnd
%extra = OpLabel
OpBranch %head
"""

# The latch is an OpSwitch, whose targets the pass does not reason about.
SWITCH_LATCH = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %head
%head = OpLabel
OpLoopMerge %end %cont None
OpBranch %body
%body = OpLabel
OpBranch %cont
%cont = OpLabel
OpSwitch %c0 %head 1 %end
%end = OpLabel
OpReturn
OpFunctionEnd
"""

# A conditional latch that can leave to a block inside the construct rather than to the merge. A counted latch there
# would have to leave the continue construct, which spirv-val rejects, so the pass refuses the module instead.
LATCH_INSIDE = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpBranch %head
%head = OpLabel
OpLoopMerge %end %cont None
OpBranch %body
%body = OpLabel
OpBranch %cont
%cont = OpLabel
OpBranchConditional %false %head %body
%end = OpLabel
OpReturn
OpFunctionEnd
"""


def validate(tmp_path, text):
    """Assemble and validate `text`, or skip when SPIRV-Tools is not installed."""
    if not HAVE_SPIRV:
        pytest.skip("needs spirv-as and spirv-val")
    sa, sv = tmp_path / "loop.spvasm", tmp_path / "loop.spv"
    sa.write_text(text)
    asm = subprocess.run(["spirv-as", "--target-env", "vulkan1.2", str(sa), "-o", str(sv)],
                         capture_output=True, text=True)
    assert asm.returncode == 0, "spirv-as: " + asm.stderr
    val = subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(sv)], capture_output=True, text=True)
    assert val.returncode == 0, "spirv-val: " + val.stderr


def test_zero_limit_returns_the_module_unchanged():
    for module in (CONDITIONAL_LATCH, UNCONDITIONAL_LATCH):
        assert spirv_loop_guard.add_guards(module, 0) is module


def test_limits_outside_the_32_bit_counter_are_refused():
    for limit in (-1, spirv_loop_guard.MAX_LIMIT + 1, True, 1.5, "16", None):
        with pytest.raises(ValueError):
            spirv_loop_guard.add_guards(CONDITIONAL_LATCH, limit)


def test_conditional_latch_is_counted_and_exits_at_the_budget(tmp_path):
    guarded = spirv_loop_guard.add_guards(CONDITIONAL_LATCH, 16)
    assert "%loop_guard_0_count = OpPhi %uint %loop_guard_c1 %entry %loop_guard_0_next %cont" in guarded
    assert "%loop_guard_0_next = OpIAdd %uint %loop_guard_0_count" in guarded
    assert "%loop_guard_0_room = OpULessThan %bool %loop_guard_0_count" in guarded
    # The loop's own condition is kept, ANDed with the budget; the exit target stays the merge block.
    assert "%loop_guard_0_take = OpLogicalAnd %bool %false %loop_guard_0_room" in guarded
    assert "OpBranchConditional %loop_guard_0_take %head %end" in guarded
    validate(tmp_path, guarded)


def test_unconditional_latch_exits_and_feeds_the_merge_phi(tmp_path):
    guarded = spirv_loop_guard.add_guards(UNCONDITIONAL_LATCH, 16)
    assert "OpBranchConditional %loop_guard_0_room %head %end" in guarded
    # The merge block's phi gains an incoming value from the guard's edge, and it is undefined.
    assert "%loop_guard_undef0 = OpUndef %uint" in guarded
    assert "%r = OpPhi %uint %c0 %head %loop_guard_undef0 %cont" in guarded
    validate(tmp_path, guarded)


def test_a_conditional_branch_to_the_header_twice_is_a_back_edge():
    both = CONDITIONAL_LATCH.replace("OpBranchConditional %false %head %end", "OpBranchConditional %false %head %head")
    guarded = spirv_loop_guard.add_guards(both, 16)
    assert "OpBranchConditional %loop_guard_0_room %head %end" in guarded


def test_shapes_that_cannot_be_bounded_are_refused():
    for module, what in ((TWO_BACK_EDGES, "one back edge"), (SWITCH_LATCH, "OpSwitch"),
                         (LATCH_INSIDE, "without the merge block")):
        with pytest.raises(spirv_loop_guard.LoopGuardError, match=what):
            spirv_loop_guard.add_guards(module, 16)


def test_the_translator_switch_bounds_its_loops(tmp_path, monkeypatch, make_shader):
    # The wiring in translate(): a translated loop is guarded by default, and --loop-limit 0 (here the global) gives
    # the unguarded module back. Both must still assemble and validate. The branch is written with its numeric
    # offset because llvm-mc's -show-encoding prints an unresolved label as a symbol, not an offset.
    shader = make_shader("s_mov_b32 s8, 0\ns_add_u32 s8, s8, 1\ns_cmp_lt_u32 s8, 4\n"
                         "s_cbranch_scc0 -3\nv_mov_b32_e32 v0, s8\nexp mrt0 v0, v0, v0, v0 vm\ns_endpgm\n",
                         name="ps_loop_guard")
    guarded = shader.translate()
    assert "OpLoopMerge" in guarded and "%loop_guard_0_count = OpPhi" in guarded
    shader.validate(guarded)
    monkeypatch.setattr(rdna2spirv, "LOOP_LIMIT", 0)
    plain = shader.translate()
    assert "OpLoopMerge" in plain and "%loop_guard_" not in plain
    shader.validate(plain)
    assert guarded != plain


def test_a_module_without_loops_is_returned_untouched():
    module = HEAD + """%main = OpFunction %void None %fn
%entry = OpLabel
OpReturn
OpFunctionEnd
"""
    assert spirv_loop_guard.add_guards(module) == module


def test_default_limit_is_the_translator_default():
    # Written out rather than read from the module, so changing either default fails this test.
    assert spirv_loop_guard.DEFAULT_LIMIT == 65536
    assert rdna2spirv.LOOP_LIMIT == 65536
    guarded = spirv_loop_guard.add_guards(CONDITIONAL_LATCH)
    assert "%loop_guard_c65536 = OpConstant %uint 65536" in guarded
