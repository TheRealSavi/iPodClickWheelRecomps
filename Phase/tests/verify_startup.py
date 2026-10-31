"""Compare native startup with unchanged ARM execution at the first unsupported SDK call.

Run with uv run --with unicorn==2.1.4 python Phase/tests/verify_startup.py --image PATH.
Private memory snapshots and the receipt stay under Phase/build/.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

import unicorn as u
from unicorn import arm_const as a

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
sys.path.insert(0, str(PROJECT.parent / "common/tools"))
from image_contract import IMAGE_SHA256, checked_image
from recomp.image import EAppImage

REGIONS = [
    (0x11000000, 0x800000),
    (0x18000000, 0x800000),
    (0x19000000, 0x4000000),
    (0x40000000, 0x20000),
]
REGISTERS = [getattr(a, f"UC_ARM_REG_R{i}") for i in range(15)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    args = parser.parse_args()
    image = EAppImage(checked_image(args.image))
    dump = PROJECT / "build/startup-native"
    native = subprocess.run(
        [
            str(PROJECT / "build/phase-startup.exe"),
            str(args.image.resolve()),
            "--dump-state",
            str(dump),
        ],
        capture_output=True,
        text=True,
        timeout=15,
    )
    # This is an acceptance boundary, not a generic 'any failure is success'.
    expected_stop = "unimplemented OpenGLES[52], lr=180610f8"
    if native.returncode != 2 or expected_stop not in native.stderr:
        raise AssertionError(native.stderr)
    machine = u.Uc(u.UC_ARCH_ARM, u.UC_MODE_ARM)
    for base, length in REGIONS:
        machine.mem_map(base, length)
    machine.mem_write(image.load_base, image.data)
    machine.reg_write(a.UC_ARM_REG_CPSR, 0xD3)
    machine.reg_write(a.UC_ARM_REG_SP, 0x117FFF00)
    context = 0x19000000
    sentinel = 0x11000000
    next_arena = 0x19010000
    clock_us = 0
    calls = []
    stopped = False

    def sdk(uc, address, size, user):
        nonlocal next_arena, clock_us, stopped
        thunk = image.thunks[address]
        values = [uc.reg_read(reg) for reg in REGISTERS]
        calls.append(
            f"SDK {thunk.framework}[{thunk.ordinal}] lr={values[14]:08x} "
            + " ".join(f"r{i}={values[i]:08x}" for i in range(4))
        )
        if thunk.framework == "MemoryAlloc" and thunk.ordinal == 0:
            count = (values[0] + 15) & ~15
            if not count or next_arena + count > 0x1D000000:
                raise AssertionError("oracle arena exhausted")
            uc.reg_write(a.UC_ARM_REG_R0, next_arena)
            next_arena += count
        elif thunk.framework == "miscTBD" and thunk.ordinal == 12:
            clock_us += 1
            uc.mem_write(values[0], struct.pack("<I", clock_us))
            uc.reg_write(a.UC_ARM_REG_R0, clock_us)
        else:
            if (thunk.framework, thunk.ordinal, values[14]) != (
                "OpenGLES",
                52,
                0x180610F8,
            ):
                raise AssertionError(calls[-1])
            stopped = True
            uc.emu_stop()
            return
        uc.reg_write(a.UC_ARM_REG_PC, values[14])

    # Hooks only at framework thunks; every game instruction runs in Unicorn.
    for address in image.thunks:
        machine.hook_add(u.UC_HOOK_CODE, sdk, begin=address, end=address)

    def execute(entry):
        machine.reg_write(a.UC_ARM_REG_R0, context)
        machine.reg_write(a.UC_ARM_REG_R1, context + 0x100)
        machine.reg_write(a.UC_ARM_REG_R2, 0)
        machine.reg_write(a.UC_ARM_REG_R3, 0)
        machine.reg_write(a.UC_ARM_REG_LR, sentinel)
        machine.emu_start(entry, sentinel, timeout=10_000_000, count=5_000_000)
        if not stopped and machine.reg_read(a.UC_ARM_REG_PC) != sentinel:
            raise AssertionError("ARM execution exhausted its budget")

    execute(image.u32(0x18000014))
    clock_us += 16667
    execute(image.u32(0x18000024))
    assert stopped, "did not reach graphics boundary"
    native_calls = re.findall(r"^SDK .*$", native.stderr, re.MULTILINE)
    assert native_calls == calls, (native_calls, calls)
    comparisons = []
    for base, length in REGIONS:
        actual = (dump / f"{base:08x}.bin").read_bytes()
        expected = bytes(machine.mem_read(base, length))
        if actual != expected:
            differences = [
                (hex(base + i), actual[i : i + 4].hex(), expected[i : i + 4].hex())
                for i in range(0, length, 4)
                if actual[i : i + 4] != expected[i : i + 4]
            ]
            raise AssertionError((hex(base), differences[:20], len(differences)))
        comparisons.append(
            {
                "base": hex(base),
                "bytes": length,
                "native_sha256": hashlib.sha256(actual).hexdigest(),
                "arm_sha256": hashlib.sha256(expected).hexdigest(),
            }
        )
    native_registers = [
        int(x) for x in (dump / "registers.txt").read_text().splitlines()
    ]
    arm_registers = [machine.reg_read(r) for r in REGISTERS] + [
        machine.reg_read(a.UC_ARM_REG_CPSR)
    ]
    assert native_registers == arm_registers, (native_registers, arm_registers)
    receipt = {
        "image_sha256": IMAGE_SHA256,
        "sdk_calls": len(calls),
        "stop": expected_stop,
        "memory": comparisons,
        "registers_match": True,
        "scope": "Original initialization and first-frame prefix with identical arena and diagnostic-clock adapters; no renderer or audio.",
    }
    (PROJECT / "build/startup-verification.json").write_text(
        json.dumps(receipt, indent=2) + "\n"
    )
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
