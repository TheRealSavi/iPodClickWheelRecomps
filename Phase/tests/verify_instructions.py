"""Differential authored ARMv5TE arithmetic/transfer checks on native Windows."""

import json
import random
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_recompiler import emit, ENTRY

words = [0xE16FCF11, 0x116F1F10]
words += [0xE103B054 | (kind << 21) for kind in range(4)]
for kind in range(4):
    for x in range(2):
        for y in range(2):
            rn = 0 if kind == 3 or kind == 1 and x else 3
            words.append(
                0xE1000080
                | kind << 21
                | 2 << 16
                | rn << 12
                | 1 << 8
                | x << 5
                | y << 6
                | 4
            )
words += [0xE1C044F0, 0xE1C024D0, 0xE1E044F0, 0xE0C024D0, 0xE18040F1, 0xE18020D1]
sources = []
for index, word in enumerate(words):
    _, source = emit([word, 0xE12FFF1E])
    sources.append(source.replace("f_18000100", f"fixture_{index}"))
dispatch = "\n".join(f"case {i}: fixture_{i}(cpu); return;" for i in range(len(words)))
(ROOT / "gen/instruction-fixtures.h").write_text(
    "#pragma once\nnamespace phase::game {\n"
    + "\n".join(sources)
    + "\nvoid instruction_fixture(unsigned i, Cpu& cpu) { switch(i) {\n"
    + dispatch
    + '\ndefault: fatal("unknown instruction fixture"); } }\n}\n',
    encoding="utf-8",
)
if "--generate" in sys.argv:
    print(f"generated {len(words)} instruction fixtures")
    sys.exit()

import unicorn as u
from unicorn import arm_const as a

randomizer = random.Random(3168002)
registers = [getattr(a, f"UC_ARM_REG_R{i}") for i in range(15)]
cases, expected = [], []
for index, word in enumerate(words):
    for iteration in range(64):
        values = [randomizer.getrandbits(32) for _ in range(15)]
        if iteration < 8:
            values = [
                (
                    0,
                    1,
                    0x7FFFFFFF,
                    0x80000000,
                    0xFFFFFFFF,
                    0x80008000,
                    0x7FFF7FFF,
                    0xFFFF0000,
                )[iteration]
            ] * 15
        flags = (iteration % 16) << 28 | (0x8000000 if iteration & 16 else 0)
        if index >= len(words) - 6:
            values[0], values[1] = 0x19050000, 8
        memory = [randomizer.getrandbits(32) for _ in range(32)]
        machine = u.Uc(u.UC_ARCH_ARM, u.UC_MODE_ARM)
        machine.mem_map(0x18000000, 0x1000)
        machine.mem_write(ENTRY, struct.pack("<I", word))
        machine.mem_map(0x19050000, 0x1000)
        machine.mem_write(0x19050000, struct.pack("<32I", *memory))
        machine.reg_write(a.UC_ARM_REG_CPSR, flags | 0x10)
        for register, value in zip(registers, values, strict=True):
            machine.reg_write(register, value)
        machine.emu_start(ENTRY, ENTRY + 4, count=1)
        expected.append(
            [machine.reg_read(r) for r in registers]
            + [machine.reg_read(a.UC_ARM_REG_CPSR) & 0xFF000000]
            + list(struct.unpack("<32I", machine.mem_read(0x19050000, 128)))
        )
        cases.append([index, flags] + values + memory)
result = subprocess.run(
    [str(ROOT / "build/phase-startup.exe"), "--instructions"],
    input="\n".join(" ".join(map(str, case)) for case in cases),
    text=True,
    capture_output=True,
    timeout=20,
)
assert result.returncode == 0, result.stderr
lines = result.stdout.splitlines()
assert len(lines) == len(expected), (len(lines), len(expected))
for index, (line, oracle) in enumerate(zip(lines, expected, strict=True)):
    actual = list(map(int, line.split()))
    assert actual == oracle, (hex(words[cases[index][0]]), index, actual, oracle)
report = {
    "matching_cases": len(cases),
    "instruction_encodings": len(words),
    "scope": "Authored CLZ, saturating arithmetic, signed halfword multiply, dual load/store; all registers, flags and touched memory match Unicorn 2.1.4. BLX/jump tables covered separately by structural tests.",
}
(ROOT / "build/instruction-verification.json").write_text(
    json.dumps(report, indent=2) + "\n"
)
print(json.dumps(report, indent=2))
