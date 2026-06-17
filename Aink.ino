#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "EPD_1in54_V2.h"
#include "weather_icons.h"
#include "weather_service.h"
#include "stock_service.h"
#include "epaper_canvas.h"
#include "btn_input.h"
#include "ui_lvgl.h"
#include "ui_home.h"
#include "ui_weather.h"
#include "ui_stock.h"
#include "ui_settings.h"
#include "ui_nav.h"
#include "ui_answers.h"
#include "ui_vision.h"
#include "ui_voice.h"
#include "ui_clock.h"
#include "ui_refresh.h"
#include "settings_api.h"
#include "app_locale.h"
#include "voice_service.h"

extern "C" {
#include "qrcode.h"
}

// 中国时区 UTC+8
#define GMT_OFFSET_SEC           (8 * 3600)
#define DAYLIGHT_OFFSET_SEC      0

// 状态栏高度（16px 字体 + 分隔线）
#define STATUS_BAR_HEIGHT        20

// WiFi 配网
#define WIFI_PORTAL_DNS_PORT     53
#define PORTAL_WIFI_SCAN_MAX     20
#define PREFS_NAMESPACE          "epaper"
#define PREFS_KEY_SSID           "ssid"
#define PREFS_KEY_PASS           "pass"
#define QR_CODE_BUFFER_SIZE      350
#define WIFI_RECONNECT_MAX_FAIL  5
#define WIFI_CONNECT_TIMEOUT_MS  10000UL
#define WIFI_RECONNECT_BACKOFF_MS 2000UL
#define NTP_SYNC_TIMEOUT_MS      15000UL
#define NETWORK_IDLE_AFTER_INPUT_MS 600UL
#define DISPLAY_COALESCE_MS      80U
#define DISPLAY_UPLOAD_GUARD_MS  700U

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

struct PortalWifiScanEntry {
  char ssid[33];
  int32_t rssi;
};

static PortalWifiScanEntry portalScanResults[PORTAL_WIFI_SCAN_MAX];
static int portalScanCount = 0;
static bool displayRefreshPending = false;
static UiRefreshMode pendingDisplayRefreshMode = UI_REFRESH_NONE;
static uint8_t pendingDisplayRequestCount = 0;
static unsigned long pendingDisplaySinceMs = 0;
static unsigned long lastDisplayUploadMs = 0;

enum NetworkState {
  NET_IDLE = 0,
  NET_WIFI_WAIT,
  NET_NTP_WAIT,
  NET_WEATHER_FETCH,
};

enum DisplayBootState {
  DISPLAY_BOOT_READY = 0,
  DISPLAY_BOOT_CLEAR_WAIT,
};

static NetworkState networkState = NET_IDLE;
static DisplayBootState displayBootState = DISPLAY_BOOT_READY;
static bool networkTimeSyncRequested = false;
static bool networkWeatherForcePending = false;
static bool networkWeatherServicePending = false;
static bool networkStockForcePending = false;
static bool networkDnsConfigured = false;
static unsigned long lastUserInputMs = 0;
static unsigned long wifiConnectStartMs = 0;
static unsigned long nextWifiAttemptMs = 0;
static unsigned long ntpSyncStartMs = 0;

static void serviceNetworkStateMachine(bool allowBlockingWork);
static void serviceStockNameRetry(bool wifiConnected, bool inputIdle);
static void requestNetworkWeatherFetch(bool force);
static bool serviceDisplayBootState(void);
static bool isWifiConnected();
static bool isZeroIp(const IPAddress &ip);
static void configureStationDns(void);
static void enterPortalMode();
static void drawStatusBarRegion(UBYTE *image, int batteryPercent, bool wifiConnected,
                                bool showWeather, WeatherIconKind weatherIcon, int weatherTempC);
static void refreshMainUiOnDisplay(UiRefreshMode mode);
static void requestDisplayRefresh(UiRefreshMode mode);
static void serviceDisplayRefresh(bool force);

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
  snprintf(portalApSsid, sizeof(portalApSsid), "Aink-%04X", (uint16_t)(chipId & 0xFFFF));
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

static bool hasStoredWiFiCredentials() {
  String storedSsid;
  String storedPass;
  return loadStoredWiFiCredentials(storedSsid, storedPass);
}

static void saveStoredWiFiCredentials(const String &ssid, const String &pass) {
  devicePrefs.begin(PREFS_NAMESPACE, false);
  devicePrefs.putString(PREFS_KEY_SSID, ssid);
  devicePrefs.putString(PREFS_KEY_PASS, pass);
  devicePrefs.end();
}

static bool startStoredWiFiConnect() {
  String storedSsid;
  String storedPass;
  if (!loadStoredWiFiCredentials(storedSsid, storedPass)) {
    Serial.println("[WiFi] No stored credentials");
    return false;
  }

  Serial.printf("[WiFi] Connecting to %s (async)...\n", storedSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(storedSsid.c_str(), storedPass.c_str());
  wifiConnectStartMs = millis();
  networkState = NET_WIFI_WAIT;
  return true;
}

static void beginNetworkTimeSync() {
  Serial.println("[NTP] sync requested");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             "ntp.aliyun.com", "pool.ntp.org", "cn.ntp.org.cn");
  ntpSyncStartMs = millis();
  networkTimeSyncRequested = true;
  networkState = NET_NTP_WAIT;
}

