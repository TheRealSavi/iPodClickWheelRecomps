"""Authored instruction fixtures for Phase's upstream recompiler gaps."""

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "common/tools"))
from recomp.arm import Op, decode
from recomp.cfg import Role, discover
from recomp.cpp import FunctionEmitter
from recomp.functions import Function, Kind, _is_call_site
from recomp.image import EAppImage

BASE = 0x18000000
ENTRY = BASE + 0x100


def fixture(words):
    data = bytearray(0x300)
    data[:4] = b"eapp"
    struct.pack_into("<I", data, 0x14, ENTRY)
    for i, word in enumerate(words):
        struct.pack_into("<I", data, 0x100 + 4 * i, word)
    return EAppImage(bytes(data))


def emit(words):
    image = fixture(words)
    body = discover(ENTRY, image, {ENTRY})
    function = Function(ENTRY, ENTRY + len(words) * 4, "test", Kind.GAME, 0, 0, False)
    return body, FunctionEmitter(body, function, image, {ENTRY: function}).emit()


class PhaseInstructions(unittest.TestCase):
    def test_absolute_pointer_jump_table_does_not_decode_pointer_as_code(self):
        words = [
            0xE3500001,
            0x979FF100,
            0xE12FFF1E,
            ENTRY + 24,
            ENTRY + 32,
            0,
            0xE3A01001,
            0xE12FFF1E,
            0xE3A01002,
            0xE12FFF1E,
        ]
        body, source = emit(words)
        self.assertEqual(body.roles[ENTRY + 4], Role.JUMP_TABLE)
        self.assertNotIn(ENTRY + 12, body.instructions)
        self.assertIn("case 1: goto L_18000120", source)

    def test_signed_word_half_multiply(self):
        insn = decode(ENTRY, 0xE12AA18B)
        self.assertEqual(
            (insn.name, insn.rd, insn.rn, insn.rm, insn.rs), ("smlawb", 10, 10, 11, 1)
        )
        _, source = emit([0xE12AA18B, 0xE12FFF1E])
        self.assertIn("signed_half_multiply", source)

    def test_saturating_add(self):
        insn = decode(ENTRY, 0xE103B054)
        self.assertEqual((insn.name, insn.rd, insn.rm, insn.rn), ("qadd", 11, 4, 3))
        _, source = emit([0xE103B054, 0xE12FFF1E])
        self.assertIn("saturating_arithmetic", source)

    def test_clz(self):
        insn = decode(ENTRY, 0xE16FCF11)
        self.assertEqual(insn.op.value, "clz")
        self.assertEqual((insn.rd, insn.rm), (12, 1))
        body, source = emit([0xE16FCF11, 0xE12FFF1E])
        self.assertEqual(len(body.instructions), 2)
        self.assertIn("count_leading_zeros(cpu.r[1])", source)

    def test_blx_returns_to_following_instruction(self):
        body, source = emit([0xE12FFF31, 0xE3A00007, 0xE12FFF1E])
        self.assertEqual(body.roles[ENTRY], Role.INDIRECT_CALL)
        self.assertEqual(len(body.instructions), 3)
        self.assertIn("cpu.r[LR] = 0x18000104u", source)

    def test_conditional_blx_preserves_condition(self):
        _, source = emit([0x112FFF32, 0xE12FFF1E])
        self.assertIn("if (cond_ne(cpu))", source)

    def test_blx_lr_captures_target_before_writing_link(self):
        _, source = emit([0xE12FFF3E, 0xE12FFF1E])
        self.assertLess(source.index("target = cpu.r[LR]"), source.index("cpu.r[LR] ="))
        self.assertTrue(_is_call_site(0xE12FFF3E))

    def test_negative_offset_import_thunk(self):
        data = bytearray(fixture([0xE12FFF1E]).data)
        struct.pack_into("<I", data, 0x10, BASE + 0x180)
        data[0x180:0x189] = b"Settings\0"
        struct.pack_into("<III", data, 0x1B0, 1, BASE + 0x1C0, 0xE51FF004)
        image = EAppImage(bytes(data))
        self.assertEqual(image.thunks[BASE + 0x1B8].framework, "Settings")

    def test_doubleword_store_and_load(self):
        store = decode(ENTRY, 0xE1C044F0)
        self.assertEqual(
            (store.op.value, store.load, store.rd, store.rn), ("ldrd", False, 4, 0)
        )
        self.assertEqual(store.offset.immediate, 0x40)
        _, source = emit([0xE1C044F0, 0xE1C024D0, 0xE12FFF1E])
        self.assertIn("st32(address + 4u, cpu.r[5])", source)
        self.assertIn("cpu.r[3] = ld32(address + 4u)", source)


if __name__ == "__main__":
    unittest.main()
