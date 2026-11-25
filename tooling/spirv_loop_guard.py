"""Bound every structured loop of a translated module, so a loop whose exit never becomes reachable still stops.

A translated shader carries the console program's control flow, and the translator cannot prove that every loop's
exit is reachable for every input: a condition computed lane by lane, or an exit our block analysis places behind a
path that some input never takes, would spin forever on the GPU and stall the frame loop. The upstream fork's
story-load work measured exactly that failure (a translated loop whose exit never became reachable, five-star-recomp
b12a952), which is why this pass is a correctness fix and not a coverage one.

The pass gives every loop header a budget: at most `limit` visits per invocation **per entry** to the loop. The
`limit`-th visit may still take the back edge; a visit that would be `limit` + 1 instead leaves through the loop's
own merge block, with the current register values (an OpUndef feeds any OpPhi the merge block takes from the new
edge). A loop that finishes before the budget keeps exactly the control flow it had; a loop that reaches it stops
with whatever it has computed. Nested loops each get their own counter, and a counter restarts whenever its loop is
entered, so an inner loop is bounded once per outer iteration, not once per invocation.

The module is SPIR-V assembly text. For each OpLoopMerge header the pass

* inserts a counter OpPhi at the header whose incoming value is 1 from every entry edge and `counter + 1` along the
  back edge (so a fresh entry resets the budget),
* inserts the increment and a comparison against the limit in the block that holds the back edge, and
* replaces that back edge's terminator with one that takes the original back edge only while the budget lasts and
  the loop's merge block once it is spent.

The pass needs the one-back-edge shape a structured loop has, and refuses the module (LoopGuardError) instead of
emitting something it cannot reason about. In particular a conditional back edge must take the merge block on one
arm (the normal "loop or leave" latch): SPIR-V requires the back edge to come from inside the continue construct, and
a block of the continue construct may leave to the merge block only when it is the continue target itself, so a
second (counted) latch block in there is rejected by spirv-val ("the continue construct ... is not structurally post
dominated by the back-edge block", measured with SPIRV-Tools 2026.1). The upstream fork's pass splits such an edge
anyway; this tree refuses it, which is the measured answer (docs/translator_fixes.md, "the loop guard").

`limit = 0` disables the pass and returns the module unchanged, so the unguarded module is still reproducible.
The default is the fork's 65536. The limit is a semantic truncation by design: a legitimate loop that visits one
header more often than the limit is cut short and exits through its merge with its current values, so this is a
guard against a hang, not an exactness claim.

The need and the shape were cross-checked against the public GPL-3.0 project ubiquitarianism/leonida, file
tools/spirv_loop_guard.py at commit 273b3bdf9 ("boot through the story load"); this implementation, its tests and
its limits are this tree's own (docs/translator_fixes.md has the mapping and the measurement).
"""
import re

DEFAULT_LIMIT = 65536        # header visits per invocation per loop entry before the guard leaves through the merge
MAX_LIMIT = 0xFFFFFFFF       # the counter is an unsigned 32-bit word

PREFIX = "%loop_guard_"

# Only the instructions this pass reasons about are scanned, so a large module costs one pass over the text:
# labels, merge instructions, branch terminators, phis and the declarations the pass may have to add.
_SCAN = re.compile(
    r"(?m)^[ \t]*(?:(?P<result>%[\w.]+)[ \t]*=[ \t]*)?"
    r"(?P<op>Op(?:Label|LoopMerge|SelectionMerge|Phi|Function|TypeInt|TypeBool|Constant|BranchConditional|Branch|"
    r"Switch))\b(?P<operands>[^;\n]*)")


class LoopGuardError(ValueError):
    """The module's control flow is not the one-back-edge shape this pass can bound safely."""


def _scan(text):
    """Index what the pass needs: label offsets, loop merges, branch terminators and their predecessors, the phis of
    each block (with the offset an extra incoming pair goes at), and the module's uint/bool types and constants."""
    labels, merges, loop_merge, terminators, phis = {}, {}, {}, {}, {}
    preds, constants = {}, {}
    uint = boolean = first_function = None
    block = None
    for match in _SCAN.finditer(text):
        result, op, operands = match.group("result"), match.group("op"), match.group("operands")
        args = operands.split()
        if op == "OpFunction":
            if first_function is None:
                first_function = match.start()
        elif op == "OpTypeInt" and args == ["32", "0"]:
            uint = result
        elif op == "OpTypeBool":
            boolean = result
        elif op == "OpConstant" and uint is not None and args and args[0] == uint:
            constants[int(args[1], 0)] = result
        elif op == "OpLabel":
            block = result
            labels[block] = match.end()
        elif op == "OpPhi":
            if block is not None:
                phis.setdefault(block, []).append((match.end(), args[0]))
        elif op in ("OpLoopMerge", "OpSelectionMerge"):
            merges[block] = match.start()
            if op == "OpLoopMerge":
                loop_merge[block] = args[0]
        elif op in ("OpBranch", "OpBranchConditional", "OpSwitch"):
            terminators[block] = (match.start(), match.end(), op, args)
            targets = (args[:1] if op == "OpBranch" else
                       args[1:3] if op == "OpBranchConditional" else args[1::2])
            for dest in dict.fromkeys(targets):
                preds.setdefault(dest, []).append(block)
    return {"labels": labels, "merges": merges, "loop_merge": loop_merge, "terminators": terminators,
            "phis": phis, "preds": preds, "constants": constants, "uint": uint, "boolean": boolean,
            "first_function": first_function}


