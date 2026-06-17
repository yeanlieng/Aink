#!/usr/bin/env python3
"""Rasterize tile PNG/SVG icons into 1-bit C bitmask headers for ESP32."""

from __future__ import annotations

import io
import os
import re
import sys
import xml.etree.ElementTree as ET

try:
    from PIL import Image
except ImportError:
    print("Install deps: pip install pillow", file=sys.stderr)
    sys.exit(1)

try:
    import cairosvg
except ImportError:
    cairosvg = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
OUTPUT_PATH = os.path.join(PROJECT_DIR, "settings_icons.h")

ICON_SIZE = 32
RASTER_SIZE = 512
THRESHOLD = 200

ICON_FILES = [
    ("clock_face_bitmap", "tile_clock.svg", "Clock tile icon."),
    ("weather_app_bitmap", "tile_weather.svg", "Weather tile icon."),
    ("vision_eye_bitmap", "tile_camera.svg", "Camera tile icon."),
    ("answerbook_bitmap", "tile_answerbook.svg", "Book of Answers tile icon."),
    ("stock_chart_bitmap", "tile_stock.svg", "Stocks tile icon."),
    ("settings_gear_bitmap", "tile_settings.svg", "Settings tile icon."),
]


def load_binary(path: str) -> Image.Image:
    img = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img)
    return img.convert("L")


def load_svg_binary(path: str) -> Image.Image:
    rect_icon = load_rect_svg_binary(path)
    if rect_icon is not None:
        return rect_icon

    if cairosvg is None:
        print("Install deps: pip install cairosvg", file=sys.stderr)
        sys.exit(1)
    png = cairosvg.svg2png(url=path, output_width=RASTER_SIZE, output_height=RASTER_SIZE)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img)
    gray = img.convert("L")
    return fit_binary_icon(gray)


def is_black_fill(fill: str | None, inherited_black: bool) -> bool:
    if fill is None:
        return inherited_black
    fill = fill.strip().lower()
    return fill in ("black", "#000", "#000000", "rgb(0,0,0)")


def load_rect_svg_binary(path: str) -> Image.Image | None:
    root = ET.parse(path).getroot()
    view_box = root.attrib.get("viewBox")
    if view_box is None:
        return None
    parts = view_box.replace(",", " ").split()
    if len(parts) != 4:
        return None
    min_x, min_y, view_w, view_h = [float(part) for part in parts]
    if view_w <= 0 or view_h <= 0:
        return None

    rects: list[tuple[float, float, float, float]] = []

    def walk(node: ET.Element, inherited_black: bool) -> None:
        tag = node.tag.rsplit("}", 1)[-1]
        black = is_black_fill(node.attrib.get("fill"), inherited_black)
        if tag == "rect" and black:
            try:
                x = float(node.attrib.get("x", "0"))
                y = float(node.attrib.get("y", "0"))
                w = float(node.attrib.get("width", "0"))
                h = float(node.attrib.get("height", "0"))
            except ValueError:
                return
            if w > 0 and h > 0:
                rects.append((x, y, w, h))
        for child in node:
            walk(child, black)

    walk(root, False)
    if not rects:
        return None

    img = Image.new("1", (ICON_SIZE, ICON_SIZE), 1)
    for x, y, w, h in rects:
        left = max(0, int((x - min_x) * ICON_SIZE / view_w))
        top = max(0, int((y - min_y) * ICON_SIZE / view_h))
        right = min(ICON_SIZE, max(left + 1, int((x + w - min_x) * ICON_SIZE / view_w)))
        bottom = min(ICON_SIZE, max(top + 1, int((y + h - min_y) * ICON_SIZE / view_h)))
        for py in range(top, bottom):
            for px in range(left, right):
                img.putpixel((px, py), 0)
    return img


def rasterize_tile_icon(source_path: str) -> Image.Image:
    if source_path.lower().endswith(".svg"):
        return load_svg_binary(source_path)
    return fit_binary_icon(load_binary(source_path))


