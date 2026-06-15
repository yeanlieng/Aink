#!/usr/bin/env python3
"""Rebuild tools/cn_font_symbols.txt (UI strings + 《现代汉语常用字表》3500 字)."""

from __future__ import annotations

import urllib.request
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA_DIR = TOOLS_DIR / "data"
COMMON_3500_PATH = DATA_DIR / "common_cn_3500.txt"
OUTPUT_PATH = TOOLS_DIR / "cn_font_symbols.txt"

DAVID_SHEH_3500_URL = (
    "https://raw.githubusercontent.com/DavidSheh/CommonChineseCharacter/"
    "master/3500%E5%B8%B8%E7%94%A8%E5%AD%97.txt"
)

# Static Chinese copy used in LVGL (keep in sync with app_locale / UI).
UI_STRINGS = [
    "天气",
    "AI识图",
    "应用3",
    "设置",
    "在线",
    "离线",
    "详情",
    "敬请期待",
    "短按A拍照",
    "分析中...",
    "相机不可用",
    "请配置API",
    "请检查网络",
    "请求失败",
    "提供商不支持",
    "设置",
    "无线",
    "模型",
    "显示",
    "关于",
    "网络",
    "重新配网",
    "忘记网络",
    "提供商",
    "模型",
    "API: 已配置",
    "API: 未配置",
    "配置 API",
    "清除 API",
    "屏幕",
    "刷新",
    "自动",
    "版本",
    "暂无数据",
    "体感",
    "湿度",
    "风速",
    "紫外线",
    "晴",
    "多云",
    "雨",
    "雪",
    "雷暴",
    "雾",
    "低",
    "中",
    "高",
    "极",
    "优",
    "良",
    "敏感",
    "不健康",
    "轻度污染",
    "中度污染",
    "重度污染",
    "周日",
    "周一",
    "周二",
    "周三",
    "周四",
    "周五",
    "周六",
    "月",
    "日",
    "数",
]

# Full-width / CJK punctuation (3500 字表不含标点，单独追加到 LVGL Symbols).
CN_PUNCTUATION = (
    "，。、；：？！"
    "“”‘’"
    "（）《》【】"
    "—…·￥"
    "「」『』"
    "〈〉"
)

HEADER = """# Aink LVGL 中文字体 Symbols 字表
# 用法：复制下面「SYMBOLS 一行」到 https://lvgl.io/tools/fontconverter
# Range 另填：0x20-0x7F（保留英文、数字、标点）
# Size：12 与 14 各生成一份；Bpp：1；Format：C array
#
# 字表组成：《现代汉语常用字表》3500 字 + UI 专有字 + 中文标点
# 不含：动态城市名、WiFi SSID、Portal 网页文案
# 重新生成：python tools/build_cn_symbols.py
#
# 注意：3500 字会显著增大 aink_3500_12.c / aink_3500_14.c 体积与编译时间

"""


def download_common_3500() -> str:
    raw = urllib.request.urlopen(DAVID_SHEH_3500_URL, timeout=30).read()
    text = raw.decode("utf-8-sig")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    last = lines[-1]
    seen: set[str] = set()
    ordered: list[str] = []
    for ch in last:
        if ord(ch) <= 127 or ch in seen:
            continue
        seen.add(ch)
        ordered.append(ch)
    return "".join(ordered)


def collect_ui_han() -> str:
    chars: set[str] = set()
    for text in UI_STRINGS:
        for ch in text:
            if ord(ch) > 127:
                chars.add(ch)
    return "".join(sorted(chars))


def load_common_3500() -> str:
    try:
        return download_common_3500()
    except OSError as exc:
        if not COMMON_3500_PATH.is_file():
            raise SystemExit(
                f"Failed to download 3500 chars and no cache at {COMMON_3500_PATH}: {exc}"
            ) from exc
        print(f"Download failed ({exc}); using cached {COMMON_3500_PATH.name}")
        return COMMON_3500_PATH.read_text(encoding="utf-8").strip()


def merge_symbols(common: str, ui_extra: str, punctuation: str) -> str:
    out: list[str] = []
    seen: set[str] = set()
    for block in (common, ui_extra, punctuation):
        for ch in block:
            if ch not in seen:
                seen.add(ch)
                out.append(ch)
    return "".join(out)


def write_common_3500_file(common: str) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    COMMON_3500_PATH.write_text(common + "\n", encoding="utf-8")
    legacy = DATA_DIR / "common_cn_1000.txt"
    if legacy.exists():
        legacy.unlink()


def main() -> None:
    common = load_common_3500()
    write_common_3500_file(common)
    ui_extra = collect_ui_han()
    symbols = merge_symbols(common, ui_extra, CN_PUNCTUATION)

    body = (
        HEADER
        + f"# --- SYMBOLS 一行（3500 常用字 + UI + 标点，共 {len(symbols)} 字）---\n"
        + symbols
        + "\n"
    )
    OUTPUT_PATH.write_text(body, encoding="utf-8", newline="\n")
    print(f"Wrote {COMMON_3500_PATH.name}: {len(common)} chars")
    print(f"Wrote {OUTPUT_PATH.name}: {len(symbols)} chars total")


if __name__ == "__main__":
    main()
