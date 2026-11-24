"""Reconstruct the scalar-memory words a disassembler refused because the console's SGPR file is wider than its model.

The retail title's compute programs declare 112 scalar registers (`rsrc1` granularity field, wave32), while LLVM's
RDNA2 model allows 106. A 64-bit scalar-memory instruction whose register tuple crosses that boundary is rejected:
`s_buffer_load_dwordx2/x4/x8/x16` whose destination or base reaches past s105, and `buffer_load/store_dword*` whose
resource descriptor does. The disassembler skips the first four bytes, re-anchors four bytes later, and decodes the
second half (and sometimes the following instruction) as something else, so the listing loses the instruction and
the true offset of everything after it.

What this tool does, and only where it is certain:

* It walks the listing against the program bytes with the translator's own acceptance rules: a line whose printed
  encoding is the bytes at its offset, differs only in a field llvm-mc re-canonicalises, makes the decoder read an
  inline literal, or is the console's `v_dot4_i32_i8` form the translator rebuilds from the blob, is copied
  through as it was written (the translator's predicates are imported, so the tool cannot accept what it refuses);
* where the bytes disagree, it rebuilds the rejected scalar-memory forms whose field layout is derived from
  `llvm-mc --show-encoding` on in-range operands. The table is re-verified at startup by decoding and
  re-assembling llvm-mc's own output (both signs of the 21-bit offset, both offset forms, glc/dlc), so a layout
  drift is a hard error and never a wrong line;
* a reconstruction is accepted only when every register it names fits the console's 112-register file, and the
  encoding it is built from is exactly the program's bytes;
* after a reconstruction it reconciles the listing's mis-decoded lines with a fresh decode from the re-anchored
  offset: the instructions the disassembler's re-anchoring swallowed are re-emitted from llvm-mc and the
  mis-decoded lines are dropped, only when the printed lengths account for exactly the bytes involved. Consecutive
  rejected words are rebuilt as one run, and a run of them is the common case in these programs;
* the finished listing is verified end to end against the program bytes, the way the translator reads it (every
  instruction in order, all bytes covered). A listing that does not verify is never written: the tool prints why
  and leaves the input alone (with --out it copies the input verbatim, so the caller still gets one file per
  input), so a word it cannot explain can never turn into a rewritten listing.

Run it on the disassembler's own listing (`shader_disasm.py` output), not on one a previous repair pass edited: the
reconciliation assumes the listing's instruction boundaries are the disassembler's. --mcpu must match the target in
the listing's header.

Usage:
    python -I tools/widen_smem_decode.py <listing.s> --blob <program.code> [--out <repaired.s>]
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import shader_ir                                                                  # noqa: E402
from rdna2spirv import (base_op, mix_inline_literal, mix_literal, same_encoding)   # noqa: E402

# The console's VOP3P v_dot4_i32_i8 words need the translator's measured-form predicates to be recognised in a
# listing. They live in rdna2spirv.py (the translator owns that measurement); without them the tool still repairs
# the register-file words and simply does not accept a listing that contains such a word.
try:
    from rdna2spirv import console_dot4_word, restore_vop3p_dot4
except ImportError:                                        # pragma: no cover - old translator checkout
    console_dot4_word = restore_vop3p_dot4 = None

LLVM_MC = os.environ.get("LLVM_MC") or r"F:\tools\llvm-mc\llvm-mc.exe"
TRIPLE = "amdgcn--amdpal"
MCPU = "gfx1030"

LINE = re.compile(r"^\t(.*?)\s*;\s*encoding:\s*\[([0-9a-fx, ]+)\]\s*$")
HDR = re.compile(r"code 0x([0-9a-f]+)\.\.0x([0-9a-f]+)")

CONSOLE_SGPRS = 112        # the console's register file (rsrc1 granularity field, wave32)
LLVM_SGPRS = 106           # LLVM's model: s0..s105, then 106 is vcc_lo

# Scalar buffer loads: name -> destination width in dwords. Opcode keys and the field layout below are derived and
# re-verified from llvm-mc at startup (table()).
SMEM_FORMS = {
    "s_buffer_load_dword": 1, "s_buffer_load_dwordx2": 2, "s_buffer_load_dwordx4": 4,
    "s_buffer_load_dwordx8": 8, "s_buffer_load_dwordx16": 16,
}
# Buffer (MUBUF) accesses whose data operand is a plain dword tuple.
MUBUF_FORMS = {
    "buffer_load_dword": 1, "buffer_load_dwordx2": 2, "buffer_load_dwordx4": 4,
    "buffer_store_dword": 1, "buffer_store_dwordx2": 2, "buffer_store_dwordx4": 4,
}

SMEM_TABLE = {}
MUBUF_TABLE = {}


def sgpr(reg, lanes):
    return "s%d" % reg if lanes == 1 else "s[%d:%d]" % (reg, reg + lanes - 1)


def vgpr(reg, lanes):
    return "v%d" % reg if lanes == 1 else "v[%d:%d]" % (reg, reg + lanes - 1)


def assemble(text):
    p = subprocess.run([LLVM_MC, "--triple=" + TRIPLE, "--mcpu=" + MCPU, "--show-encoding"],
                       input=text + "\n", capture_output=True, text=True, errors="replace")
    m = re.search(r"encoding:\s*\[([0-9a-fx, ]+)\]", p.stdout)
    return bytes(int(x, 16) for x in m.group(1).split(",")) if m else None


def disassemble(code):
    text = " ".join("0x%02x" % b for b in code)
    p = subprocess.run([LLVM_MC, "--disassemble", "--triple=" + TRIPLE, "--mcpu=" + MCPU, "--show-encoding"],
                       input=text, capture_output=True, text=True, errors="replace")
    lines = []
    for ln in p.stdout.splitlines():
        m = LINE.match(ln)
        if m:
            lines.append((m.group(1).strip(), bytes(int(x, 16) for x in m.group(2).split(","))))
    return lines


def smem_text(w0, w1):
    """Text for a scalar buffer load from its two encoding dwords, or None if it is not one of the known forms.

    Field layout (llvm-mc ground truth, re-verified at startup): sbase = (w0 & 0x3f) * 2, sdata = (w0 >> 6) & 0x7f,
    opcode = w0 bits 18..25, dlc = w0 bit 14, glc = w0 bit 16, soffset = w1 bits 25..31 (125 = null, and the
    immediate offset is then the third operand), offset = w1 bits 0..20 as a signed 21-bit value (bit 20 is the
    sign; llvm-mc prints a negative one as -0x...)."""
    hit = SMEM_TABLE.get((w0 >> 18) & 0xFF)
    if not hit:
        return None
    name, lanes = hit
    sdata = (w0 >> 6) & 0x7F
    sbase = (w0 & 0x3F) * 2
    soffset = (w1 >> 25) & 0x7F
    offset = w1 & 0x1FFFFF
    if offset & 0x100000:
        offset -= 0x200000
    glc, dlc = (w0 >> 16) & 1, (w0 >> 14) & 1
    if sdata + lanes > CONSOLE_SGPRS or sbase + 4 > CONSOLE_SGPRS:
        return None
    if soffset != 125 and soffset >= LLVM_SGPRS:   # such a register offset would be printed back as vcc/ttmp
        return None
    parts = [sgpr(sdata, lanes), sgpr(sbase, 4)]
    if soffset == 125:
        parts.append("null" if offset == 0 else ("-0x%x" % -offset if offset < 0 else "0x%x" % offset))
        text = name + " " + ", ".join(parts)
    else:
        parts.append(sgpr(soffset, 1))
        text = name + " " + ", ".join(parts)
        if offset:
            text += " offset:%s" % ("-0x%x" % -offset if offset < 0 else "0x%x" % offset)
    if glc:
        text += " glc"
    if dlc:
        text += " dlc"
    return text


def mubuf_text(w0, w1):
    """Text for a buffer load/store from its two encoding dwords, or None if it is outside the covered shape.

    Field layout (llvm-mc ground truth, re-verified at startup): resource = ((w1 >> 16) & 0x1f) * 4,
    vdata = (w1 >> 8) & 0xff, vaddr = w1 & 0xff, soffset = w1 >> 24; offen = w0 bit 12, offset = w0 bits 0..11.
    Only the shape the console code actually contains is built: offen, no idxen, offset 0, an inline-constant-zero
    soffset and no glc/dlc/lds/slc/tfe."""
    hit = MUBUF_TABLE.get((w0 >> 18) & 0xFF)
    if not hit:
        return None
    name, lanes = hit
    srsrc = ((w1 >> 16) & 0x1F) * 4
    vdata = (w1 >> 8) & 0xFF
    vaddr = w1 & 0xFF
    soffset = w1 >> 24
    if (w0 >> 12) & 1 != 1 or (w0 >> 13) & 1 or (w0 >> 14) & 1 or (w0 >> 15) & 1 or (w0 >> 16) & 1:
        return None
    if w0 & 0xFFF or soffset != 0x80 or (w1 >> 22) & 1 or (w1 >> 23) & 1:
        return None
    if srsrc + 4 > CONSOLE_SGPRS or vdata + lanes > 256:
        return None
    return "%s %s, %s, %s, 0 offen" % (name, vgpr(vdata, lanes), vgpr(vaddr, 1), sgpr(srsrc, 4))


def ground_truth_ok(cond, msg):
    if not cond:
        sys.exit("llvm-mc ground truth no longer matches the layout this tool encodes: " + msg)


def table():
    """Build the opcode-key tables, then decode and re-assemble llvm-mc's own output as a round-trip proof."""
    for name, lanes in SMEM_FORMS.items():
        text = "%s %s, s[16:19], s12 offset:0x7800" % (name, sgpr(8, lanes))
        raw = assemble(text)
        ground_truth_ok(raw is not None and len(raw) == 8, name)
        SMEM_TABLE[(int.from_bytes(raw[:4], "little") >> 18) & 0xFF] = (name, lanes)
        ground_truth_ok(smem_text(int.from_bytes(raw[:4], "little"), int.from_bytes(raw[4:], "little")) == text,
                        "%s does not round-trip" % name)
    for name, lanes in MUBUF_FORMS.items():
        text = "%s %s, v23, s[8:11], 0 offen" % (name, vgpr(58, lanes))
        raw = assemble(text)
        ground_truth_ok(raw is not None and len(raw) == 8, name)
        MUBUF_TABLE[(int.from_bytes(raw[:4], "little") >> 18) & 0xFF] = (name, lanes)
        ground_truth_ok(mubuf_text(int.from_bytes(raw[:4], "little"), int.from_bytes(raw[4:], "little")) == text,
                        "%s does not round-trip" % name)
    for text in ("s_buffer_load_dword s8, s[8:11], 0xa8 glc",
                 "s_buffer_load_dwordx2 s[8:9], s[16:19], s9 offset:0x1234 dlc",
                 "s_buffer_load_dwordx4 s[92:95], s[16:19], s12 offset:0x12345 glc dlc"):
        raw = assemble(text)
        ground_truth_ok(raw is not None and len(raw) == 8, text)
        got = smem_text(int.from_bytes(raw[:4], "little"), int.from_bytes(raw[4:], "little"))
        ground_truth_ok(got == text, "%r decoded as %r" % (text, got))
    # Offsets are 21-bit and signed; llvm-mc's assembler refuses a negative immediate, so for that form the ground
    # truth is the decoder's own print. Check both signs, with a register offset and with the null offset.
    for name, lanes in SMEM_FORMS.items():
        bases = [assemble("%s %s, s[16:19], s12 offset:0x7800" % (name, sgpr(8, lanes))),
                 assemble("%s %s, s[16:19], 0x7800" % (name, sgpr(8, lanes)))]
        for base in bases:
            ground_truth_ok(base is not None and len(base) == 8, name)
            for sign in (0, 0x100000):
                w0 = int.from_bytes(base[:4], "little")
                w1 = (int.from_bytes(base[4:], "little") & ~0x100000) | sign
                want = smem_text(w0, w1)
                got = disassemble(base[:4] + w1.to_bytes(4, "little"))
                ground_truth_ok(bool(got) and got[0][0] == want,
                                "%s offset 0x%x decoded as %r, expected %r" % (
                                    name, w1 & 0x1FFFFF, got[0][0] if got else None, want))
    print("%d scalar-load and %d buffer form(s) identified at %s%s" % (
        len(SMEM_TABLE), len(MUBUF_TABLE), MCPU,
        "" if console_dot4_word else "; console v_dot4_i32_i8 form not recognised (old translator checkout)"))