static bool pollNetworkTimeSync() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    Serial.printf("[NTP] synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    networkTimeSyncRequested = false;
    return true;
  }

  if (millis() - ntpSyncStartMs >= NTP_SYNC_TIMEOUT_MS) {
    Serial.println("[NTP] sync timeout");
    networkTimeSyncRequested = false;
    return true;
  }
  return false;
}

static void requestNetworkWeatherFetch(bool force) {
  networkWeatherServicePending = true;
  if (force) {
    networkWeatherForcePending = true;
  }
}

static void configureStationDns(void) {
  if (networkDnsConfigured || !isWifiConnected()) {
    return;
  }

  const IPAddress dns1(223, 5, 5, 5);
  const IPAddress dns2(119, 29, 29, 29);
  if (WiFi.setDNS(dns1, dns2)) {
    Serial.printf("[WiFi] DNS set: %s, %s\n",
                  dns1.toString().c_str(), dns2.toString().c_str());
  } else {
    Serial.println("[WiFi] DNS set failed");
  }

  IPAddress mimoIp;
  if (WiFi.hostByName("api.xiaomimimo.com", mimoIp) && !isZeroIp(mimoIp)) {
    Serial.printf("[WiFi] MiMo DNS api.xiaomimimo.com -> %s\n",
                  mimoIp.toString().c_str());
    networkDnsConfigured = true;
  } else {
    Serial.println("[WiFi] MiMo DNS lookup failed");
  }
}

static void serviceNetworkStateMachine(bool allowBlockingWork) {
  if (portalModeActive) {
    return;
  }

  const unsigned long now = millis();
  const bool wifiConnected = isWifiConnected();

  switch (networkState) {
    case NET_WIFI_WAIT:
      if (wifiConnected) {
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        configureStationDns();
        wifiReconnectFailures = 0;
        networkState = NET_IDLE;
        beginNetworkTimeSync();
        requestNetworkWeatherFetch(true);
        networkStockForcePending = true;
        requestDisplayRefresh(UI_REFRESH_QUALITY);
      } else if (now - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
        WiFi.disconnect();
        wifiReconnectFailures++;
        nextWifiAttemptMs = now + WIFI_RECONNECT_BACKOFF_MS;
        networkState = NET_IDLE;
        Serial.printf("[WiFi] Reconnect failed (%d/%d)\n",
                      wifiReconnectFailures, WIFI_RECONNECT_MAX_FAIL);
        if (wifiReconnectFailures >= WIFI_RECONNECT_MAX_FAIL) {
          Serial.println("[WiFi] Entering config portal after reconnect failures");
          enterPortalMode();
        }
      }
      return;

    case NET_NTP_WAIT:
      if (!wifiConnected) {
        networkTimeSyncRequested = false;
        networkState = NET_IDLE;
        return;
      }
      if (pollNetworkTimeSync()) {
        networkState = NET_IDLE;
        requestDisplayRefresh(UI_REFRESH_QUALITY);
      }
      return;

    case NET_WEATHER_FETCH:
      if (!wifiConnected) {
        networkWeatherServicePending = false;
        networkWeatherForcePending = false;
        networkState = NET_IDLE;
        return;
      }
      if (!allowBlockingWork) {
        return;
      }
      if (!weather_service_is_busy()) {
        weather_service_request_update(networkWeatherForcePending);
      }
      networkWeatherForcePending = false;
      networkWeatherServicePending = false;
      networkState = NET_IDLE;
      return;

    case NET_IDLE:
    default:
      break;
  }

  if (!wifiConnected) {
    networkDnsConfigured = false;
    if (hasStoredWiFiCredentials() && now >= nextWifiAttemptMs) {
      if (!startStoredWiFiConnect()) {
        nextWifiAttemptMs = now + WIFI_RECONNECT_BACKOFF_MS;
      }
    }
    return;
  }

  configureStationDns();

  if (networkTimeSyncRequested) {
    beginNetworkTimeSync();
    return;
  }

  if (networkWeatherServicePending) {
    networkState = NET_WEATHER_FETCH;
    serviceNetworkStateMachine(allowBlockingWork);
    return;
  }

  if (allowBlockingWork) {
    if (!weather_service_is_busy()) {
      weather_service_request_update(false);
    }
  } else {
    requestNetworkWeatherFetch(false);
  }

  if (weather_service_consume_fresh_fetch()) {
    requestDisplayRefresh(UI_REFRESH_QUALITY);
  }

  if (allowBlockingWork && !weather_service_is_busy()) {
    const bool stockForce = networkStockForcePending;
    networkStockForcePending = false;
    stock_service_update(stockForce);
    if (stock_service_consume_fresh_fetch()) {
      requestDisplayRefresh(UI_REFRESH_QUALITY);
    }
  }
}

