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


if __name__ == "__main__":
    unittest.main()
