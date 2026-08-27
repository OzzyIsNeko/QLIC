#include <qlic/qlic.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  HEADER_SIZE = 28,
  FOOTER_SIZE = 4,
  WIDTH = 17,
  HEIGHT = 17,
  PIXELS = WIDTH * HEIGHT,
  PALETTE_COUNT = 257,
  PALETTE_BYTES = PALETTE_COUNT * 4,
  INDEX_BYTES = PIXELS * 2,
  PAYLOAD_SIZE = 1 + PALETTE_BYTES + INDEX_BYTES,
  FILE_SIZE = HEADER_SIZE + PAYLOAD_SIZE + FOOTER_SIZE,
  MODE_CPAL_WIRE = 13,
  TRANSFORM_CPAL_PLANAR_WIRE = 13,
  CODEC_STORE_WITH_CRC = 0x80,
  LAYOUT_INTERLEAVED16 = 0,
  LAYOUT_SPLIT16 = 1
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

static uint8_t palette_value(uint32_t index, unsigned channel) {
  switch (channel) {
  case 0:
    return (uint8_t)index;
  case 1:
    return (uint8_t)(index >> 8);
  case 2:
    return (uint8_t)(index * 37u + 11u);
  default:
    return (uint8_t)(index * 13u + 97u);
  }
}

static void make_file(uint8_t file[FILE_SIZE], unsigned layout) {
  memset(file, 0, FILE_SIZE);
  memcpy(file, "QLIC", 4u);
  wr32(file + 4, WIDTH);
  wr32(file + 8, HEIGHT);
  file[12] = MODE_CPAL_WIRE;
  file[13] = TRANSFORM_CPAL_PLANAR_WIRE;
  file[14] = 9u;
  file[15] = CODEC_STORE_WITH_CRC;
  wr32(file + 16, PALETTE_COUNT);
  wr64(file + 20, PAYLOAD_SIZE);

  uint8_t *payload = file + HEADER_SIZE;
  size_t position = 0;
  payload[position++] = (uint8_t)layout;
  for (unsigned channel = 0; channel < 4u; ++channel) {
    uint8_t previous = 0;
    for (uint32_t index = 0; index < PALETTE_COUNT; ++index) {
      uint8_t value = palette_value(index, channel);
      payload[position++] =
          index ? (uint8_t)(value - previous) : value;
      previous = value;
    }
  }
  size_t low = position;
  size_t high = low + PIXELS;
  for (uint32_t pixel = 0; pixel < PIXELS; ++pixel) {
    uint32_t index = pixel % PALETTE_COUNT;
    if (layout == LAYOUT_INTERLEAVED16) {
      payload[position++] = (uint8_t)index;
      payload[position++] = (uint8_t)(index >> 8);
    } else {
      payload[low + pixel] = (uint8_t)index;
      payload[high + pixel] = (uint8_t)(index >> 8);
    }
  }
  if (layout == LAYOUT_SPLIT16)
    position += INDEX_BYTES;
  if (position != PAYLOAD_SIZE) {
    fprintf(stderr, "internal planar fixture size mismatch\n");
    exit(100);
  }
  wr32(file + HEADER_SIZE + PAYLOAD_SIZE,
       crc32(file, HEADER_SIZE + PAYLOAD_SIZE));
}

static int decode(const uint8_t *file, size_t size, qlic_image *out) {
  qlic_decode_limits limits;
  qlic_decode_limits_default(&limits);
  limits.max_file_bytes = 4096u;
  limits.max_payload_bytes = 4096u;
  limits.max_pixels = 4096u;
  limits.max_animation_bytes = 4096u;
  limits.max_frames = 4u;
  return qlic_decode_rgba(file, size, &limits, out);
}