static void serviceStockNameRetry(bool wifiConnected, bool inputIdle) {
  if (!wifiConnected || !inputIdle || portalModeActive) {
    return;
  }
  if (weather_service_is_busy()) {
    return;
  }
  if (stock_service_needs_name_fetch() && stock_service_retry_names()) {
    requestDisplayRefresh(UI_REFRESH_QUALITY);
  }
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

static void portalHtmlEscape(const String &in, String &out) {
  out = "";
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in.charAt(i);
    if (c == '&') {
      out += "&amp;";
    } else if (c == '"') {
      out += "&quot;";
    } else if (c == '<') {
      out += "&lt;";
    } else if (c == '>') {
      out += "&gt;";
    } else {
      out += c;
    }
  }
}

static void portalHtmlAppendConfiguredBadge(String &html, bool configured) {
  if (configured) {
    html += F("<span class=\"badge\">已配置</span>");
  }
}

static void portalSortScanResults() {
  for (int i = 0; i < portalScanCount - 1; i++) {
    for (int j = i + 1; j < portalScanCount; j++) {
      if (portalScanResults[j].rssi > portalScanResults[i].rssi) {
        const PortalWifiScanEntry tmp = portalScanResults[i];
        portalScanResults[i] = portalScanResults[j];
        portalScanResults[j] = tmp;
      }
    }
  }
}

static void portalScanNetworks() {
  portalScanCount = 0;
  Serial.println("[Portal] Scanning WiFi...");
  const int found = WiFi.scanNetworks(false, true);
  if (found < 0) {
    Serial.printf("[Portal] Scan failed (%d)\n", found);
    WiFi.scanDelete();
    return;
  }

  for (int i = 0; i < found && portalScanCount < PORTAL_WIFI_SCAN_MAX; i++) {
    String ssid = WiFi.SSID(i);
    ssid.trim();
    if (ssid.length() == 0 || ssid.length() > 32) {
      continue;
    }

    bool merged = false;
    for (int j = 0; j < portalScanCount; j++) {
      if (ssid == portalScanResults[j].ssid) {
        if (WiFi.RSSI(i) > portalScanResults[j].rssi) {
          portalScanResults[j].rssi = WiFi.RSSI(i);
        }
        merged = true;
        break;
      }
    }
    if (merged) {
      continue;
    }

    ssid.toCharArray(portalScanResults[portalScanCount].ssid,
                     sizeof(portalScanResults[portalScanCount].ssid));
    portalScanResults[portalScanCount].rssi = WiFi.RSSI(i);
    portalScanCount++;
  }

  WiFi.scanDelete();
  portalSortScanResults();
  Serial.printf("[Portal] Scan done, %d networks\n", portalScanCount);
}

static bool portalScanContainsSsid(const char *ssid) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }
  for (int i = 0; i < portalScanCount; i++) {
    if (strcmp(portalScanResults[i].ssid, ssid) == 0) {
      return true;
    }
  }
  return false;
}

static void portalEnsureApMode() {
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
  }
  if (WiFi.softAPIP()[0] == 0) {
    WiFi.softAP(portalApSsid);
    delay(100);
  }
}

static void portalRestoreApAfterFailedConnect() {
  Serial.println("[Portal] Connect failed, restoring AP");
  WiFi.disconnect(true);
  delay(300);
  portalEnsureApMode();
  WiFi.softAP(portalApSsid);
  delay(100);
  portalScanNetworks();
  portalDnsServer.start(WIFI_PORTAL_DNS_PORT, "*", WiFi.softAPIP());
}

static void portalAppendWifiNetworkFields(String &html, const char *preferredSsid) {
  const bool hasPreferred = preferredSsid != nullptr && preferredSsid[0] != '\0';
  const bool preferredInScan = hasPreferred && portalScanContainsSsid(preferredSsid);

  html += F(R"rawliteral(<select id="ssid" name="ssid">
        <option value="">请选择 WiFi</option>
)rawliteral");

  for (int i = 0; i < portalScanCount; i++) {
    String escOption;
    portalHtmlEscape(String(portalScanResults[i].ssid), escOption);
    html += F("<option value=\"");
    html += escOption;
    html += '"';
    if (hasPreferred && strcmp(portalScanResults[i].ssid, preferredSsid) == 0) {
      html += F(" selected");
    }
    html += '>';
    html += escOption;
    html += " (";
    html += String(portalScanResults[i].rssi);
    html += F(" dBm)</option>\n");
  }

  if (hasPreferred && !preferredInScan) {
    String escPreferred;
    portalHtmlEscape(String(preferredSsid), escPreferred);
    html += F("<option value=\"");
    html += escPreferred;
    html += F("\" selected>");
    html += escPreferred;
    html += F(" (已保存)</option>\n");
  }

  html += F(R"rawliteral(</select>
      <p class="hint"><a href="/?rescan=1">刷新 WiFi 列表</a>（扫描约需数秒）</p>
      <label for="ssid_manual">或手动输入名称</label>
      <input id="ssid_manual" name="ssid_manual" maxlength="32" autocomplete="off"
             placeholder="列表中没有时填写" value=")rawliteral");

  if (hasPreferred && !preferredInScan) {
    String escManual;
    portalHtmlEscape(String(preferredSsid), escManual);
    html += escManual;
  }

  html += F(R"rawliteral(">
    <p class="hint">优先从列表选择；手动输入仅在该 WiFi 未出现在扫描结果时使用。</p>
)rawliteral");
}

