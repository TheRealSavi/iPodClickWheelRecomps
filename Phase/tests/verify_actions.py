"""Replay formerly unmapped menu actions against the owner's original game.

Creates isolated profiles; never reads or writes the interactive save directory.
Screenshots and SDK logs stay in the ignored build directory for visual review.
"""

import argparse
import json
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from verify_menu import PROJECT, checked_image


class Input:
    def __init__(self):
        self.frame = 300
        self.rows = []

    def key(self, scan, down):
        self.rows.append(f"{self.frame} {scan:x} {int(down)}")

    def press(self, scan=0x1F):
        self.key(scan, True)
        self.frame += 2
        self.key(scan, False)
        self.frame += 80
        return self

    def wheel(self, halves):
        ring = [0x18, 0x19, 0x27, 0x35, 0x34, 0x33, 0x25, 0x17]
        direction = 1 if halves >= 0 else -1
        active = {ring[0]}
        self.key(ring[0], True)
        self.frame += 10
        for half in range(1, abs(halves) + 1):
            index = (half // 2 * direction) % 8
            wanted = {ring[index]}
            if half % 2:
                wanted.add(ring[(index + direction) % 8])
            for scan in sorted(wanted - active):
                self.key(scan, True)
            for scan in sorted(active - wanted):
                self.key(scan, False)
            active = wanted
            self.frame += 10
        for scan in sorted(active):
            self.key(scan, False)
        self.frame += 30
        return self

    def choose(self, steps):
        if steps:
            self.wheel(steps)
        return self.press()


def profiles(directory):
    data = (directory / "state.bin").read_bytes()
    assert data[:8] == b"PHSAVE01"
    assert struct.unpack_from("<I", data, 8)[0] == 2
    next_id, count = struct.unpack_from("<II", data, 12)
    offset = 20
    result = {}
    for _ in range(count):
        identity, size = struct.unpack_from("<II", data, offset)
        offset += 8
        result[identity] = data[offset : offset + size].decode("utf-8")
        offset += size
    return next_id, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    checked_image(args.image)
    output = PROJECT / "build/menu-actions"
    output.mkdir(exist_ok=True)
    receipts = []
    with tempfile.TemporaryDirectory(prefix="phase-actions-") as temporary:
        root = Path(temporary)

        def run(label, script, source=None, expected=()):
            saves = root / label
            if source:
                shutil.copytree(source, saves)
            else:
                saves.mkdir()
            replay = output / f"{label}.txt"
            replay.write_text("\n".join(script.rows) + "\n")
            result = subprocess.run(
                [
                    str(PROJECT / "build/phase-startup.exe"),
                    str(args.image.resolve()),
                    "--assets",
                    str(args.assets.resolve()),
                    "--save-dir",
                    str(saves),
                    "--frames",
                    str(script.frame + 60),
                    "--input-script",
                    str(replay),
                    "--screenshot",
                    str(output / f"{label}.ppm"),
                ],
                capture_output=True,
                text=True,
                timeout=90,
                check=False,
            )
            (output / f"{label}.log").write_text(result.stderr)
            assert result.returncode == 0, result.stderr[-3000:]
            for text in (
                *expected,
                "STAGE shutdown-complete",
                "STAGE teardown-returned",
            ):
                assert text in result.stderr, (label, text)
            receipts.append(
                {
                    "case": label,
                    "profiles": profiles(saves),
                    "calls": sorted(
                        set(re.findall(r"SDK (\w+\[\d+\])", result.stderr))
                    ),
                }
            )
            print(f"PASS {label}: {profiles(saves)}", flush=True)
            return saves

        seed = run(
            "seed", Input().press().wheel(-3).press(), expected=("SDK Users[4]",)
        )
        assert profiles(seed) == (2, {1: "A"})
        run(
            "brightness",
            Input().choose(3).choose(1).wheel(-8).press(0x11),
            seed,
            ("SDK miscTBD[2]", "SDK miscTBD[1]", "DISPLAY brightness="),
        )
        renamed = run(
            "rename",
            Input()
            .choose(3)
            .choose(2)
            .choose(1)
            .press()
            .wheel(1)
            .press()
            .wheel(-4)
            .press(),
            seed,
            ("SDK Users[9]",),
        )
        assert profiles(renamed) == (2, {1: "AB"})
        reloaded = run("reload-renamed", Input(), renamed, ("FILE open save_1.dtb",))
        assert profiles(reloaded) == (2, {1: "AB"})
        deleted = run(
            "delete",
            Input().choose(3).choose(2).choose(2).press().press(),
            seed,
            ("SDK Users[3]",),
        )
        assert profiles(deleted) == (2, {})
        recreated = run(
            "recreate",
            Input().press().wheel(-3).press(),
            deleted,
            ("SDK Users[4]", "SAVE committed save_2.dtb"),
        )
        assert profiles(recreated) == (3, {2: "A"})
        two = run(
            "second-user",
            Input().choose(3).choose(2).press().wheel(1).press().wheel(-4).press(),
            seed,
            ("SDK Users[4]",),
        )
        assert profiles(two) == (3, {1: "A", 2: "B"})
        remaining = run(
            "delete-first-of-two",
            Input().choose(3).choose(2).choose(3).wheel(-1).press().press(),
            two,
            ("SDK Users[3]",),
        )
        assert profiles(remaining) == (3, {2: "B"})
        run("reload-second-id", Input(), remaining, ("FILE open save_2.dtb",))
    (output / "receipt.json").write_text(json.dumps(receipts, indent=2) + "\n")
    print(
        f"PASS: {len(receipts)} native menu-action/restart replays; receipts in {output}"
    )


if __name__ == "__main__":
    main()
