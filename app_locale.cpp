#include "app_locale.h"

#include "settings_api.h"
#include "stock_service.h"
#include "ui_home.h"
#include "ui_settings.h"
#include "ui_stock.h"
#include "ui_weather.h"
#include "ui_vision.h"

static AppLanguage s_lang = APP_LANG_EN;

static const char *kStringsEn[TR_COUNT] = {
    "Weather",
    "AI Vision",
    "Stocks",
    "Settings",
    "Online",
    "Offline",
    "Detail",
    "Coming soon",
    "App",

    "AI Vision",
    "Press A to capture",
    "Analyzing...",
    "Camera unavailable",
    "Set API in Settings",
    "Check WiFi",
    "Request failed",
    "Provider unsupported",

    "Settings",
    "WiFi",
    "Model",
    "Display",
    "About",
    "Network",
    "Reconfigure WiFi",
    "Forget WiFi",
    "Provider",
    "Model",
    "API: OK",
    "API: --",
    "Configure API",
    "Clear API Key",
    "Weather: OK",
    "Weather: --",
    "Configure Weather",
    "Clear Weather API",
    "Panel: 200x200",
    "Refresh: auto",
    "Version 0.1",
    "Language",
    "English",
    "中文",

    "No data",
    "Check WiFi",
    "Feels %dC",
    "Hum %d%%",
    "Hum --",
    "Wind %dkm/h",
    "Wind --",
    "UV --",
    "UV%d %s",
    "UV %d.%d",
    "AQI %d %s",
    "AQI --",

    "Stocks",
    "No quotes yet",
    "Updated %02d:%02d",
    "Stocks: custom",
    "Stocks: default",
    "Configure Stocks",
    "Clear Watchlist",
    "Name",
    "Price",
    "Chg",

    "SUN",
    "MON",
    "TUE",
    "WED",
    "THU",
    "FRI",
    "SAT",
};

static const char *kStringsZh[TR_COUNT] = {
    "天气",
    "AI识图",
    "股票",
    "设置",
    "在线",
    "离线",
    "详情",
    "敬请期待",
    "应用",

    "AI识图",
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
    "天气: 已配置",
    "天气: 未配置",
    "配置天气 API",
    "清除天气 API",
    "屏幕: 200x200",
    "刷新: 自动",
    "版本 0.1",
    "语言",
    "English",
    "中文",

    "暂无数据",
    "请检查网络",
    "体感 %dC",
    "湿度 %d%%",
    "湿度 --",
    "风速 %dkm/h",
    "风速 --",
    "UV --",
    "UV%d %s",
    "UV %d.%d",
    "AQI %d %s",
    "AQI --",

    "股票",
    "暂无行情",
    "更新 %02d:%02d",
    "自选: 已配置",
    "自选: 默认",
    "配置自选股",
    "清除自选",
    "名称",
    "现价",
    "涨跌",

    "周日",
    "周一",
    "周二",
    "周三",
    "周四",
    "周五",
    "周六",
};

void app_locale_init(void) {
  s_lang = settings_api_get_language();
}

AppLanguage app_locale_get(void) {
  return s_lang;
}

const char *app_tr(AppStrId id) {
  if ((unsigned)id >= TR_COUNT) {
    return "";
  }
  return s_lang == APP_LANG_ZH ? kStringsZh[id] : kStringsEn[id];
}

const char *app_tr_weekday(int tmWday) {
  if (tmWday < 0 || tmWday > 6) {
    return "--";
  }
  return app_tr((AppStrId)(TR_WEEKDAY_SUN + tmWday));
}

void app_locale_set(AppLanguage lang) {
  if (lang != APP_LANG_EN && lang != APP_LANG_ZH) {
    return;
  }
  if (s_lang == lang) {
    return;
  }
  s_lang = lang;
  settings_api_set_language(lang);
  app_locale_refresh_all();
}

void app_locale_toggle(void) {
  app_locale_set(s_lang == APP_LANG_EN ? APP_LANG_ZH : APP_LANG_EN);
}

void app_locale_refresh_all(void) {
  stock_service_on_locale_changed();
  ui_home_refresh_locale();
  ui_settings_refresh();
  ui_weather_refresh();
  ui_stock_refresh();
  ui_vision_refresh();
}