def dot4_line(blob, pc, enc, ins):
    """True if this 4-byte listing line is the disassembler's decode of the second word of the console's 8-byte
    VOP3P v_dot4_i32_i8 at `pc`, which the translator rebuilds from the bytes. Its true length is 8."""
    if len(enc) != 4 or pc + 8 > len(blob) or console_dot4_word is None:
        return False
    if console_dot4_word(blob, pc) is None:
        return False
    return same_encoding(blob[pc + 4:pc + 8], enc, ins["op"], len(ins["args"]))


def instruction_length(blob, pc, ins):
    """Bytes of the instruction this listing line stands for at `pc`, or 0 if the bytes there are something else.

    Mirrors the translator's check_printed_encodings order exactly: same encoding, a canonicalised field, a mix
    literal, the console v_dot4_i32_i8 restoring its dropped fields, or the 4-byte line the disassembler emitted for
    the second word of a console v_dot4 word the translator rebuilds (8 bytes)."""
    enc = bytes.fromhex(ins["enc"]) if ins.get("enc") else b""
    if not enc:
        return 0
    if pc + len(enc) <= len(blob) and same_encoding(blob[pc:pc + len(enc)], enc, ins["op"], len(ins["args"])):
        return len(enc)
    raw = blob[pc:pc + len(enc)]
    if mix_literal(raw, enc, base_op(ins["op"])) is not None:
        return len(enc)
    if pc + len(enc) + 4 <= len(blob) and \
            mix_inline_literal(blob[pc:pc + len(enc) + 4], enc, base_op(ins["op"])) is not None:
        return len(enc) + 4
    if restore_vop3p_dot4 is not None and len(enc) == 8 and pc + 8 <= len(blob) and \
            restore_vop3p_dot4(dict(ins), blob[pc:pc + 8], enc):
        return len(enc)
    if dot4_line(blob, pc, enc, ins):
        return 8
    return 0


