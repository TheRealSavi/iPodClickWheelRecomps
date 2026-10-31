"""Generate Phase locally from its owner's plaintext image and optional survey."""

import argparse
import json
import sys
from pathlib import Path

from image_contract import IMAGE_SHA256, checked_image

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT.parent / "common/tools"))
from recomp.functions import GhidraEntry, Kind, build_function_table
from recomp.generate import Bindings, Generator
from recomp.image import EAppImage
from discovery import pc_relative_code_addresses, stored_function_pointers


def constructor_entries(image: EAppImage) -> set[int]:
    """Recover armcc's table-relative constructors in the pinned Phase build."""
    start = (image.u32(0x1800287C) + 0x18002854) & 0xFFFFFFFF
    end = (image.u32(0x18002880) + 0x1800285C) & 0xFFFFFFFF
    if not (image.load_base <= start <= end <= image.end) or start % 4 or end % 4:
        raise ValueError("invalid constructor table bounds")
    entries = {(image.u32(slot) + slot) & 0xFFFFFFFC for slot in range(start, end, 4)}
    if not all(image.contains(target) for target in entries):
        raise ValueError("constructor target outside image")
    return entries


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--functions", type=Path, help="optional address/name/size TSV from Ghidra"
    )
    parser.add_argument(
        "--extra-entry", action="append", default=[], type=lambda x: int(x, 0)
    )
    args = parser.parse_args()
    image = EAppImage(checked_image(args.image))
    survey = []
    if args.functions:
        for row in args.functions.read_text().splitlines():
            address, name, size = row.split("\t")
            survey.append(GhidraEntry(int(address, 16), int(size), 0))
    # No game routines are replaced. Missing platform services stop in the host.
    bindings = Bindings()
    # armcc's constructor walker at 0x18002844 uses table-relative signed
    # offsets, not absolute function pointers. Recover the exact bounds from
    # its two literals; the image hash above pins this interpretation.
    constructors = constructor_entries(image)
    functions = build_function_table(
        image, survey, set(args.extra_entry) | constructors, image.end, set()
    )
    generator = Generator(
        image, functions, bindings, set(), "Phase build 3168002", namespace="phase"
    )
    generator.discover_all()
    known = {f.entry for f in generator.table.values() if f.kind is not Kind.THUNK}
    stored = stored_function_pointers(image, known)
    formed = pc_relative_code_addresses(image, known | stored)
    functions = build_function_table(
        image, survey, known | stored | formed, image.end, set()
    )
    generator = Generator(
        image, functions, bindings, set(), "Phase build 3168002", namespace="phase"
    )
    generator.discover_all()
    output = PROJECT / "gen"
    report = generator.write_all(output)
    receipt = {
        "image_sha256": IMAGE_SHA256,
        "functions": len(generator.bodies),
        "instructions": report.instructions,
        "survey_entries": len(survey),
        "constructors": len(constructors),
        "stored_entries": len(stored),
        "pc_relative_entries": len(formed),
        "frameworks": {f.name: len(f.thunks) for f in image.frameworks},
        "runtime_replacements": [],
    }
    (output / "generation.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
