"""A runtime configure without Vulkan headers must say so loudly, or fail when asked to.

Everything under ``if(TARGET Vulkan::Headers)`` in port/CMakeLists.txt (the D3D12 COM objects, the Vulkan D3D12
device in palmetto and most of ctest) is left out without Vulkan headers. It used to be one STATUS line, so a build
whose VULKAN_SDK never reached CMake ran 45 of 125 tests and reported "100% tests passed".

Runs port/cmake/vulkan_headers_missing.cmake with ``cmake -P`` (no compiler needed); skipped without cmake.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULE = ROOT / "port" / "cmake" / "vulkan_headers_missing.cmake"
CMAKE_LISTS = ROOT / "port" / "CMakeLists.txt"
CMAKE = shutil.which("cmake")


def run_module(require):
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "driver.cmake"
        script.write_text(f'include("{MODULE.as_posix()}")\npalmetto_vulkan_headers_missing()\n', encoding="utf-8")
        args = [CMAKE]
        if require is not None:
            args.append(f"-DPALMETTO_REQUIRE_VULKAN={require}")
        args += ["-P", str(script)]
        return subprocess.run(args, capture_output=True, text=True, timeout=60)


@unittest.skipUnless(CMAKE, "cmake not on PATH")
class VulkanMissingMessageTest(unittest.TestCase):
    def test_default_warns_and_names_what_is_left_out(self):
        r = run_module(None)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("CMake Warning", r.stderr)
        text = " ".join(r.stderr.split())
        for part in ("Vulkan D3D12 device in palmetto", "PALMETTO_D3D12=vulkan", "D3D12 layer and GPU tests",
                     "CMakeCache.txt", "PALMETTO_VULKAN_HEADERS_FETCH=ON", "PALMETTO_REQUIRE_VULKAN=ON"):
            self.assertIn(part, text)

    def test_off_still_warns(self):
        r = run_module("OFF")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("CMake Warning", r.stderr)

    def test_require_fails_the_configure(self):
        r = run_module("ON")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("CMake Error", r.stderr)
        self.assertIn("Vulkan headers not found", r.stderr)


class CMakeListsUsesTheModuleTest(unittest.TestCase):
    def test_no_headers_branch_calls_the_module(self):
        text = CMAKE_LISTS.read_text(encoding="utf-8")
        self.assertIn('option(PALMETTO_REQUIRE_VULKAN ', text)
        # The else() of if(TARGET Vulkan::Headers) is the last top-level else before its endif.
        m = re.search(r"^if\(TARGET Vulkan::Headers\)\n.*?^else\(\)\n(.*?)^endif\(\)", text, re.S | re.M)
        self.assertIsNotNone(m, "if(TARGET Vulkan::Headers) ... else() ... endif() not found at top level")
        self.assertIn("palmetto_vulkan_headers_missing()", m.group(1))
        self.assertNotIn("message(STATUS", m.group(1))

    def test_ci_requires_vulkan(self):
        # The runtime workflow is .github/workflows/port.yml now (runtime.yml was removed), and the checkout's
        # CMake source tree is port/ (runtime/ was renamed). Measured today: port.yml has three configure steps,
        # each passing -DPALMETTO_REQUIRE_VULKAN=ON.
        ci = (ROOT / ".github" / "workflows" / "port.yml").read_text(encoding="utf-8")
        configures = re.findall(r"cmake -S port -B port/build.*?(?=\n\s*- name:)", ci, re.S)
        self.assertGreaterEqual(len(configures), 2)
        for c in configures:
            self.assertIn("-DPALMETTO_REQUIRE_VULKAN=ON", c)


if __name__ == "__main__":
    unittest.main()
