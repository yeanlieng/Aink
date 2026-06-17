# Aink

Aink is a 2×2 app platform for **Seeed XIAO ESP32-S3 Sense** + **Waveshare 1.54 inch B&W e-paper** (200×200). It ships with WiFi provisioning, LVGL UI, button navigation, tiered e-paper refresh, and pluggable mini-apps.

## Hardware

| Part | Notes |
|------|-------|
| MCU | Seeed Studio **XIAO ESP32-S3 Sense** (camera module required for AI Vision) |
| Display | Waveshare [EPD_1in54_V2](https://www.waveshare.com/1.54inch-e-paper.htm) (200×200) |
| Button A | D6 / GPIO43 (active LOW, internal pull-up) |
| Button B | D7 / GPIO44 (active LOW, internal pull-up) |

See `DEV_Config.h` for EPD wiring (DIN D10, CLK D8, CS D9, DC D3, RST D1, BUSY D0).

### Arduino board settings (required)

| Option | Value |
|--------|--------|
| Board | **Seeed XIAO ESP32S3 Sense** |
| PSRAM | **OPI PSRAM → Enabled** |
| Partition | **Huge APP (3MB+)** |
| LVGL | **8.3.x** (not 9.x) |
| ESP32 core | **3.x** (camera driver) |

Boot log should show `[Camera] ready (240x240 JPEG)` and non-zero `freePsram`.

## Features

### Platform

- WiFi captive portal + QR setup; credentials stored in NVS
- Status bar: single datetime line, dynamic weather icon/temp, battery (hand-drawn 8×16 ASCII on the physical right edge)
- 2×2 launcher with tile icons; A = move focus, B = open app, A long = back
- LVGL 8.3 main UI (200×180) + 1-bit e-paper flush
- Refresh modes: FAST / NAV / QUALITY / FULL
- NTP (UTC+8)

### Apps

| Tile | Status |
|------|--------|
| Weather | QWeather forecast, metrics, AQI/UV, 3-day outlook (tomorrow onward) |
| **AI Vision** | Camera capture + cloud poetic description (≤40 Chinese chars); eye icon on launcher |
| App 3 | Placeholder |
| Settings | WiFi, **QWeather key/host**, AI provider/model/API key, Display, About; gear icon on launcher; EN/ZH |

### Weather (QWeather / 和风)

- Register at [console.qweather.com](https://console.qweather.com) — copy **API Key** and **API Host** (project-specific, e.g. `xxx.re.qweatherapi.com`; no `https://` prefix).
- Configure via captive portal (**天气 API** card) or **Settings → WiFi → Configure Weather**.
- Flow: IP geolocation (`ip-api.com`, HTTP) → QWeather geo lookup (LocationID) → `/v7/weather/now`, `/airquality/v1/current/{lat}/{lon}`, `/v7/weather/7d` (includes `uvIndex` for today), optional UV indices fallback on your API Host (HTTPS).
- Auth: **`X-QW-Api-Key` header** (not `key=` in URL). Responses are **gzip-compressed**; firmware decompresses with embedded **`puff.c`** (no system `zlib.h`).
- Detail page shows **city** (`adm2`), not district. Three-day row shows **tomorrow, day after, and day after that** (today excluded).
- Refreshes every 30 minutes when WiFi is connected.

### AI Vision

- **A short press** (serial `n`) on the vision screen: capture → HTTPS → show result on e-paper
- Providers: **OpenAI**, **Gemini**, **Kimi Platform**, **MiMo Token Plan**
- Vision-capable models:
  - MiMo: `mimo-v2.5`, `mimo-v2-omni` (not `mimo-v2.5-pro`)
  - Kimi: recommend `moonshot-v1-8k-vision-preview`
  - OpenAI: `gpt-4o-mini`, `gpt-4o`, etc.
- MiMo auth: `Authorization: Bearer <key>` (Token Plan / platform key)
- Camera pauses during HTTPS to avoid `cam_hal: FB-OVF` (frame buffer overflow warnings)

Configure API key via captive portal or **Settings → Model**. MiMo keys often start with `tp-`; Kimi Platform uses keys from [platform.kimi.ai](https://platform.kimi.ai).

### Voice Interaction

- **B double-click** (serial `v`): start recording from the XIAO ESP32S3 Sense PDM microphone.
- **B double-click again**: stop recording, wrap the captured PCM as 16 kHz mono WAV, transcribe it with Xiaomi MiMo ASR, then send the transcript as text to Xiaomi MiMo.
- Input format is finalized as **transcribed text**, not raw audio, for the chat/LLM stage. The audio is used only for STT.
- Current STT implementation uses Xiaomi MiMo `mimo-v2.5-asr` through `/v1/chat/completions` with `input_audio.data` set to a `data:audio/wav;base64,...` URL.
- The downstream LLM request also uses Xiaomi MiMo `/v1/chat/completions`; select **MiMo Token Plan** in Settings so the saved API key matches this voice flow.
- During the speaking state, **A short press** interrupts the speaker state and keeps the LLM text visible on the Voice screen.

The microphone uses the Sense expansion board PDM pins: GPIO42 clock and GPIO41 data. Speaker/TTS playback is represented by an interruptible state machine until a concrete speaker amp/DAC pinout and playback driver are added.

### i18n

- Default language: **English**
- Switch: **Settings → Display → Language** (press B to toggle)
- Preference saved in NVS (`lang` key)
- UI fonts: `aink_3500_12.c` / `aink_3500_14.c` (~3500 common Chinese + UI strings)

## Quick start

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software) or arduino-cli
2. Board package: **esp32 by Espressif** (3.x), board **XIAO ESP32S3 Sense**
3. Enable **OPI PSRAM** and **Huge APP** partition
4. Library: **lvgl 8.3.x** (not 9.x)
5. Open `Aink.ino` (this folder is the sketch root)
6. Upload; serial monitor **115200**

First boot without saved WiFi enters AP portal mode automatically.

### Serial button simulation

With `BTN_SERIAL_SIM=1` in `btn_input.h`:

| Key | Action |
|-----|--------|
| n | A click (next / **capture in AI Vision**) |
| p | A double (prev) |
| b | A long (back) |
| c | B confirm |
| v | B double (voice record toggle) |
| h | help |

### Test vision API on PC (before flashing)

Same HTTP payload as the device; useful to validate API key and model:

```powershell
cd Aink
$env:MIMO_API_KEY = "your-key"
python tools/test_vision_api.py --provider mimo --image path\to\photo.jpg
```

Providers: `mimo`, `kimi`, `openai`. See `tools/test_vision_api.py --help`.

## Fonts and icons

### Chinese fonts

Regenerate symbol list and fonts:

```bash
python tools/build_cn_symbols.py
python tools/build_fonts.py
```

`build_fonts.py` needs Node.js (`npx`) and writes `aink_3500_12.c` / `aink_3500_14.c` (Noto Sans SC is cached under `tools/fonts/`, gitignored).

Manual option: [LVGL Font Converter](https://lvgl.io/tools/fontconverter) (**LVGL 8.x**):

Converter settings:

- Bpp: **1**, size **12** / **14**
- Range: `0x20-0x7F`
- Symbols: paste from `tools/cn_font_symbols.txt` (3500 common chars + UI strings + CJK punctuation)
- Output: `aink_3500_12.c`, `aink_3500_14.c`
- If the converter adds `.static_bitmap = 0` (LVGL 9), **remove that line** for LVGL 8.3

Update `ui_fonts.h` / `lv_conf.h` if you rename outputs.

### Weather and launcher icons

```bash
python tools/svg_to_weather_icons.py
python tools/png_to_tile_icons.py
```

- Weather icons: rasterize `wi-*.svg` → `weather_icons.h` (16×16).
- Launcher icons: **`stock.svg`** is committed; place **`gear.png`** and **`eye.png`** in the sketch root (local only; gitignored). Run `png_to_tile_icons.py` → `settings_icons.h` (32×32 outline bitmaps for all launcher tiles).

## Project layout

```
Aink.ino              Boot, WiFi portal, status bar, refresh orchestration
epaper_canvas.*       Framebuffer, rotation, EPD upload
ui_home / ui_nav      Launcher (tile icons) and navigation
ui_weather.*          Weather app
ui_vision.*           AI Vision UI
ui_settings.*         Settings app (multi-level menu)
vision_service.*      Camera → HTTPS vision API → normalized text
camera_service.*      XIAO Sense camera (240×240 JPEG)
ai_model_config.*     Provider/model presets and URLs
app_locale.*          EN/ZH strings
settings_api.*        NVS (WiFi, language, AI key, QWeather key/host)
weather_service.*     QWeather fetch, parse, icon mapping
weather_gzip.*          Gzip decompress wrapper (uses puff)
puff.c / puff.h         Embedded deflate decompressor (public domain)
settings_icons.h      Launcher gear, eye, and stock bitmaps (generated)
aink_3500_12/14.c     LVGL CJK fonts (~3500 chars)
tools/                Font/icons/API test scripts
```

## Troubleshooting

| Symptom | Check |
|---------|--------|
| All Chinese shows □ | LVGL **8.3.x**; remove `.static_bitmap` from font `.c` if present |
| `[Camera] init failed` / `frame buffer malloc failed` | **PSRAM Enabled**; partition ≥ 3MB; Sense board with camera |
| `[Vision] HTTP -1` | PSRAM + WiFi; test with `tools/test_vision_api.py` on PC |
| `[Weather] QWeather key/host not configured` | Settings → WiFi → Configure Weather, or portal **天气 API** card |
| `[Weather] now HTTP -1` | WiFi; API Host (no `https://`) and Key at [console.qweather.com](https://console.qweather.com) |
| `[Weather] QWeather code=401/403` | Key invalid or Host does not match project; use console Host, not legacy `devapi.qweather.com` |
| `[Weather] air request failed` / `air parse failed` | Air Quality uses `/airquality/v1/current/{lat}/{lon}` (not legacy `/v7/air/now`); confirm your QWeather plan includes Air Quality API |
| `[Weather] gzip …` missing / parse failed | Reflash latest firmware (`puff.c` + `weather_gzip.cpp` in sketch) |
| `undefined reference to puff` | Clean build (Sketch → Clean All); ensure `puff.c` and `puff.h` are in sketch folder |
| `provider unsupported` | MiMo unsupported model → use **v2.5** / **v2-omni** |
| Screen stuck on「分析中」 | Reflash latest firmware (NAV refresh after capture) |
| `cam_hal: FB-OVF` | Harmless warning if capture works; camera pauses during HTTPS |

## Notes for contributors

- Do not break status-bar orientation or portal-only horizontal mirror (`epaper_canvas.cpp`).
- E-paper is 1-bit: LVGL grays are thresholded in flush; avoid large gray fills.
- Do not commit API keys, local test images, or source PNGs (`gear.png` / `eye.png` are gitignored; commit generated `settings_icons.h` instead).

## License

MIT — see `LICENSE`. Waveshare EPD driver files retain their original header terms.
