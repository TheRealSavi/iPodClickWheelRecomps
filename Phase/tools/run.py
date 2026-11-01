"""Hash-check and launch Phase; optional bounds are for diagnostic sessions."""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

from image_contract import IMAGE_SHA256, checked_image

PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--timeout", type=float, help="optional diagnostic time limit in seconds"
    )
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--audio", action="store_true")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--visible", action="store_true")
    parser.add_argument(
        "--render-scale",
        type=int,
        choices=range(1, 5),
        default=1,
        help="internal drawing scale: 1 original, 2–4 experimental HD",
    )
    saves = parser.add_mutually_exclusive_group()
    saves.add_argument(
        "--save-dir", type=Path, help="separate local profile/score directory"
    )
    saves.add_argument(
        "--ephemeral", action="store_true", help="discard profiles and scores on exit"
    )
    parser.add_argument(
        "--quiet-sdk",
        action="store_true",
        help="omit per-service tracing during longer previews",
    )
    parser.add_argument("--frames", type=int, help="optional diagnostic frame limit")
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument(
        "--input-script", type=Path, help="frame, hex scan code, down (1/0) per row"
    )
    args = parser.parse_args()
    checked_image(args.image)
    timeout = (
        args.timeout if args.timeout is not None else (None if args.visible else 15)
    )
    frames = args.frames if args.frames is not None else (None if args.visible else 300)
    if timeout is not None and not 0 < timeout <= 600:
        parser.error("timeout must be between zero and 600 seconds")
    if frames is not None and not 1 <= frames <= 36000:
        parser.error("frames must be between 1 and 36000")
    if not args.assets and (
        args.visible
        or args.screenshot
        or args.audio
        or args.library
        or args.save_dir
        or args.render_scale != 1
    ):
        parser.error("graphics, audio, and library options require --assets")
    command = [str(PROJECT / "build/phase-startup.exe"), str(args.image.resolve())]
    if args.assets:
        command += [
            "--assets",
            str(args.assets.resolve()),
            "--render-scale",
            str(args.render_scale),
        ]
        if frames is not None:
            command += ["--frames", str(frames)]
    if args.audio:
        command += ["--audio"]
    if args.library:
        command += ["--library", str(args.library.resolve())]
    if args.visible and not args.ephemeral and args.save_dir is None:
        local = os.environ.get("LOCALAPPDATA")
        if not local:
            parser.error("LOCALAPPDATA unavailable; pass --save-dir or --ephemeral")
        args.save_dir = Path(local) / "iPodClickWheelRecomps" / "Phase"
    if args.save_dir:
        command += ["--save-dir", str(args.save_dir.resolve())]
    if args.visible:
        command += ["--visible"]
    if args.quiet_sdk:
        command += ["--quiet-sdk"]
    if args.screenshot:
        command += ["--screenshot", str(args.screenshot.resolve())]
    if args.input_script:
        command += ["--input-script", str(args.input_script.resolve())]
    try:
        log_path = None
        if timeout is None:
            # Normal sessions may last hours. Keep output on disk rather than
            # growing a captured-output buffer for the entire play session.
            with tempfile.NamedTemporaryFile(
                prefix="session-", suffix=".log", dir=PROJECT / "build", delete=False
            ) as log:
                log_path = log.name
                result = subprocess.run(
                    command,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=None,
                    check=False,
                )
                log.seek(0, os.SEEK_END)
                log.seek(max(0, log.tell() - 128 * 1024))
                output, errors = "", log.read().decode("utf-8", errors="replace")
        else:
            result = subprocess.run(
                command, capture_output=True, text=True, timeout=timeout, check=False
            )
            output, errors = result.stdout, result.stderr
        receipt = {
            "image_sha256": IMAGE_SHA256,
            "exit": result.returncode,
            "stdout": output,
            "stderr": errors,
            "timed_out": False,
            "log": log_path,
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
