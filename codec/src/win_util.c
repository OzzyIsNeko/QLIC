#include "win_util.h"
#include <wchar.h>

static int wrepeat(wchar_t *out, size_t cap, size_t *size, wchar_t value,
                   size_t count) {
  if (*size >= cap || count > cap - 1u - *size)
    return 0;
  wmemset(out + *size, value, count);
  *size += count;
  return 1;
}

static int wpush(wchar_t *out, size_t cap, size_t *size, wchar_t value) {
  return wrepeat(out, cap, size, value, 1u);
}

int wquote(const wchar_t *src, wchar_t *out, size_t cap) {
  if (!src || !out || cap < 3u)
    return 0;
  /* Match CommandLineToArgvW, including trailing slashes. */
  const wchar_t *cursor = src;
  size_t n = 0;
  if (!wpush(out, cap, &n, L'"'))
    return 0;
  while (*cursor) {
    size_t slashes = 0;
    while (*cursor == L'\\') {
      ++slashes;
      ++cursor;
    }
    if (!*cursor) {
      if (!wrepeat(out, cap, &n, L'\\', slashes))
        return 0;
      if (!wrepeat(out, cap, &n, L'\\', slashes))
        return 0;
      break;
    }
    if (*cursor == L'"') {
      if (!wrepeat(out, cap, &n, L'\\', slashes))
        return 0;
      if (!wrepeat(out, cap, &n, L'\\', slashes))
        return 0;
      if (!wpush(out, cap, &n, L'\\'))
        return 0;
      if (!wpush(out, cap, &n, L'"'))
        return 0;
      ++cursor;
      continue;
    }
    if (!wrepeat(out, cap, &n, L'\\', slashes))
      return 0;
    if (!wpush(out, cap, &n, *cursor++))
      return 0;
  }
  if (!wpush(out, cap, &n, L'"'))
    return 0;
  out[n] = 0;
  return 1;
}
