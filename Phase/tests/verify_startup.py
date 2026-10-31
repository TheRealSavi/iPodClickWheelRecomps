"""Compare native startup with unchanged ARM execution at the first unsupported SDK call.

Run with uv run --with unicorn==2.1.4 python Phase/tests/verify_startup.py --image PATH.
Private memory snapshots and the receipt stay under Phase/build/.
"""

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

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
    parser.add_argument(
        "--assets",
        type=Path,
        help="also verify three rendered frames and archive loading",
    )
    args = parser.parse_args()
    image = EAppImage(checked_image(args.image))
    dump = PROJECT / (
        "build/graphics-native" if args.assets else "build/startup-native"
    )
    screenshot = PROJECT / "build/verified-graphics.ppm"
    native = subprocess.run(
        [
            str(PROJECT / "build/phase-startup.exe"),
            str(args.image.resolve()),
            "--dump-state",
            str(dump),
            *(
                [
                    "--assets",
                    str(args.assets.resolve()),
                    "--frames",
                    "3",
                    "--screenshot",
                    str(screenshot),
                ]
                if args.assets
                else []
            ),
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    # This is an acceptance boundary, not a generic 'any failure is success'.
    expected_stop = "unimplemented OpenGLES[52], lr=180610f8"
    if args.assets:
        if native.returncode or "STAGE frame-returned=2 state=1" not in native.stderr:
            raise AssertionError(native.stderr)
    elif native.returncode != 2 or expected_stop not in native.stderr:
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
    objects = {51: 1, 52: 1}
    texture_mode = 0x2100
    files = {}
    next_file = 1
    completions = []

    def read32(address):
        return struct.unpack("<I", machine.mem_read(address, 4))[0]

    def write32(address, value):
        machine.mem_write(address, struct.pack("<I", value))

    def string_at(address):
        return bytes(machine.mem_read(address, 1024)).split(b"\0", 1)[0].decode()

    def graphics_sdk(thunk, values):
        nonlocal texture_mode, next_file
        ordinal = thunk.ordinal
        r0, r1, r2, r3 = values[:4]
        result = 0
        if thunk.framework == "OpenGLES":
            if ordinal in (51, 52):
                for i in range(r0):
                    write32(r1 + 4 * i, objects[ordinal])
                    objects[ordinal] += 1
            elif ordinal == 127:
                assert (r0, r1) == (0x2300, 0x2200)
                texture_mode = r2
            elif ordinal == 63:
                assert (r0, r1) == (0x2300, 0x2200)
                write32(r2, texture_mode)
            elif ordinal in (140, 160, 165):
                result = 1
            elif ordinal == 71:
                assert r0 == 0xC11  # scissor was never enabled
            elif ordinal not in (0, 5, 8, 9, 14, 18, 28, 32, 36, 163):
                raise AssertionError((thunk.framework, ordinal))
        elif thunk.framework == "InputEvents" and ordinal == 0:
            write32(r0, 0)
            write32(r1, 0)
        elif thunk.framework == "AsyncFileIO":
            request = r3 if ordinal == 0 else r0
            obj = read32(request + 0x14)
            count = 0
            if ordinal == 0:
                assert (r0, r2) == (0, 1)
                name = string_at(r1)
                assert name in ("main.hdr", "main_0.ark")
                content = (args.assets / name).read_bytes()
                files[next_file] = [content, 0]
                write32(request + 0x34, next_file)
                next_file += 1
                count = len(content)
                write32(request + 0x30, 0)
                assert not read32(request + 0x20)
            elif ordinal in (1, 2):
                handle = read32(request + 0x34)
                operation = machine.mem_read(request + 0x10, 1)[0]
                if operation == 2:
                    del files[handle]
                else:
                    assert operation == 3
                    content, position = files[handle]
                    chunk = content[position : position + read32(request + 0x24)]
                    machine.mem_write(read32(request + 0x20), chunk)
                    count = len(chunk)
                    files[handle][1] += count
                    write32(request + 0x30, files[handle][1])
            else:
                raise AssertionError((thunk.framework, ordinal))
            write32(request + 0x28, 0)
            write32(request + 0x2C, count)
            if obj:
                write32(obj + 8, count)
            write32(request, 0)
            completions.append(request)
            result = 1
        else:
            raise AssertionError((thunk.framework, ordinal))
        machine.reg_write(a.UC_ARM_REG_R0, result)

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
        elif args.assets:
            graphics_sdk(thunk, values)
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
        machine.emu_start(entry, sentinel, timeout=30_000_000, count=30_000_000)
        if not stopped and machine.reg_read(a.UC_ARM_REG_PC) != sentinel:
            raise AssertionError("ARM execution exhausted its budget")

    execute(image.u32(0x18000014))
    for frame in range(3 if args.assets else 1):
        if args.assets:
            write32(context + 0x38, 0x11010000)
            head = 0
            for request in reversed(completions):
                write32(request, head)
                head = request
            completions.clear()
            write32(context + 0x2C, head)
        clock_us += 16667
        execute(image.u32(0x18000024))
    assert bool(args.assets) or stopped, "did not reach graphics boundary"
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
    if args.assets:
        framebuffer = machine.mem_read(0x11010000, 320 * 240 * 2)
        rgb = bytearray()
        for (pixel,) in struct.iter_unpack("<H", framebuffer):
            rgb.extend(
                (
                    (pixel >> 11) * 255 // 31,
                    ((pixel >> 5) & 63) * 255 // 63,
                    (pixel & 31) * 255 // 31,
                )
            )
        assert screenshot.read_bytes() == b"P6\n320 240\n255\n" + rgb
    receipt = {
        "image_sha256": IMAGE_SHA256,
        "sdk_calls": len(calls),
        "stop": "three complete frames" if args.assets else expected_stop,
        "memory": comparisons,
        "registers_match": True,
        "scope": "Original initialization and three frames, native software framebuffer, header read and archive open; identical diagnostic host contracts, no audio/input fidelity."
        if args.assets
        else "Original initialization and first-frame prefix with identical arena and diagnostic-clock adapters; no renderer or audio.",
    }
    (
        PROJECT
        / (
            "build/graphics-verification.json"
            if args.assets
            else "build/startup-verification.json"
        )
    ).write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
