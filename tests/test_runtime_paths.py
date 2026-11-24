"""The Runtime workflow's path filter still lists every file under tooling/ that its jobs read.

    python -I tests/test_runtime_paths.py

``.github/workflows/port.yml`` starts on the paths it lists, not on everything outside an ignore list
(BUILD-856): a change to a tool the runtime build never reads should not queue a Windows, Wine and llvmpipe run.
The price is that the list can fall behind. If ``tooling/rdna2spirv.py`` starts importing a new ``tooling/foo.py``,
that pull request still runs Runtime (it touches ``rdna2spirv.py``), but a later one that changes only
``foo.py`` would skip the GPU tests without anyone noticing. This test recomputes what the jobs read and fails
when an entry is missing:

* the seeds are the code that runs during the jobs: the CMake files under ``port/`` and the workflow's
  ``jobs:``, with ``#`` comments removed, and the Python under ``port/`` plus the scripts the workflow runs.
  A ``tooling/<name>`` a CMake line or a job step spells out is needed; so is a ``tools`` module a Python seed
  imports (the scripts and shader checks put ``tooling/`` on ``sys.path``) and a ``tooling/`` file it names in a
  string, such as ``os.path.join(TOOLS, "rdna2spirv.py")``;
* from there, each needed tool's sibling imports, the ``tooling/`` data files it names (``gfx10_formats.json`` is
  loaded by name, which a ``.py``-only scan misses), modules it loads with ``importlib`` and ``tooling/``
  subdirectories it names, to a fixed point.

Comments, docstrings and the C++, shader and header sources are not read as dependencies: a ``tooling/<name>`` in
them says where a table or a test came from, not that the build runs it, and counting those would make every
such note a required entry.

It also checks that the ``push`` and ``pull_request`` lists are the same, and that no ``tooling/`` entry names a
file that no longer exists. Listing more than the jobs need is allowed on purpose: an extra run costs one
queue slot, a missing entry loses the build for a tool the workflow reads.

Only tracked sources and the workflow are read; no game files, no build tools, no PyYAML.
"""
import ast
import os
import re
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKFLOW = ".github/workflows/port.yml"

# Entries that are needed for reasons a source scan cannot see: the build's own trees, the workflow, and the
# file that decides the line endings every checked-out source gets.
ALWAYS = ("port/**", "vendor/**", WORKFLOW, ".gitattributes")

TOOLS_REF = re.compile(r"tooling/([A-Za-z0-9_][A-Za-z0-9_.-]*)")
SCRIPT_REF = re.compile(r"tooling/scripts/[A-Za-z0-9_./-]+\.py")


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def path_lists(root):
    """{event: [paths]} for the events that filter on paths, exactly as the workflow spells them."""
    text = read(root, WORKFLOW)
    lists = {}
    for event in ("push", "pull_request"):
        match = re.search(r"^  %s:\n((?:    .*\n)+)" % event, text, re.M)
        if not match:
            raise AssertionError(f"{WORKFLOW} has no `{event}:` trigger the way this test expects it")
        body = match.group(1)
        paths = re.search(r"^    paths:\n((?:      - .*\n)+)", body, re.M)
        if not paths:
            raise AssertionError(f"{WORKFLOW} `{event}:` has no `paths:` list; update this test with the workflow")
        lists[event] = [line.strip()[1:].strip().strip("\"'") for line in paths.group(1).splitlines()]
    return lists


def covered(path, patterns):
    for pattern in patterns:
        if pattern.endswith("/**"):
            if path.startswith(pattern[:-2]):
                return True
        elif path == pattern:
            return True
    return False


