#include "gbk_utf8.h"
#include "stock_service.h"

#include "app_locale.h"
#include "settings_api.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOCK_FETCH_ACTIVE_MS   (5UL * 60UL * 1000)
#define STOCK_FETCH_IDLE_MS     (30UL * 60UL * 1000)
#define STOCK_RETRY_INTERVAL_MS (5UL * 60UL * 1000)
#define STOCK_HTTP_TIMEOUT_MS   15000
#define STOCK_PREFS_NAMESPACE   "epaper"
#define STOCK_PREFS_NAMES_KEY   "stk_names"

typedef enum {
  STOCK_MARKET_CN = 0,
  STOCK_MARKET_US = 1,
} StockMarketKind;

typedef struct {
  char userSymbol[STOCK_SYMBOL_LEN];
  char sinaCode[16];
  StockMarketKind market;
} StockRequestItem;

static StockSnapshot s_snapshot = {};
static unsigned long s_lastFetchMs = 0;
static unsigned long s_lastAttemptMs = 0;
static bool s_freshFetchPending = false;

typedef struct {
  char symbol[STOCK_SYMBOL_LEN];
  char name[STOCK_NAME_LEN];
} StockNameCacheEntry;

static StockNameCacheEntry s_nameCache[STOCK_MAX_ITEMS];
static int s_nameCacheCount = 0;
static char s_nameCacheWatchlistKey[128] = "";
static bool s_nameCacheComplete = false;

static bool isCnSymbol(const char *symbol) {
  if (symbol == nullptr) {
    return false;
  }
  const size_t len = strlen(symbol);
  if (len != 8) {
    return false;
  }
  if ((strncmp(symbol, "sh", 2) != 0 && strncmp(symbol, "sz", 2) != 0) ||
      !isdigit((unsigned char)symbol[2])) {
    return false;
  }
  for (size_t i = 2; i < len; i++) {
    if (!isdigit((unsigned char)symbol[i])) {
      return false;
    }
  }
  return true;
}

static bool isUsSymbol(const char *symbol) {
  if (symbol == nullptr || symbol[0] == '\0') {
    return false;
  }
  size_t len = strlen(symbol);
  if (len < 1 || len > 5) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    if (!isalpha((unsigned char)symbol[i])) {
      return false;
    }
  }
  return true;
}

static void normalizeUserSymbol(const char *raw, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  if (raw == nullptr) {
    return;
  }

  size_t start = 0;
  while (raw[start] == ' ' || raw[start] == '\t') {
    start++;
  }

  size_t end = strlen(raw);
  while (end > start && (raw[end - 1] == ' ' || raw[end - 1] == '\t')) {
    end--;
  }

  char token[STOCK_SYMBOL_LEN];
  size_t len = end - start;
  if (len >= sizeof(token)) {
    len = sizeof(token) - 1;
  }
  memcpy(token, raw + start, len);
  token[len] = '\0';

  if (isCnSymbol(token)) {
    snprintf(out, outLen, "%c%c%s", (char)tolower((unsigned char)token[0]),
             (char)tolower((unsigned char)token[1]), token + 2);
    return;
  }

  for (size_t i = 0; i < len && i + 1 < outLen; i++) {
    out[i] = (char)toupper((unsigned char)token[i]);
  }
  out[len < outLen ? len : outLen - 1] = '\0';
}

static bool toSinaCode(const char *userSymbol, char *out, size_t outLen, StockMarketKind *outMarket) {
  if (userSymbol == nullptr || out == nullptr || outLen == 0 || outMarket == nullptr) {
    return false;
  }

  if (isCnSymbol(userSymbol)) {
    snprintf(out, outLen, "%s", userSymbol);
    *outMarket = STOCK_MARKET_CN;
    return true;
  }

  if (isUsSymbol(userSymbol)) {
    char lower[8] = {};
    for (size_t i = 0; userSymbol[i] != '\0' && i + 1 < sizeof(lower); i++) {
      lower[i] = (char)tolower((unsigned char)userSymbol[i]);
    }
    snprintf(out, outLen, "gb_%s", lower);
    *outMarket = STOCK_MARKET_US;
    return true;
  }

  return false;
}

