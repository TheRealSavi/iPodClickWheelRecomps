"""Exercise the private five-song iOpenPod fixture through audible Hard gameplay.

No music, original code, or game pixels are distributed with this replay.
"""

import argparse
import hashlib
import json
import re
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
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()
    checked_image(args.image)
    preparation = json.loads((args.library / "preparation.json").read_text())
    if (
        len(preparation["tracks"]) != 5
        or preparation["tracks"][0]["title"] != "Poppies"
    ):
        raise ValueError("Replay requires the documented private five-track corpus")
    dependencies = {
        p: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in args.library.rglob("*")
        if p.is_file()
    }
    screenshot = PROJECT / "build/playback-verification.ppm"
    # Give the native resume countdown headroom on a busy machine. Keep the
    # minimum real playback assertion below rather than weakening it.
    frames = 2600
    result = subprocess.run(
        [
            str(PROJECT / "build/phase-startup.exe"),
            str(args.image.resolve()),
            "--assets",
            str(args.assets.resolve()),
            "--library",
            str(args.library.resolve()),
            "--audio",
            "--quiet-sdk",
            "--frames",
            str(frames),
            "--input-script",
            str(PROJECT / "tests/playback-input.txt"),
            "--screenshot",
            str(screenshot),
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    (PROJECT / "build/playback-verification.log").write_text(result.stderr)
    if result.returncode:
        raise AssertionError(result.stderr[-3000:])
    pid = preparation["tracks"][0]["pid"]
    for expected in (
        "LIBRARY loaded tracks=5 playlists=1",
        f"FILE open Phase Data/F000/{pid}",
        "T0001.wav rate=44100 channels=2",
        "AUDIO paused",
        "AUDIO resumed",
        f"STAGE frame-returned={frames - 1} state=1",
    ):
        if expected not in result.stderr:
            raise AssertionError(f"Missing playback evidence: {expected}")
    positions = re.findall(r"AUDIO stopped at (\d+) ms", result.stderr)
    assert positions and int(positions[-1]) > 3000, positions
    pixels = screenshot.read_bytes().split(b"\n", 3)[3]
    # The original gameplay targets use bright green. Exclude the menu battery.
    green = sum(
        pixels[i + 1] > 160 and pixels[i] < 120 and pixels[i + 2] < 140
        for i in range(320 * 90 * 3, len(pixels), 3)
    )
    assert len(pixels) == 320 * 240 * 3 and green > 30, "gameplay highway not rendered"
    assert all(
        hashlib.sha256(p.read_bytes()).hexdigest() == digest
        for p, digest in dependencies.items()
    ), "mounted library mutated"
    receipt = {
        "track_pid": pid,
        "playback_ms": int(positions[-1]),
        "green_pixels": green,
        "pause_resume": True,
        "mounted_files_unchanged": True,
    }
    (PROJECT / "build/playback-verification.json").write_text(
        json.dumps(receipt, indent=2) + "\n"
    )
    print("PASS playback:", receipt)


if __name__ == "__main__":
    main()
