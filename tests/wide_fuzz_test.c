#include <qlic/qlic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint32_t random32(void) {
  uint64_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state = x;
  return (uint32_t)(x >> 16);
}

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc >> 1) ^
            (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc ^ UINT32_C(0xffffffff);
}

static void write32le(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static uint32_t sample_value(uint32_t pattern, uint32_t x, uint32_t y,
                             uint32_t channel, uint32_t maximum) {
  switch (pattern % 6u) {
  case 0:
    return 0u;
  case 1:
    return maximum;
  case 2:
    return (x * 3001u + y * 7919u + channel * 11003u + 123u) & maximum;
  case 3:
    return ((x ^ y) & 1u) ? maximum : 0u;
  case 4:
    return (x == 0u && y == 0u) ? channel & maximum
                                : (maximum - channel) & maximum;
  default:
    return random32() & maximum;
  }
}

static int rejected(const uint8_t *data, size_t size) {
  qlic_wide_image decoded = {0};
  int result = qlic_decode_wide(data, size, NULL, &decoded);
  int ok = result != QLIC_OK && decoded.pixels == NULL;
  qlic_wide_image_free(&decoded);
  return ok;
}

static int rejected_or_exact_alias(const uint8_t *data, size_t size,
                                   const uint8_t *pixels, size_t stride,
                                   size_t row_bytes, uint32_t width,
                                   uint32_t height, uint32_t channels,
                                   uint32_t bits, uint64_t *alias_count) {
  qlic_wide_image decoded = {0};
  int result = qlic_decode_wide(data, size, NULL, &decoded);
  if (result != QLIC_OK) {
    qlic_wide_image_free(&decoded);
    return 1;
  }
  int exact = decoded.width == width && decoded.height == height &&
              decoded.channels == channels &&
              decoded.bits_per_sample == bits && decoded.stride == row_bytes;
  for (uint32_t y = 0; y < height && exact; ++y)
    exact = memcmp((uint8_t *)decoded.pixels + (size_t)y * row_bytes,
                   pixels + (size_t)y * stride, row_bytes) == 0;
  qlic_wide_image_free(&decoded);
  if (exact)
    ++*alias_count;
  return exact;
}

static int fuzz_case(uint32_t number, uint64_t *mutation_count,
                     uint64_t *alias_count) {
  static const uint32_t channel_choices[] = {1u, 3u, 4u};
  uint32_t bits = 9u + random32() % 16u;
  uint32_t channels = channel_choices[random32() % 3u];
  uint32_t width = 1u + random32() % 96u;
  uint32_t height = 1u + random32() % 96u;
  if (number % 11u == 0u)
    width = 1u;
  if (number % 13u == 0u)
    height = 1u;
  if (number % 47u == 0u) {
    width = 257u;
    height = 3u;
  }
  if (number % 53u == 0u) {
    width = 3u;
    height = 257u;
  }
  size_t storage = bits <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)width * channels * storage;
  size_t stride = row_bytes + (random32() % 6u) * storage;
  size_t pixels_size = (size_t)(height - 1u) * stride + row_bytes;
  uint8_t *pixels = (uint8_t *)malloc(pixels_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_wide_image decoded = {0};
  int ok = 0;
  if (!pixels)
    return 0;
  memset(pixels, 0xa5, pixels_size);
  uint32_t maximum = (UINT32_C(1) << bits) - 1u;
  uint32_t pattern = random32();
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t *row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      for (uint32_t channel = 0; channel < channels; ++channel) {
        uint32_t value =
            sample_value(pattern, x, y, channel, maximum);
        uint8_t *destination =
            row + ((size_t)x * channels + channel) * storage;
        if (storage == sizeof(uint16_t)) {
          uint16_t short_value = (uint16_t)value;
          memcpy(destination, &short_value, sizeof(short_value));
        } else {
          memcpy(destination, &value, sizeof(value));
        }
      }
    }
  }

  qlic_encode_options options;
  qlic_encode_options_default(&options);
  options.threads = 1u + number % 3u;
  if (qlic_encode_wide(pixels, pixels_size, width, height, stride, channels,
                       bits, &options, &encoded, &encoded_size) != QLIC_OK) {
    fprintf(stderr, "case %u encode failed: %s\n", number,
            qlic_last_error());
    goto done;
  }
  if (qlic_decode_wide(encoded, encoded_size, NULL, &decoded) != QLIC_OK ||
      decoded.width != width || decoded.height != height ||
      decoded.channels != channels || decoded.bits_per_sample != bits ||
      decoded.stride != row_bytes ||
      decoded.pixels_size != row_bytes * (size_t)height) {
    fprintf(stderr, "case %u decode failed: %s\n", number,
            qlic_last_error());
    goto done;
  }
  for (uint32_t y = 0; y < height; ++y) {
    if (memcmp((uint8_t *)decoded.pixels + (size_t)y * row_bytes,
               pixels + (size_t)y * stride, row_bytes) != 0) {
      fprintf(stderr, "case %u sample mismatch\n", number);
      goto done;
    }
  }
  qlic_wide_image_free(&decoded);

  qlic_info_ex info = {0};
  info.struct_size = sizeof(info);
  if (qlic_get_info_ex(encoded, encoded_size, NULL, &info) != QLIC_OK ||
      info.width != width || info.height != height ||
      info.channels != channels || info.bits_per_sample != bits) {
    fprintf(stderr, "case %u metadata mismatch\n", number);
    goto done;
  }
  qlic_image reduced = {0};
  if (qlic_decode_rgba(encoded, encoded_size, NULL, &reduced) !=
          QLIC_UNSUPPORTED_FORMAT ||
      reduced.rgba) {
    fprintf(stderr, "case %u silently reduced wide samples\n", number);
    qlic_image_free(&reduced);
    goto done;
  }
  qlic_image_free(&reduced);

  size_t cut = encoded_size ? random32() % encoded_size : 0u;
  if (!rejected(encoded, cut)) {
    fprintf(stderr, "case %u accepted truncation at %zu\n", number, cut);
    goto done;
  }
  ++*mutation_count;

  uint8_t *mutated = (uint8_t *)malloc(encoded_size);
  if (!mutated)
    goto done;
  memcpy(mutated, encoded, encoded_size);
  size_t flip = random32() % (encoded_size - 4u);
  mutated[flip] ^= (uint8_t)(1u << (random32() & 7u));
  if (!rejected(mutated, encoded_size)) {
    fprintf(stderr, "case %u accepted unchecked mutation at %zu\n", number,
            flip);
    free(mutated);
    goto done;
  }
  ++*mutation_count;

  memcpy(mutated, encoded, encoded_size);
  size_t payload_byte = 28u + random32() % (encoded_size - 32u);
  mutated[payload_byte] ^= (uint8_t)(1u << (random32() & 7u));
  write32le(mutated + encoded_size - 4u,
            crc32(mutated, encoded_size - 4u));
  if (!rejected_or_exact_alias(mutated, encoded_size, pixels, stride,
                               row_bytes, width, height, channels, bits,
                               alias_count)) {
    fprintf(stderr,
            "case %u accepted pixel-changing payload mutation at %zu\n",
            number, payload_byte);
    free(mutated);
    goto done;
  }
  ++*mutation_count;
  free(mutated);

  uint8_t random_file[512] = {0};
  size_t random_size = random32() % sizeof(random_file);
  for (size_t index = 0; index < random_size; ++index)
    random_file[index] = (uint8_t)random32();
  if (!rejected(random_file, random_size)) {
    fprintf(stderr, "case %u accepted random input\n", number);
    goto done;
  }
  ++*mutation_count;
  ok = 1;

done:
  qlic_wide_image_free(&decoded);
  qlic_free(encoded);
  free(pixels);
  return ok;
}

int main(int argc, char **argv) {
  uint32_t cases = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 96u;
  uint64_t seed =
      argc > 2 ? (uint64_t)strtoull(argv[2], NULL, 10) : UINT64_C(20260810);
  if (!cases || !seed) {
    fprintf(stderr, "usage: qlic-wide-fuzz-test [positive-cases] [seed]\n");
    return 2;
  }
  rng_state = seed;
  uint64_t mutations = 0;
  uint64_t aliases = 0;
  for (uint32_t number = 0; number < cases; ++number) {
    if (!fuzz_case(number, &mutations, &aliases))
      return 1;
  }
  printf("wide fuzz passed: cases=%u mutations=%llu exact-aliases=%llu "
         "seed=%llu\n",
         cases, (unsigned long long)mutations, (unsigned long long)aliases,
         (unsigned long long)seed);
  return 0;
}
