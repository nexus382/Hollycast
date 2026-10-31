#!/usr/bin/env python3
"""Exercise missing-surface detection through the comparator's CLI (Pillow required)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


class CaptureComparisonTest(unittest.TestCase):
    def compare(self, reference, candidate):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            reference.save(folder / "reference.png")
            candidate.save(folder / "candidate.png")
            return subprocess.run(
                [sys.executable, str(Path(__file__).with_name("compare_capture.py")),
                 str(folder / "reference.png"), str(folder / "candidate.png")],
                capture_output=True, text=True)

    def test_visible_content_disappearing_in_dark_scene_fails(self):
        reference = Image.new("RGB", (10, 10))
        reference.putpixel((4, 4), (255, 255, 255))
        result = self.compare(reference, Image.new("RGB", (10, 10)))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("visible reference surfaces are absent", result.stderr)

    def test_identical_visible_scene_passes(self):
        reference = Image.new("RGB", (10, 10))
        reference.putpixel((4, 4), (255, 255, 255))
        result = self.compare(reference, reference)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_empty_reference_cannot_establish_success(self):
        result = self.compare(Image.new("RGB", (10, 10)), Image.new("RGB", (10, 10)))
        self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