def fit_binary_icon(img: Image.Image) -> Image.Image:
    img = img.convert("L")
    pixels = img.load()
    xs: list[int] = []
    ys: list[int] = []
    for y in range(img.height):
        for x in range(img.width):
            if pixels[x, y] < THRESHOLD:
                xs.append(x)
                ys.append(y)

    if xs:
        pad = max(1, min(img.width, img.height) // 24)
        left = max(0, min(xs) - pad)
        top = max(0, min(ys) - pad)
        right = min(img.width, max(xs) + pad + 1)
        bottom = min(img.height, max(ys) + pad + 1)
        img = img.crop((left, top, right, bottom))

    binary = img.point(lambda p: 0 if p < THRESHOLD else 255, "1").convert("L")
    scale = min((ICON_SIZE - 2) / binary.width, (ICON_SIZE - 2) / binary.height)
    out_w = max(1, round(binary.width * scale))
    out_h = max(1, round(binary.height * scale))
    resized = binary.resize((out_w, out_h), Image.Resampling.LANCZOS)
    resized = resized.point(lambda p: 0 if p < THRESHOLD else 255, "1")

    out = Image.new("1", (ICON_SIZE, ICON_SIZE), 1)
    out.paste(resized, ((ICON_SIZE - out_w) // 2, (ICON_SIZE - out_h) // 2))
    return out


def load_existing_bitmaps(header_path: str) -> dict[str, list[int]]:
    if not os.path.isfile(header_path):
        return {}
    text = open(header_path, encoding="utf-8").read()
    out: dict[str, list[int]] = {}
    for match in re.finditer(
        r"static const uint32_t (\w+)\[SETTINGS_ICON_SIZE\] = \{\s*([^}]+)\};",
        text,
        re.MULTILINE,
    ):
        var_name = match.group(1)
        values = [
            int(v.strip().rstrip("u"), 0)
            for v in match.group(2).split(",")
            if v.strip()
        ]
        if len(values) == ICON_SIZE:
            out[var_name] = values
    return out


def downsample(img: Image.Image, size: int) -> Image.Image:
    src_w, src_h = img.size
    out = Image.new("1", (size, size), 1)
    for y in range(size):
        y0 = y * src_h // size
        y1 = max(y0 + 1, (y + 1) * src_h // size)
        for x in range(size):
            x0 = x * src_w // size
            x1 = max(x0 + 1, (x + 1) * src_w // size)
            block = img.crop((x0, y0, x1, y1))
            if block.getextrema()[0] == 0:
                out.putpixel((x, y), 0)
    return out


def to_outline(img: Image.Image) -> Image.Image:
    w, h = img.size
    out = Image.new("1", (w, h), 1)
    for y in range(h):
        for x in range(w):
            if img.getpixel((x, y)) != 0:
                continue
            for dx, dy in (
                (-1, 0),
                (1, 0),
                (0, -1),
                (0, 1),
                (-1, -1),
                (1, -1),
                (-1, 1),
                (1, 1),
            ):
                nx, ny = x + dx, y + dy
                if nx < 0 or ny < 0 or nx >= w or ny >= h or img.getpixel((nx, ny)) == 1:
                    out.putpixel((x, y), 0)
                    break
    return out


def row_masks(img: Image.Image) -> list[int]:
    size = img.size[0]
    rows: list[int] = []
    for y in range(size):
        mask = 0
        for x in range(size):
            if img.getpixel((x, y)) == 0:
                mask |= 1 << (size - 1 - x)
        rows.append(mask)
    return rows


def preview(rows: list[int], size: int) -> None:
    for mask in rows:
        line = "".join("#" if (mask >> (size - 1 - x)) & 1 else "." for x in range(size))
        print(line)


def main() -> None:
    existing = load_existing_bitmaps(OUTPUT_PATH)

    lines: list[str] = [
        "#ifndef SETTINGS_ICONS_H",
        "#define SETTINGS_ICONS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SETTINGS_ICON_SIZE {ICON_SIZE}",
        "",
    ]

    for var_name, filename, comment in ICON_FILES:
        source_path = os.path.join(PROJECT_DIR, filename)
        if os.path.isfile(source_path):
            icon = rasterize_tile_icon(source_path)
            masks = row_masks(icon)
            print(f"\n{filename}:")
            preview(masks, ICON_SIZE)
        elif var_name in existing:
            masks = existing[var_name]
            print(f"\n{filename}: missing source, keeping existing {var_name}", file=sys.stderr)
        else:
            print(f"Missing {source_path}", file=sys.stderr)
            sys.exit(1)

        lines.append(f"/** {comment} Source: {filename} */")
        lines.append(f"static const uint32_t {var_name}[SETTINGS_ICON_SIZE] = {{")
        for mask in masks:
            lines.append(f"  0x{mask:08X}u,")
        lines.append("};")
        lines.append("")

    lines.append("#endif")
    lines.append("")

    with open(OUTPUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"\nWrote {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
