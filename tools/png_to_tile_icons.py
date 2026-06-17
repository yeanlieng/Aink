#!/usr/bin/env python3
"""Rasterize tile PNG/SVG icons into 1-bit C bitmask headers for ESP32."""

from __future__ import annotations

import io
import os
import re
import sys

try:
    from PIL import Image, ImageDraw
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
    ("settings_gear_bitmap", "gear.png", "Gear outline (settings tile).", True),
    ("vision_eye_bitmap", "eye.png", "Eye outline (AI vision tile).", True),
    ("answerbook_bitmap", "answerbook.svg", "Book icon (answers tile).", False),
    ("stock_chart_bitmap", "stock.svg", "Stock chart outline (stocks tile).", True),
    ("clock_face_bitmap", "clock.svg", "Analog clock outline (clock tile).", True),
]


def load_binary(path: str) -> Image.Image:
    img = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img)
    gray = img.convert("L")
    return gray.point(lambda p: 0 if p < THRESHOLD else 255, "1")


def cubic_points(p0, p1, p2, p3, steps: int = 48) -> list[tuple[float, float]]:
    out = []
    for i in range(steps + 1):
      t = i / steps
      mt = 1.0 - t
      x = mt * mt * mt * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t * t * t * p3[0]
      y = mt * mt * mt * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t * t * t * p3[1]
      out.append((x, y))
    return out


def render_answerbook_svg_binary() -> Image.Image:
    scale = RASTER_SIZE / 30.0
    img = Image.new("L", (RASTER_SIZE, RASTER_SIZE), 255)
    draw = ImageDraw.Draw(img)

    def s_point(point):
        return (point[0] * scale, point[1] * scale)

    def s_width(width: float) -> int:
        return max(1, round(width * scale))

    def draw_path(points, fill=255, outline=0, width=1.0):
        scaled = [s_point(p) for p in points]
        draw.polygon(scaled, fill=fill)
        draw.line(scaled + [scaled[0]], fill=outline, width=s_width(width), joint="curve")

    left = []
    left += cubic_points((15, 6), (15, 6), (9, 5), (4, 7))
    left.append((4, 24))
    left += cubic_points((4, 24), (9, 22), (15, 23), (15, 23))
    draw_path(left, width=1.5)

    right = []
    right += cubic_points((15, 6), (15, 6), (21, 5), (26, 7))
    right.append((26, 24))
    right += cubic_points((26, 24), (21, 22), (15, 23), (15, 23))
    draw_path(right, width=1.5)

    draw.line([s_point((15, 6)), s_point((15, 23))], fill=0, width=s_width(2.0))

    for y0, y1 in ((11, 11.5), (14, 14.5), (17, 17.5)):
        draw.line([s_point((7, y0)), s_point((13, y1))], fill=0, width=s_width(0.9))
    for y0, y1 in ((11.5, 11), (14.5, 14), (17.5, 17)):
        draw.line([s_point((17, y0)), s_point((23, y1))], fill=0, width=s_width(0.9))

    bottom = []
    bottom.append((4, 24))
    bottom += cubic_points((4, 24), (9, 22), (15, 23), (15, 23))
    bottom += cubic_points((15, 23), (21, 22), (26, 24), (26, 24))
    bottom.append((26, 25.5))
    bottom += cubic_points((26, 25.5), (21, 23.5), (15, 24.5), (15, 24.5))
    bottom += cubic_points((15, 24.5), (9, 23.5), (4, 25.5), (4, 25.5))
    draw_path(bottom, fill=0, outline=0, width=0.5)

    r = 1.2 * scale
    cx, cy = s_point((15, 4.5))
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=0)

    return img.point(lambda p: 0 if p < THRESHOLD else 255, "1")


def load_svg_binary(path: str) -> Image.Image:
    if cairosvg is None:
        if os.path.basename(path).lower() == "answerbook.svg":
            return render_answerbook_svg_binary()
        print("Install deps: pip install cairosvg", file=sys.stderr)
        sys.exit(1)
    png = cairosvg.svg2png(url=path, output_width=RASTER_SIZE, output_height=RASTER_SIZE)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img)
    gray = img.convert("L")
    return gray.point(lambda p: 0 if p < THRESHOLD else 255, "1")


def rasterize_tile_icon(source_path: str, outline: bool) -> Image.Image:
    if source_path.lower().endswith(".svg"):
        scaled = load_svg_binary(source_path)
    else:
        src = load_binary(source_path)
        w, h = src.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        cropped = src.crop((left, top, left + side, top + side))
        scaled = cropped.resize((RASTER_SIZE, RASTER_SIZE), Image.Resampling.LANCZOS)
        scaled = scaled.point(lambda p: 0 if p < THRESHOLD else 255, "1")
    icon = downsample(scaled, ICON_SIZE)
    return to_outline(icon) if outline else icon


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

    for var_name, filename, comment, outline in ICON_FILES:
        source_path = os.path.join(PROJECT_DIR, filename)
        can_rasterize = os.path.isfile(source_path)
        if can_rasterize and filename.lower().endswith(".svg") and cairosvg is None:
            can_rasterize = os.path.basename(filename).lower() == "answerbook.svg"

        if can_rasterize:
            icon = rasterize_tile_icon(source_path, outline)
            masks = row_masks(icon)
            print(f"\n{filename}:")
            preview(masks, ICON_SIZE)
        elif var_name in existing:
            masks = existing[var_name]
            if os.path.isfile(source_path):
                reason = "cannot rasterize without cairosvg"
            else:
                reason = "missing source"
            print(f"\n{filename}: {reason}, keeping existing {var_name}", file=sys.stderr)
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
