#!/usr/bin/env python3
"""Regenerate aink_3500_12.c / aink_3500_14.c via npx lv_font_conv."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = Path(__file__).resolve().parent
SYMBOLS_PATH = TOOLS / "cn_font_symbols.txt"
FONT_PATH = TOOLS / "fonts" / "NotoSansSC-VariableFont_wght.ttf"

LVGL_INCLUDE_BLOCK = """\
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif
"""

BROKEN_INCLUDE = """\
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
"""


def read_symbols() -> str:
    text = SYMBOLS_PATH.read_text(encoding="utf-8")
    match = re.search(r"# --- SYMBOLS 一行.*---\n(.+)\n", text, re.DOTALL)
    if not match:
        raise SystemExit(f"Could not parse symbols from {SYMBOLS_PATH}")
    return match.group(1).strip()


def build(size: int, symbols: str) -> None:
    out = ROOT / f"aink_3500_{size}.c"
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if npx is None:
        raise SystemExit("npx not found; install Node.js or regenerate fonts via lvgl.io/tools/fontconverter")

    cmd = [
        npx,
        "--yes",
        "lv_font_conv",
        "--bpp",
        "1",
        "--size",
        str(size),
        "--no-compress",
        "--font",
        str(FONT_PATH),
        "--symbols",
        symbols,
        "--range",
        "32-127",
        "--format",
        "lvgl",
        "-o",
        str(out),
    ]
    print(f"Building {out.name} ({len(symbols)} symbols, {size}px)...")
    subprocess.run(cmd, check=True, cwd=ROOT)
    patch_lvgl_include(out)


def patch_lvgl_include(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if BROKEN_INCLUDE in text:
        text = text.replace(BROKEN_INCLUDE, LVGL_INCLUDE_BLOCK, 1)
        path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    if not FONT_PATH.is_file():
        raise SystemExit(f"Missing font: {FONT_PATH}")
    symbols = read_symbols()
    for size in (12, 14):
        build(size, symbols)
    print("Done.")


if __name__ == "__main__":
    main()
