"""Hash-check and run the native startup probe with an external wall-clock bound."""

import argparse
import json
from pathlib import Path
import subprocess

from image_contract import IMAGE_SHA256, checked_image

PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    checked_image(args.image)
    if not 0 < args.timeout <= 60:
        parser.error("timeout must be between zero and 60 seconds")
    command = [str(PROJECT / "build/phase-startup.exe"), str(args.image.resolve())]
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=args.timeout
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
