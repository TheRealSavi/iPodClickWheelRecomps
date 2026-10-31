"""Create a native profile, exit, and verify Phase restores its menu on restart."""

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from verify_menu import ASSETS, MENU_TEXT, PROJECT, checked_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    checked_image(args.image)
    for name, expected in ASSETS.items():
        assert hashlib.sha256((args.assets / name).read_bytes()).hexdigest() == expected
    with tempfile.TemporaryDirectory(prefix="phase-restart-") as temporary:
        saves = Path(temporary) / "saves"
        results = []
        for label in ("create", "reload"):
            screenshot = PROJECT / f"build/persistence-{label}.ppm"
            command = [
                str(PROJECT / "build/phase-startup.exe"),
                str(args.image.resolve()),
                "--assets",
                str(args.assets.resolve()),
                "--save-dir",
                str(saves),
                "--frames",
                "400",
                "--screenshot",
                str(screenshot),
            ]
            if label == "create":
                command += ["--input-script", str(PROJECT / "tests/menu-input.txt")]
            result = subprocess.run(
                command, capture_output=True, text=True, timeout=30, check=False
            )
            (PROJECT / f"build/persistence-{label}.log").write_text(result.stderr)
            assert result.returncode == 0, result.stderr[-4000:]
            pixels = screenshot.read_bytes().split(b"\n", 3)[3]
            assert len(pixels) == 320 * 240 * 3
            mask = bytes(
                min(pixels[(y * 320 + x) * 3 : (y * 320 + x) * 3 + 3]) > 220
                for y in range(86, 220)
                for x in range(35, 180)
            )
            assert hashlib.sha256(mask).hexdigest() == MENU_TEXT, label
            results.append(result.stderr)
        assert "SDK Users[4]" in results[0]
        assert "SAVE committed save_1.dtb" in results[0]
        assert "SAVE loaded profiles=1 records=4 recovered=0" in results[1]
        for log in results:
            assert "STAGE shutdown-complete" in log
            assert "STAGE teardown-returned" in log
        assert "SAVE committed en/stats" in results[0]
        assert "SDK Users[4]" not in results[1], "restart created a replacement profile"
        assert "FILE open save_1.dtb" in results[1]
        assert "FILE missing save_1" not in results[1], "native save failed to reload"
        receipt = {
            "profile_created": True,
            "profile_restored_without_input": True,
            "native_save_reopened": True,
            "native_shutdown_and_teardown_completed": True,
            "main_menu_text_sha256": MENU_TEXT,
            "scope": "Two separate native processes; original profile/save loader and rendered menu.",
        }
        (PROJECT / "build/persistence-verification.json").write_text(
            json.dumps(receipt, indent=2) + "\n"
        )
        print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