static bool portalPersistNonWifiConfig(String *errorOut) {
  if (portalServer.hasArg("api_key")) {
    String apiKey = portalServer.arg("api_key");
    apiKey.trim();
    if (apiKey.length() > 0) {
      settings_api_set_api_key(apiKey.c_str());
    }
  }

  if (portalServer.hasArg("weather_host")) {
    String apiHost = portalServer.arg("weather_host");
    apiHost.trim();
    if (apiHost.length() > 0) {
      String apiKey =
          portalServer.hasArg("weather_api_key") ? portalServer.arg("weather_api_key") : "";
      apiKey.trim();
      if (apiKey.length() == 0) {
        if (settings_api_has_weather_api()) {
          char existingKey[128];
          settings_api_get_weather_api_key(existingKey, sizeof(existingKey));
          settings_api_set_weather_api_host(apiHost.c_str());
          settings_api_set_weather_api_key(existingKey);
        } else if (errorOut != nullptr) {
          *errorOut = String("天气 API Key 不能为空");
          return false;
        }
      } else {
        settings_api_set_weather_api_host(apiHost.c_str());
        settings_api_set_weather_api_key(apiKey.c_str());
      }
      weather_service_reset();
    }
  }

  if (portalServer.hasArg("watchlist")) {
    String watchlist = portalServer.arg("watchlist");
    watchlist.trim();
    if (watchlist.length() > 0) {
      settings_api_set_watchlist(watchlist.c_str());
      stock_service_invalidate_name_cache();
      stock_service_reset();
    }
  }

  return true;
}

