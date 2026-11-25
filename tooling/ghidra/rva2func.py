"""Map an image RVA to its containing PE function using the x64 exception table.

The .pdata directory (PE data directory index 3) holds a RUNTIME_FUNCTION entry (begin RVA,
end RVA, unwind RVA) for every function the compiler emitted unwind info for. That is the
authoritative function map of an x64 MSVC image and it needs no analysis pass, so a caller
return address measured at run time becomes the exact range to hand a decompiler.

    python -I tooling/ghidra/rva2func.py <image> 0x325c0 0x24821e3
    python -I tooling/ghidra/rva2func.py <image> --list 0x32000 0x33500

RVAs are hex, with or without `0x`. Only RVAs and sizes are printed - never file bytes - so the
output can be pasted into a report. Standard library only; no PE library needed.
"""
import argparse
import bisect
import struct
import sys


class Image:
    """A PE image's section table and exception directory, read from the file."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        data = self.data
        if data[:2] != b"MZ":
            raise ValueError("%s is not a PE image (no MZ signature)" % path)
        pe = self._u32(0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s has no PE signature" % path)
        coff = pe + 4
        sections = self._u16(coff + 2)
        opt_size = self._u16(coff + 16)
        opt = coff + 20
        magic = self._u16(opt)
        if magic == 0x20B:      # PE32+
            self.image_base = self._u64(opt + 24)
            dirs = opt + 112
        elif magic == 0x10B:    # PE32
            self.image_base = self._u32(opt + 28)
            dirs = opt + 96
        else:
            raise ValueError("unsupported optional header magic 0x%x" % magic)
        self.sections = []
        for i in range(sections):
            at = opt + opt_size + i * 40
            name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
            self.sections.append((name, self._u32(at + 12), self._u32(at + 8),
                                  self._u32(at + 20), self._u32(at + 16)))
        exc_rva, exc_size = struct.unpack_from("<II", data, dirs + 3 * 8)
        self.functions = []
        if exc_rva and exc_size:
            raw = self.raw(exc_rva, exc_size)
            for i in range(0, len(raw) - 11, 12):
                begin, end, unwind = struct.unpack_from("<III", raw, i)
                if begin and end > begin:
                    self.functions.append((begin, end, unwind))
        self.functions.sort()
        self._begins = [f[0] for f in self.functions]

    def _u16(self, at):
        return struct.unpack_from("<H", self.data, at)[0]

    def _u32(self, at):
        return struct.unpack_from("<I", self.data, at)[0]

    def _u64(self, at):
        return struct.unpack_from("<Q", self.data, at)[0]

    def raw(self, rva, size):
        """The file bytes of [rva, rva+size), or an error when the range leaves its section."""
        for _name, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                offset = rva - vaddr
                if offset + size <= rawsize:
                    return self.data[rawptr + offset:rawptr + offset + size]
                raise ValueError("RVA 0x%x crosses the section's raw data" % rva)
        raise ValueError("RVA 0x%x is not in a section" % rva)

    def containing(self, rva):
        """(begin, end, unwind) of the function holding `rva`, or None."""
        i = bisect.bisect_right(self._begins, rva) - 1
        if i >= 0:
            begin, end, unwind = self.functions[i]
            if begin <= rva < end:
                return begin, end, unwind
        return None

    def in_range(self, lo, hi):
        """Functions whose begin RVA is in [lo, hi)."""
        return [f for f in self.functions if lo <= f[0] < hi]


def parse_rva(text):
    return int(str(text).strip(), 16)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("image", help="the PE image (the title or a shipped DLL)")
    parser.add_argument("rva", nargs="*", help="RVA(s) to map to their containing function")
    parser.add_argument("--list", nargs=2, metavar=("LO", "HI"),
                        help="list the functions with a begin RVA in [LO, HI)")
    args = parser.parse_args(argv)
    try:
        image = Image(args.image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.list:
        for begin, end, _unwind in image.in_range(parse_rva(args.list[0]), parse_rva(args.list[1])):
            print("0x%x-0x%x  (size 0x%x)" % (begin, end, end - begin))
        return 0
    if not args.rva:
        parser.error("give at least one RVA, or --list LO HI")
    status = 0
    for text in args.rva:
        try:
            rva = parse_rva(text)
        except ValueError:
            print("0x%s  not a hex RVA" % text)
            status = 2
            continue
        found = image.containing(rva)
        if not found:
            print("0x%x  no function in the exception table" % rva)
            status = 1
            continue
        begin, end, _unwind = found
        print("0x%x  function 0x%x-0x%x  (offset +0x%x, size 0x%x)"
              % (rva, begin, end, rva - begin, end - begin))
    return status


if __name__ == "__main__":
    sys.exit(main())
