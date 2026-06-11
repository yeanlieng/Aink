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
- Status bar (hand-drawn 8×16 ASCII on the physical right edge)
- 2×2 launcher: A = move focus, B = open app, A long = back
- LVGL 8.3 main UI (200×180) + 1-bit e-paper flush
- Refresh modes: FAST / NAV / QUALITY / FULL
- NTP (UTC+8)

### Apps

| Tile | Status |
|------|--------|
| Weather | Forecast, metrics, AQI, 3-day outlook |
| **AI Vision** | Camera capture + cloud poetic description (≤40 Chinese chars) |
| App 3 | Placeholder |
| Settings | WiFi, **QWeather key/host**, AI provider/model/API key, Display, About; EN/ZH |

### AI Vision

- **A short press** (serial `n`) on the vision screen: capture → HTTPS → show result on e-paper
- Providers: **OpenAI**, **Gemini**, **Kimi Platform**, **MiMo Token Plan**
- Vision-capable models:
  - MiMo: `mimo-v2.5`, `mimo-v2-omni` (not `mimo-v2.5-pro`)
  - Kimi: recommend `moonshot-v1-8k-vision-preview`
  - OpenAI: `gpt-4o-mini`, `gpt-4o`, etc.
- MiMo auth: `Authorization: Bearer <key>` (Token Plan / platform key)
- Camera pauses during HTTPS to avoid `cam_hal: FB-OVF` (frame buffer overflow warnings)

### Settings → Model

- **Provider**: OpenAI / Gemini / Kimi Platform / MiMo Token Plan
- **Model**: per-provider preset list
- **API key**: stored in NVS on device only (portal can save key + auto-restart)

Configure API key via captive portal or Settings. MiMo keys often start with `tp-`; Kimi Platform uses keys from [platform.kimi.ai](https://platform.kimi.ai).

### Weather (QWeather / 和风)

- Register at [console.qweather.com](https://console.qweather.com) — copy **API Key** and **API Host** (e.g. `xxx.re.qweatherapi.com`, no `https://` prefix).
- Configure via captive portal (**天气 API** card) or **Settings → WiFi → Configure Weather**.
- Without Key + Host, weather tile shows no data; serial logs `[Weather] QWeather key/host not configured`.
- Refreshes every 30 minutes when WiFi is connected.

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
5. Open `epaper_wifi.ino` (this folder is the sketch root)
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
| v | B double (voice stub) |
| h | help |

### Test vision API on PC (before flashing)

Same HTTP payload as the device; useful to validate API key and model:

```powershell
cd epaper_wifi
$env:MIMO_API_KEY = "your-key"
python tools/test_vision_api.py --provider mimo --image path\to\photo.jpg
```

Providers: `mimo`, `kimi`, `openai`. See `tools/test_vision_api.py --help`.

## Chinese fonts

Regenerate symbol list and fonts with [LVGL Font Converter](https://lvgl.io/tools/fontconverter) (**LVGL 8.x**):

```bash
python tools/build_cn_symbols.py
```

Converter settings:

- Bpp: **1**, size **12** / **14**
- Range: `0x20-0x7F`
- Symbols: paste from `tools/cn_font_symbols.txt` (3500 common chars + UI strings)
- Output: `aink_3500_12.c`, `aink_3500_14.c`
- If the converter adds `.static_bitmap = 0` (LVGL 9), **remove that line** for LVGL 8.3

Update `ui_fonts.h` / `lv_conf.h` if you rename outputs.

```bash
python tools/svg_to_weather_icons.py
```

## Project layout

```
epaper_wifi.ino       Boot, WiFi portal, status bar, refresh orchestration
epaper_canvas.*       Framebuffer, rotation, EPD upload
ui_home / ui_nav      Launcher and navigation
ui_weather.*          Weather app
ui_vision.*           AI Vision UI
ui_settings.*         Settings app (multi-level menu)
vision_service.*      Camera → HTTPS vision API → normalized text
camera_service.*      XIAO Sense camera (240×240 JPEG)
ai_model_config.*     Provider/model presets and URLs
app_locale.*          EN/ZH strings
settings_api.*        NVS (WiFi, language, AI key, QWeather key/host)
weather_service.*     QWeather (和风) forecast + AQI + geo
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
| `[Weather] now HTTP -1` | Check API Host (no `https://` prefix) and Key at [console.qweather.com](https://console.qweather.com) |
| `provider unsupported` | MiMo unsupported model → use **v2.5** / **v2-omni** |
| Screen stuck on「分析中」 | Reflash latest firmware (NAV refresh after capture) |
| `cam_hal: FB-OVF` | Harmless warning if capture works; camera pauses during HTTPS in latest code |

## Notes for contributors

- Do not break status-bar orientation or portal-only horizontal mirror (`epaper_canvas.cpp`).
- E-paper is 1-bit: LVGL grays are thresholded in flush; avoid large gray fills.
- Weather uses **QWeather (和风)** — register at [console.qweather.com](https://console.qweather.com), copy **API Key** and **API Host**. Configure via captive portal or **Settings → WiFi → Configure Weather**. IP geolocation still uses `ip-api.com` (HTTP); forecast/AQI use your QWeather Host (HTTPS, domestic CDN).
- Do not commit API keys or local test images.

## License

MIT — see `LICENSE`. Waveshare EPD driver files retain their original header terms.
