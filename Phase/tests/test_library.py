"""Authored binary fixtures test the actual C++ iTunesDB reader and SDK bridge."""

import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

EXE = Path(__file__).resolve().parents[1] / "build/phase-startup.exe"


def record(tag, size, children=b"", fields=()):
    header = bytearray(size)
    struct.pack_into("<4sII", header, 0, tag, size, size + len(children))
    for offset, fmt, value in fields:
        struct.pack_into("<" + fmt, header, offset, value)
    return bytes(header) + children


def string(kind, text):
    encoded = text.encode("utf-16le")
    return record(
        b"mhod",
        24,
        struct.pack("<4I", 1, len(encoded), 1, 0) + encoded,
        ((12, "I", kind),),
    )


def database():
    tracks = []
    for index, media in ((1, 1), (2, 1), (3, 2)):
        text = string(1, "Café" if index == 1 else "B") + string(
            2, ":iPod_Control:Music:F00:A.wav"
        )
        text += string(3, "Album") + string(4, "Alpha" if index == 1 else "Beta")
        text += string(5, "Rock")
        tracks.append(
            record(
                b"mhit",
                0xD4,
                text,
                (
                    (16, "I", index),
                    (0x28, "I", 169733),
                    (0x70, "Q", 0xABCDEF0123456788 + index),
                    (0xD0, "I", media),
                ),
            )
        )
    track_list = record(b"mhlt", 12, b"".join(tracks), ((8, "I", len(tracks)),))
    items = b"".join(record(b"mhip", 76, fields=((24, "I", i),)) for i in (1, 2, 1, 3))
    playlist = record(
        b"mhyp", 184, string(1, "Phase") + items, ((0x1C, "Q", 0x8765432112345678),)
    )
    master = record(
        b"mhyp", 184, string(1, "Library"), ((0x14, "B", 1), (0x1C, "Q", 1))
    )
    playlists = record(b"mhlp", 12, master + playlist, ((8, "I", 2),))
    return record(
        b"mhbd",
        32,
        record(b"mhsd", 16, track_list, ((12, "I", 1),))
        + record(b"mhsd", 16, playlists, ((12, "I", 3),)),
    )


@unittest.skipUnless(EXE.exists(), "Build the Windows target first")
class LibraryContracts(unittest.TestCase):
    def run_fixture(self, data):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "iPod_Control/iTunes/iTunesDB"
            target.parent.mkdir(parents=True)
            target.write_bytes(data)
            result = subprocess.run(
                [str(EXE), "--test-library", temporary],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            self.assertEqual(target.read_bytes(), data, "reader mutated iTunesDB")
            return result

    def test_metadata_bridge(self):
        result = self.run_fixture(database())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS library", result.stdout)

    def test_truncated_database_is_rejected(self):
        result = self.run_fixture(database()[:-1])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected uncompressed iTunesDB", result.stderr)

    def test_zero_record_size_is_rejected(self):
        data = bytearray(database())
        struct.pack_into("<I", data, data.index(b"mhit") + 8, 0)
        result = self.run_fixture(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("short iTunesDB record", result.stderr)
