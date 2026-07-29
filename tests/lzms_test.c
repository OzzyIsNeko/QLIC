#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <compressapi.h>

#include "lzms.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const uint8_t *p) {
  return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

static int palette_mode(unsigned mode) {
  return mode == 5u || mode == 11u || mode == 12u;
}

static int run_file(const char *path) {
  FILE *file = NULL;
  if (fopen_s(&file, path, "rb") || !file)
    return 0;
  int ok = fseek(file, 0, SEEK_END) == 0;
  long measured = ok ? ftell(file) : -1;
  if (measured < 32 || fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *data = ok ? (uint8_t *)malloc((size_t)measured) : NULL;
  if (!data ||
      fread(data, 1, (size_t)measured, file) != (size_t)measured)
    ok = 0;
  fclose(file);
  if (!ok) {
    free(data);
    return 0;
  }
  uint32_t palette_count = read32(data + 16);
  size_t palette_size =
      palette_mode(data[12]) ? (size_t)palette_count * 4u : 0u;
  size_t start = 28u + palette_size;
  size_t compressed_size = (size_t)measured - start - 4u;
  uint64_t payload64 = read64(data + 20);
  if (memcmp(data, "QLIC", 4) || (data[15] & 0x7fu) != 3u ||
      payload64 > SIZE_MAX) {
    free(data);
    return 0;
  }
  size_t payload_size = (size_t)payload64;
  uint8_t *decoded = (uint8_t *)malloc(payload_size);
  uint8_t *windows = (uint8_t *)malloc(payload_size);
  DECOMPRESSOR_HANDLE decoder = NULL;
  SIZE_T written = 0;
  ok = decoded && windows &&
       CreateDecompressor(COMPRESS_ALGORITHM_LZMS, NULL, &decoder) &&
       Decompress(decoder, data + start, compressed_size, windows,
                  payload_size, &written) &&
       written == payload_size &&
       qlic_lzms_decompress(data + start, compressed_size, decoded,
                            payload_size) &&
       memcmp(decoded, windows, payload_size) == 0;
  if (decoder)
    CloseDecompressor(decoder);
  free(decoded);
  free(windows);
  free(data);
  if (!ok)
    fprintf(stderr, "LZMS mismatch: %s\n", path);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  for (int index = 1; index < argc; ++index)
    if (!run_file(argv[index]))
      return 1;
  printf("LZMS checks passed: %d\n", argc - 1);
  return 0;
}
