"""Hash-check and run the native startup probe with an external wall-clock bound."""

import argparse
import json
import subprocess
from pathlib import Path

from image_contract import IMAGE_SHA256, checked_image

PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--audio", action="store_true")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--visible", action="store_true")
    parser.add_argument(
        "--quiet-sdk",
        action="store_true",
        help="omit per-service tracing during longer previews",
    )
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument(
        "--input-script", type=Path, help="frame, hex scan code, down (1/0) per row"
    )
    args = parser.parse_args()
    checked_image(args.image)
    if not 0 < args.timeout <= 600:
        parser.error("timeout must be between zero and 600 seconds")
    if not 1 <= args.frames <= 36000:
        parser.error("frames must be between 1 and 36000")
    if not args.assets and (
        args.visible or args.screenshot or args.audio or args.library
    ):
        parser.error("graphics, audio, and library options require --assets")
    command = [str(PROJECT / "build/phase-startup.exe"), str(args.image.resolve())]
    if args.assets:
        command += [
            "--assets",
            str(args.assets.resolve()),
            "--frames",
            str(args.frames),
        ]
    if args.audio:
        command += ["--audio"]
    if args.library:
        command += ["--library", str(args.library.resolve())]
    if args.visible:
        command += ["--visible"]
    if args.quiet_sdk:
        command += ["--quiet-sdk"]
    if args.screenshot:
        command += ["--screenshot", str(args.screenshot.resolve())]
    if args.input_script:
        command += ["--input-script", str(args.input_script.resolve())]
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=args.timeout, check=False
        )
        receipt = {
            "image_sha256": IMAGE_SHA256,
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as error:

        def decoded(value):
            return (
                value.decode(errors="replace")
                if isinstance(value, bytes)
                else value or ""
            )

        receipt = {
            "image_sha256": IMAGE_SHA256,
            "exit": None,
            "timed_out": True,
            "stdout": decoded(error.stdout),
            "stderr": decoded(error.stderr),
        }
    (PROJECT / "build/startup.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(receipt["stdout"], end="")
    print(receipt["stderr"], end="")
    if receipt["timed_out"]:
        print("STOP: external wall-clock timeout; process terminated")
        return 124
    return receipt["exit"]


if __name__ == "__main__":
    raise SystemExit(main())