def tracked(root, top):
    result = subprocess.run(["git", "ls-files", "-z", "--", top], cwd=root, capture_output=True, text=True)
    if result.returncode == 0 and result.stdout:
        return [n for n in result.stdout.split("\0") if n]
    names = []                                     # no git: walk, as tests/test_docs_links.py does
    for base, dirs, files in os.walk(os.path.join(root, top)):
        dirs[:] = [d for d in dirs if d not in (".git", "__pycache__", "build")]
        for name in files:
            names.append(os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/"))
    return names


def python_refs(source):
    """(modules, strings) of a Python file: what it imports, including importlib.import_module("x") and
    __import__("x"), and its string constants other than docstrings. Comments never reach the AST."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return set(), set()
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                docstrings.add(id(first.value))
    modules, strings = set(), set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            modules.add(node.module.split(".")[0])
        elif isinstance(node, ast.Call) and node.args and isinstance(node.args[0], ast.Constant):
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
            if name in ("import_module", "__import__") and isinstance(node.args[0].value, str):
                modules.add(node.args[0].value.split(".")[0])
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings:
            strings.add(node.value)
    return modules, strings


def strip_hash_comments(text):
    """CMake and YAML without their # comments (a # inside a quoted string is rare enough in both to ignore)."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def needed(root):
    """{tools entry: why} for everything under tooling/ the Runtime jobs read."""
    tools = os.path.join(root, "tooling")
    entries = os.listdir(tools)
    modules = {e[:-3] for e in entries if e.endswith(".py")}
    data = {e for e in entries if os.path.isfile(os.path.join(tools, e)) and not e.endswith(".py")}
    subdirs = {e for e in entries if os.path.isdir(os.path.join(tools, e)) and not e.startswith((".", "__"))}

    def entry(name):
        name = name.rstrip(".")
        if name in subdirs:
            return f"tooling/{name}/**"
        if name in modules or name.endswith(".py") and name[:-3] in modules:
            return f"tooling/{name if name.endswith('.py') else name + '.py'}"
        if name in data:
            return f"tooling/{name}"
        return None

    def spelled(text):
        for name in TOOLS_REF.findall(text):
            first = name.split("/")[0]
            yield first if first in subdirs else name

    def python_names(source, own_dir):
        """tooling/ entries a Python file imports or names in a string; own_dir's files shadow tooling/."""
        imports, strings = python_refs(source)
        names = {m + ".py" for m in imports
                 if m in modules and not (own_dir and os.path.exists(os.path.join(own_dir, m + ".py")))}
        for s in strings:
            names.update(spelled(s))
            base = s.rstrip("/").split("/")[-1]
            if base in data or base in subdirs or base.endswith(".py") and base[:-3] in modules:
                if not (own_dir and "/" not in s and os.path.exists(os.path.join(own_dir, base))):
                    names.add(base)
        return names

    workflow = read(root, WORKFLOW)
    # Only what the jobs run: the trigger's own list would make every listed entry "needed".
    jobs = strip_hash_comments(workflow[workflow.find("\njobs:"):] if "\njobs:" in workflow else "")
    found = {}
    for name in spelled(jobs):
        hit = entry(name)
        if hit:
            found.setdefault(hit, f"run by {WORKFLOW}")
    runtime = tracked(root, "runtime")
    cmake = [f for f in runtime if f.endswith(".cmake") or f.split("/")[-1] == "CMakeLists.txt"]
    python = [f for f in runtime if f.endswith(".py")] + sorted(set(SCRIPT_REF.findall(jobs)))
    for seed in cmake:
        for name in spelled(strip_hash_comments(read(root, seed))):
            hit = entry(name)
            if hit:
                found.setdefault(hit, f"named in {seed}")
    for seed in python:
        if os.path.isfile(os.path.join(root, seed)):
            own = os.path.dirname(os.path.join(root, seed))
            for name in python_names(read(root, seed), own):
                hit = entry(name)
                if hit:
                    found.setdefault(hit, f"read by {seed}")

    queue = list(found)
    while queue:
        current = queue.pop()
        if current.endswith("/**"):
            sources = [f for f in tracked(root, current[:-3]) if f.endswith(".py")]
        elif current.endswith(".py"):
            sources = [current]
        else:
            continue
        for source in sources:
            for name in python_names(read(root, source), None):
                hit = entry(name)
                if hit and hit not in found:
                    found[hit] = f"via {source}"
                    queue.append(hit)
    return found


def problems(root):
    lists = path_lists(root)
    issues = []
    events = sorted(lists)
    for other in events[1:]:
        if lists[other] != lists[events[0]]:
            issues.append(f"the `{events[0]}` and `{other}` path lists differ")
    patterns = lists[events[0]]
    for must in ALWAYS:
        if must not in patterns:
            issues.append(f"{must}: missing (always needed)")
    for script in sorted(set(SCRIPT_REF.findall(read(root, WORKFLOW)))):
        if not covered(script, patterns):
            issues.append(f"{script}: run by the workflow but not listed")
    for path, why in sorted(needed(root).items()):
        if not covered(path, patterns):
            issues.append(f"{path}: missing ({why})")
    for pattern in patterns:
        if pattern.startswith("tooling/") and not pattern.endswith("/**"):
            if not os.path.exists(os.path.join(root, pattern)):
                issues.append(f"{pattern}: listed but does not exist")
    return issues


class RepositoryRuntimePathsTest(unittest.TestCase):
    def test_the_scan_sees_the_repository(self):
        found = needed(ROOT)
        # The translator the GPU tests run through and the stub generator CMake runs at configure time.
        self.assertIn("tooling/rdna2spirv.py", found)
        self.assertIn("tooling/gen_d3d12_stubs.py", found)

    def test_every_tool_the_jobs_read_triggers_the_workflow(self):
        issues = problems(ROOT)
        self.assertFalse(issues, f"{WORKFLOW} path filter:\n" + "\n".join(f"  {i}" for i in issues))


class SyntheticTreeTest(unittest.TestCase):
    """The scan flags what a path grep misses, on a tree built for the purpose."""

    def build(self, tmp, listed):
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

        paths = "".join(f'      - "{p}"\n' for p in ALWAYS + ("tooling/scripts/**",) + tuple(listed))
        write(WORKFLOW, "name: Runtime\n\non:\n  push:\n    branches: [main]\n    paths:\n" + paths +
              "  pull_request:\n    paths:\n" + paths + "\njobs:\n  x:\n    steps:\n"
              "      - run: python3 -I tooling/scripts/run_gpu_tests.py\n")
        write("port/CMakeLists.txt", "add_custom_command(COMMAND python tooling/translator.py)\n")
        write("tooling/scripts/run_gpu_tests.py", "import sys\nimport subgroup_info\n")
        write("tooling/translator.py", "import lanes\nimport json\nTABLE = 'formats.json'\nRUN = 'tooling/runner.py'\n")
        write("tooling/runner.py", "X = 1\n")
        write("tooling/lanes.py", "import importlib\nM = importlib.import_module('late')\n")
        write("tooling/late.py", "ORACLE = 'oracle'\n")
        write("tooling/oracle/run.py", "import helper\n")
        write("tooling/helper.py", "X = 1\n")
        write("tooling/formats.json", "{}\n")
        write("tooling/subgroup_info.py", "X = 1\n")
        write("tooling/unrelated.py", "X = 1\n")
        # Notes, not reads: none of these may make tooling/unrelated.py a required entry.
        write("port/src/table.h", "// generated by tooling/unrelated.py\nconst char *k = \"tooling/unrelated.py\";\n")
        write("port/test/shaders/probe.asm", "; checked with tooling/unrelated.py\n")
        write("port/test/shaders/check.py", '"""Words from tooling/unrelated.py."""\n# see tooling/unrelated.py\n')
        write("port/cmake/extra.cmake", "# tooling/unrelated.py made this list\n")

    def test_a_complete_list_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.build(tmp, ["tooling/translator.py", "tooling/lanes.py", "tooling/late.py", "tooling/oracle/**",
                             "tooling/helper.py", "tooling/formats.json", "tooling/subgroup_info.py",
                             "tooling/runner.py"])
            self.assertEqual(problems(tmp), [])
            self.assertNotIn("tooling/unrelated.py", needed(tmp))

    def test_each_kind_of_dependency_is_reported_when_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.build(tmp, ["tooling/translator.py", "tooling/gone.py"])
            issues = "\n".join(problems(tmp))
            for missing in ("tooling/lanes.py", "tooling/late.py", "tooling/oracle/**", "tooling/helper.py",
                            "tooling/formats.json", "tooling/subgroup_info.py", "tooling/runner.py"):
                self.assertIn(f"{missing}: missing", issues)
            self.assertIn("tooling/gone.py: listed but does not exist", issues)

    def test_lists_that_differ_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.build(tmp, [])
            path = os.path.join(tmp, WORKFLOW)
            text = read(tmp, WORKFLOW).replace('    paths:\n', '    paths:\n      - "tooling/extra.py"\n', 1)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            self.assertTrue(any("path lists differ" in i for i in problems(tmp)))


if __name__ == "__main__":
    unittest.main()
