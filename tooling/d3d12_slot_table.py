"""Print the absolute vtable slot of every method of a D3D12 interface, from an SDK d3d12.h.

A console (Xbox) D3D12 object's vtable slots can only be named by comparing the title's
observed call order with a real interface layout. This extracts that layout from the
Windows SDK header the port already builds against, including the inherited base chain, so
the slot number in a trace or a decompiled call site can be looked up instead of recalled.

    python -I tooling/d3d12_slot_table.py --header "<Windows Kits>/10/Include/<version>/um/d3d12.h"
    python -I tooling/d3d12_slot_table.py --header <path> --interface ID3D12GraphicsCommandList --json
    python -I tooling/d3d12_slot_table.py --header <path> --interface ID3D12Device --interface ID3D12CommandQueue

The header path can also come from the D3D12_HEADER environment variable. Only the header is
read; no game files. The base chain is resolved recursively from each interface's
`: public BASE` clause, with IUnknown's three methods (QueryInterface, AddRef, Release) as the
root, so slot numbers are absolute vtable indices - the same numbers a console trace reports.

Standard library only.
"""
import argparse
import json
import os
import re
import sys

IUNKNOWN = ["QueryInterface", "AddRef", "Release"]
# Interfaces the parser needs as roots even though the header declares them with a macro.
DEFAULT_INTERFACES = ["ID3D12Device", "ID3D12CommandQueue", "ID3D12CommandAllocator", "ID3D12Fence",
                      "ID3D12GraphicsCommandList", "ID3D12DescriptorHeap", "ID3D12Resource"]
# A method line: "virtual void STDMETHODCALLTYPE Name(" or "STDMETHOD(Name)(".
METHOD = re.compile(r"STDMETHODCALLTYPE\s+(\w+)\s*\(|STDMETHOD\((\w+)\)")
DECLARATION = re.compile(r"^\s*(\w+)\s*:\s*public\s+(\w+)\s*$", re.M)
# The self-test's header: three interfaces in one chain, enough to exercise the absolute-slot
# arithmetic without an SDK install.
SYNTHETIC_HEADER = """
MIDL_INTERFACE("00000000-0000-0000-0000-000000000001")
    ID3D12Object : public IUnknown
    {
    public:
        virtual HRESULT STDMETHODCALLTYPE GetPrivateData( ) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPrivateData( ) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface( ) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetName( ) = 0;
    };
MIDL_INTERFACE("00000000-0000-0000-0000-000000000002")
    ID3D12DeviceChild : public ID3D12Object
    {
    public:
        virtual void STDMETHODCALLTYPE GetDevice( ) = 0;
    };
MIDL_INTERFACE("00000000-0000-0000-0000-000000000003")
    ID3D12Target : public ID3D12DeviceChild
    {
    public:
        virtual void STDMETHODCALLTYPE First( ) = 0;
        virtual void STDMETHODCALLTYPE Second( ) = 0;
    };
"""


def selftest():
    """True when the parser puts a three-interface chain at the expected absolute slots."""
    table = interfaces(SYNTHETIC_HEADER)
    rows = {slot: method for slot, method, _owner in chain(table, "ID3D12Target")}
    expected = {0: "QueryInterface", 2: "Release", 3: "GetPrivateData", 6: "SetName",
                7: "GetDevice", 8: "First", 9: "Second"}
    failures = ["slot %d: %r != %r" % (slot, rows.get(slot), method)
                for slot, method in expected.items() if rows.get(slot) != method]
    if len(rows) != 10:
        failures.append("expected 10 slots, got %d" % len(rows))
    for failure in failures:
        print("d3d12_slot_table self-test: " + failure, file=sys.stderr)
    return not failures


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def interfaces(header):
    """{name: (base or None, [own method names])} for every interface declared with `: public BASE`."""
    text = _strip_comments(header)
    found = {}
    for match in DECLARATION.finditer(text):
        name, base = match.group(1), match.group(2)
        if not name.startswith("ID3D12") and name not in ("IUnknown",):
            continue
        brace = text.find("{", match.end())
        if brace < 0:
            continue
        depth = 0
        end = -1
        for i in range(brace, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end < 0:
            continue
        body = text[brace:end]
        methods = []
        for m in METHOD.finditer(body):
            method = m.group(1) or m.group(2)
            if method not in methods:
                methods.append(method)
        found[name] = (base, methods)
    return found


def chain(table, name):
    """[(absolute slot, method, declaring interface)] for interface `name`, base chain first."""
    base, methods = table.get(name, (None, []))
    out = []
    if base is None or base in ("IUnknown",):
        out = [(slot, method, "IUnknown") for slot, method in enumerate(IUNKNOWN)]
    else:
        out = chain(table, base)
    start = len(out)
    out.extend((start + i, method, name) for i, method in enumerate(methods))
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--header", default=os.environ.get("D3D12_HEADER"),
                        help="path to the SDK's d3d12.h (default: $D3D12_HEADER)")
    parser.add_argument("--interface", action="append", default=[],
                        help="interface to print (repeatable; default: the console objects' interfaces)")
    parser.add_argument("--json", action="store_true", help="print one JSON object instead of text")
    parser.add_argument("--list", action="store_true", help="list every ID3D12 interface the header declares")
    parser.add_argument("--selftest", action="store_true",
                        help="run the parser's self-test on a synthetic header and exit")
    args = parser.parse_args(argv)
    if args.selftest:
        if not selftest():
            return 1
        print("d3d12_slot_table: self-test passed")
        return 0
    if not args.header:
        parser.error("--header is required (or set D3D12_HEADER)")
    try:
        with open(args.header, encoding="utf-8-sig", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        parser.error("cannot read %s: %s" % (args.header, error))
    table = interfaces(text)
    if not table:
        parser.error("%s does not declare the D3D12 interfaces (is it the SDK's d3d12.h?)" % args.header)
    if args.list:
        for name in sorted(table):
            print(name)
        return 0
    names = args.interface or DEFAULT_INTERFACES
    result = {}
    for name in names:
        if name not in table:
            print("d3d12_slot_table: %s is not declared in %s" % (name, args.header), file=sys.stderr)
            result[name] = []
            continue
        result[name] = [{"slot": slot, "method": method, "declared_by": owner}
                        for slot, method, owner in chain(table, name)]
    if args.json:
        print(json.dumps({"header": os.path.basename(args.header), "interfaces": result}, indent=2))
        return 0 if all(result.values()) else 1
    for name, rows in result.items():
        print("== %s ==" % name)
        for row in rows:
            print("%4d  %-8s  %s::%s" % (row["slot"], "+0x%x" % (row["slot"] * 8),
                                         row["declared_by"], row["method"]))
    return 0 if all(result.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
