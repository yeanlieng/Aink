#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include "EPD_1in54_V2.h"
#include "weather_icons.h"
#include "weather_service.h"
#include "epaper_canvas.h"
#include "btn_input.h"
#include "ui_lvgl.h"
#include "ui_home.h"
#include "ui_weather.h"
#include "ui_settings.h"
#include "ui_nav.h"
#include "ui_vision.h"
#include "ui_refresh.h"
#include "settings_api.h"
#include "app_locale.h"
#include "camera_service.h"

extern "C" {
#include "qrcode.h"
}

// 中国时区 UTC+8
#define GMT_OFFSET_SEC           (8 * 3600)
#define DAYLIGHT_OFFSET_SEC      0

// 状态栏高度（16px 字体 + 分隔线）
#define STATUS_BAR_HEIGHT        20

// WiFi 配网
#define WIFI_CONNECT_TIMEOUT_MS  20000
#define WIFI_PORTAL_DNS_PORT     53
#define PREFS_NAMESPACE          "epaper"
#define PREFS_KEY_SSID           "ssid"
#define PREFS_KEY_PASS           "pass"
#define QR_CODE_BUFFER_SIZE      350
#define WIFI_RECONNECT_MAX_FAIL  5

// XIAO ESP32-S3 板载电池电压经 D0/GPIO1 分压接入 ADC（与 EPD BUSY 同脚）
#define BATTERY_ADC_GPIO         1
#define BATTERY_V_MIN            3.0f
#define BATTERY_V_MAX            4.2f

static WebServer portalServer(80);
static DNSServer portalDnsServer;
static Preferences devicePrefs;

static int lastDisplayedMinute = -1;
static int lastWifiState = -1;
static int lastWeatherIcon = -1;
static int lastWeatherTemp = -999;
static bool portalModeActive = false;
static bool portalWebStarted = false;
static int wifiReconnectFailures = 0;
static char portalApSsid[24];

static bool syncNetworkTime();
static void drawStatusBarRegion(UBYTE *image, int batteryPercent, bool wifiConnected,
                                bool showWeather, WeatherIconKind weatherIcon, int weatherTempC);
static void refreshMainUiOnDisplay(UiRefreshMode mode);

static void setEpaperPixel(UBYTE *image, UWORD lx, UWORD ly, bool black) {
  (void)image;
  epaper_set_pixel(lx, ly, black);
}

static void drawHorizontalLine(UBYTE *image, UWORD y) {
  for (UWORD x = 0; x < EPD_1IN54_V2_WIDTH; x++) {
    setEpaperPixel(image, x, y, true);
  }
}

static void drawText(UBYTE *image, const char *text, UWORD x, UWORD y) {
  const UBYTE font8x16[][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0x7C,0x18,0x18,0x00},
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
  };

  for (const char *p = text; *p; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    const UWORD charX = x + (UWORD)(p - text) * 8;
    if (c >= ' ' && c <= 'Z') {
      UWORD idx = (UBYTE)c - ' ';
      for (UWORD row = 0; row < 16; row++) {
        UBYTE bits = font8x16[idx][row];
        for (UWORD col = 0; col < 8; col++) {
          if (bits & (0x80 >> col)) {
            setEpaperPixel(image, charX + col, y + row, true);
          }
        }
      }
    }
  }
}

static void buildPortalApSsid() {
  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(portalApSsid, sizeof(portalApSsid), "Epaper-%04X", (uint16_t)(chipId & 0xFFFF));
}

static bool loadStoredWiFiCredentials(String &outSsid, String &outPass) {
  devicePrefs.begin(PREFS_NAMESPACE, true);
  if (!devicePrefs.isKey(PREFS_KEY_SSID)) {
    devicePrefs.end();
    return false;
  }
  outSsid = devicePrefs.getString(PREFS_KEY_SSID, "");
  outPass = devicePrefs.getString(PREFS_KEY_PASS, "");
  devicePrefs.end();
  return outSsid.length() > 0;
}

