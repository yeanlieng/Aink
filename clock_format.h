#ifndef CLOCK_FORMAT_H
#define CLOCK_FORMAT_H

#include <stddef.h>
#include <stdbool.h>
#include <time.h>

/** Hours:minutes only; 12h appends AM/PM when use24h is false. */
void clock_format_hm(char *out, size_t outLen, const struct tm *timeinfo, bool use24h);

/** Weekday + month/day, locale-aware weekday via app_tr_weekday. */
void clock_format_date(char *out, size_t outLen, const struct tm *timeinfo);

#endif