static void handlePortalRoot() {
  if (portalServer.hasArg("rescan")) {
    portalScanNetworks();
  }

  char ssidBuf[64];
  char weatherHostBuf[80];
  char watchlistBuf[140];
  settings_api_get_wifi_ssid(ssidBuf, sizeof(ssidBuf));

  String storedSsid;
  String storedPass;
  const bool hasStoredWifi = loadStoredWiFiCredentials(storedSsid, storedPass);
  const char *preferredSsid = hasStoredWifi ? storedSsid.c_str()
                                             : (ssidBuf[0] != '\0' ? ssidBuf : nullptr);
  const bool hasAiKey = settings_api_has_api_key();
  const bool hasWeatherApi = settings_api_has_weather_api();
  settings_api_get_weather_api_host(weatherHostBuf, sizeof(weatherHostBuf));

  watchlistBuf[0] = '\0';
  if (settings_api_has_watchlist()) {
    settings_api_get_watchlist(watchlistBuf, sizeof(watchlistBuf));
  }

  String escWeatherHost;
  String escWatchlist;
  portalHtmlEscape(String(weatherHostBuf), escWeatherHost);
  portalHtmlEscape(String(watchlistBuf), escWatchlist);

  String html;
  html.reserve(6200);
  html += F(R"rawliteral(
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
    input, select { width: 100%; box-sizing: border-box; padding: 10px; font-size: 16px; border: 1px solid #ccc; border-radius: 8px; background: #fff; }
    button { margin-top: 18px; width: 100%; padding: 12px; font-size: 16px; border: 0; border-radius: 8px; background: #111; color: #fff; }
    .hint { margin-top: 16px; font-size: 13px; color: #666; line-height: 1.5; }
    .badge { display: inline-block; margin-left: 8px; padding: 2px 8px; font-size: 12px; color: #0a6; background: #e8f8ef; border-radius: 999px; vertical-align: middle; }
    a { color: #06c; }
  </style>
</head>
<body>
  <form action="/save" method="POST">
  <div class="card">
    <h2>WiFi</h2>
      <label for="ssid">WiFi 网络)rawliteral");
  portalHtmlAppendConfiguredBadge(html, hasStoredWifi || ssidBuf[0] != '\0');
  html += F(R"rawliteral(</label>
)rawliteral");
  portalAppendWifiNetworkFields(html, preferredSsid);
  html += F(R"rawliteral(      <label for="pass">WiFi 密码)rawliteral");
  portalHtmlAppendConfiguredBadge(html, hasStoredWifi && storedPass.length() > 0);
  html += F(R"rawliteral(</label>
      <input id="pass" name="pass" type="password" maxlength="64" autocomplete="off"
             placeholder=")rawliteral");
  if (hasStoredWifi && storedPass.length() > 0) {
    html += F("已保存，留空则沿用");
  }
  html += F(R"rawliteral(">
    <p class="hint">修改 WiFi 时，密码留空将沿用已保存的密码（需与列表中或手动输入的 SSID 一致）。</p>
  </div>
  <div class="card">
    <h2>AI API</h2>
      <label for="api_key">API Key)rawliteral");
  portalHtmlAppendConfiguredBadge(html, hasAiKey);
  html += F(R"rawliteral(</label>
      <input id="api_key" name="api_key" type="password" maxlength="128" autocomplete="off"
             placeholder=")rawliteral");
  if (hasAiKey) {
    html += F("已保存，留空则不修改");
  } else {
    html += F("Provider API Key");
  }
  html += F(R"rawliteral(">
    <p class="hint">Provider 与 Model 在设备 Settings → Model 中选择。Kimi Platform 请使用 platform.kimi.ai 的 Key；MiMo Token Plan 请使用 tp- 开头的订阅 Key。Key 仅存于本机 NVS，不会上传到其他服务器。</p>
  </div>
  <div class="card">
    <h2>天气 API（和风）</h2>
      <label for="weather_host">API Host)rawliteral");
  portalHtmlAppendConfiguredBadge(html, weatherHostBuf[0] != '\0');
  html += F(R"rawliteral(</label>
      <input id="weather_host" name="weather_host" maxlength="64" autocomplete="off"
             placeholder="xxx.re.qweatherapi.com" value=")rawliteral");
  html += escWeatherHost;
  html += F(R"rawliteral(">
      <label for="weather_api_key">API Key)rawliteral");
  portalHtmlAppendConfiguredBadge(html, hasWeatherApi);
  html += F(R"rawliteral(</label>
      <input id="weather_api_key" name="weather_api_key" type="password" maxlength="128" autocomplete="off"
             placeholder=")rawliteral");
  if (hasWeatherApi) {
    html += F("已保存，留空则不修改");
  } else {
    html += F("QWeather Key");
  }
  html += F(R"rawliteral(">
    <p class="hint">Host 与 Key 在 <a href="https://console.qweather.com">console.qweather.com</a> 控制台获取。仅存于本机 NVS。也可在 Settings → WiFi 中配置。</p>
  </div>
  <div class="card">
    <h2>自选股</h2>
      <label for="watchlist">代码列表（最多 5 个，逗号分隔)rawliteral");
  portalHtmlAppendConfiguredBadge(html, settings_api_has_watchlist());
  html += F(R"rawliteral(</label>
      <input id="watchlist" name="watchlist" maxlength="128" autocomplete="off"
             placeholder="sh600519,AAPL,MSFT" value=")rawliteral");
  html += escWatchlist;
  html += F(R"rawliteral(">
    <p class="hint">A 股：<code>sh600519</code> / <code>sz000001</code>；美股：<code>AAPL</code> / <code>TSLA</code>。保存时会替换整份列表，请在现有代码基础上增删。行情来自新浪 hq.sinajs.cn，无需 API Key。也可在 Settings → 股票 中配置。</p>
  </div>
  <div class="card">
    <button type="submit">保存全部配置</button>
    <p class="hint">一次保存本页所有配置（WiFi、AI API、天气 API、自选股）。各 Key 留空则保留已保存的值。保存后会尝试连接 WiFi 并重启设备。</p>
  </div>
  </form>
</body>
</html>
)rawliteral");

  portalServer.send(200, "text/html; charset=utf-8", html);
}

static void handlePortalSave() {
  String persistError;
  if (!portalPersistNonWifiConfig(&persistError)) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>" + persistError + "</p><a href='/'>返回</a>");
    return;
  }

  if (!portalServer.hasArg("ssid") && !portalServer.hasArg("ssid_manual")) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>请选择或填写 WiFi 名称</p><a href='/'>返回</a>");
    return;
  }

  String newSsid = portalServer.hasArg("ssid_manual") ? portalServer.arg("ssid_manual") : "";
  newSsid.trim();
  if (newSsid.length() == 0 && portalServer.hasArg("ssid")) {
    newSsid = portalServer.arg("ssid");
    newSsid.trim();
  }

  String newPass = portalServer.hasArg("pass") ? portalServer.arg("pass") : "";
  newPass.trim();

  if (newPass.length() == 0) {
    String oldSsid;
    String oldPass;
    if (loadStoredWiFiCredentials(oldSsid, oldPass) && oldSsid == newSsid) {
      newPass = oldPass;
    }
  }

  if (newSsid.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>SSID 不能为空</p><a href='/'>返回</a>");
    return;
  }

  Serial.printf("[Portal] Trying SSID: %s\n", newSsid.c_str());
  portalDnsServer.stop();
  WiFi.softAPdisconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(300);
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
                      "</head><body><h2>配置已保存</h2><p>WiFi 已连接，设备即将重启...</p></body></html>");
    delay(1500);
    ESP.restart();
    return;
  }

  portalRestoreApAfterFailedConnect();

  Serial.printf("[Portal] Connect failed, status=%d\n", WiFi.status());
  portalServer.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "</head><body><h2>WiFi 连接失败</h2><p>API 与其它配置已保存，但 WiFi 连接失败，请检查名称和密码。</p>"
                    "<a href='/'>返回重试</a></body></html>");
}

static void handlePortalSaveAi() {
  if (!portalPersistNonWifiConfig(nullptr)) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>天气 API Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

  String apiKey = portalServer.hasArg("api_key") ? portalServer.arg("api_key") : "";
  apiKey.trim();
  if (apiKey.length() == 0 && !settings_api_has_api_key()) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>API Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

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
  String persistError;
  if (!portalPersistNonWifiConfig(&persistError)) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>" + persistError + "</p><a href='/'>返回</a>");
    return;
  }

  if (!settings_api_has_weather_api()) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>Host 或 API Key 不能为空</p><a href='/'>返回</a>");
    return;
  }

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