static int valid_pixels(const qlic_image *image) {
  if (image->width != WIDTH || image->height != HEIGHT ||
      image->rgba_size != PIXELS * 4u)
    return 0;
  for (uint32_t pixel = 0; pixel < PIXELS; ++pixel) {
    uint32_t index = pixel % PALETTE_COUNT;
    for (unsigned channel = 0; channel < 4u; ++channel) {
      if (image->rgba[(size_t)pixel * 4u + channel] !=
          palette_value(index, channel))
        return 0;
    }
  }
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

static int reject(uint8_t *file, size_t size) {
  if (!file || size < FOOTER_SIZE)
    return 1;
  wr32(file + size - FOOTER_SIZE, crc32(file, size - FOOTER_SIZE));
  qlic_image image = {0};
  int status = decode(file, size, &image);
  qlic_image_free(&image);
  return status != QLIC_OK;
}

int main(int argc, char **argv) {
  unsigned fuzz_cases = 20000u;
  uint32_t seed = UINT32_C(0xc0a113a7);
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

  uint8_t interleaved[FILE_SIZE];
  uint8_t split[FILE_SIZE];
  make_file(interleaved, LAYOUT_INTERLEAVED16);
  make_file(split, LAYOUT_SPLIT16);
  for (unsigned layout = 0; layout < 2u; ++layout) {
    const uint8_t *file = layout ? split : interleaved;
    qlic_image image = {0};
    if (decode(file, FILE_SIZE, &image) != QLIC_OK || !valid_pixels(&image)) {
      fprintf(stderr, "valid planar cpalette layout %u failed\n", layout);
      qlic_image_free(&image);
      return 3;
    }
    qlic_image_free(&image);
  }

  uint8_t malformed[FILE_SIZE];
  memcpy(malformed, interleaved, FILE_SIZE);
  malformed[HEADER_SIZE] = 2u;
  if (!reject(malformed, sizeof(malformed)))
    return 4;
  memcpy(malformed, interleaved, FILE_SIZE);
  malformed[14] = 10u;
  if (!reject(malformed, sizeof(malformed)))
    return 5;
  memcpy(malformed, interleaved, FILE_SIZE);
  wr32(malformed + 16, 256u);
  if (!reject(malformed, sizeof(malformed)))
    return 6;
  memcpy(malformed, interleaved, FILE_SIZE);
  size_t first_index = HEADER_SIZE + 1u + PALETTE_BYTES;
  malformed[first_index] = 1u;
  malformed[first_index + 1u] = 1u;
  if (!reject(malformed, sizeof(malformed)))
    return 7;
  qlic_image truncated_image = {0};
  if (decode(interleaved, FILE_SIZE - 1u, &truncated_image) == QLIC_OK) {
    qlic_image_free(&truncated_image);
    return 8;
  }
  qlic_image_free(&truncated_image);

  for (unsigned pass = 0; pass < fuzz_cases; ++pass) {
    uint8_t mutated[FILE_SIZE];
    const uint8_t *base = (pass & 1u) ? split : interleaved;
    memcpy(mutated, base, FILE_SIZE);
    unsigned changes = 1u + random32(&seed) % 8u;
    for (unsigned change = 0; change < changes; ++change) {
      size_t position = 4u + random32(&seed) % (FILE_SIZE - 8u);
      mutated[position] ^= (uint8_t)(1u << (random32(&seed) & 7u));
    }
    wr32(mutated + FILE_SIZE - FOOTER_SIZE,
         crc32(mutated, FILE_SIZE - FOOTER_SIZE));
    qlic_image image = {0};
    (void)decode(mutated, FILE_SIZE, &image);
    qlic_image_free(&image);

    size_t truncated = random32(&seed) % FILE_SIZE;
    memset(&image, 0, sizeof(image));
    (void)decode(mutated, truncated, &image);
    qlic_image_free(&image);
  }

  printf("planar cpalette fuzz passed: cases=%u mutations=%u seed=%u "
         "final-state=%u\n",
         fuzz_cases, fuzz_cases * 2u, initial_seed, seed);
  return 0;
}
