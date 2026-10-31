"""Replay keyboard input through Phase's original name entry and check rendered menu text."""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
from image_contract import IMAGE_SHA256, checked_image

ASSETS = {
    "main.hdr": "15c365d262b8f173c31006c50ba4aef1343bef089fe5eff7b839f3521769ef5c",
    "main_0.ark": "f1f7e85156a4d0ea371637ea23c20139d698b149a32e3753970a60524a748a73",
    "global_soundbank.dat": "aaaa3564d1835a36f44caf88ee9f27ff00926a18c1f4fac13c63062004ae1660",
}
# White text mask from the visually verified original main menu at frame 399.
# Excludes the logo and most of the animated background. No game pixels are shipped.
MENU_TEXT = "2ba4d1e1b787650e5913f77e12cb6fa9405151d947e7c3d956e165f201105081"
EXIT_TEXT = "86e917ffc491087a1d8b5af22a05a97f5a0cac9ddfb9b37afd1274ef2e87311c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    checked_image(args.image)
    for name, expected in ASSETS.items():
        if hashlib.sha256((args.assets / name).read_bytes()).hexdigest() != expected:
            raise ValueError(
                f"Menu fixture requires the known build-3168002 asset: {name}"
            )
    screenshot = PROJECT / "build/menu-verification.ppm"
    result = subprocess.run(
        [
            str(PROJECT / "build/phase-startup.exe"),
            str(args.image.resolve()),
            "--assets",
            str(args.assets.resolve()),
            "--frames",
            "400",
            "--input-script",
            str(PROJECT / "tests/menu-input.txt"),
            "--screenshot",
            str(screenshot),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    (PROJECT / "build/menu-verification.log").write_text(result.stderr)
    if result.returncode or "STAGE frame-returned=399 state=1" not in result.stderr:
        raise AssertionError(result.stderr[-4000:])
    for service in ("Users[4]", "Users[8]", "AsyncFileIO[16]", "Metadata[61]"):
        assert f"SDK {service}" in result.stderr, service
    data = screenshot.read_bytes()
    assert data.startswith(b"P6\n320 240\n255\n")
    rgb = data.split(b"\n", 3)[3]
    assert len(rgb) == 320 * 240 * 3
    mask = bytes(
        min(rgb[(y * 320 + x) * 3 : (y * 320 + x) * 3 + 3]) > 220
        for y in range(86, 220)
        for x in range(35, 180)
    )
    digest = hashlib.sha256(mask).hexdigest()
    assert digest == MENU_TEXT, f"Main-menu text changed or not reached: {digest}"
    # A real Menu button at the main menu asks to exit. This catches a button
    # mapping copied from another title even when Select and the wheel work.
    back_script = PROJECT / "build/menu-back-input.txt"
    back_script.write_text(
        (PROJECT / "tests/menu-input.txt").read_text() + "330 11 1\n332 11 0\n"
    )
    back_image = PROJECT / "build/menu-back-verification.ppm"
    back = subprocess.run(
        [
            str(PROJECT / "build/phase-startup.exe"),
            str(args.image.resolve()),
            "--assets",
            str(args.assets.resolve()),
            "--frames",
            "370",
            "--input-script",
            str(back_script),
            "--screenshot",
            str(back_image),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    (PROJECT / "build/menu-back-verification.log").write_text(back.stderr)
    assert back.returncode == 0, back.stderr[-4000:]
    pixels = back_image.read_bytes().split(b"\n", 3)[3]
    back_mask = bytes(
        min(pixels[(y * 320 + x) * 3 : (y * 320 + x) * 3 + 3]) > 220
        for y in range(70, 220)
        for x in range(40, 280)
    )
    assert hashlib.sha256(back_mask).hexdigest() == EXIT_TEXT, (
        "W did not reach Exit Game confirmation"
    )
    receipt = {
        "image_sha256": IMAGE_SHA256,
        "assets": ASSETS,
        "frames": 400,
        "profile_created": True,
        "menu_text_sha256": digest,
        "menu_button_verified": True,
        "scope": "Keyboard replay through native name entry and menu rendering; session-local saves, silent audio, empty music library.",
    }
    (PROJECT / "build/menu-verification.json").write_text(
        json.dumps(receipt, indent=2) + "\n"
    )
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
