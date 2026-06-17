#!/usr/bin/env python3
"""
Local smoke test for Aink vision_service HTTP requests.

Mirrors the ESP32 payload (OpenAI-compatible chat/completions + base64 JPEG).
Run this on PC first; when it succeeds, flash the firmware and test on device.

Examples:
  set MIMO_API_KEY=tp-your-key
  python tools/test_vision_api.py --provider mimo --image photo.jpg

  python tools/test_vision_api.py --provider mimo --image photo.jpg --api-key tp-xxx --dry-run
  python tools/test_vision_api.py --provider kimi --image photo.jpg --api-key sk-xxx --model moonshot-v1-8k-vision-preview
"""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

VISION_MAX_TOKENS = 1024
VISION_OUTPUT_MAX_CHARS = 40

SYSTEM_PROMPT_ZH = (
    "你是墨水屏诗人。根据照片写一句中文，不超过40字，凝练有诗意。"
    "只输出这一句正文，不要思考过程、不要解释、不要引号、不要标题。"
)
USER_PROMPT_ZH = "请直接输出一句描述。"

PROVIDERS = {
    "mimo": {
        "url": "https://api.xiaomimimo.com/v1/chat/completions",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
        "max_tokens_key": "max_completion_tokens",
        "default_model": "mimo-v2.5",
        "env_key": "MIMO_API_KEY",
    },
    "kimi": {
        "url": "https://api.moonshot.ai/v1/chat/completions",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
        "max_tokens_key": "max_tokens",
        "default_model": "moonshot-v1-8k-vision-preview",
        "env_key": "MOONSHOT_API_KEY",
    },
    "openai": {
        "url": "https://api.openai.com/v1/chat/completions",
        "auth_header": "Authorization",
        "auth_prefix": "Bearer ",
        "max_tokens_key": "max_tokens",
        "default_model": "gpt-4o-mini",
        "env_key": "OPENAI_API_KEY",
    },
}


def load_image_base64(path: Path) -> tuple[str, str]:
    data = path.read_bytes()
    mime, _ = mimetypes.guess_type(str(path))
    if mime is None or not mime.startswith("image/"):
        mime = "image/jpeg"
    b64 = base64.b64encode(data).decode("ascii")
    return mime, b64


def build_body(model: str, max_tokens_key: str, mime: str, b64: str, provider: str) -> dict:
    """Same structure as vision_service.cpp buildOpenAiCompatibleBody."""
    image_url = f"data:{mime};base64,{b64}"
    body: dict = {
        "model": model,
        max_tokens_key: VISION_MAX_TOKENS,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT_ZH},
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "image_url": {"url": image_url}},
                    {"type": "text", "text": USER_PROMPT_ZH},
                ],
            },
        ],
    }
    if provider == "mimo":
        body["thinking"] = {"type": "disabled"}
    return body


def looks_like_reasoning_chain(text: str) -> bool:
    if not text:
        return False
    if any(marker in text for marker in ("用户现在", "首先看", "用户查询", "首先，", "首先,")):
        return True
    return "不对" in text and len(text) > 48


def extract_last_quoted(text: str) -> str:
    matches = re.findall(r"[「]([^」\n]{4,80})[」]", text)
    if matches:
        return matches[-1].strip()
    matches = re.findall(r'"([^"\n]{4,80})"', text)
    if matches:
        return matches[-1].strip()
    return ""


def normalize_vision_output(content: str, reasoning: str, raw_body: str = "") -> str:
    content = (content or "").strip()
    reasoning = (reasoning or "").strip()

    candidate = ""
    if content and not looks_like_reasoning_chain(content):
        candidate = content
    elif content and looks_like_reasoning_chain(content):
        candidate = extract_last_quoted(content) or content
    elif reasoning:
        candidate = extract_last_quoted(reasoning) or extract_last_quoted(raw_body)
    elif raw_body:
        candidate = extract_last_quoted(raw_body)

    candidate = candidate.strip().strip("\"'「」")
    if len(candidate) > VISION_OUTPUT_MAX_CHARS:
        candidate = candidate[:VISION_OUTPUT_MAX_CHARS]
    return candidate


