"""Authored fixtures; no original game bytes are needed for these tests."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from generate import constructor_entries
from image_contract import checked_image
from discovery import stored_function_pointers
from recomp.image import EAppImage
from recomp.functions import build_function_table
from recomp.generate import Bindings, Generator

BASE = 0x18000000


def constructor_fixture():
    data = bytearray(0x3000)
    data[:4] = b"eapp"
    struct.pack_into("<I", data, 0x287C, 0x2900 - 0x2854)
    struct.pack_into("<I", data, 0x2880, 0x2908 - 0x285C)
    # One negative displacement and one positive, with low-bit flags.
    struct.pack_into("<I", data, 0x2900, (0x100 - 0x2900) & 0xFFFFFFFF)
    struct.pack_into("<I", data, 0x2904, (0x2A00 - 0x2904) | 3)
    return data


class StartupTools(unittest.TestCase):
    def test_generated_source_is_utf8_on_windows(self):
        data = bytearray(0x200)
        data[:4] = b"eapp"
        struct.pack_into("<I", data, 0x14, BASE + 0x100)
        struct.pack_into("<I", data, 0x100, 0xE12FFF1E)
        image = EAppImage(data)
        functions = build_function_table(image, [], set(), image.end, set())
        generator = Generator(
            image, functions, Bindings(), set(), "authored fixture", "phase"
        )
        generator.discover_all()
        with tempfile.TemporaryDirectory() as temporary:
            generator.write_all(Path(temporary))
            source = (Path(temporary) / "game_00.cpp").read_text(encoding="utf-8")
            self.assertIn("authored fixture", source)

    def test_constructor_offsets_are_relative_to_each_slot(self):
        self.assertEqual(
            constructor_entries(EAppImage(constructor_fixture())),
            {BASE + 0x100, BASE + 0x2A00},
        )

    def test_invalid_constructor_bounds_are_rejected(self):
        data = constructor_fixture()
        struct.pack_into("<I", data, 0x2880, 0x4000)
        with self.assertRaisesRegex(ValueError, "bounds"):
            constructor_entries(EAppImage(data))

    def test_external_constructor_target_is_rejected(self):
        data = constructor_fixture()
        struct.pack_into("<I", data, 0x2900, 0x01000000)
        with self.assertRaisesRegex(ValueError, "target outside"):
            constructor_entries(EAppImage(data))

    def test_stored_pointer_with_external_call_is_not_seeded(self):
        data = bytearray(0x400)
        data[:4] = b"eapp"
        struct.pack_into("<I", data, 0x80, BASE + 0x180)
        struct.pack_into("<II", data, 0x180, 0xEB7FFFFF, 0xE12FFF1E)
        self.assertNotIn(
            BASE + 0x180,
            stored_function_pointers(EAppImage(data), {BASE + 0x100, BASE + 0x300}),
        )

    def test_valid_stored_function_pointer_is_seeded(self):
        data = bytearray(0x400)
        data[:4] = b"eapp"
        struct.pack_into("<I", data, 0x80, BASE + 0x180)
        struct.pack_into("<I", data, 0x180, 0xE12FFF1E)
        self.assertIn(
            BASE + 0x180,
            stored_function_pointers(EAppImage(data), {BASE + 0x100, BASE + 0x300}),
        )

    def test_same_size_wrong_image_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "wrong.bin"
            path.write_bytes(b"eapp" + bytes(947248))
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                checked_image(path)


if __name__ == "__main__":
    unittest.main()
