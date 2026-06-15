#ifndef GBK_UTF8_H
#define GBK_UTF8_H

#include <stddef.h>

/** Convert a null-terminated GBK string to UTF-8. Returns bytes written (excl. NUL). */
size_t gbk_to_utf8(const char *gbk, char *utf8, size_t utf8Len);

#endif
