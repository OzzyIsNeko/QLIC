#include <qlic/qlic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FUZZ_CASES = 20000 };

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
}

static void wr32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8u);
  p[2] = (uint8_t)(value >> 16u);
  p[3] = (uint8_t)(value >> 24u);
}

static void fix_crc(uint8_t *data, size_t size) {
  wr32(data + size - 4u, crc32(data, size - 4u));
}

static uint32_t random32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13u;
  value ^= value >> 17u;
  value ^= value << 5u;
  *state = value;
  return value;
}

static int read_fixture(uint8_t **data, size_t *size) {
  char path[1024];
  int length = snprintf(path, sizeof(path), "%s/%s", QLIC_FIXTURE_DIR,
                        "planar-med-lzms.qlic");
  if (length < 0 || (size_t)length >= sizeof(path))
    return 0;
  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") || !file)
    return 0;
#else
  file = fopen(path, "rb");
  if (!file)
    return 0;
#endif
  int ok = fseek(file, 0, SEEK_END) == 0;
  long measured = ok ? ftell(file) : -1;
  if (measured < 32 || fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *bytes = ok ? (uint8_t *)malloc((size_t)measured) : NULL;
  if (!bytes)
    ok = 0;
  if (ok && fread(bytes, 1, (size_t)measured, file) != (size_t)measured)
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

static int decode_once(const uint8_t *data, size_t size,
                       const qlic_decode_limits *limits) {
  qlic_image image = {0};
  int status = qlic_decode_rgba(data, size, limits, &image);
  qlic_image_free(&image);
  return status;
}

int main(void) {
  uint8_t *fixture = NULL;
  size_t size = 0;
  if (!read_fixture(&fixture, &size))
    return 2;
  uint8_t *mutated = (uint8_t *)malloc(size);
  if (!mutated) {
    free(fixture);
    return 3;
  }
  qlic_decode_limits limits;
  qlic_decode_limits_default(&limits);
  limits.max_file_bytes = UINT64_C(1048576);
  limits.max_payload_bytes = UINT64_C(1048576);
  limits.max_pixels = UINT64_C(1048576);
  limits.max_animation_bytes = UINT64_C(1048576);
  limits.max_frames = 16u;
  if (decode_once(fixture, size, &limits) != QLIC_OK) {
    free(mutated);
    free(fixture);
    return 4;
  }

  /* The same six-sample payload must safely exercise every edge shape. */
  static const uint32_t dimensions[][2] = {{1u, 6u}, {2u, 3u},
                                            {3u, 2u}, {6u, 1u}};
  for (size_t i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); ++i) {
    memcpy(mutated, fixture, size);
    wr32(mutated + 4u, dimensions[i][0]);
    wr32(mutated + 8u, dimensions[i][1]);
    fix_crc(mutated, size);
    if (decode_once(mutated, size, &limits) != QLIC_OK) {
      free(mutated);
      free(fixture);
      return 5;
    }
  }

  /* Planar MED is valid only for RGB/RGBA, LZMS, and no palette metadata. */
  for (unsigned invalid = 0; invalid < 4u; ++invalid) {
    memcpy(mutated, fixture, size);
    if (invalid == 0u)
      mutated[12] = 1u;
    else if (invalid == 1u)
      mutated[15] = UINT8_C(0x80);
    else if (invalid == 2u)
      mutated[14] = 8u;
    else
      wr32(mutated + 16u, 1u);
    fix_crc(mutated, size);
    if (decode_once(mutated, size, &limits) == QLIC_OK) {
      free(mutated);
      free(fixture);
      return 6;
    }
  }

  uint32_t state = UINT32_C(0x23d47a91);
  unsigned accepted = 0;
  for (unsigned pass = 0; pass < FUZZ_CASES; ++pass) {
    memcpy(mutated, fixture, size);
    size_t presented = size;
    unsigned kind = random32(&state) % 6u;
    if (kind == 0u || kind == 2u) {
      unsigned changes = kind == 0u ? 1u : 1u + random32(&state) % 4u;
      for (unsigned change = 0; change < changes; ++change) {
        size_t pos = 28u + random32(&state) % (size - 32u);
        mutated[pos] ^= (uint8_t)(1u << (random32(&state) & 7u));
      }
      fix_crc(mutated, size);
    } else if (kind == 1u) {
      size_t pos = 4u + random32(&state) % 24u;
      mutated[pos] ^= (uint8_t)(1u << (random32(&state) & 7u));
      fix_crc(mutated, size);
    } else if (kind == 3u) {
      for (unsigned byte = 0; byte < 8u; ++byte)
        mutated[20u + byte] = (uint8_t)random32(&state);
      fix_crc(mutated, size);
    } else if (kind == 4u) {
      presented = random32(&state) % size;
    } else {
      wr32(mutated + 4u, 1u + random32(&state) % 64u);
      wr32(mutated + 8u, 1u + random32(&state) % 64u);
      fix_crc(mutated, size);
    }
    if (decode_once(mutated, presented, &limits) == QLIC_OK)
      ++accepted;
  }
  printf("planar MED fuzz passed: cases=%u accepted=%u seed=%08x\n",
         (unsigned)FUZZ_CASES, accepted, UINT32_C(0x23d47a91));
  free(mutated);
  free(fixture);
  return 0;
}
