#include "win_util.h"
#include <string.h>

static int wpush(wchar_t *out, size_t cap, size_t *size, wchar_t value) {
  if (*size >= cap - 1u)
    return 0;
  out[(*size)++] = value;
  return 1;
}

int wquote(const wchar_t *src, wchar_t *out, size_t cap) {
  if (!src || !out || cap < 3u)
    return 0;
  /* this has to mirror CommandLineToArgvW, especially trailing slashes */
  size_t length = wcslen(src);
  size_t pos = 0;
  size_t n = 0;
  if (!wpush(out, cap, &n, L'"'))
    return 0;
  while (pos < length) {
    size_t slashes = 0;
    while (pos < length && src[pos] == L'\\') {
      ++slashes;
      ++pos;
    }
    if (pos == length) {
      for (size_t i = 0; i < slashes; ++i) {
        if (!wpush(out, cap, &n, L'\\') ||
            !wpush(out, cap, &n, L'\\'))
          return 0;
      }
      break;
    }
    if (src[pos] == L'"') {
      for (size_t i = 0; i < slashes; ++i) {
        if (!wpush(out, cap, &n, L'\\') ||
            !wpush(out, cap, &n, L'\\'))
          return 0;
      }
      if (!wpush(out, cap, &n, L'\\') ||
          !wpush(out, cap, &n, L'"'))
        return 0;
      ++pos;
      continue;
    }
    for (size_t i = 0; i < slashes; ++i)
      if (!wpush(out, cap, &n, L'\\'))
        return 0;
    if (!wpush(out, cap, &n, src[pos++]))
      return 0;
  }
  if (!wpush(out, cap, &n, L'"'))
    return 0;
  out[n] = 0;
  return 1;
}
