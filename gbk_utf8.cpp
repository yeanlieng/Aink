#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "gbk_utf8.h"
#include "gbk_table.h"

static uint16_t gbk_code_to_unicode(uint16_t gbkCode) {
  int lo = 0;
  int hi = GBK_TABLE_COUNT - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const uint16_t code = kGbkCodes[mid];
    if (code == gbkCode) {
      return kGbkUnicode[mid];
    }
    if (code < gbkCode) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return 0;
}

static size_t unicode_to_utf8(uint16_t cp, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return 0;
  }
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    if (outLen < 2) {
      return 0;
    }
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (outLen < 3) {
    return 0;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

size_t gbk_to_utf8(const char *gbk, char *utf8, size_t utf8Len) {
  if (gbk == nullptr || utf8 == nullptr || utf8Len == 0) {
    return 0;
  }

  size_t outUsed = 0;
  utf8[0] = '\0';

  for (size_t i = 0; gbk[i] != '\0';) {
    const unsigned char b0 = (unsigned char)gbk[i];
    uint16_t cp = 0;

    if (b0 < 0x80) {
      cp = b0;
      i++;
    } else if (b0 >= 0x81 && b0 <= 0xFE && gbk[i + 1] != '\0') {
      const unsigned char b1 = (unsigned char)gbk[i + 1];
      if (b1 >= 0x40) {
        const uint16_t gbkCode = ((uint16_t)b0 << 8) | b1;
        cp = gbk_code_to_unicode(gbkCode);
        i += 2;
      } else {
        i++;
        continue;
      }
    } else {
      i++;
      continue;
    }

    if (cp == 0) {
      continue;
    }

    char chunk[4];
    const size_t chunkLen = unicode_to_utf8(cp, chunk, sizeof(chunk));
    if (chunkLen == 0 || outUsed + chunkLen >= utf8Len) {
      break;
    }
    memcpy(utf8 + outUsed, chunk, chunkLen);
    outUsed += chunkLen;
    utf8[outUsed] = '\0';
  }

  return outUsed;
}