def wide_form(blob, pos, end):
    """Text for a scalar-memory word at `pos` whose register tuple LLVM's model cannot hold, or None.

    Only a word LLVM would reject is rebuilt: a buffer load whose destination or base tuple reaches past s105, or a
    buffer access whose resource descriptor does. An in-range word is left to the disassembler's own line."""
    if pos + 8 > end:
        return None
    w0 = int.from_bytes(blob[pos:pos + 4], "little")
    w1 = int.from_bytes(blob[pos + 4:pos + 8], "little")
    hi6 = w0 >> 26
    if hi6 == 0x3D:
        hit = SMEM_TABLE.get((w0 >> 18) & 0xFF)
        if not hit:
            return None
        name, lanes = hit
        sdata = (w0 >> 6) & 0x7F
        sbase = (w0 & 0x3F) * 2
        soffset = (w1 >> 25) & 0x7F
        if sdata + lanes <= LLVM_SGPRS and sbase + 4 <= LLVM_SGPRS and (soffset == 125 or soffset < LLVM_SGPRS):
            return None
        return smem_text(w0, w1)
    if hi6 == 0x38:
        if ((w1 >> 16) & 0x1F) * 4 + 4 <= LLVM_SGPRS:
            return None
        return mubuf_text(w0, w1)
    return None


