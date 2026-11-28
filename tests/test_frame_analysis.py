"""frame_analysis.py: the capture/readback PPM reader used for frame evidence.

    python -I tests/test_frame_analysis.py

Every fixture is built here as a synthetic PPM in a temporary directory (four
known bands, a black frame, a truncated file, an index with a missing and an
extra frame, a write-control readback), so the test needs no game data, no GPU
and makes no repository writes. The reader is loaded from
tooling/scripts/frame_analysis.py by path, so it does not depend on sys.path.
"""
import importlib.util
import os
import shutil
import struct
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "tooling", "scripts", "frame_analysis.py")
BANDS = ((204, 51, 102), (51, 204, 102), (51, 102, 204), (255, 255, 255))
WRITE_CONTROL = (51, 102, 204)


def load_tool():
    spec = importlib.util.spec_from_file_location("frame_analysis", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FRAME = load_tool()


def write_ppm(path, width, height, pixels):
    with open(path, "wb") as handle:
        handle.write(("P6\n%d %d\n255\n" % (width, height)).encode("ascii"))
        handle.write(bytes(pixels))


def band_frame(width, height):
    data = bytearray()
    for _y in range(height):
        for x in range(width):
            band = (4 * x) // width
            data.extend(BANDS[band if band < 4 else 3])
    return bytes(data)


def index_line(number, width, height, mean):
    return ("frame %d format 44 (VK_FORMAT_B8G8R8A8_UNORM) extent %dx%d"
            " mean rgb %.2f %.2f %.2f"
            % (number, width, height, mean[0], mean[1], mean[2]))


class FrameAnalysisTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="frame_analysis_test_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def make_capture(self, name, frames, width, height, index_lines=None):
        directory = os.path.join(self.root, name)
        os.makedirs(directory)
        for number, pixels in frames:
            write_ppm(os.path.join(directory, "frame_%05d.ppm" % number),
                      width, height, pixels)
        if index_lines is not None:
            with open(os.path.join(directory, "index.txt"), "w",
                      encoding="utf-8", newline="\n") as handle:
                handle.write("\n".join(index_lines) + "\n")
        return directory

    def test_four_band_capture_stats_and_index(self):
        width, height = 160, 90
        frame = band_frame(width, height)
        mean = [sum(frame[c::3]) / float(width * height) for c in range(3)]
        lines = [index_line(number, width, height, mean) for number in (1, 2)]
        capture = self.make_capture("capture-bands", [(1, frame), (2, frame)],
                                    width, height, lines)
        report = FRAME.analyse_capture(capture)
        self.assertEqual(report["summary"]["frame_files"], 2)
        self.assertEqual(report["summary"]["non_black"], 2)
        self.assertEqual(report["summary"]["synthetic_matches"], 2)
        stats = report["frames"][0]
        self.assertAlmostEqual(stats["mean"][0], 140.25, places=6)
        self.assertAlmostEqual(stats["mean"][1], 153.00, places=6)
        self.assertAlmostEqual(stats["mean"][2], 165.75, places=6)
        self.assertEqual(stats["min"], [51, 51, 102])
        self.assertEqual(stats["max"], [255, 255, 255])
        self.assertEqual(stats["non_black"], stats["pixels"])
        checks = {check["check"]: check for check in report["checks"]}
        self.assertTrue(checks["index_vs_files"]["ok"])
        self.assertTrue(checks["index_vs_computed"]["ok"])
        self.assertEqual(report["summary"]["best_frame"], "frame_00001.ppm")

    def test_black_capture_is_flagged(self):
        capture = self.make_capture("capture-black",
                                    [(1, bytes(8 * 6 * 3))], 8, 6,
                                    [index_line(1, 8, 6, [0.0, 0.0, 0.0])])
        report = FRAME.analyse_capture(capture)
        self.assertEqual(report["summary"]["black"], 1)
        self.assertFalse(report["summary"]["any_non_black"])
        self.assertEqual(report["frames"][0]["max"], [0, 0, 0])

    def test_truncated_ppm_is_an_error_not_black(self):
        width, height = 16, 9
        directory = os.path.join(self.root, "capture-truncated")
        os.makedirs(directory)
        path = os.path.join(directory, "frame_00001.ppm")
        with open(path, "wb") as handle:
            handle.write(("P6\n16 9\n255\n").encode("ascii")
                         + band_frame(width, height)[:-9])
        report = FRAME.analyse_capture(directory)
        self.assertEqual(len(report["errors"]), 1)
        self.assertIn("truncated", report["errors"][0]["error"])
        self.assertIn("error", report["frames"][0])

    def test_index_mismatch_reported(self):
        width, height = 16, 9
        frame = band_frame(width, height)
        lines = [index_line(1, width, height, [0.0, 0.0, 0.0]),
                 index_line(99, width, height, [1.0, 2.0, 3.0])]
        capture = self.make_capture("capture-mismatch",
                                    [(1, frame), (2, bytes(16 * 9 * 3))],
                                    width, height, lines)
        report = FRAME.analyse_capture(capture)
        checks = {check["check"]: check for check in report["checks"]}
        self.assertEqual(checks["index_vs_files"]["index_without_file"], [99])
        self.assertEqual(checks["index_vs_files"]["file_without_index"], [2])
        self.assertFalse(checks["index_vs_computed"]["ok"])

    def test_readback_write_control_classification(self):
        directory = os.path.join(self.root, "readback-selftest")
        os.makedirs(directory)
        write_ppm(os.path.join(directory,
                              "readback_draw6382_res1889f6b6720_16x16.ppm"),
                  16, 16, bytes(WRITE_CONTROL) * (16 * 16))
        write_ppm(os.path.join(directory,
                              "readback_draw6383_res1889f6b6720_16x16.ppm"),
                  16, 16, bytes(16 * 16 * 3))
        report = FRAME.analyse_readback(directory)
        self.assertEqual(report["summary"]["write_control"], 1)
        self.assertEqual(report["summary"]["black"], 1)
        self.assertFalse(report["summary"]["any_content"])
        parsed = report["frames"][0]["readback"]
        self.assertEqual(parsed["draw"], 6382)
        self.assertEqual(parsed["resource"], "1889f6b6720")

    def test_png_round_trip_and_downsample(self):
        width, height = 160, 90
        frame = band_frame(width, height)
        path = os.path.join(self.root, "best.png")
        written = FRAME.write_png(path, width, height, frame)
        self.assertEqual(written["width"], width)
        with open(path, "rb") as handle:
            payload = handle.read()
        self.assertEqual(payload[:8], FRAME.PNG_SIGNATURE)
        self.assertEqual(struct.unpack(">II", payload[16:24]), (width, height))
        position = 8
        idat = b""
        while position + 8 <= len(payload):
            length = struct.unpack(">I", payload[position:position + 4])[0]
            tag = payload[position + 4:position + 8]
            if tag == b"IDAT":
                idat = payload[position + 8:position + 8 + length]
            position += 12 + length
        expected = b"".join(bytes(1) + frame[y * width * 3:(y + 1) * width * 3]
                              for y in range(height))
        self.assertEqual(zlib.decompress(idat), expected)
        small = FRAME.write_png(os.path.join(self.root, "small.png"),
                                width, height, frame, 64)
        self.assertEqual((small["width"], small["height"]), (54, 30))

    def test_arm_directory_resolution_and_outputs(self):
        arm_root = os.path.join(self.root, "fsrstable")
        boot = os.path.join(arm_root, "boot-tag1")
        capture = os.path.join(arm_root, "capture-tag1")
        readback = os.path.join(arm_root, "readback-tag1")
        for directory in (boot, capture, readback):
            os.makedirs(directory)
        width, height = 160, 90
        frame = band_frame(width, height)
        mean = [sum(frame[c::3]) / float(width * height) for c in range(3)]
        write_ppm(os.path.join(capture, "frame_00001.ppm"), width, height, frame)
        with open(os.path.join(capture, "index.txt"), "w", encoding="utf-8",
                  newline="\n") as handle:
            handle.write(index_line(1, width, height, mean) + "\n")
        write_ppm(os.path.join(readback,
                              "readback_draw6382_res1889f6b6720_16x16.ppm"),
                  16, 16, bytes(WRITE_CONTROL) * (16 * 16))
        with open(os.path.join(boot, "meta.txt"), "w", encoding="utf-8",
                  newline="\n") as handle:
            handle.write("tag=tag1\n")
            handle.write("timeout=30\n")
            handle.write("env=PALMETTO_CAPTURE=%s PALMETTO_READBACK_PPM=%s"
                         " PALMETTO_PRESENT_SYNTHETIC=8\n" % (capture, readback))
        with open(os.path.join(boot, "summary_console.txt"), "w",
                  encoding="utf-8", newline="\n") as handle:
            handle.write("VERDICT: ALIVE 30.0 s\n")
        with open(os.path.join(boot, "stderr.log"), "w", encoding="utf-8",
                  newline="\n") as handle:
            handle.write("[rt] replay: the Vulkan device was lost\n")
        report = FRAME.analyse_arm("tag1", arm_root)
        self.assertEqual(report["boot"], boot)
        self.assertEqual(len(report["captures"]), 1)
        self.assertEqual(len(report["readbacks"]), 1)
        self.assertEqual(report["summary"]["verdict"], "ALIVE 30.0 s")
        self.assertEqual(report["stderr"]["device_lost"], 1)
        self.assertEqual(report["switches"]["PALMETTO_PRESENT_SYNTHETIC"], "8")
        output = FRAME.write_outputs(report, os.path.join(self.root, "analysis"), 64)
        names = [os.path.basename(item["path"]) for item in output["written"]]
        self.assertIn("summary.txt", names)
        self.assertIn("summary.json", names)
        self.assertTrue(any(name.startswith("best_frame_capture_") for name in names))
        self.assertTrue(any(name.startswith("best_frame_readback_") for name in names))

    def test_suppressed_switches_are_named(self):
        arm_root = os.path.join(self.root, "fsrstable")
        boot = os.path.join(arm_root, "boot-tag2")
        os.makedirs(boot)
        with open(os.path.join(boot, "meta.txt"), "w", encoding="utf-8",
                  newline="\n") as handle:
            handle.write("tag=tag2\n")
            handle.write("env=PALMETTO_CAPTURE=first PALMETTO_CAPTURE="
                         " PALMETTO_READBACK_PPM=\n")
        report = FRAME.analyse_arm("tag2", arm_root)
        self.assertIn("capture", report["suppressed"])
        self.assertIn("readback", report["suppressed"])
        self.assertIn("suppressed by design", FRAME.one_line_summary(report))

def encode_png(path, width, height, rgb, filters):
    """Write an 8-bit RGB PNG with the given per-row filter types (test helper)."""
    channels = 3
    stride = width * channels
    raw = bytearray()
    previous = bytes(stride)
    for y in range(height):
        row = rgb[y * stride:(y + 1) * stride]
        kind = filters[y % len(filters)]
        residual = bytearray(stride)
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if kind == 0:
                predictor = 0
            elif kind == 1:
                predictor = left
            elif kind == 2:
                predictor = up
            elif kind == 3:
                predictor = (left + up) >> 1
            else:
                prediction = left + up - upleft
                distances = (abs(prediction - left), abs(prediction - up),
                             abs(prediction - upleft))
                predictor = (left, up, upleft)[distances.index(min(distances))]
            residual[i] = (row[i] - predictor) & 0xFF
        raw.append(kind)
        raw.extend(residual)
        previous = row
    payload = (FRAME.PNG_SIGNATURE
               + FRAME._png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
               + FRAME._png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
               + FRAME._png_chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(payload)


class FrameImageTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="frame_image_test_")
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def gradient(self, width, height):
        rgb = bytearray()
        for y in range(height):
            for x in range(width):
                rgb.extend(((x * 17) & 0xFF, (y * 41) & 0xFF, (x * 7 + y * 3) & 0xFF))
        return bytes(rgb)

    def test_png_reader_unfilters_every_filter_type(self):
        width, height = 12, 6
        rgb = self.gradient(width, height)
        path = os.path.join(self.root, "filters.png")
        encode_png(path, width, height, rgb, [0, 1, 2, 3, 4])
        decoded = FRAME.read_png(path)
        self.assertEqual(decoded.get("data"), rgb, decoded.get("error"))
        self.assertEqual((decoded["width"], decoded["height"]), (width, height))

    def test_png_reader_black_fast_path(self):
        path = os.path.join(self.root, "black.png")
        encode_png(path, 10, 5, bytes(10 * 5 * 3), [0, 1, 2, 4])
        decoded = FRAME.read_png(path)
        stats = FRAME.pixel_stats(decoded["data"], decoded["width"], decoded["height"])
        self.assertTrue(stats["black"])
        self.assertEqual(stats["non_black"], 0)
        self.assertIn("fast path", str(decoded.get("filters")))

    def test_images_mode_classifies_png_screenshots(self):
        directory = os.path.join(self.root, "images")
        os.makedirs(directory)
        width, height = 12, 6
        encode_png(os.path.join(directory, "shot_001.png"), width, height,
                   self.gradient(width, height), [0])
        encode_png(os.path.join(directory, "shot_002.png"), 10, 5,
                   bytes(10 * 5 * 3), [0])
        report = FRAME.analyse_images(directory)
        self.assertEqual(report["summary"]["content"], 1)
        self.assertEqual(report["summary"]["black"], 1)
        self.assertEqual(report["summary"]["best_frame"], "shot_001.png")
        output = FRAME.write_outputs(report, os.path.join(self.root, "analysis"), 0)
        copied = [item for item in output["written"] if item.get("copied_png")]
        self.assertEqual(len(copied), 1)
        self.assertTrue(os.path.isfile(copied[0]["path"]))


if __name__ == "__main__":
    unittest.main()
