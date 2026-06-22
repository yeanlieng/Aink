#!/usr/bin/env python3
"""Convert a 200x200 monochrome BMP into boot_splash_image.h."""

from __future__ import annotations

import argparse
import io
from pathlib import Path

from PIL import Image


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="200x200 monochrome BMP source")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("boot_splash_image.h"),
        help="output header path (default: boot_splash_image.h)",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        default=128,
        help="threshold used if the input is not already 1-bit (default: 128)",
    )
    return parser.parse_args()


def load_as_1bit_bmp_bytes(path: Path, threshold: int) -> bytes:
    img = Image.open(path)
    if img.size != (200, 200):
        raise SystemExit(f"expected 200x200 image, got {img.size[0]}x{img.size[1]}")

    gray = img.convert("L")
    mono = gray.point(lambda px: 255 if px >= threshold else 0, mode="1")
    out = io.BytesIO()
    mono.save(out, format="BMP")
    return out.getvalue()


def write_header(path: Path, bmp: bytes) -> None:
    lines = [
        "#ifndef BOOT_SPLASH_IMAGE_H",
        "#define BOOT_SPLASH_IMAGE_H",
        "",
        "#include <Arduino.h>",
        "#include <pgmspace.h>",
        "",
        f"#define BOOT_SPLASH_BMP_LEN {len(bmp)}U",
        "static const uint8_t BOOT_SPLASH_BMP[] PROGMEM = {",
    ]

    for i in range(0, len(bmp), 16):
        chunk = bmp[i : i + 16]
        lines.append("  " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")

    lines.extend(["};", "", "#endif", ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    args = parse_args()
    if not args.image.is_file():
        raise SystemExit(f"image not found: {args.image}")
    if not 0 <= args.threshold <= 255:
        raise SystemExit("--threshold must be between 0 and 255")

    bmp = load_as_1bit_bmp_bytes(args.image, args.threshold)
    write_header(args.out, bmp)
    print(f"wrote {args.out} ({len(bmp)} bytes)")


if __name__ == "__main__":
    main()
