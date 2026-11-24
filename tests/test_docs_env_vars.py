"""Every PALMETTO_* / PALMETTO_* environment variable the runtime and build_port.py read is named in a document.

    python -I tests/test_docs_env_vars.py

`port/README.md` documents the runtime's variables one by one, but five that the `xgameruntime` shim reads
(`PALMETTO_DEVICE_TYPE` and four more) were in no document at all. This collects the variable names that appear as
string literals in `port/src`, `port/shims` and `build_port.py` and checks that each is written in some
tracked Markdown file. Sources and Markdown only: no build, no GPU, no game files.
"""
import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# A variable name as a string literal: "PALMETTO_X", L"PALMETTO_X" or 'PALMETTO_X'.
LITERAL = re.compile(r"""["']((?:PALMETTO|PALMETTO)_[A-Z0-9_]+)["']""")
CODE = ("port/src/", "port/shims/", "build_port.py")
# Set by the runtime for itself, not by a person: palmetto passes the first to the copy of itself it starts with the
# image base reserved (port/src/base_reservation.h), and the second is a name that is only looked up to make
# the CRT build its environment table (port/shims/process.cpp).
INTERNAL = {"PALMETTO_BASE_RESERVED", "PALMETTO_RT_ENV_INIT"}


def tracked_files():
    try:
        out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
        return [p for p in out.splitlines() if p]
    except (OSError, subprocess.CalledProcessError):
        found = []
        for root, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__", "vendor", ".pytest_cache")]
            found += [os.path.relpath(os.path.join(root, f), ROOT).replace(os.sep, "/") for f in files]
        return found


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def variables_read(sources):
    """{variable: first "file:line" that names it} over `sources` ({name: text})."""
    found = {}
    for name in sorted(sources):
        for number, line in enumerate(sources[name].splitlines(), 1):
            for variable in LITERAL.findall(line):
                found.setdefault(variable, f"{name}:{number}")
    return found


def undocumented(sources, pages):
    """[(variable, where it is read)] for variables no page of `pages` ({name: text}) names as a whole word."""
    text = "\n".join(pages.values())
    return [(variable, where) for variable, where in sorted(variables_read(sources).items())
            if variable not in INTERNAL and not re.search(rf"(?<![A-Z0-9_]){variable}(?![A-Z0-9_])", text)]


class EnvironmentVariablesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        files = tracked_files()
        cls.sources = {f: read(f) for f in files
                       if f.startswith(CODE) and f.endswith((".cpp", ".h", ".py"))}
        cls.pages = {f: read(f) for f in files if f.endswith(".md") and not f.startswith("vendor/")}

    def test_the_scan_finds_the_variables(self):
        # A moved folder or a changed spelling must not turn the check below into a no-op.
        found = variables_read(self.sources)
        self.assertGreaterEqual(len(found), 20, sorted(found))
        for known in ("PALMETTO_UNIMPLEMENTED", "PALMETTO_D3D12", "PALMETTO_DEVICE_TYPE"):
            self.assertIn(known, found)
        self.assertGreater(len(self.pages), 20)

    def test_the_internal_variables_are_still_read(self):
        self.assertEqual(sorted(INTERNAL - set(variables_read(self.sources))), [])

    def test_every_variable_is_named_in_a_document(self):
        missing = undocumented(self.sources, self.pages)
        self.assertEqual(missing, [], "read by the code and named in no .md file:\n" + "\n".join(
            f"  {variable} ({where})" for variable, where in missing))

    def test_an_undocumented_variable_is_reported(self):
        sources = {"port/shims/x.cpp": 'GetEnvironmentVariableA("PALMETTO_ONE", s, n);\n'
                                          'kName[] = L"PALMETTO_TWO";\n// PALMETTO_COMMENT is not a literal\n',
                   "build_port.py": "os.environ.get('PALMETTO_THREE')\nos.environ.get(\"PALMETTO_BASE_RESERVED\")\n"}
        self.assertEqual(variables_read(sources),
                         {"PALMETTO_THREE": "build_port.py:1", "PALMETTO_BASE_RESERVED": "build_port.py:2",
                          "PALMETTO_ONE": "port/shims/x.cpp:1", "PALMETTO_TWO": "port/shims/x.cpp:2"})
        # A longer name that contains the variable does not document it.
        pages = {"a.md": "Set `PALMETTO_ONE=1`.", "b.md": "PALMETTO_TWO_MORE and XPALMETTO_THREE"}
        self.assertEqual(undocumented(sources, pages),
                         [("PALMETTO_THREE", "build_port.py:1"), ("PALMETTO_TWO", "port/shims/x.cpp:2")])


if __name__ == "__main__":
    unittest.main()