static void saveStoredWiFiCredentials(const String &ssid, const String &pass) {
  devicePrefs.begin(PREFS_NAMESPACE, false);
  devicePrefs.putString(PREFS_KEY_SSID, ssid);
  devicePrefs.putString(PREFS_KEY_PASS, pass);
  devicePrefs.end();
}

static bool tryConnectStoredWiFi(unsigned long timeoutMs) {
  String storedSsid;
  String storedPass;
  if (!loadStoredWiFiCredentials(storedSsid, storedPass)) {
    Serial.println("[WiFi] No stored credentials");
    return false;
  }

  Serial.printf("[WiFi] Connecting to %s...\n", storedSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(storedSsid.c_str(), storedPass.c_str());

  const unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs >= timeoutMs) {
      Serial.println(" TIMEOUT");
      return false;
    }
    delay(500);
  }

  Serial.println();
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());
  wifiReconnectFailures = 0;
  return true;
}

static bool drawQrCodeScaled(UBYTE *image, const char *text, UWORD areaX, UWORD areaY, UWORD areaSize) {
  static uint8_t qrBuffer[QR_CODE_BUFFER_SIZE];
  QRCode qrcode;
  bool ok = false;

  for (uint8_t version = 2; version <= 7; version++) {
    if (qrcode_initText(&qrcode, qrBuffer, version, ECC_LOW, text) == 0) {
      ok = true;
      break;
    }
  }
  if (!ok) {
    Serial.println("[QR] encode failed");
    return false;
  }

  const uint8_t moduleCount = qrcode.size;
  UWORD scale = areaSize / moduleCount;
  if (scale < 2) {
    scale = 2;
  }

  const UWORD qrPixels = moduleCount * scale;
  const UWORD originX = areaX + (areaSize - qrPixels) / 2;
  const UWORD originY = areaY + (areaSize - qrPixels) / 2;

  for (uint8_t row = 0; row < moduleCount; row++) {
    for (uint8_t col = 0; col < moduleCount; col++) {
      if (!qrcode_getModule(&qrcode, col, row)) {
        continue;
      }
      for (UWORD sy = 0; sy < scale; sy++) {
        for (UWORD sx = 0; sx < scale; sx++) {
          setEpaperPixel(image, originX + col * scale + sx, originY + row * scale + sy, true);
        }
      }
    }
  }
  return true;
}

static void drawSetupScreen(UBYTE *image) {
  char qrPayload[96];
  snprintf(qrPayload, sizeof(qrPayload), "WIFI:T:nopass;S:%s;;", portalApSsid);

  memset(image, 0xFF, 5000);
  drawText(image, "WIFI SETUP", 44, 6);
  drawText(image, "SCAN QR", 56, 22);
  drawQrCodeScaled(image, qrPayload, 30, 38, 140);
  drawText(image, portalApSsid, 28, 168);
  drawText(image, "192.168.4.1", 52, 182);
}