def _splice(text, edits):
    """Apply (start, end, replacement) edits, which may not overlap; an insertion at a replaced range's start goes
    before the replacement, as the edits are sorted by (start, end) and applied in that order."""
    out, done = [], 0
    for start, end, replacement in sorted(edits, key=lambda edit: (edit[0], edit[1])):
        out.append(text[done:start])
        out.append(replacement)
        done = end
    out.append(text[done:])
    return "".join(out)


def add_guards(text, limit=DEFAULT_LIMIT):
    """Return `text` with every structured loop bounded at `limit` header visits per entry (0: unchanged)."""
    if not isinstance(limit, int) or isinstance(limit, bool) or not 0 <= limit <= MAX_LIMIT:
        raise ValueError(f"loop limit must be a whole number in 0..{MAX_LIMIT}, not {limit!r}")
    if not limit or "OpLoopMerge" not in text:
        return text
    index = _scan(text)
    if not index["loop_merge"]:
        return text
    prefix = PREFIX
    while prefix in text:           # never collide with an id the shader already has
        prefix += "_"
    labels, merges, terminators = index["labels"], index["merges"], index["terminators"]
    uint, boolean = index["uint"], index["boolean"]
    decls = []
    if uint is None:
        uint = prefix + "uint"
        decls.append(f"{uint} = OpTypeInt 32 0")
    if boolean is None:
        boolean = prefix + "bool"
        decls.append(f"{boolean} = OpTypeBool")
    constants = dict(index["constants"])
    for value in (1, limit):
        if value not in constants:
            constants[value] = f"{prefix}c{value}"
            decls.append(f"{constants[value]} = OpConstant {uint} {value}")
    one, bound = constants[1], constants[limit]
    edits, undef_types = [], {}

    for number, (header, exit_label) in enumerate(index["loop_merge"].items()):
        # A structured loop has one back edge, and the translator emits dominators first, so it is the predecessor
        # that comes after the header in the module.
        backwards = [p for p in index["preds"].get(header, []) if labels.get(p, -1) >= labels[header]]
        if len(backwards) != 1:
            raise LoopGuardError(f"loop at {header}: expected one back edge, found {len(backwards)}")
        back = backwards[0]
        start, end, op, args = terminators[back]
        if op == "OpSwitch":
            raise LoopGuardError(f"loop at {header}: its back edge is an OpSwitch, which this pass does not bound")
        targets = args[1:3] if op == "OpBranchConditional" else args[:1]
        if set(targets) == {header}:
            op = "OpBranch"             # both arms of the conditional are the back edge
        elif op == "OpBranchConditional" and exit_label not in targets:
            raise LoopGuardError(
                f"loop at {header}: its back edge branches to {' '.join(targets)} without the merge block "
                f"{exit_label}; a counted latch there would leave the continue construct, which spirv-val rejects")
        incoming = " ".join(f"{prefix}{number}_next {back}" if p == back else f"{one} {p}"
                            for p in index["preds"][header])
        edits.append((labels[header], labels[header], f"\n{prefix}{number}_count = OpPhi {uint} {incoming}"))
        steps = [f"{prefix}{number}_next = OpIAdd {uint} {prefix}{number}_count {one}",
                 f"{prefix}{number}_room = OpULessThan {boolean} {prefix}{number}_count {bound}"]
        if op == "OpBranch":
            branch = f"OpBranchConditional {prefix}{number}_room {header} {exit_label}"
        elif args[1] == header:
            steps.append(f"{prefix}{number}_take = OpLogicalAnd {boolean} {args[0]} {prefix}{number}_room")
            branch = f"OpBranchConditional {prefix}{number}_take {args[1]} {args[2]}"
        else:
            steps.extend((f"{prefix}{number}_spent = OpLogicalNot {boolean} {prefix}{number}_room",
                          f"{prefix}{number}_take = OpLogicalOr {boolean} {args[0]} {prefix}{number}_spent"))
            branch = f"OpBranchConditional {prefix}{number}_take {args[1]} {args[2]}"
        where = merges.get(back, start)          # an OpSelectionMerge stays second-to-last in its block
        edits.append((where, where, "\n".join(steps) + "\n"))
        edits.append((start, end, branch))
        if exit_label not in targets:
            # The merge block gains a predecessor it did not have: its phis need a value from the new edge, and an
            # undefined one is the honest answer for a loop that was cut short.
            for at, ty in index["phis"].get(exit_label, []):
                if ty not in undef_types:
                    undef_types[ty] = f"{prefix}undef{len(undef_types)}"
                    decls.append(f"{undef_types[ty]} = OpUndef {ty}")
                edits.append((at, at, f" {undef_types[ty]} {back}"))

    if index["first_function"] is None:
        raise LoopGuardError("module has OpLoopMerge but no OpFunction")
    if decls:
        edits.append((index["first_function"], index["first_function"], "\n".join(decls) + "\n"))
    return _splice(text, edits)