def misdecode_run(blob, pos, q, end, body, i):
    """Drop the listing's mis-decoded lines for a run of reconstructed words ending at q, and re-emit any true
    instruction the last mis-decode swallowed past q.

    The disassembler rejected the run's first word four bytes in, so its next line decodes from pos + 4 and the
    following ones continue from there. A fresh decode from pos + 4 reproduces exactly those lines. Consuming them
    while the position advances must reach q, the first true boundary after the run: there the streams meet again
    and the repair is done. If the last mis-decoded line runs past q, the instructions it swallowed are decoded
    from q (a true boundary) and re-emitted, accepted only when their printed lengths fill exactly the bytes up to
    the disassembler's next position. Returns (lines to drop, new position, swallowed lines) or None."""
    model = disassemble(blob[pos + 4:min(end, q + 0x400)])
    if not model:
        return None
    at, k = pos + 4, i
    for text, enc in model:
        if at >= q or k >= len(body):
            return None
        if body[k][1] != enc or body[k][3].split(",")[0] != text.split(",")[0]:
            return None
        k += 1
        nxt = at + len(enc)
        if nxt == q:
            return k - i, q, []
        if nxt > q:
            tail = disassemble(blob[q:nxt])
            if not tail or len(tail) > 16 or sum(len(e) for _, e in tail) != nxt - q:
                return None
            return k - i, nxt, tail
        at = nxt
    return None