static void showSetupScreenOnEpaper() {
  epaper_set_portal_mirror(true);
  drawSetupScreen(epaper_get_buffer());
  EPD_1IN54_V2_Init();
  EPD_1IN54_V2_DisplayPartBaseImage(epaper_get_buffer());
  Serial.println("[EPD] setup QR screen shown");
}

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Aink Setup</title>
  <style>
    body { font-family: sans-serif; margin: 24px; background: #f5f5f5; }
    .card { background: #fff; border-radius: 12px; padding: 20px; max-width: 420px; margin: 0 auto 16px; box-shadow: 0 2px 8px rgba(0,0,0,.08); }
    h2 { margin-top: 0; }
    label { display: block; margin: 12px 0 6px; font-size: 14px; color: #444; }
    input { width: 100%; box-sizing: border-box; padding: 10px; font-size: 16px; border: 1px solid #ccc; border-radius: 8px; }
    button { margin-top: 18px; width: 100%; padding: 12px; font-size: 16px; border: 0; border-radius: 8px; background: #111; color: #fff; }
    .hint { margin-top: 16px; font-size: 13px; color: #666; line-height: 1.5; }
  </style>
</head>
<body>
  <div class="card">
    <h2>WiFi</h2>
    <form action="/save" method="POST">
      <label for="ssid">WiFi 名称</label>
      <input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
      <label for="pass">WiFi 密码</label>
      <input id="pass" name="pass" type="password" maxlength="64" autocomplete="off">
      <button type="submit">连接 WiFi</button>
    </form>
    <p class="hint">保存后设备会尝试连接路由器并重启。</p>
  </div>
  <div class="card">
    <h2>AI API</h2>
    <form action="/save_ai" method="POST">
      <label for="api_key">API Key</label>
      <input id="api_key" name="api_key" type="password" maxlength="128" required autocomplete="off"
             placeholder="Provider API Key">
      <button type="submit">保存 API Key</button>
    </form>
    <p class="hint">Provider 与 Model 在设备 Settings → Model 中选择。Kimi Platform 请使用 platform.kimi.ai 的 Key；MiMo Token Plan 请使用 tp- 开头的订阅 Key。Key 仅存于本机 NVS，不会上传到其他服务器。</p>
  </div>
  <div class="card">
    <h2>天气 API（和风）</h2>
    <form action="/save_weather" method="POST">
      <label for="weather_host">API Host</label>
      <input id="weather_host" name="weather_host" maxlength="64" required autocomplete="off"
             placeholder="xxx.re.qweatherapi.com">
      <label for="weather_api_key">API Key</label>
      <input id="weather_api_key" name="weather_api_key" type="password" maxlength="128" required autocomplete="off"
             placeholder="QWeather Key">
      <button type="submit">保存天气 API</button>
    </form>
    <p class="hint">Host 与 Key 在 <a href="https://console.qweather.com">console.qweather.com</a> 控制台获取。仅存于本机 NVS。也可在 Settings → WiFi 中配置。</p>
  </div>
</body>
</html>
)rawliteral";

static void handlePortalRoot() {
  portalServer.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

static void handlePortalSave() {
  if (!portalServer.hasArg("ssid")) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>SSID 不能为空</p><a href='/'>返回</a>");
    return;
  }

  String newSsid = portalServer.arg("ssid");
  String newPass = portalServer.hasArg("pass") ? portalServer.arg("pass") : "";
  newSsid.trim();

  if (newSsid.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>SSID 不能为空</p><a href='/'>返回</a>");
    return;
  }

  Serial.printf("[Portal] Trying SSID: %s\n", newSsid.c_str());
  portalDnsServer.stop();
  WiFi.softAPdisconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(newSsid.c_str(), newPass.c_str());

  const unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    saveStoredWiFiCredentials(newSsid, newPass);
    Serial.println("[Portal] Connected, restarting into normal mode");
    portalServer.send(200, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "</head><body><h2>WiFi 已连接</h2><p>设备即将重启...</p></body></html>");
    delay(1500);
    ESP.restart();
    return;
  }

  Serial.println("[Portal] Connect failed, restoring AP");
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(portalApSsid);
  portalDnsServer.start(WIFI_PORTAL_DNS_PORT, "*", WiFi.softAPIP());

  portalServer.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "</head><body><h2>连接失败</h2><p>请检查 WiFi 名称和密码</p>"
                    "<a href='/'>返回重试</a></body></html>");
}

static void handlePortalSaveAi() {
  if (!portalServer.hasArg("api_key")) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>API Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

  String apiKey = portalServer.arg("api_key");
  apiKey.trim();
  if (apiKey.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>API Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

  settings_api_set_api_key(apiKey.c_str());

  String storedSsid;
  String storedPass;
  if (loadStoredWiFiCredentials(storedSsid, storedPass)) {
    portalServer.send(200, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "</head><body><h2>API Key 已保存</h2>"
                      "<p>设备即将重启并返回主界面...</p></body></html>");
    delay(1500);
    ESP.restart();
    return;
  }

  portalServer.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "</head><body><h2>API Key 已保存</h2>"
                    "<p>请先配置 WiFi，保存后设备会自动重启。</p>"
                    "<a href='/'>返回</a></body></html>");
}

static void handlePortalSaveWeather() {
  if (!portalServer.hasArg("weather_host") || !portalServer.hasArg("weather_api_key")) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>Host 与 Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

  String apiHost = portalServer.arg("weather_host");
  String apiKey = portalServer.arg("weather_api_key");
  apiHost.trim();
  apiKey.trim();
  if (apiHost.length() == 0 || apiKey.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>Host 与 Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

  settings_api_set_weather_api_host(apiHost.c_str());
  settings_api_set_weather_api_key(apiKey.c_str());
  weather_service_reset();

  String storedSsid;
  String storedPass;
  if (loadStoredWiFiCredentials(storedSsid, storedPass)) {
    portalServer.send(200, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "</head><body><h2>天气 API 已保存</h2>"
                      "<p>设备即将重启并拉取天气...</p></body></html>");
    delay(1500);
    ESP.restart();
    return;
  }

  portalServer.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "</head><body><h2>天气 API 已保存</h2>"
                    "<p>请先配置 WiFi，保存后设备会自动重启。</p>"
                    "<a href='/'>返回</a></body></html>");
}

static void setupPortalWebRoutes() {
  if (portalWebStarted) {
    return;
  }

  portalServer.on("/", HTTP_GET, handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  portalServer.on("/save_ai", HTTP_POST, handlePortalSaveAi);
  portalServer.on("/save_weather", HTTP_POST, handlePortalSaveWeather);
  portalServer.on("/generate_204", HTTP_GET, handlePortalRoot);
  portalServer.on("/hotspot-detect.html", HTTP_GET, handlePortalRoot);
  portalServer.on("/fwlink", HTTP_GET, handlePortalRoot);
  portalServer.onNotFound([]() {
    portalServer.sendHeader("Location", "http://192.168.4.1/", true);
    portalServer.send(302, "text/plain", "");
  });
  portalServer.begin();
  portalWebStarted = true;
}

static void enterPortalMode() {
  buildPortalApSsid();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(portalApSsid);
  portalDnsServer.start(WIFI_PORTAL_DNS_PORT, "*", WiFi.softAPIP());
  setupPortalWebRoutes();

  portalModeActive = true;
  Serial.printf("[Portal] AP: %s  open  IP: %s\n",
                portalApSsid, WiFi.softAPIP().toString().c_str());
  showSetupScreenOnEpaper();
}

static void startNormalOperation() {
  portalModeActive = false;
  epaper_set_portal_mirror(false);
  if (WiFi.status() == WL_CONNECTED) {
    syncNetworkTime();
  }

  if (camera_service_init()) {
    Serial.println("[Camera] initialized");
  } else {
    Serial.println("[Camera] unavailable (no module or init error)");
  }

  Serial.println("[EPD] full clear (~25s)...");
  EPD_1IN54_V2_Init();
  EPD_1IN54_V2_Clear();
  Serial.println("[EPD] clear done");

  lastDisplayedMinute = -1;
  lastWifiState = -1;
  lastWeatherIcon = -1;
  lastWeatherTemp = -999;
  weather_service_reset();

  btn_input_init();
  ui_lvgl_init();
  app_locale_init();
  ui_home_init();
  ui_weather_init();
  ui_vision_init();
  ui_settings_init();
  ui_nav_init();
  ui_lvgl_prepare();

  refreshMainUiOnDisplay(UI_REFRESH_FULL);
  weather_service_update(true);
  refreshMainUiOnDisplay(UI_REFRESH_QUALITY);
}

static void drawWifiIcon(UBYTE *image, UWORD ox, UWORD oy, bool connected) {
  for (int row = 0; row < 9; row++) {
    for (int col = 0; col < 15; col++) {
      setEpaperPixel(image, ox + col, oy + row, false);
    }
  }

  static const uint16_t disconnected_icon[9] = {
    0b010000000001000,
    0b001000000010000,
    0b000100000100000,
    0b000010001000000,
    0b000001010000000,
    0b000000100000000,
    0b000000000000000,
    0b000000000000000,
    0b000000100000000
  };

  static const uint16_t connected_icon[9] = {
    0b000111111111000,
    0b001100000011000,
    0b000000000000000,
    0b000001111100000,
    0b000011000110000,
    0b000000000000000,
    0b000000111000000,
    0b000000000000000,
    0b000000010000000
  };

  const uint16_t *icon = connected ? connected_icon : disconnected_icon;
  for (int row = 0; row < 9; row++) {
    const uint16_t mask = icon[row];
    for (int col = 0; col < 15; col++) {
      if ((mask >> (14 - col)) & 0x01) {
        setEpaperPixel(image, ox + col, oy + row, true);
      }
    }
  }
}

static bool isWifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

static void drawWeatherIcon(UBYTE *image, UWORD ox, UWORD oy, WeatherIconKind kind) {
  if ((unsigned)kind >= WEATHER_ICON_COUNT) {
    kind = WEATHER_ICON_CLOUDY;
  }

  for (int row = 0; row < WEATHER_ICON_SIZE; row++) {
    for (int col = 0; col < WEATHER_ICON_SIZE; col++) {
      setEpaperPixel(image, ox + col, oy + row, false);
    }
  }

  for (int row = 0; row < WEATHER_ICON_SIZE; row++) {
    const uint16_t mask = weather_icon_bitmaps[kind][row];
    for (int col = 0; col < WEATHER_ICON_SIZE; col++) {
      if ((mask >> (WEATHER_ICON_SIZE - 1 - col)) & 0x01) {
        setEpaperPixel(image, ox + col, oy + row, true);
      }
    }
  }
}

// 简易电池图标：外框 + 电量填充
static void drawBatteryIcon(UBYTE *image, UWORD x, UWORD y, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  for (UWORD dy = 0; dy < 8; dy++) {
    for (UWORD dx = 0; dx < 14; dx++) {
      bool edge = (dy == 0 || dy == 7 || dx == 0 || dx == 13);
      setEpaperPixel(image, x + dx, y + dy, edge);
    }
  }
  for (UWORD dy = 2; dy < 6; dy++) {
    setEpaperPixel(image, x + 14, y + dy, true);
  }

  UWORD fillWidth = (UWORD)((percent * 10) / 100);
  if (fillWidth < 1 && percent > 0) fillWidth = 1;
  for (UWORD dy = 2; dy <= 5; dy++) {
    for (UWORD dx = 2; dx < 2 + fillWidth; dx++) {
      setEpaperPixel(image, x + dx, y + dy, true);
    }
  }
}

static int readBatteryPercent() {
  uint32_t mvSum = 0;
  for (int i = 0; i < 8; i++) {
    mvSum += analogReadMilliVolts(BATTERY_ADC_GPIO);
    delay(2);
  }
  float voltage = (mvSum / 8.0f) * 2.0f / 1000.0f;

  if (voltage < 0.3f) {
    return -1;
  }
  if (voltage > BATTERY_V_MAX + 0.3f) {
    return 100;
  }

  float pct = (voltage - BATTERY_V_MIN) / (BATTERY_V_MAX - BATTERY_V_MIN) * 100.0f;
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return (int)(pct + 0.5f);
}

static bool syncNetworkTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             "ntp.aliyun.com", "pool.ntp.org", "cn.ntp.org.cn");

  struct tm timeinfo;
  for (int i = 0; i < 30; i++) {
    if (getLocalTime(&timeinfo)) {
      Serial.printf("[NTP] synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      return true;
    }
    delay(500);
  }
  Serial.println("[NTP] sync failed");
  return false;
}

static const char* weekdayShort(int wday) {
  static const char* names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  if (wday < 0 || wday > 6) return "---";
  return names[wday];
}

static void drawStatusBarRegion(UBYTE *image, int batteryPercent, bool wifiConnected,
                                bool showWeather, WeatherIconKind weatherIcon, int weatherTempC) {
  (void)image;
  const UWORD barY = 2;
  const UWORD barLineY = 19;
  const UWORD dateX = 18;
  const UWORD timeX = 76;
  const UWORD weatherIconX = 118;
  const UWORD weatherIconY = barY + 2;
  const UWORD weatherTempX = 134;
  const UWORD battIconX = 159;
  const UWORD battTextX = 173;

  drawWifiIcon(image, 2, barY + 4, wifiConnected);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    drawText(image, "NO TIME", dateX, barY);
    drawHorizontalLine(image, barLineY);
    return;
  }

  char leftLine[32];
  snprintf(leftLine, sizeof(leftLine), "%s %d/%d",
           weekdayShort(timeinfo.tm_wday),
           timeinfo.tm_mon + 1,
           timeinfo.tm_mday);

  char timeLine[16];
  snprintf(timeLine, sizeof(timeLine), "%02d:%02d",
           timeinfo.tm_hour, timeinfo.tm_min);

  drawText(image, leftLine, dateX, barY);
  drawText(image, timeLine, timeX, barY);

  if (showWeather) {
    drawWeatherIcon(image, weatherIconX, weatherIconY, weatherIcon);
    char tempLine[8];
    if (weatherTempC >= -9 && weatherTempC <= 99) {
      snprintf(tempLine, sizeof(tempLine), "%2dC", weatherTempC);
    } else {
      snprintf(tempLine, sizeof(tempLine), "--C");
    }
    drawText(image, tempLine, weatherTempX, barY);
  }

  if (batteryPercent < 0) {
    drawBatteryIcon(image, battIconX, barY + 2, 100);
  } else {
    char battText[8];
    snprintf(battText, sizeof(battText), "%d%%", batteryPercent);
    drawBatteryIcon(image, battIconX, barY + 2, batteryPercent);
    drawText(image, battText, battTextX, barY);
  }

  drawHorizontalLine(image, barLineY);
}

static void refreshMainUiOnDisplay(UiRefreshMode mode) {
  if (mode == UI_REFRESH_NONE) {
    return;
  }

  const bool fullInit = (mode == UI_REFRESH_FULL);
  const bool fastEpd = (mode == UI_REFRESH_FAST || mode == UI_REFRESH_NAV);
  const bool fullLvgl = (mode != UI_REFRESH_FAST);

  if (mode == UI_REFRESH_QUALITY || mode == UI_REFRESH_FULL) {
    weather_service_update(fullInit);
    if (ui_nav_is_weather()) {
      ui_weather_refresh();
    } else if (ui_nav_is_home()) {
      ui_home_refresh_weather();
    }
  }

  ui_lvgl_prepare();
  if (fullLvgl) {
    epaper_clear_white();
    ui_lvgl_refresh();
  } else {
    ui_lvgl_refresh_partial();
  }

  const int batteryPercent = readBatteryPercent();
  const bool wifiConnected = isWifiConnected();
  const int wifiState = wifiConnected ? 1 : 0;

  if (fullLvgl) {
    WeatherSnapshot weather = {};
    weather_service_get_snapshot(&weather);
    drawStatusBarRegion(epaper_get_buffer(), batteryPercent, wifiConnected,
                        weather.valid, weather.icon, weather.tempC);
  }

  epaper_upload_mode(fullInit, fastEpd);

  if (!fullLvgl || mode == UI_REFRESH_NAV) {
    return;
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    lastDisplayedMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    lastWifiState = wifiState;
    WeatherSnapshot weather = {};
    weather_service_get_snapshot(&weather);
    if (weather.valid) {
      lastWeatherIcon = (int)weather.icon;
      lastWeatherTemp = weather.tempC;
    }
    Serial.printf("[Status] %s %d/%d %02d:%02d wifi=%s weather=%s batt=%d%%\n",
                  weekdayShort(timeinfo.tm_wday),
                  timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min,
                  wifiConnected ? "on" : "off",
                  weather.valid ? "ok" : "n/a",
                  batteryPercent);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== epaper wifi ===");

  DEV_Module_Init();

  if (settings_api_consume_force_portal_boot()) {
    Serial.println("[WiFi] Entering config portal (settings request)");
    enterPortalMode();
  } else if (tryConnectStoredWiFi(WIFI_CONNECT_TIMEOUT_MS)) {
    startNormalOperation();
  } else {
    Serial.println("[WiFi] Entering config portal");
    enterPortalMode();
  }
}

void loop() {
  if (portalModeActive) {
    portalDnsServer.processNextRequest();
    portalServer.handleClient();
    delay(10);
    return;
  }

  weather_service_update(false);

  btn_input_update();
  btn_input_serial_poll();
  ui_lvgl_tick();

  BtnAction btnAction = BTN_ACTION_NONE;
  while (btn_input_consume(&btnAction)) {
    UiRefreshMode navMode = UI_REFRESH_NONE;
    if (ui_nav_handle(btnAction, &navMode)) {
      refreshMainUiOnDisplay(navMode);
      if (ui_vision_consume_capture_request()) {
        Serial.println("[Vision] capture pipeline running (blocks up to ~60s)");
        Serial.flush();
        ui_vision_run_capture();
        Serial.println("[Vision] capture pipeline done");
        Serial.flush();
        /* DisplayPart 与「分析中」时一致；QUALITY 的 BaseImage 在此状态下可能不更新可见区域 */
        refreshMainUiOnDisplay(UI_REFRESH_NAV);
      }
    }
  }

  const bool wifiConnected = isWifiConnected();
  const int wifiState = wifiConnected ? 1 : 0;
  WeatherSnapshot weatherSnap = {};
  weather_service_get_snapshot(&weatherSnap);
  const int weatherIconState = weatherSnap.valid ? (int)weatherSnap.icon : -1;
  const int weatherTempState = weatherSnap.valid ? weatherSnap.tempC : -999;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    const int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    const bool weatherChanged =
        weatherIconState != lastWeatherIcon || weatherTempState != lastWeatherTemp;
    if (currentMinute != lastDisplayedMinute ||
        wifiState != lastWifiState ||
        weatherChanged) {
      refreshMainUiOnDisplay(UI_REFRESH_QUALITY);
    }
  } else if (wifiConnected) {
    syncNetworkTime();
  } else {
    if (wifiState != lastWifiState) {
      refreshMainUiOnDisplay(UI_REFRESH_QUALITY);
    }
    if (!tryConnectStoredWiFi(10000)) {
      wifiReconnectFailures++;
      Serial.printf("[WiFi] Reconnect failed (%d/%d)\n",
                    wifiReconnectFailures, WIFI_RECONNECT_MAX_FAIL);
      if (wifiReconnectFailures >= WIFI_RECONNECT_MAX_FAIL) {
        Serial.println("[WiFi] Restarting into config portal");
        ESP.restart();
      }
    } else {
      syncNetworkTime();
      weather_service_update(true);
      refreshMainUiOnDisplay(UI_REFRESH_QUALITY);
    }
  }

  delay(50);
}
