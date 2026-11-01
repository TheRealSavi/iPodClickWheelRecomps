"""Check that HD observation leaves original game state unchanged through the menu."""

import argparse
import filecmp
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
from image_contract import checked_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    checked_image(args.image)
    snapshots = PROJECT / "build/hd-state-verification"
    for scale in (1, 4):
        destination = snapshots / str(scale)
        destination.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [
                str(PROJECT / "build/phase-startup.exe"),
                str(args.image.resolve()),
                "--assets",
                str(args.assets.resolve()),
                "--frames",
                "400",
                "--quiet-sdk",
                "--render-scale",
                str(scale),
                "--input-script",
                str(PROJECT / "tests/menu-input.txt"),
                "--dump-state",
                str(destination),
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        (destination / "session.log").write_text(result.stderr)
        assert result.returncode == 0, result.stderr[-4000:]
        assert "frame-returned=399 state=1" in result.stderr
        if scale == 4:
            assert "Phase HD: 1280x960" in result.stderr
            assert "Phase HD unavailable" not in result.stderr
    for name in (
        "11000000.bin",
        "18000000.bin",
        "19000000.bin",
        "40000000.bin",
        "registers.txt",
    ):
        assert filecmp.cmp(
            snapshots / "1" / name, snapshots / "4" / name, shallow=False
        ), name
    print(
        "PASS: original framebuffer, all guest memory, registers and flags identical after 400 frames with HD on/off"
    )


if __name__ == "__main__":
    main()