static int parseWatchlist(StockRequestItem *items, int maxItems) {
  char watchlist[128];
  settings_api_get_watchlist(watchlist, sizeof(watchlist));

  int count = 0;
  char *savePtr = nullptr;
  char *token = strtok_r(watchlist, ",", &savePtr);
  while (token != nullptr && count < maxItems) {
    char normalized[STOCK_SYMBOL_LEN];
    normalizeUserSymbol(token, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
      token = strtok_r(nullptr, ",", &savePtr);
      continue;
    }

    StockMarketKind market = STOCK_MARKET_CN;
    char sinaCode[16];
    if (!toSinaCode(normalized, sinaCode, sizeof(sinaCode), &market)) {
      token = strtok_r(nullptr, ",", &savePtr);
      continue;
    }

    snprintf(items[count].userSymbol, sizeof(items[count].userSymbol), "%s", normalized);
    snprintf(items[count].sinaCode, sizeof(items[count].sinaCode), "%s", sinaCode);
    items[count].market = market;
    count++;
    token = strtok_r(nullptr, ",", &savePtr);
  }
  return count;
}

static bool isCnMarketOpen(const struct tm *timeInfo) {
  if (timeInfo == nullptr) {
    return false;
  }
  if (timeInfo->tm_wday == 0 || timeInfo->tm_wday == 6) {
    return false;
  }
  const int mins = timeInfo->tm_hour * 60 + timeInfo->tm_min;
  return mins >= (9 * 60 + 15) && mins <= (15 * 60);
}

static bool isUsMarketOpen(const struct tm *timeInfo) {
  if (timeInfo == nullptr) {
    return false;
  }
  if (timeInfo->tm_wday == 0 || timeInfo->tm_wday == 6) {
    return false;
  }
  const int mins = timeInfo->tm_hour * 60 + timeInfo->tm_min;
  /* US regular session ~ Beijing 22:30-05:00 (approximate, no DST). */
  return mins >= (22 * 60 + 30) || mins < (5 * 60);
}

static bool shouldUseActiveRefresh(const StockRequestItem *items, int count) {
  struct tm timeInfo = {};
  if (!getLocalTime(&timeInfo)) {
    return true;
  }

  bool hasCn = false;
  bool hasUs = false;
  for (int i = 0; i < count; i++) {
    if (items[i].market == STOCK_MARKET_CN) {
      hasCn = true;
    } else {
      hasUs = true;
    }
  }

  if (hasCn && isCnMarketOpen(&timeInfo)) {
    return true;
  }
  if (hasUs && isUsMarketOpen(&timeInfo)) {
    return true;
  }
  return false;
}

static int priceStringToX100(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return -1;
  }
  const double value = atof(text);
  if (value <= 0.0) {
    return -1;
  }
  return (int)(value * 100.0 + 0.5);
}

static int changePctToX10(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }
  const double value = atof(text);
  return (int)(value * 10.0 + (value >= 0.0 ? 0.5 : -0.5));
}

static const char *findField(const char *line, int index) {
  if (line == nullptr || index < 0) {
    return nullptr;
  }

  const char *cursor = line;
  for (int i = 0; i < index; i++) {
    cursor = strchr(cursor, ',');
    if (cursor == nullptr) {
      return nullptr;
    }
    cursor++;
  }
  return cursor;
}