def resync(blob, pos, end, body, i):
    """Reconcile the listing with a fresh decode from pos + 8 (the first true boundary after a reconstructed word).

    The disassembler dropped one or two 4-byte words at `pos` and then emitted the listing's lines for the rest of
    that word. The listing line k that equals fresh line m is where the streams meet again; the fresh lines before
    it are the swallowed instructions to re-emit and the listing lines before k are mis-decoded lines to drop. The
    repair is accepted only when the dropped lines' printed lengths fill exactly the bytes from the re-anchored
    start to that meeting point, i.e. when (8 + emitted bytes - dropped bytes) is 4 (one word skipped before
    decoding) or 8 (two)."""
    fresh = disassemble(blob[pos + 8:min(end, pos + 8 + 0x400)])
    if not fresh:
        return None
    # each fresh line's true offset: a console v_dot4 line stands for 8 bytes, everything else for its printed length
    offsets = []
    at = pos + 8
    for text, enc in fresh:
        offsets.append(at)
        ins = shader_ir.parse_line("\t" + text + " ; encoding: [" + ",".join("0x%02x" % b for b in enc) + "]")
        if ins and dot4_line(blob, at, enc, ins):
            at += 8
        else:
            at += len(enc)
    dropped = 0
    for k in range(i, min(i + 8, len(body))):
        for m in range(min(17, len(fresh))):
            text, enc = fresh[m]
            if enc == body[k][1] and text.split(",")[0] == body[k][3].split(",")[0]:
                if (8 + (offsets[m] - (pos + 8)) - dropped) in (4, 8):
                    return k - i, offsets[m], fresh[:m]
        dropped += len(body[k][1])
    return None


def verify(blob, start, end, pairs):
    """Walk the finished listing the way the translator does; return None if it accounts for the bytes, else why."""
    pos = start
    for text, enc in pairs:
        ins = shader_ir.parse_line("\t" + text + " ; encoding: [" + ",".join("0x%02x" % b for b in enc) + "]")
        if not ins:
            return "unparseable line: " + text
        n = instruction_length(blob, pos, ins)
        if not n:
            return "line does not match the bytes at 0x%x: %s" % (pos, text.split(",")[0])
        pos += n
    if pos != end:
        return "listing covers %d of %d bytes" % (pos - start, end - start)
    return None