static void handlePortalSaveStock() {
  if (!portalServer.hasArg("watchlist")) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>自选股不能为空</p><a href='/'>返回</a>");
    return;
  }

  String watchlist = portalServer.arg("watchlist");
  watchlist.trim();
  if (watchlist.length() == 0) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>自选股不能为空</p><a href='/'>返回</a>");
    return;
  }

  if (!portalPersistNonWifiConfig(nullptr)) {
    portalServer.send(400, "text/html; charset=utf-8",
                      "<meta charset=utf-8><p>保存失败</p><a href='/'>返回</a>");
    return;
  }

  String storedSsid;
  String storedPass;
  if (loadStoredWiFiCredentials(storedSsid, storedPass)) {
    portalServer.send(200, "text/html; charset=utf-8",
                      "<!DOCTYPE html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "</head><body><h2>自选股已保存</h2>"
                      "<p>设备即将重启并拉取行情...</p></body></html>");
    delay(1500);
    ESP.restart();
    return;
  }

  portalServer.send(200, "text/html; charset=utf-8",
                    "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<meta name=viewport content='width=device-width,initial-scale=1'>"
                    "</head><body><h2>自选股已保存</h2>"
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
  portalServer.on("/save_stock", HTTP_POST, handlePortalSaveStock);
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
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(portalApSsid);
  delay(100);
  portalScanNetworks();
  portalDnsServer.start(WIFI_PORTAL_DNS_PORT, "*", WiFi.softAPIP());
  setupPortalWebRoutes();

  portalModeActive = true;
  displayBootState = DISPLAY_BOOT_READY;
  Serial.printf("[Portal] AP: %s  open  IP: %s\n",
                portalApSsid, WiFi.softAPIP().toString().c_str());
  showSetupScreenOnEpaper();
}

static bool serviceDisplayBootState(void) {
  if (displayBootState == DISPLAY_BOOT_READY) {
    return true;
  }

  if (displayBootState == DISPLAY_BOOT_CLEAR_WAIT) {
    if (EPD_1IN54_V2_PollBusyWait()) {
      return false;
    }
    EPD_1IN54_V2_Enter_Partial();
    epaper_mark_partial_ready();
    displayBootState = DISPLAY_BOOT_READY;
    Serial.println("[EPD] partial ready after async white clear");
    return true;
  }

  return false;
}

