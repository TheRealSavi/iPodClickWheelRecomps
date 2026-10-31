"""Verify durable host saves in separate processes, without original game assets."""

import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

EXE = Path(__file__).resolve().parents[1] / "build/phase-startup.exe"


@unittest.skipUnless(EXE.exists(), "Build the Windows target first")
class PersistenceContracts(unittest.TestCase):
    def run_store(self, directory, mode, success=True):
        result = subprocess.run(
            [str(EXE), "--test-persistence", str(directory), mode],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0 if success else 2, result.stderr)
        return result

    def test_restart_and_stale_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            (Path(directory) / "pending-interrupted.tmp").write_bytes(b"incomplete")
            self.run_store(directory, "verify")

    def test_resource_kinds_remain_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "namespaces")
            self.run_store(directory, "verify-namespaces")

    def test_rename_delete_create_preserve_other_players(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            self.run_store(directory, "edit-profiles")
            self.run_store(directory, "verify-edits")

    def test_version_one_migrates_without_changing_native_records(self):
        def word(n):
            return struct.pack("<I", n)

        def blob(data):
            return word(len(data)) + data

        legacy = b"PHSAVE01" + word(1) + word(2) + blob(b"Alice") + blob(b"Bob")
        legacy += word(2)
        for name, data in [
            (b"save_1.dtb", bytes([0, 255, 3, 17])),
            (b"save_2.dtb", bytes([6, 5, 4])),
        ]:
            legacy += word(1) + blob(name) + blob(data)
        legacy += word(zlib.crc32(legacy))
        with tempfile.TemporaryDirectory() as directory:
            primary = Path(directory) / "state.bin"
            primary.write_bytes(legacy)
            self.run_store(directory, "verify")
            self.assertEqual(
                primary.read_bytes(), legacy, "read-only load rewrote snapshot"
            )
            self.run_store(directory, "edit-profiles")
            self.assertEqual(primary.read_bytes()[8:12], word(2))
            self.run_store(directory, "verify-edits")

    def test_corrupt_primary_recovers_previous_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            primary = Path(directory) / "state.bin"
            backup = Path(directory) / "state.bak"
            data = bytearray(primary.read_bytes())
            data[-1] ^= 1
            primary.write_bytes(data)
            self.run_store(directory, "verify-backup")
            prior_backup = backup.read_bytes()
            self.run_store(directory, "update")
            self.assertEqual(backup.read_bytes(), prior_backup)
            self.run_store(directory, "verify")

    def test_lost_primary_recovers_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            (Path(directory) / "state.bin").unlink()
            self.run_store(directory, "verify-backup")

    def test_unknown_version_does_not_roll_back(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            primary = Path(directory) / "state.bin"
            data = bytearray(primary.read_bytes())
            data[8:12] = (99).to_bytes(4, "little")
            primary.write_bytes(data)
            result = self.run_store(directory, "verify", success=False)
            self.assertIn("unsupported snapshot version", result.stderr)
            self.assertEqual(primary.read_bytes(), data)

    def test_both_corrupt_fail_without_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            for name in ("state.bin", "state.bak"):
                (Path(directory) / name).write_bytes(b"broken")
            self.run_store(directory, "verify", success=False)
            self.assertEqual((Path(directory) / "state.bin").read_bytes(), b"broken")

    def test_concurrent_writer_denied(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            with subprocess.Popen(
                [str(EXE), "--test-persistence", directory, "hold"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            ) as holder:
                try:
                    self.assertEqual(holder.stdout.readline().strip(), "LOCKED")
                    result = self.run_store(directory, "update", success=False)
                    self.assertIn("cannot lock", result.stderr)
                finally:
                    holder.communicate("\n", timeout=10)
            self.run_store(directory, "verify")

    def test_locked_primary_write_fails_without_acknowledgement(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            self.run_store(directory, "failed-write")
            self.run_store(directory, "verify")

    def test_failed_commit_keeps_last_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "seed")
            primary = Path(directory) / "state.bin"
            primary.unlink()  # Recover backup, which does not contain save_2 yet.
            self.run_store(directory, "verify-backup")
            primary.mkdir()  # Block replacement with an invalid target.
            self.run_store(directory, "update", success=False)
            primary.rmdir()
            self.run_store(directory, "verify-backup")

    def test_bounds_and_protected_roots(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_store(directory, "overlap", success=False)
            self.assertFalse((Path(directory) / "forbidden").exists())
            self.run_store(directory, "limits")


if __name__ == "__main__":
    unittest.main()