def parse_assistant_message(response_json: dict, raw_body: str = "") -> tuple[str, str, str]:
    choices = response_json.get("choices") or []
    if not choices:
        return "", "", ""
    message = choices[0].get("message") or {}
    content = (message.get("content") or "").strip()
    reasoning = (message.get("reasoning_content") or "").strip()
    normalized = normalize_vision_output(content, reasoning, raw_body)
    return normalized, content, reasoning


def post_json(url: str, auth_header: str, auth_value: str, body: dict, timeout: int) -> tuple[int, str]:
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header(auth_header, auth_value)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            return resp.status, text
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", errors="replace")
        return exc.code, text
    except urllib.error.URLError as exc:
        return -1, str(exc.reason)


def main() -> int:
    parser = argparse.ArgumentParser(description="Test vision API locally (same payload as ESP32).")
    parser.add_argument(
        "--provider",
        choices=sorted(PROVIDERS.keys()),
        default="mimo",
        help="AI provider preset (default: mimo)",
    )
    parser.add_argument("--image", required=True, type=Path, help="JPEG/PNG image file path")
    parser.add_argument("--model", help="Override model id")
    parser.add_argument("--api-key", help="API key (or set provider env var)")
    parser.add_argument("--timeout", type=int, default=60, help="HTTP timeout seconds")
    parser.add_argument("--dry-run", action="store_true", help="Build JSON only, do not POST")
    parser.add_argument("--print-curl", action="store_true", help="Print equivalent curl command")
    args = parser.parse_args()

    if not args.image.is_file():
        print(f"Image not found: {args.image}", file=sys.stderr)
        return 1

    preset = PROVIDERS[args.provider]
    api_key = args.api_key or os.environ.get(preset["env_key"], "")
    if not api_key and not args.dry_run:
        print(
            f"Missing API key. Pass --api-key or set {preset['env_key']}.",
            file=sys.stderr,
        )
        return 1

    model = args.model or preset["default_model"]
    mime, b64 = load_image_base64(args.image)
    body = build_body(model, preset["max_tokens_key"], mime, b64, args.provider)
    payload = json.dumps(body, ensure_ascii=False)
    print(f"provider={args.provider} model={model}")
    print(f"image={args.image} bytes={args.image.stat().st_size} b64_len={len(b64)} json_bytes={len(payload.encode('utf-8'))}")

    if args.print_curl:
        auth_value = f"{preset['auth_prefix']}{api_key or '$API_KEY'}"
        print("\n# PowerShell (save JSON to body.json first):")
        print(f"# curl.exe -X POST \"{preset['url']}\" ^")
        print(f"#   -H \"Content-Type: application/json\" ^")
        print(f"#   -H \"{preset['auth_header']}: {auth_value}\" ^")
        print("#   -d \"@body.json\"")

    if args.dry_run:
        print("\n[dry-run] JSON preview (first 500 chars):")
        print(payload[:500] + ("..." if len(payload) > 500 else ""))
        return 0

    auth_value = f"{preset['auth_prefix']}{api_key}"
    print(f"\nPOST {preset['url']} ...")
    status, text = post_json(preset["url"], preset["auth_header"], auth_value, body, args.timeout)
    print(f"HTTP {status}")
    if status <= 0:
        print(text)
        return 2

    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        print(text[:2000])
        return 3

    if status < 200 or status >= 300:
        print(json.dumps(data, ensure_ascii=False, indent=2)[:2000])
        return 4

    content, raw_content, raw_reasoning = parse_assistant_message(data, text)
    if not content:
        choice = (data.get("choices") or [{}])[0]
        print("finish_reason:", choice.get("finish_reason"))
        print("usage:", data.get("usage"))
        print("raw content:", repr(raw_content[:120] if raw_content else ""))
        print("raw reasoning (first 120):", repr(raw_reasoning[:120] if raw_reasoning else ""))
    print("\n--- assistant ---")
    print(content or "(empty content)")
    print("--- end ---")
    return 0 if content else 5


if __name__ == "__main__":
    raise SystemExit(main())