static void startNormalOperation() {
  portalModeActive = false;
  epaper_set_portal_mirror(false);

  Serial.println("[EPD] full clear (~25s, async)...");
  EPD_1IN54_V2_Init();
  EPD_1IN54_V2_ClearAsync();
  displayBootState = DISPLAY_BOOT_CLEAR_WAIT;

  lastDisplayedMinute = -1;
  lastWifiState = -1;
  lastWeatherIcon = -1;
  lastWeatherTemp = -999;
  networkState = NET_IDLE;
  networkTimeSyncRequested = true;
  networkWeatherForcePending = false;
  networkWeatherServicePending = false;
  networkStockForcePending = false;
  lastUserInputMs = 0;
  wifiConnectStartMs = 0;
  nextWifiAttemptMs = 0;
  ntpSyncStartMs = 0;
  weather_service_reset();
  stock_service_reset();

  btn_input_init();
  ui_lvgl_init();
  app_locale_init();
  voice_service_init();
  ui_home_init();
  ui_weather_init();
  ui_stock_init();
  ui_answers_init();
  ui_vision_init();
  ui_voice_init();
  ui_clock_init();
  ui_settings_init();
  ui_nav_init();
  ui_lvgl_prepare();

  requestDisplayRefresh(UI_REFRESH_NAV);
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

static bool isZeroIp(const IPAddress &ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
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

static const char* weekdayShort(int wday) {
  static const char* names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  if (wday < 0 || wday > 6) return "---";
  return names[wday];
}

static UWORD statusBarTextWidth(const char *text) {
  return (UWORD)strlen(text) * 8;
}

static void buildStatusDateTimeLine(const struct tm *timeinfo, char *out, size_t outLen) {
  snprintf(out, outLen, "%s %d/%d %02d:%02d",
           weekdayShort(timeinfo->tm_wday),
           timeinfo->tm_mon + 1,
           timeinfo->tm_mday,
           timeinfo->tm_hour,
           timeinfo->tm_min);
}

static bool statusBarWeatherFits(UWORD dateTimeWidth, UWORD tempWidth, UWORD rightBoundX,
                                 UWORD *weatherIconX, UWORD *weatherTempX) {
  const UWORD gap = 2;
  *weatherIconX = 18 + dateTimeWidth + gap;
  *weatherTempX = *weatherIconX + 16 + gap;
  return (*weatherTempX + tempWidth + gap) <= rightBoundX;
}

static void drawStatusBarRegion(UBYTE *image, int batteryPercent, bool wifiConnected,
                                bool showWeather, WeatherIconKind weatherIcon, int weatherTempC) {
  (void)image;
  const UWORD barY = 2;
  const UWORD barLineY = 19;
  const UWORD dateX = 18;
  UWORD weatherIconX = 118;
  const UWORD weatherIconY = barY - 1;
  UWORD weatherTempX = 134;
  const UWORD battIconX = EPD_1IN54_V2_WIDTH - 15;

  drawWifiIcon(image, 2, barY + 4, wifiConnected);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    drawText(image, "NO TIME", dateX, barY);
    drawHorizontalLine(image, barLineY);
    return;
  }

  char dateTimeLine[32];
  char tempLine[8] = {};

  if (showWeather) {
    if (weatherTempC >= -9 && weatherTempC <= 99) {
      snprintf(tempLine, sizeof(tempLine), "%2dC", weatherTempC);
    } else {
      snprintf(tempLine, sizeof(tempLine), "--C");
    }
  }

  buildStatusDateTimeLine(&timeinfo, dateTimeLine, sizeof(dateTimeLine));
  if (showWeather) {
    (void)statusBarWeatherFits(statusBarTextWidth(dateTimeLine), statusBarTextWidth(tempLine),
                               battIconX, &weatherIconX, &weatherTempX);
  }

  drawText(image, dateTimeLine, dateX, barY);

  if (showWeather) {
    drawWeatherIcon(image, weatherIconX, weatherIconY, weatherIcon);
    drawText(image, tempLine, weatherTempX, barY);
  }

  const int battLevel = (batteryPercent < 0) ? 100 : batteryPercent;
  drawBatteryIcon(image, battIconX, barY + 2, battLevel);

  drawHorizontalLine(image, barLineY);
}

static void refreshMainUiOnDisplay(UiRefreshMode mode) {
  if (mode == UI_REFRESH_NONE) {
    return;
  }

  const bool fullInit = (mode == UI_REFRESH_FULL);
  const bool fastEpd = (mode == UI_REFRESH_FAST ||
                        mode == UI_REFRESH_PREVIEW ||
                        mode == UI_REFRESH_NAV);
  const bool fullLvgl = (mode != UI_REFRESH_FAST &&
                         mode != UI_REFRESH_PREVIEW);

  if (mode == UI_REFRESH_QUALITY || mode == UI_REFRESH_FULL) {
    if (ui_nav_is_weather()) {
      ui_weather_refresh();
    } else if (ui_nav_is_stock()) {
      ui_stock_refresh();
    } else if (ui_nav_is_clock()) {
      ui_clock_refresh();
    } else if (ui_nav_is_answers()) {
      ui_answers_refresh();
    } else if (ui_nav_is_home()) {
      ui_home_refresh_weather();
      ui_home_refresh_stocks();
      ui_home_refresh_clock();
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

  if (!epaper_upload_mode_async(fullInit, fastEpd)) {
    Serial.println("[Display] upload skipped: EPD busy");
    requestDisplayRefresh(mode);
    return;
  }
  lastDisplayUploadMs = millis();

  if (!fullLvgl || mode == UI_REFRESH_NAV) {
    return;
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
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

static int refreshModePriority(UiRefreshMode mode) {
  switch (mode) {
    case UI_REFRESH_FULL:
      return 4;
    case UI_REFRESH_QUALITY:
      return 3;
    case UI_REFRESH_NAV:
      return 2;
    case UI_REFRESH_PREVIEW:
    case UI_REFRESH_FAST:
      return 1;
    default:
      return 0;
  }
}

static UiRefreshMode strongerRefreshMode(UiRefreshMode a, UiRefreshMode b) {
  return refreshModePriority(b) > refreshModePriority(a) ? b : a;
}

static const char *refreshModeName(UiRefreshMode mode) {
  switch (mode) {
    case UI_REFRESH_PREVIEW:
      return "FAST";
    case UI_REFRESH_FAST:
      return "FAST";
    case UI_REFRESH_NAV:
      return "NAV";
    case UI_REFRESH_QUALITY:
      return "QUALITY";
    case UI_REFRESH_FULL:
      return "FULL";
    default:
      return "NONE";
  }
}

static void requestDisplayRefresh(UiRefreshMode mode) {
  if (mode == UI_REFRESH_NONE) {
    return;
  }

  const unsigned long now = millis();
  const bool previewRequest = (mode == UI_REFRESH_PREVIEW);
  if (!displayRefreshPending) {
    displayRefreshPending = true;
    pendingDisplayRefreshMode = mode;
    pendingDisplayRequestCount = 1;
    pendingDisplaySinceMs = now;
  } else if (previewRequest) {
    if (refreshModePriority(pendingDisplayRefreshMode) <= refreshModePriority(UI_REFRESH_PREVIEW)) {
      pendingDisplayRefreshMode = UI_REFRESH_PREVIEW;
      pendingDisplayRequestCount = 1;
    }
  } else {
    if (pendingDisplayRefreshMode == UI_REFRESH_PREVIEW && mode == UI_REFRESH_FAST) {
      pendingDisplayRefreshMode = UI_REFRESH_FAST;
      pendingDisplayRequestCount = 1;
      return;
    }
    pendingDisplayRequestCount++;
    pendingDisplayRefreshMode = strongerRefreshMode(pendingDisplayRefreshMode, mode);
    if (pendingDisplayRefreshMode == UI_REFRESH_FAST && pendingDisplayRequestCount > 1) {
      pendingDisplayRefreshMode = UI_REFRESH_NAV;
    }
  }
}

static void serviceDisplayRefresh(bool force) {
  if (!serviceDisplayBootState()) {
    return;
  }

  if (epaper_upload_active()) {
    (void)epaper_poll_upload();
    return;
  }

  if (!displayRefreshPending) {
    return;
  }

  unsigned long now = millis();
  if (!force && now - pendingDisplaySinceMs < DISPLAY_COALESCE_MS) {
    return;
  }

  if (lastDisplayUploadMs != 0 && now - lastDisplayUploadMs < DISPLAY_UPLOAD_GUARD_MS) {
    if (!force) {
      return;
    }
  }

  const UiRefreshMode mode = pendingDisplayRefreshMode;
  const uint8_t count = pendingDisplayRequestCount;
  displayRefreshPending = false;
  pendingDisplayRefreshMode = UI_REFRESH_NONE;
  pendingDisplayRequestCount = 0;

  Serial.printf("[Display] flush %s (%u request%s)\n",
                refreshModeName(mode), count, count == 1 ? "" : "s");
  refreshMainUiOnDisplay(mode);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Aink ===");
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(4096);
    Serial.printf("[Heap] PSRAM enabled, malloc>=4096 to extmem heap=%u psram=%u block=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  } else {
    Serial.println("[Heap] PSRAM not found");
  }

  DEV_Module_Init();

  if (settings_api_consume_force_portal_boot()) {
    Serial.println("[WiFi] Entering config portal (settings request)");
    enterPortalMode();
  } else if (hasStoredWiFiCredentials()) {
    Serial.println("[WiFi] Stored credentials found; showing UI before network bootstrap");
    startNormalOperation();
  } else {
    Serial.println("[WiFi] No stored credentials; entering config portal");
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

  btn_input_update();
  btn_input_serial_poll();
  ui_lvgl_tick();

  BtnAction btnAction = BTN_ACTION_NONE;
  while (btn_input_consume(&btnAction)) {
    lastUserInputMs = millis();
    UiRefreshMode navMode = UI_REFRESH_NONE;
    if (ui_nav_handle(btnAction, &navMode)) {
      requestDisplayRefresh(navMode);
      if (ui_vision_consume_capture_request()) {
        serviceDisplayRefresh(true);
        Serial.println("[Vision] capture pipeline starting (async)");
        Serial.flush();
        (void)ui_vision_run_capture();
        requestDisplayRefresh(UI_REFRESH_NAV);
      }
    }
  }

  UiRefreshMode visionMode = UI_REFRESH_NONE;
  if (ui_vision_service(&visionMode)) {
    requestDisplayRefresh(visionMode);
  }

  UiRefreshMode answersMode = UI_REFRESH_NONE;
  if (ui_answers_service(&answersMode)) {
    requestDisplayRefresh(answersMode);
  }

  UiRefreshMode voiceMode = UI_REFRESH_NONE;
  if (ui_voice_service(&voiceMode)) {
    requestDisplayRefresh(voiceMode);
  }

  const bool wifiConnected = isWifiConnected();
  const int wifiState = wifiConnected ? 1 : 0;
  WeatherSnapshot weatherSnap = {};
  weather_service_get_snapshot(&weatherSnap);
  const int weatherIconState = weatherSnap.valid ? (int)weatherSnap.icon : -1;
  const int weatherTempState = weatherSnap.valid ? weatherSnap.tempC : -999;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    const int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    const bool weatherChanged =
        weatherIconState != lastWeatherIcon || weatherTempState != lastWeatherTemp;
    if (currentMinute != lastDisplayedMinute ||
        wifiState != lastWifiState ||
        weatherChanged) {
      requestDisplayRefresh(UI_REFRESH_QUALITY);
    }
  } else {
    if (wifiConnected) {
      networkTimeSyncRequested = true;
    }
    if (wifiState != lastWifiState) {
      requestDisplayRefresh(UI_REFRESH_QUALITY);
    }
  }

  const bool voiceBusy = voice_service_is_busy();
  const VoiceState voiceState = voice_service_state();
  if (!voiceBusy || voiceState == VOICE_STATE_RECORDING) {
    serviceDisplayRefresh(false);
  }
  const bool inputIdle = lastUserInputMs == 0 ||
                         (millis() - lastUserInputMs) >= NETWORK_IDLE_AFTER_INPUT_MS;
  const bool visionIdle = !ui_vision_is_busy();
  const bool answersIdle = !ui_answers_is_busy();
  const bool voiceIdle = !voiceBusy;
  serviceNetworkStateMachine(displayBootState == DISPLAY_BOOT_READY &&
                             !displayRefreshPending &&
                             !epaper_upload_active() &&
                             inputIdle &&
                             visionIdle &&
                             answersIdle &&
                             voiceIdle);
  serviceStockNameRetry(wifiConnected, inputIdle && visionIdle && answersIdle && voiceIdle);
  delay(50);
}