def format_line(text, enc):
    return "\t%-46s ; encoding: [%s]" % (text, ", ".join("0x%02x" % b for b in enc))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("listing")
    ap.add_argument("--blob", required=True)
    ap.add_argument("--out")
    ap.add_argument("--mcpu", default="gfx1030")
    args = ap.parse_args()
    global MCPU
    MCPU = args.mcpu
    table()

    lines = open(args.listing, encoding="utf-8", errors="replace").read().splitlines()
    m = HDR.search(lines[0]) if lines else None
    if not m:
        sys.exit("no code range header in the listing")
    start, end = int(m.group(1), 16), int(m.group(2), 16)
    declared = re.search(r"\b(gfx\d+)\b", lines[0])
    if declared and declared.group(1) != args.mcpu:
        sys.exit("the listing was disassembled for %s but --mcpu is %s; pass --mcpu %s" % (
            declared.group(1), args.mcpu, declared.group(1)))
    try:
        with open(args.blob, "rb") as f:
            blob = f.read()
    except OSError as e:
        sys.exit("cannot read the program bytes %s: %s" % (args.blob, e.strerror or e))
    if end > len(blob):
        sys.exit("the listing declares code up to 0x%x but the blob is %d bytes" % (end, len(blob)))

    body = []                       # (original line, encoding, parsed instruction, text before the encoding)
    for ln in lines[1:]:
        if ln.startswith("; ---"):
            break
        mm = LINE.match(ln)
        if not mm:
            continue
        ins = shader_ir.parse_line(ln)
        if not ins:
            sys.exit("cannot parse listing line, refusing to edit: " + ln.strip())
        body.append((ln, bytes(int(x, 16) for x in mm.group(2).split(",")), ins, mm.group(1).strip()))

    out = [lines[0]]                # output lines, preserving the original text of every untouched line
    pairs = []                      # (text, encoding) of the output body, for the end-to-end check
    pos, i = start, 0
    rebuilt = emitted = dropped = 0
    while i < len(body):
        ln, enc, ins, txt = body[i]
        n = instruction_length(blob, pos, ins)
        if n:
            out.append(ln)
            pairs.append((txt, enc))
            pos += n
            i += 1
            continue
        run, q = [], pos
        while wide_form(blob, q, end):
            run.append((q, wide_form(blob, q, end)))
            q += 8
        if run:
            step = misdecode_run(blob, pos, q, end, body, i) if len(run) > 1 else None
            if step is None and len(run) == 1:
                step = resync(blob, pos, end, body, i)
            if step is not None:
                drop, newpos, swallow = step
                for at, rec in run:
                    out.append(format_line(rec, blob[at:at + 8]))
                    pairs.append((rec, blob[at:at + 8]))
                for t, e in swallow:
                    out.append(format_line(t, e))
                    pairs.append((t, e))
                emitted += len(swallow)
                dropped += drop
                rebuilt += len(run)
                i += drop
                pos = newpos
                continue
        out.append(ln)              # not understood here: copy through untouched, in the listing's own order
        pairs.append((txt, enc))
        pos += len(enc)
        i += 1

    problem = verify(blob, start, end, pairs)
    summary = "rebuilt %d word(s), re-emitted %d swallowed instruction(s), dropped %d mis-decoded line(s)" % (
        rebuilt, emitted, dropped)
    if problem:
        # Never write a listing this tool built from a partial reconciliation. With --out the input is copied
        # verbatim so the caller still gets one file per input and the translator sees exactly what it saw before.
        if args.out and os.path.abspath(args.out) != os.path.abspath(args.listing):
            with open(args.listing, "rb") as src, open(args.out, "wb") as dst:
                dst.write(src.read())
            print("%s; input copied unchanged to %s (%s)" % (summary, args.out, problem))
        else:
            print("%s; input left unchanged (%s)" % (summary, problem))
        return 0
    target = args.out or args.listing
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("%s; wrote %s" % (summary, target))
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
