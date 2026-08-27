#include "lzms.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0)
    return 0;
#else
  file = fopen(path, "rb");
#endif
  if (!file)
    return 0;
  int ok = fseek(file, 0, SEEK_END) == 0;
  long measured = ok ? ftell(file) : -1;
  if (measured <= 32 || fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *bytes =
      ok ? (uint8_t *)malloc((size_t)measured) : NULL;
  if (!bytes)
    ok = 0;
  if (ok && fread(bytes, 1, (size_t)measured, file) !=
                (size_t)measured)
    ok = 0;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok) {
    free(bytes);
    return 0;
  }
  *data = bytes;
  *size = (size_t)measured;
  return 1;
}

static uint32_t random32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  uint8_t *file = NULL;
  size_t file_size = 0;
  if (!read_file(argv[1], &file, &file_size))
    return 3;
  uint64_t expected64 = le64(file + 20);
  if (expected64 > SIZE_MAX) {
    free(file);
    return 4;
  }
  size_t expected = (size_t)expected64;
  uint8_t *decoded = (uint8_t *)malloc(expected ? expected : 1u);
  if (!decoded ||
      !qlic_lzms_decompress(file + 28u, file_size - 32u,
                            decoded, expected)) {
    free(decoded);
    free(file);
    return 5;
  }

  size_t compressed_size = file_size - 32u;
  for (size_t size = 0; size < compressed_size; ++size)
    (void)qlic_lzms_decompress(file + 28u, size, decoded, expected);

  uint8_t sample[256] = {0};
  uint8_t output[4096];
  uint32_t state = UINT32_C(0x6d2b79f5);
  for (unsigned pass = 0; pass < 10000u; ++pass) {
    size_t input_size = random32(&state) % sizeof(sample);
    size_t output_size = random32(&state) % sizeof(output);
    for (size_t i = 0; i < input_size; ++i)
      sample[i] = (uint8_t)random32(&state);
    (void)qlic_lzms_decompress(sample, input_size, output, output_size);
  }

  free(decoded);
  free(file);
  return 0;
}
