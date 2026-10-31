"""Exercise the compiled Windows adapter with authored assets only."""

import subprocess
import tempfile
import unittest
from pathlib import Path

EXE = Path(__file__).resolve().parents[1] / "build/phase-startup.exe"


@unittest.skipUnless(EXE.exists(), "Build the Windows target first")
class HostContracts(unittest.TestCase):
    def test_host_contracts(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "fixture.bin"
            fixture.write_bytes(b"abcdef")
            result = subprocess.run(
                [str(EXE), "--test-host", temporary],
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS:", result.stdout)
            self.assertEqual(fixture.read_bytes(), b"abcdef")
            dimmed = (Path(temporary) / "idle-dim.ppm").read_bytes().split(b"\n", 3)[3]
            restored = (
                (Path(temporary) / "idle-restored.ppm").read_bytes().split(b"\n", 3)[3]
            )
            self.assertEqual(dimmed[:3], bytes([91, 0, 0]))
            self.assertEqual(
                restored,
                (Path(temporary) / "resize-320x240.ppm")
                .read_bytes()
                .split(b"\n", 3)[3],
            )
            for width, height, left, top, content_width, content_height in (
                (960, 540, 120, 0, 720, 540),
                (500, 900, 0, 262, 500, 375),
                (319, 241, 0, 1, 319, 239),
                (160, 120, 0, 0, 160, 120),
                (320, 240, 0, 0, 320, 240),
            ):
                header, size, maximum, pixels = (
                    (Path(temporary) / f"resize-{width}x{height}.ppm")
                    .read_bytes()
                    .split(b"\n", 3)
                )
                self.assertEqual(
                    (header, size, maximum),
                    (b"P6", f"{width} {height}".encode(), b"255"),
                )
                self.assertEqual(len(pixels), width * height * 3)
                for y in range(height):
                    for x in range(width):
                        pixel = pixels[(y * width + x) * 3 : (y * width + x) * 3 + 3]
                        inside = (
                            left <= x < left + content_width
                            and top <= y < top + content_height
                        )
                        self.assertEqual(any(pixel), inside, (width, height, x, y))
                for fx, fy, color in (
                    (0.25, 0.25, b"\xff\x00\x00"),
                    (0.75, 0.25, b"\x00\xff\x00"),
                    (0.25, 0.75, b"\x00\x00\xff"),
                    (0.75, 0.75, b"\xff\xff\xff"),
                ):
                    x, y = (
                        left + int(content_width * fx),
                        top + int(content_height * fy),
                    )
                    self.assertEqual(
                        pixels[(y * width + x) * 3 : (y * width + x) * 3 + 3], color
                    )
                if width == 960:
                    # Smooth magnification blends at the red/green boundary.
                    i = (100 * width + width // 2) * 3
                    self.assertTrue(0 < pixels[i] < 255 and 0 < pixels[i + 1] < 255)


if __name__ == "__main__":
    unittest.main()
