#include <qlic/qlic.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  HEADER_SIZE = 28,
  FOOTER_SIZE = 4,
  MODE_CPAL_WIRE = 13,
  TRANSFORM_CPAL_TILES_WIRE = 12,
  CODEC_STORE_WITH_CRC = 0x80
};

static void wr32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static void wr64(uint8_t *p, uint64_t value) {
  wr32(p, (uint32_t)value);
  wr32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
}

static size_t make_file(uint8_t *file, size_t capacity,
                        const uint8_t *payload, size_t payload_size,
                        uint32_t palette_count, uint8_t index_bits) {
  size_t size = HEADER_SIZE + payload_size + FOOTER_SIZE;
  if (size > capacity)
    return 0;
  memset(file, 0, size);
  memcpy(file, "QLIC", 4u);
  wr32(file + 4, 8u);
  wr32(file + 8, 8u);
  file[12] = MODE_CPAL_WIRE;
  file[13] = TRANSFORM_CPAL_TILES_WIRE;
  file[14] = index_bits;
  file[15] = CODEC_STORE_WITH_CRC;
  wr32(file + 16, palette_count);
  wr64(file + 20, payload_size);
  memcpy(file + HEADER_SIZE, payload, payload_size);
  wr32(file + HEADER_SIZE + payload_size,
       crc32(file, HEADER_SIZE + payload_size));
  return size;
}

static int decode(const uint8_t *file, size_t size, qlic_image *out) {
  qlic_decode_limits limits;
  qlic_decode_limits_default(&limits);
  limits.max_file_bytes = 1024u;
  limits.max_payload_bytes = 1024u;
  limits.max_pixels = 4096u;
  limits.max_animation_bytes = 1024u;
  limits.max_frames = 4u;
  return qlic_decode_rgba(file, size, &limits, out);
}

static int expect_bad(const uint8_t *payload, size_t payload_size,
                      uint32_t palette_count, uint8_t index_bits) {
  uint8_t file[256];
  size_t size = make_file(file, sizeof(file), payload, payload_size,
                          palette_count, index_bits);
  qlic_image image = {0};
  int status = size ? decode(file, size, &image) : QLIC_ERROR;
  qlic_image_free(&image);
  return status != QLIC_OK;
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
  unsigned fuzz_cases = 20000u;
  uint32_t seed = UINT32_C(0x7a11e12d);
  if (argc > 1) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (errno || !end || *end || parsed > 10000000ul)
      return 2;
    fuzz_cases = (unsigned)parsed;
  }
  if (argc > 2) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(argv[2], &end, 0);
    if (errno || !end || *end || parsed > UINT32_MAX)
      return 2;
    seed = (uint32_t)parsed;
  }
  if (argc > 3)
    return 2;
  uint32_t initial_seed = seed;

  /* One 8x8 tile, two RGBA colors, local IDs {0,1}, alternating pixels. */
  uint8_t payload[64] = {
      3,
      1, 2, 3, 255,
      4, 5, 6, 128,
      1, 0, 0,
      0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
  const size_t payload_size = 20u;
  uint8_t file[256];
  size_t file_size = make_file(file, sizeof(file), payload, payload_size,
                               2u, 1u);
  qlic_image image = {0};
  if (!file_size || decode(file, file_size, &image) != QLIC_OK ||
      image.width != 8u || image.height != 8u || image.rgba_size != 256u) {
    fprintf(stderr, "valid tile-palette stream was rejected\n");
    qlic_image_free(&image);
    return 3;
  }
  for (size_t i = 0; i < 64u; ++i) {
    const uint8_t *want = payload + 1u + (i & 1u) * 4u;
    if (memcmp(image.rgba + i * 4u, want, 4u) != 0) {
      fprintf(stderr, "valid tile-palette pixels differ at %zu\n", i);
      qlic_image_free(&image);
      return 4;
    }
  }
  qlic_image_free(&image);

  uint8_t malformed[96];
  memcpy(malformed, payload, payload_size);
  malformed[0] = 2;
  if (!expect_bad(malformed, payload_size, 2u, 1u))
    return 5;
  memcpy(malformed, payload, payload_size);
  malformed[9] = 2;
  if (!expect_bad(malformed, payload_size, 2u, 1u))
    return 6;
  memcpy(malformed, payload, payload_size);
  malformed[10] = 2;
  if (!expect_bad(malformed, payload_size, 2u, 1u))
    return 7;
  if (!expect_bad(payload, payload_size - 1u, 2u, 1u))
    return 8;
  memcpy(malformed, payload, payload_size);
  malformed[payload_size] = 0;
  if (!expect_bad(malformed, payload_size + 1u, 2u, 1u))
    return 9;

  /* A third local color requires two-bit indexes; binary 3 must be rejected. */
  memset(malformed, 0, sizeof(malformed));
  malformed[0] = 3;
  memcpy(malformed + 1, payload + 1, 8u);
  malformed[9] = 7;
  malformed[10] = 8;
  malformed[11] = 9;
  malformed[12] = 255;
  malformed[13] = 2;
  malformed[14] = 0;
  malformed[15] = 0;
  malformed[16] = 0;
  malformed[17] = 3;
  if (!expect_bad(malformed, 33u, 3u, 2u))
    return 10;

  /* Exercise header, checksum, varint, palette, packed-index and truncation
     checks under strict allocation limits. Successful alternate streams are
     valid too; every successful image still has to be safely releasable. */
  for (unsigned pass = 0; pass < fuzz_cases; ++pass) {
    uint8_t mutated[256];
    memcpy(mutated, file, file_size);
    unsigned changes = 1u + random32(&seed) % 6u;
    for (unsigned change = 0; change < changes; ++change) {
      size_t position = 4u + random32(&seed) % (file_size - 8u);
      mutated[position] ^= (uint8_t)(1u << (random32(&seed) & 7u));
    }
    wr32(mutated + file_size - FOOTER_SIZE,
         crc32(mutated, file_size - FOOTER_SIZE));
    qlic_image fuzz_image = {0};
    (void)decode(mutated, file_size, &fuzz_image);
    qlic_image_free(&fuzz_image);

    size_t truncated = random32(&seed) % file_size;
    memset(&fuzz_image, 0, sizeof(fuzz_image));
    (void)decode(mutated, truncated, &fuzz_image);
    qlic_image_free(&fuzz_image);
  }

  printf("tile-palette fuzz passed: cases=%u mutations=%u seed=%u "
         "final-state=%u\n",
         fuzz_cases, fuzz_cases * 2u, initial_seed, seed);
  return 0;
}