static bool parseFieldCopy(const char *line, int index, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  const char *start = findField(line, index);
  if (start == nullptr) {
    return false;
  }

  const char *end = strchr(start, ',');
  size_t len = end != nullptr ? (size_t)(end - start) : strlen(start);
  if (len >= outLen) {
    len = outLen - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
  return out[0] != '\0';
}

static bool parseSinaQuoteLine(const char *sinaCode, const char *payload, StockMarketKind market,
                               StockQuote *outQuote) {
  if (sinaCode == nullptr || payload == nullptr || outQuote == nullptr) {
    return false;
  }

  char nameField[STOCK_NAME_LEN];
  if (!parseFieldCopy(payload, 0, nameField, sizeof(nameField))) {
    return false;
  }

  if (market == STOCK_MARKET_US) {
    char priceField[16];
    char changeField[16];
    if (!parseFieldCopy(payload, 1, priceField, sizeof(priceField)) ||
        !parseFieldCopy(payload, 2, changeField, sizeof(changeField))) {
      return false;
    }
    const int priceX100 = priceStringToX100(priceField);
    if (priceX100 < 0) {
      return false;
    }
    outQuote->priceX100 = priceX100;
    outQuote->changePctX10 = changePctToX10(changeField);
  } else {
    char prevField[16];
    char priceField[16];
    if (!parseFieldCopy(payload, 2, prevField, sizeof(prevField)) ||
        !parseFieldCopy(payload, 3, priceField, sizeof(priceField))) {
      return false;
    }
    const int priceX100 = priceStringToX100(priceField);
    const int prevX100 = priceStringToX100(prevField);
    if (priceX100 < 0 || prevX100 <= 0) {
      return false;
    }
    outQuote->priceX100 = priceX100;
    const int diffX100 = priceX100 - prevX100;
    outQuote->changePctX10 =
        (int)((diffX100 * 1000LL + (diffX100 >= 0 ? prevX100 / 2 : -(prevX100 / 2))) / prevX100);
  }

  char nameUtf8[STOCK_NAME_LEN];
  if (gbk_to_utf8(nameField, nameUtf8, sizeof(nameUtf8)) > 0 && nameUtf8[0] != '\0') {
    snprintf(outQuote->name, sizeof(outQuote->name), "%s", nameUtf8);
  } else if (nameField[0] != '\0') {
    snprintf(outQuote->name, sizeof(outQuote->name), "%s", nameField);
  }

  outQuote->quoteValid = true;
  return true;
}

static void buildWatchlistKey(const StockRequestItem *items, int count, char *out, size_t outLen) {
  if (items == nullptr || out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  size_t used = 0;
  for (int i = 0; i < count; i++) {
    const char *sym = items[i].userSymbol;
    const size_t symLen = strlen(sym);
    if (symLen == 0) {
      continue;
    }
    if (used > 0) {
      if (used + 1 >= outLen) {
        return;
      }
      out[used++] = ',';
      out[used] = '\0';
    }
    if (used + symLen >= outLen) {
      return;
    }
    memcpy(out + used, sym, symLen);
    used += symLen;
    out[used] = '\0';
  }
}

static const char *lookupCachedName(const char *symbol) {
  if (symbol == nullptr) {
    return nullptr;
  }
  for (int i = 0; i < s_nameCacheCount; i++) {
    if (strcmp(s_nameCache[i].symbol, symbol) == 0) {
      return s_nameCache[i].name;
    }
  }
  return nullptr;
}

static bool quoteHasDisplayName(const StockQuote *quote) {
  if (quote == nullptr || quote->name[0] == '\0') {
    return false;
  }
  return strcmp(quote->name, quote->symbol) != 0;
}

static void applyNameCache(StockSnapshot *snap) {
  if (snap == nullptr) {
    return;
  }
  for (int i = 0; i < snap->count; i++) {
    StockQuote *quote = &snap->items[i];
    const char *cached = lookupCachedName(quote->symbol);
    if (cached != nullptr && cached[0] != '\0') {
      snprintf(quote->name, sizeof(quote->name), "%s", cached);
    }
  }
}

static void upsertNameCache(const char *symbol, const char *name) {
  if (symbol == nullptr || name == nullptr || name[0] == '\0' ||
      strcmp(name, symbol) == 0) {
    return;
  }

  for (int i = 0; i < s_nameCacheCount; i++) {
    if (strcmp(s_nameCache[i].symbol, symbol) == 0) {
      snprintf(s_nameCache[i].name, sizeof(s_nameCache[i].name), "%s", name);
      return;
    }
  }

  if (s_nameCacheCount >= STOCK_MAX_ITEMS) {
    return;
  }

  snprintf(s_nameCache[s_nameCacheCount].symbol, sizeof(s_nameCache[s_nameCacheCount].symbol), "%s",
           symbol);
  snprintf(s_nameCache[s_nameCacheCount].name, sizeof(s_nameCache[s_nameCacheCount].name), "%s",
           name);
  s_nameCacheCount++;
}

static bool watchlistHasCachedNames(const StockRequestItem *items, int count) {
  if (items == nullptr || count <= 0) {
    return false;
  }

  char key[128];
  buildWatchlistKey(items, count, key, sizeof(key));
  if (key[0] == '\0' || strcmp(key, s_nameCacheWatchlistKey) != 0) {
    return false;
  }

  for (int i = 0; i < count; i++) {
    const char *cached = lookupCachedName(items[i].userSymbol);
    if (cached == nullptr || cached[0] == '\0' || strcmp(cached, items[i].userSymbol) == 0) {
      return false;
    }
  }
  return true;
}

static void clearNameCache(void) {
  s_nameCacheCount = 0;
  s_nameCacheWatchlistKey[0] = '\0';
  s_nameCacheComplete = false;
}

static void clearNameCacheNvs(void) {
  Preferences prefs;
  prefs.begin(STOCK_PREFS_NAMESPACE, false);
  prefs.remove(STOCK_PREFS_NAMES_KEY);
  prefs.end();
}

static int countDisplayNames(const StockSnapshot *snap) {
  if (snap == nullptr) {
    return 0;
  }
  int named = 0;
  for (int i = 0; i < snap->count; i++) {
    if (quoteHasDisplayName(&snap->items[i])) {
      named++;
    }
  }
  return named;
}

static void mergeNamesFromPriorSnapshot(StockSnapshot *snap) {
  if (snap == nullptr || !s_snapshot.valid) {
    return;
  }
  for (int i = 0; i < snap->count; i++) {
    StockQuote *quote = &snap->items[i];
    if (quoteHasDisplayName(quote)) {
      continue;
    }
    for (int j = 0; j < s_snapshot.count; j++) {
      const StockQuote *prior = &s_snapshot.items[j];
      if (strcmp(prior->symbol, quote->symbol) != 0 || !quoteHasDisplayName(prior)) {
        continue;
      }
      snprintf(quote->name, sizeof(quote->name), "%s", prior->name);
      upsertNameCache(quote->symbol, quote->name);
      break;
    }
  }
}

static void saveNameCacheToNvs(void) {
  if (s_nameCacheWatchlistKey[0] == '\0' || s_nameCacheCount <= 0) {
    return;
  }

  char buf[320];
  size_t used = snprintf(buf, sizeof(buf), "%s|", s_nameCacheWatchlistKey);
  for (int i = 0; i < s_nameCacheCount && used + 2 < sizeof(buf); i++) {
    if (i > 0) {
      buf[used++] = ',';
      buf[used] = '\0';
    }
    const int written = snprintf(buf + used, sizeof(buf) - used, "%s\t%s",
                                 s_nameCache[i].symbol, s_nameCache[i].name);
    if (written <= 0 || (size_t)written >= sizeof(buf) - used) {
      return;
    }
    used += (size_t)written;
  }

  Preferences prefs;
  prefs.begin(STOCK_PREFS_NAMESPACE, false);
  prefs.putString(STOCK_PREFS_NAMES_KEY, buf);
  prefs.end();
}

static void loadNameCacheFromNvs(const char *watchlistKey, const StockRequestItem *items, int count) {
  if (watchlistKey == nullptr || watchlistKey[0] == '\0' || items == nullptr || count <= 0 ||
      s_nameCacheCount > 0) {
    return;
  }

  Preferences prefs;
  prefs.begin(STOCK_PREFS_NAMESPACE, true);
  const String stored = prefs.getString(STOCK_PREFS_NAMES_KEY, "");
  prefs.end();
  if (stored.length() == 0) {
    return;
  }

  const int sep = stored.indexOf('|');
  if (sep <= 0) {
    return;
  }

  const String storedKey = stored.substring(0, sep);
  if (!storedKey.equals(watchlistKey)) {
    return;
  }

  String entries = stored.substring(sep + 1);
  while (entries.length() > 0) {
    const int comma = entries.indexOf(',');
    const String entry = comma >= 0 ? entries.substring(0, comma) : entries;
    const int tab = entry.indexOf('\t');
    if (tab > 0) {
      String symbol = entry.substring(0, tab);
      String name = entry.substring(tab + 1);
      symbol.trim();
      name.trim();
      if (symbol.length() > 0 && name.length() > 0) {
        upsertNameCache(symbol.c_str(), name.c_str());
      }
    }
    if (comma < 0) {
      break;
    }
    entries = entries.substring(comma + 1);
  }

  if (s_nameCacheCount <= 0) {
    return;
  }

  snprintf(s_nameCacheWatchlistKey, sizeof(s_nameCacheWatchlistKey), "%s", watchlistKey);
  s_nameCacheComplete = watchlistHasCachedNames(items, count);
}

static void syncNameCacheFromSnapshot(const StockRequestItem *items, int count,
                                      const StockSnapshot *snap) {
  if (items == nullptr || snap == nullptr || count <= 0) {
    return;
  }

  buildWatchlistKey(items, count, s_nameCacheWatchlistKey, sizeof(s_nameCacheWatchlistKey));
  for (int i = 0; i < snap->count; i++) {
    const StockQuote *quote = &snap->items[i];
    if (quoteHasDisplayName(quote)) {
      upsertNameCache(quote->symbol, quote->name);
    }
  }
  s_nameCacheComplete = watchlistHasCachedNames(items, count);
  if (s_nameCacheCount > 0) {
    saveNameCacheToNvs();
  }
}

static void applyLocalNames(const StockRequestItem *items, int count, StockSnapshot *snap) {
  if (items == nullptr || snap == nullptr || count <= 0) {
    return;
  }

  char watchlistKey[128];
  buildWatchlistKey(items, count, watchlistKey, sizeof(watchlistKey));
  applyNameCache(snap);
  mergeNamesFromPriorSnapshot(snap);
  if (watchlistKey[0] != '\0') {
    loadNameCacheFromNvs(watchlistKey, items, count);
    applyNameCache(snap);
    mergeNamesFromPriorSnapshot(snap);
  }
}

static void enrichNamesIfNeeded(const StockRequestItem *items, int count, StockSnapshot *snap) {
  if (items == nullptr || snap == nullptr || count <= 0) {
    return;
  }

  applyLocalNames(items, count, snap);
  syncNameCacheFromSnapshot(items, count, snap);

  for (int i = 0; i < snap->count; i++) {
    const StockQuote *quote = &snap->items[i];
    if (quoteHasDisplayName(quote)) {
      Serial.printf("[Stock] %s -> %s\r\n", quote->symbol, quote->name);
    }
  }
}

static bool buildSinaListParam(const StockRequestItem *items, int count, char *out, size_t outLen) {
  if (items == nullptr || out == nullptr || outLen == 0 || count <= 0) {
    return false;
  }

  out[0] = '\0';
  size_t used = 0;
  for (int i = 0; i < count; i++) {
    const char *part = items[i].sinaCode;
    const size_t partLen = strlen(part);
    if (partLen == 0) {
      continue;
    }
    if (used > 0) {
      if (used + 1 >= outLen) {
        return false;
      }
      out[used++] = ',';
      out[used] = '\0';
    }
    if (used + partLen >= outLen) {
      return false;
    }
    memcpy(out + used, part, partLen);
    used += partLen;
    out[used] = '\0';
  }
  return used > 0;
}

static bool httpGetSinaQuotes(const char *listParam, String *outBody) {
  if (listParam == nullptr || outBody == nullptr || listParam[0] == '\0') {
    return false;
  }

  char url[192];
  snprintf(url, sizeof(url), "http://hq.sinajs.cn/list=%s", listParam);

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(STOCK_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("[Stock] http begin failed");
    return false;
  }
  http.addHeader("Referer", "https://finance.sina.com.cn");

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[Stock] HTTP %d for %s\r\n", httpCode, listParam);
    http.end();
    return false;
  }

  *outBody = http.getString();
  http.end();
  return outBody->length() > 0;
}

static bool parseSinaResponse(const StockRequestItem *items, int count, const String &body,
                              StockSnapshot *outSnap) {
  if (items == nullptr || outSnap == nullptr || count <= 0) {
    return false;
  }

  outSnap->count = count;
  outSnap->valid = false;

  for (int i = 0; i < count; i++) {
    StockQuote *quote = &outSnap->items[i];
    snprintf(quote->symbol, sizeof(quote->symbol), "%s", items[i].userSymbol);
    snprintf(quote->name, sizeof(quote->name), "%s", items[i].userSymbol);
    quote->priceX100 = 0;
    quote->changePctX10 = 0;
    quote->quoteValid = false;

    char marker[32];
    snprintf(marker, sizeof(marker), "hq_str_%s=\"", items[i].sinaCode);
    int markerPos = body.indexOf(marker);
    if (markerPos < 0) {
      continue;
    }

    const int payloadStart = markerPos + (int)strlen(marker);
    const int payloadEnd = body.indexOf('"', payloadStart);
    if (payloadEnd <= payloadStart) {
      continue;
    }

    String payload = body.substring(payloadStart, payloadEnd);
    if (payload.length() == 0) {
      continue;
    }

    if (parseSinaQuoteLine(items[i].sinaCode, payload.c_str(), items[i].market, quote)) {
      outSnap->valid = true;
    }
  }

  if (outSnap->valid) {
    struct tm timeInfo = {};
    if (getLocalTime(&timeInfo)) {
      outSnap->fetchedEpoch = mktime(&timeInfo);
    } else {
      outSnap->fetchedEpoch = 0;
    }
  }
  return outSnap->valid;
}

static bool fetchStocks(StockSnapshot *outSnap) {
  if (outSnap == nullptr) {
    return false;
  }

  StockRequestItem items[STOCK_MAX_ITEMS];
  const int count = parseWatchlist(items, STOCK_MAX_ITEMS);
  if (count <= 0) {
    Serial.println("[Stock] watchlist empty");
    return false;
  }

  char listParam[96];
  if (!buildSinaListParam(items, count, listParam, sizeof(listParam))) {
    return false;
  }

  String body;
  if (!httpGetSinaQuotes(listParam, &body)) {
    return false;
  }

  parseSinaResponse(items, count, body, outSnap);
  body.clear();
  body = String();
  yield();

  if (outSnap->count <= 0) {
    return false;
  }
  if (!outSnap->valid) {
    Serial.println("[Stock] no valid quotes");
    return false;
  }

  enrichNamesIfNeeded(items, count, outSnap);

  for (int i = 0; i < outSnap->count; i++) {
    const StockQuote *q = &outSnap->items[i];
    if (!q->quoteValid) {
      Serial.printf("[Stock] %s --\r\n", q->symbol);
      continue;
    }
    Serial.printf("[Stock] %s price=%d.%02d chg=%d.%d%%\r\n",
                  q->symbol, q->priceX100 / 100, q->priceX100 % 100,
                  q->changePctX10 / 10, abs(q->changePctX10 % 10));
  }
  return true;
}

void stock_service_invalidate_name_cache(void) {
  clearNameCache();
  clearNameCacheNvs();
}

void stock_service_reset(void) {
  s_snapshot = {};
  s_lastFetchMs = 0;
  s_lastAttemptMs = 0;
  s_freshFetchPending = false;
  clearNameCache();
}

bool stock_service_consume_fresh_fetch(void) {
  if (!s_freshFetchPending) {
    return false;
  }
  s_freshFetchPending = false;
  return true;
}

void stock_service_get_snapshot(StockSnapshot *out) {
  if (out == nullptr) {
    return;
  }
  *out = s_snapshot;
}

const StockQuote *stock_service_get_tile_preview(void) {
  if (!s_snapshot.valid || s_snapshot.count <= 0) {
    return nullptr;
  }

  const StockQuote *best = nullptr;
  int bestAbs = -1;
  for (int i = 0; i < s_snapshot.count; i++) {
    const StockQuote *q = &s_snapshot.items[i];
    if (!q->quoteValid) {
      continue;
    }
    const int absChange = abs(q->changePctX10);
    if (absChange > bestAbs) {
      bestAbs = absChange;
      best = q;
    }
  }
  if (best != nullptr) {
    return best;
  }
  for (int i = 0; i < s_snapshot.count; i++) {
    if (s_snapshot.items[i].quoteValid) {
      return &s_snapshot.items[i];
    }
  }
  return nullptr;
}

bool stock_service_is_cn_symbol(const char *symbol) {
  if (symbol == nullptr) {
    return false;
  }
  return strncmp(symbol, "sh", 2) == 0 || strncmp(symbol, "sz", 2) == 0;
}

static void formatPriceAmount(int priceX100, char *out, size_t outLen) {
  if (priceX100 < 0) {
    snprintf(out, outLen, "--");
    return;
  }
  const int whole = priceX100 / 100;
  const int frac = priceX100 % 100;
  if (frac == 0) {
    snprintf(out, outLen, "%d", whole);
  } else if ((frac % 10) == 0) {
    snprintf(out, outLen, "%d.%d", whole, frac / 10);
  } else {
    snprintf(out, outLen, "%d.%02d", whole, frac);
  }
}

void stock_service_format_price(const StockQuote *quote, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  if (quote == nullptr || !quote->quoteValid || quote->priceX100 < 0) {
    snprintf(out, outLen, "--");
    return;
  }

  char amount[16];
  formatPriceAmount(quote->priceX100, amount, sizeof(amount));
  if (strcmp(amount, "--") == 0) {
    snprintf(out, outLen, "--");
    return;
  }

  if (stock_service_is_cn_symbol(quote->symbol)) {
    snprintf(out, outLen, "\xEF\xBF\xA5%s", amount);
  } else {
    snprintf(out, outLen, "$%s", amount);
  }
}

void stock_service_format_display_label(const StockQuote *quote, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';
  if (quote == nullptr) {
    snprintf(out, outLen, "--");
    return;
  }

  if (quote->name[0] != '\0' && strcmp(quote->name, quote->symbol) != 0) {
    snprintf(out, outLen, "%s", quote->name);
    return;
  }

  if (app_locale_get() == APP_LANG_ZH) {
    snprintf(out, outLen, "%s", quote->symbol);
    return;
  }

  if (strncmp(quote->symbol, "sh", 2) == 0 && strlen(quote->symbol) == 8) {
    snprintf(out, outLen, "%s SH", quote->symbol + 2);
  } else if (strncmp(quote->symbol, "sz", 2) == 0 && strlen(quote->symbol) == 8) {
    snprintf(out, outLen, "%s SZ", quote->symbol + 2);
  } else {
    snprintf(out, outLen, "%s", quote->symbol);
  }
}

void stock_service_on_locale_changed(void) {
  if (!s_snapshot.valid || s_snapshot.count <= 0) {
    return;
  }

  StockRequestItem items[STOCK_MAX_ITEMS];
  const int count = parseWatchlist(items, STOCK_MAX_ITEMS);
  if (count <= 0) {
    return;
  }

  enrichNamesIfNeeded(items, count, &s_snapshot);
}

void stock_service_update(bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  StockRequestItem items[STOCK_MAX_ITEMS];
  const int count = parseWatchlist(items, STOCK_MAX_ITEMS);
  if (count <= 0) {
    return;
  }

  const unsigned long now = millis();
  const unsigned long interval =
      shouldUseActiveRefresh(items, count) ? STOCK_FETCH_ACTIVE_MS : STOCK_FETCH_IDLE_MS;

  if (!force && s_snapshot.valid && (now - s_lastFetchMs) < interval) {
    return;
  }
  if (!force && !s_snapshot.valid && s_lastAttemptMs != 0 &&
      (now - s_lastAttemptMs) < STOCK_RETRY_INTERVAL_MS) {
    return;
  }

  s_lastAttemptMs = now;
  StockSnapshot fresh = {};
  if (fetchStocks(&fresh)) {
    s_snapshot = fresh;
    s_lastFetchMs = now;
    s_freshFetchPending = true;
    Serial.println("[Stock] fetch complete");
  }
}

void stock_service_update_prices(bool force) {
  stock_service_update(force);
}

bool stock_service_needs_name_fetch(void) {
  if (!s_snapshot.valid) {
    return false;
  }

  StockRequestItem items[STOCK_MAX_ITEMS];
  const int count = parseWatchlist(items, STOCK_MAX_ITEMS);
  if (count <= 0) {
    return false;
  }

  return countDisplayNames(&s_snapshot) < count;
}

bool stock_service_retry_names(void) {
  if (WiFi.status() != WL_CONNECTED || !s_snapshot.valid) {
    return false;
  }

  StockRequestItem items[STOCK_MAX_ITEMS];
  const int count = parseWatchlist(items, STOCK_MAX_ITEMS);
  if (count <= 0) {
    return false;
  }

  const int before = countDisplayNames(&s_snapshot);
  enrichNamesIfNeeded(items, count, &s_snapshot);
  if (countDisplayNames(&s_snapshot) <= before) {
    return false;
  }

  s_freshFetchPending = true;
  return true;
}

void stock_service_request_name_fetch(void) {
}
