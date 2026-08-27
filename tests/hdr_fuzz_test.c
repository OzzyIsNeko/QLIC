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
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc =
          (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc ^ UINT32_C(0xffffffff);
}

static void write32le(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static int reject_hdr(const uint8_t *data, size_t size) {
  qlic_hdr_image decoded = {0};
  decoded.struct_size = sizeof(decoded);
  int status = qlic_decode_hdr(data, size, NULL, &decoded);
  int rejected = status != QLIC_OK && !decoded.pixels && !decoded.icc &&
                 !decoded.metadata;
  qlic_hdr_image_free(&decoded);
  return rejected;
}

static int exact_pixels(const qlic_hdr_image *decoded, const uint8_t *pixels,
                        size_t stride, size_t row_bytes, uint32_t width,
                        uint32_t height, uint32_t channels, uint32_t bits) {
  if (decoded->width != width || decoded->height != height ||
      decoded->channels != channels || decoded->bits_per_sample != bits ||
      decoded->stride != row_bytes ||
      decoded->pixels_size != row_bytes * (size_t)height)
    return 0;
  for (uint32_t y = 0; y < height; ++y) {
    if (memcmp((const uint8_t *)decoded->pixels + (size_t)y * row_bytes,
               pixels + (size_t)y * stride, row_bytes) != 0)
      return 0;
  }
  return 1;
}

static int fuzz_case(uint32_t number, uint64_t *mutations, uint64_t *aliases) {
  static const uint32_t channel_choices[] = {1u, 3u, 4u};
  uint32_t bits = 8u + random32() % 17u;
  uint32_t channels = channel_choices[random32() % 3u];
  uint32_t width = 1u + random32() % 64u;
  uint32_t height = 1u + random32() % 64u;
  if (number % 17u == 0u)
    width = 1u;
  if (number % 19u == 0u)
    height = 1u;
  size_t storage = bits <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)width * channels * storage;
  size_t stride = row_bytes + (random32() % 4u) * storage;
  size_t pixels_size = (size_t)(height - 1u) * stride + row_bytes;
  uint8_t *pixels = (uint8_t *)malloc(pixels_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  uint8_t icc[257];
  uint8_t metadata_payloads[3][97];
  static const uint8_t metadata_tags[3][4] = {
      {'E', 'X', 'I', 'F'}, {'X', 'M', 'P', '_'}, {'I', 'P', 'T', 'C'}};
  qlic_metadata_block metadata[3] = {0};
  qlic_hdr_image input = {0};
  qlic_hdr_image decoded = {0};
  int ok = 0;
  if (!pixels)
    return 0;
  memset(pixels, 0xa5, pixels_size);
  uint32_t maximum = (UINT32_C(1) << bits) - 1u;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t *row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      for (uint32_t channel = 0; channel < channels; ++channel) {
        uint32_t value =
            (x * 3001u + y * 7919u + channel * 11003u + random32()) & maximum;
        uint8_t *destination = row + ((size_t)x * channels + channel) * storage;
        if (storage == sizeof(uint16_t)) {
          uint16_t short_value = (uint16_t)value;
          memcpy(destination, &short_value, sizeof(short_value));
        } else {
          memcpy(destination, &value, sizeof(value));
        }
      }
    }
  }
  for (size_t i = 0; i < sizeof(icc); ++i)
    icc[i] = (uint8_t)random32();
  uint32_t metadata_count = number % 4u;
  for (uint32_t block = 0; block < metadata_count; ++block) {
    memcpy(metadata[block].tag, metadata_tags[block], 4u);
    metadata[block].data = metadata_payloads[block];
    metadata[block].size = 1u + random32() % sizeof(metadata_payloads[block]);
    for (size_t index = 0; index < metadata[block].size; ++index)
      metadata[block].data[index] = (uint8_t)random32();
  }

  input.struct_size = sizeof(input);
  input.width = width;
  input.height = height;
  input.channels = channels;
  input.bits_per_sample = bits;
  input.sample_type = QLIC_SAMPLE_UINT;
  input.alpha_mode = channels == 4u ? 1u + random32() % 2u : QLIC_ALPHA_NONE;
  input.pixels = pixels;
  input.pixels_size = pixels_size;
  input.stride = stride;
  unsigned color_case = number % 5u;
  if (color_case == 1u || color_case >= 3u) {
    input.icc = icc;
    input.icc_size = 1u + random32() % sizeof(icc);
  }
  if (color_case == 2u || color_case >= 3u) {
    input.has_cicp = 1u;
    input.cicp.color_primaries = (uint16_t)random32();
    input.cicp.transfer_characteristics = (uint16_t)random32();
    input.cicp.matrix_coefficients = (uint16_t)random32();
    input.cicp.full_range = (uint8_t)(random32() & 1u);
  }
  input.color_authority = color_case == 0u   ? QLIC_COLOR_UNSPECIFIED
                          : color_case == 1u ? QLIC_COLOR_ICC
                          : color_case == 2u ? QLIC_COLOR_CICP
                          : color_case == 3u ? QLIC_COLOR_ICC_PREFERRED
                                             : QLIC_COLOR_CICP_PREFERRED;
  input.has_mastering_display = number & 1u;
  if (input.has_mastering_display) {
    for (unsigned i = 0; i < 3u; ++i) {
      input.mastering_display.primary_x[i] = (uint16_t)random32();
      input.mastering_display.primary_y[i] = (uint16_t)random32();
    }
    input.mastering_display.white_x = (uint16_t)random32();
    input.mastering_display.white_y = (uint16_t)random32();
    input.mastering_display.min_luminance = random32() % 1000u;
    input.mastering_display.max_luminance =
        input.mastering_display.min_luminance + random32();
  }
  input.has_content_light = number % 3u == 0u;
  if (input.has_content_light) {
    input.content_light.max_fall = (uint16_t)(random32() % 1000u);
    input.content_light.max_cll =
        (uint16_t)(input.content_light.max_fall +
                   random32() %
                       (UINT16_MAX - input.content_light.max_fall + 1u));
  }
  input.metadata = metadata_count ? metadata : NULL;
  input.metadata_count = metadata_count;

  if (qlic_encode_hdr(&input, NULL, &encoded, &encoded_size) != QLIC_OK) {
    fprintf(stderr, "case %u encode failed: %s\n", number, qlic_last_error());
    goto done;
  }
  decoded.struct_size = sizeof(decoded);
  if (qlic_decode_hdr(encoded, encoded_size, NULL, &decoded) != QLIC_OK ||
      !exact_pixels(&decoded, pixels, stride, row_bytes, width, height,
                    channels, bits) ||
      decoded.alpha_mode != input.alpha_mode ||
      decoded.color_authority != input.color_authority ||
      decoded.icc_size != input.icc_size ||
      (input.icc_size && memcmp(decoded.icc, input.icc, input.icc_size)) ||
      decoded.has_cicp != input.has_cicp ||
      decoded.has_mastering_display != input.has_mastering_display ||
      decoded.has_content_light != input.has_content_light ||
      decoded.metadata_count != metadata_count) {
    fprintf(stderr, "case %u exact HDR decode failed: %s\n", number,
            qlic_last_error());
    goto done;
  }
  for (uint32_t block = 0; block < metadata_count; ++block) {
    if (memcmp(decoded.metadata[block].tag, metadata[block].tag, 4u) != 0 ||
        decoded.metadata[block].size != metadata[block].size ||
        memcmp(decoded.metadata[block].data, metadata[block].data,
               metadata[block].size) != 0) {
      fprintf(stderr, "case %u ancillary metadata mismatch\n", number);
      goto done;
    }
  }
  qlic_hdr_image_free(&decoded);

  qlic_info_v2 info = {0};
  info.struct_size = sizeof(info);
  if (qlic_get_info_v2(encoded, encoded_size, NULL, &info) != QLIC_OK ||
      info.width != width || info.height != height ||
      info.channels != channels || info.bits_per_sample != bits ||
      info.alpha_mode != input.alpha_mode ||
      info.color_authority != input.color_authority) {
    fprintf(stderr, "case %u HDR info mismatch\n", number);
    goto done;
  }

  uint64_t metadata_size = input.icc_size + (input.has_cicp ? 8u : 0u) +
                           (input.has_mastering_display ? 24u : 0u) +
                           (input.has_content_light ? 4u : 0u);
  for (uint32_t block = 0; block < metadata_count; ++block)
    metadata_size += metadata[block].size;
  if (metadata_size > 1u) {
    qlic_decode_limits_v2 limits;
    qlic_decode_limits_v2_default(&limits);
    limits.max_metadata_bytes = metadata_size - 1u;
    decoded.struct_size = sizeof(decoded);
    if (qlic_decode_hdr(encoded, encoded_size, &limits, &decoded) !=
            QLIC_LIMIT_EXCEEDED ||
        decoded.pixels || decoded.icc || decoded.metadata) {
      fprintf(stderr, "case %u ignored HDR metadata limit\n", number);
      goto done;
    }
  }

  size_t cut = random32() % encoded_size;
  if (!reject_hdr(encoded, cut)) {
    fprintf(stderr, "case %u accepted truncation\n", number);
    goto done;
  }
  ++*mutations;
  uint8_t *mutated = (uint8_t *)malloc(encoded_size);
  if (!mutated)
    goto done;
  memcpy(mutated, encoded, encoded_size);
  size_t flip = random32() % (encoded_size - 4u);
  mutated[flip] ^= (uint8_t)(1u << (random32() & 7u));
  if (!reject_hdr(mutated, encoded_size)) {
    fprintf(stderr, "case %u accepted unchecked mutation\n", number);
    free(mutated);
    goto done;
  }
  ++*mutations;

  memcpy(mutated, encoded, encoded_size);
  flip = 28u + random32() % (encoded_size - 32u);
  mutated[flip] ^= (uint8_t)(1u << (random32() & 7u));
  write32le(mutated + encoded_size - 4u, crc32(mutated, encoded_size - 4u));
  decoded.struct_size = sizeof(decoded);
  int status = qlic_decode_hdr(mutated, encoded_size, NULL, &decoded);
  if (status == QLIC_OK) {
    if (!exact_pixels(&decoded, pixels, stride, row_bytes, width, height,
                      channels, bits)) {
      fprintf(stderr, "case %u accepted pixel-changing mutation\n", number);
      free(mutated);
      goto done;
    }
    ++*aliases;
  } else if (decoded.pixels || decoded.icc || decoded.metadata) {
    fprintf(stderr, "case %u leaked failed mutation output\n", number);
    free(mutated);
    goto done;
  }
  qlic_hdr_image_free(&decoded);
  free(mutated);
  ++*mutations;

  uint8_t random_file[512] = {0};
  size_t random_size = random32() % sizeof(random_file);
  for (size_t i = 0; i < random_size; ++i)
    random_file[i] = (uint8_t)random32();
  if (!reject_hdr(random_file, random_size)) {
    fprintf(stderr, "case %u accepted random input\n", number);
    goto done;
  }
  ++*mutations;
  ok = 1;

done:
  qlic_hdr_image_free(&decoded);
  qlic_free(encoded);
  free(pixels);
  return ok;
}

int main(int argc, char **argv) {
  uint32_t cases = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 96u;
  uint64_t seed =
      argc > 2 ? (uint64_t)strtoull(argv[2], NULL, 10) : UINT64_C(20260814);
  if (!cases || !seed) {
    fprintf(stderr, "usage: qlic-hdr-fuzz-test [positive-cases] [seed]\n");
    return 2;
  }
  rng_state = seed;
  uint64_t mutations = 0;
  uint64_t aliases = 0;
  for (uint32_t number = 0; number < cases; ++number) {
    if (!fuzz_case(number, &mutations, &aliases))
      return 1;
  }
  printf("HDR fuzz passed: cases=%u mutations=%llu exact-pixel-aliases=%llu "
         "seed=%llu\n",
         cases, (unsigned long long)mutations, (unsigned long long)aliases,
         (unsigned long long)seed);
  return 0;
}
