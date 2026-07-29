#include "win_util.h"

#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int round_trip(const wchar_t *value) {
  wchar_t quoted[1024];
  wchar_t command[1030] = L"x ";
  if (!wquote(value, quoted, sizeof(quoted) / sizeof(quoted[0])) ||
      wcscat_s(command, sizeof(command) / sizeof(command[0]), quoted))
    return 0;
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(command, &argc);
  int ok = argv && argc == 2 && wcscmp(argv[1], value) == 0;
  if (argv)
    LocalFree(argv);
  return ok;
}

int main(void) {
  wchar_t empty[2] = {0, 0};
  static const wchar_t *cases[] = {
      NULL,         L"plain",       L"two words",    L"tail\\",
      L"tail\\\\",  L"say\"hello",  L"one\\\"two",   L"one\\\\\"two",
      L"\\\"",      L" \\\\ \" \\", L"\x03a9\x6f22"};
  cases[0] = empty;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!round_trip(cases[i])) {
      fwprintf(stderr, L"argument round trip failed at case %zu\n", i);
      return 1;
    }
  }
  static const wchar_t alphabet[] = {L'a', L' ', L'\\', L'"'};
  /* quoting bugs cluster around slash runs next to quotes, exhaust those combinations */
  for (unsigned length = 0; length <= 7u; ++length) {
    uint32_t combinations = UINT32_C(1) << (length * 2u);
    for (uint32_t code = 0; code < combinations; ++code) {
      wchar_t value[8];
      uint32_t bits = code;
      for (unsigned i = 0; i < length; ++i) {
        value[i] = alphabet[bits & 3u];
        bits >>= 2u;
      }
      value[length] = 0;
      if (!round_trip(value)) {
        fwprintf(stderr, L"exhaustive argument round trip failed\n");
        return 1;
      }
    }
  }
  puts("Windows argument tests passed");
  return 0;
}
