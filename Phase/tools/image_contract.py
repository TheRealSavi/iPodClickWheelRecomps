"""The one supported retail image; never accept a different build by size alone."""

import hashlib
from pathlib import Path

IMAGE_SHA256 = "35834fede51f170a0b0bda8e380e1c12af0f86ca1a7952e5abfbb24ab4082258"


def checked_image(path: Path) -> bytes:
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != IMAGE_SHA256:
        raise ValueError(f"unsupported Phase image SHA-256: {digest}")
    return data
