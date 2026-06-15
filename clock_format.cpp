#include "clock_format.h"

#include "app_locale.h"

#include <stdio.h>

void clock_format_hm(char *out, size_t outLen, const struct tm *timeinfo, bool use24h) {
  if (out == nullptr || outLen == 0 || timeinfo == nullptr) {
    return;
  }

  if (use24h) {
    snprintf(out, outLen, "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    return;
  }

  int hour12 = timeinfo->tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char *suffix = (timeinfo->tm_hour < 12) ? "AM" : "PM";
  snprintf(out, outLen, "%d:%02d %s", hour12, timeinfo->tm_min, suffix);
}

void clock_format_date(char *out, size_t outLen, const struct tm *timeinfo) {
  if (out == nullptr || outLen == 0 || timeinfo == nullptr) {
    return;
  }

  snprintf(out, outLen, "%s %d/%d",
           app_tr_weekday(timeinfo->tm_wday),
           timeinfo->tm_mon + 1,
           timeinfo->tm_mday);
}
