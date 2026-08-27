#include <qlic/qlic.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sample_value(uint32_t x, uint32_t y, uint32_t channel,
                             uint32_t bits) {
  uint32_t mask = (UINT32_C(1) << bits) - 1u;
  uint64_t value = (uint64_t)x * UINT32_C(0x9e3779b1) +
                   (uint64_t)y * UINT32_C(0x85ebca6b) +
                   (uint64_t)channel * UINT32_C(0xc2b2ae35);
  value ^= value >> 13;
  value *= UINT32_C(0x27d4eb2d);
  value ^= value >> 15;
  return (uint32_t)value & mask;
}

static int parse_u32(const char *text, uint32_t *value) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(text, &end, 10);
  if (errno || !end || *end || parsed > UINT32_MAX)
    return 0;
  *value = (uint32_t)parsed;
  return 1;
}

static int layout(uint32_t width, uint32_t height, uint32_t channels,
                  uint32_t bits, size_t *bytes, size_t *stride,
                  size_t *sample_bytes) {
  if (!width || !height || (channels != 1u && channels != 3u && channels != 4u))
    return 0;
  if ((size_t)width > SIZE_MAX / channels)
    return 0;
  size_t row_samples = (size_t)width * channels;
  *sample_bytes = bits <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  if (row_samples > SIZE_MAX / *sample_bytes)
    return 0;
  *stride = row_samples * *sample_bytes;
  if ((size_t)height > SIZE_MAX / *stride)
    return 0;
  *bytes = (size_t)height * *stride;
  return 1;
}

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
  FILE *file = NULL;
  return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
  return fopen(path, mode);
#endif
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *file = open_file(path, "wb");
  if (!file)
    return 0;
  int wrote = fwrite(data, 1, size, file) == size;
  int closed = fclose(file) == 0;
  return wrote && closed;
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = open_file(path, "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0) {
    if (file)
      fclose(file);
    return NULL;
  }
  long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

static int encode_file(const char *path, uint32_t width, uint32_t height,
                       uint32_t channels, uint32_t bits, int hdr) {
  size_t bytes = 0, stride = 0, sample_bytes = 0;
  uint32_t minimum_bits = hdr ? 8u : 9u;
  if (bits < minimum_bits || bits > 24u ||
      !layout(width, height, channels, bits, &bytes, &stride,
              &sample_bytes)) {
    fprintf(stderr, "invalid wide memory-probe layout\n");
    return 2;
  }
  void *pixels = malloc(bytes);
  if (!pixels) {
    fprintf(stderr, "could not allocate %zu input bytes\n", bytes);
    return 1;
  }
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      for (uint32_t channel = 0; channel < channels; ++channel) {
        size_t index = ((size_t)y * width + x) * channels + channel;
        uint32_t value = sample_value(x, y, channel, bits);
        if (sample_bytes == sizeof(uint16_t))
          ((uint16_t *)pixels)[index] = (uint16_t)value;
        else
          ((uint32_t *)pixels)[index] = value;
      }
    }
  }
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  int status;
  if (hdr) {
    qlic_hdr_image image = {0};
    image.struct_size = sizeof(image);
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.bits_per_sample = bits;
    image.sample_type = QLIC_SAMPLE_UINT;
    image.alpha_mode =
        channels == 4u ? QLIC_ALPHA_STRAIGHT : QLIC_ALPHA_NONE;
    image.color_authority = QLIC_COLOR_CICP;
    image.pixels = pixels;
    image.pixels_size = bytes;
    image.stride = stride;
    image.has_cicp = 1u;
    image.cicp.color_primaries = 9u;
    image.cicp.transfer_characteristics = 16u;
    image.cicp.matrix_coefficients = 0u;
    image.cicp.full_range = 1u;
    image.has_mastering_display = 1u;
    image.mastering_display.max_luminance = 10000000u;
    image.mastering_display.min_luminance = 50u;
    image.has_content_light = 1u;
    image.content_light.max_cll = 1000u;
    image.content_light.max_fall = 400u;
    status = qlic_encode_hdr(&image, NULL, &encoded, &encoded_size);
  } else {
    status = qlic_encode_wide(pixels, bytes, width, height, stride, channels,
                              bits, NULL, &encoded, &encoded_size);
  }
  free(pixels);
  if (status != QLIC_OK) {
    fprintf(stderr, "encode failed: %s (%s)\n", qlic_status_string(status),
            qlic_last_error());
    qlic_free(encoded);
    return 1;
  }
  int ok = write_file(path, encoded, encoded_size);
  qlic_free(encoded);
  if (!ok) {
    fprintf(stderr, "could not write %s\n", path);
    return 1;
  }
  printf("encoded mode=%s width=%u height=%u channels=%u bits=%u raw=%zu "
         "qlic=%zu\n",
         hdr ? "hdr" : "wide", width, height, channels, bits, bytes,
         encoded_size);
  return 0;
}

static int decode_file(const char *path, uint32_t expected_width,
                       uint32_t expected_height, uint32_t expected_channels,
                       uint32_t expected_bits, int hdr, uint32_t threads) {
  size_t encoded_size = 0;
  uint8_t *encoded = read_file(path, &encoded_size);
  if (!encoded) {
    fprintf(stderr, "could not read %s\n", path);
    return 1;
  }
  qlic_wide_image image = {0};
  qlic_hdr_image hdr_image = {0};
  int status;
  if (hdr) {
    qlic_decode_limits_v2 limits;
    qlic_decode_limits_v2_default(&limits);
    limits.threads = threads;
    hdr_image.struct_size = sizeof(hdr_image);
    status = qlic_decode_hdr(encoded, encoded_size, &limits, &hdr_image);
    image.width = hdr_image.width;
    image.height = hdr_image.height;
    image.channels = hdr_image.channels;
    image.bits_per_sample = hdr_image.bits_per_sample;
    image.pixels = hdr_image.pixels;
    image.pixels_size = hdr_image.pixels_size;
    image.stride = hdr_image.stride;
    hdr_image.pixels = NULL;
  } else {
    qlic_decode_limits limits;
    qlic_decode_limits_default(&limits);
    limits.threads = threads;
    status = qlic_decode_wide(encoded, encoded_size, &limits, &image);
  }
  free(encoded);
  if (status != QLIC_OK) {
    fprintf(stderr, "decode failed: %s (%s)\n", qlic_status_string(status),
            qlic_last_error());
    qlic_wide_image_free(&image);
    qlic_hdr_image_free(&hdr_image);
    return 1;
  }
  size_t sample_bytes = image.bits_per_sample <= 16u ? sizeof(uint16_t)
                                                      : sizeof(uint32_t);
  int ok = image.width == expected_width && image.height == expected_height &&
           image.channels == expected_channels &&
           image.bits_per_sample == expected_bits &&
           image.stride ==
               (size_t)image.width * image.channels * sample_bytes;
  for (uint32_t y = 0; ok && y < image.height; ++y) {
    const uint8_t *row =
        (const uint8_t *)image.pixels + (size_t)y * image.stride;
    for (uint32_t x = 0; ok && x < image.width; ++x) {
      for (uint32_t channel = 0; channel < image.channels; ++channel) {
        size_t index = (size_t)x * image.channels + channel;
        uint32_t actual = sample_bytes == sizeof(uint16_t)
                              ? ((const uint16_t *)row)[index]
                              : ((const uint32_t *)row)[index];
        if (actual != sample_value(x, y, channel, image.bits_per_sample)) {
          ok = 0;
          break;
        }
      }
    }
  }
  if (!ok)
    fprintf(stderr, "decoded metadata or samples differ\n");
  else
    printf("decoded width=%u height=%u channels=%u bits=%u raw=%zu qlic=%zu\n",
           image.width, image.height, image.channels, image.bits_per_sample,
           image.pixels_size, encoded_size);
  qlic_wide_image_free(&image);
  qlic_hdr_image_free(&hdr_image);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 7 || argc > 9 ||
      (strcmp(argv[1], "encode") != 0 &&
       strcmp(argv[1], "decode") != 0)) {
    fprintf(stderr,
            "usage: qlic-wide-memory-probe encode|decode FILE WIDTH HEIGHT "
            "CHANNELS BITS [hdr] [threads=N]\n");
    return 2;
  }
  uint32_t width = 0, height = 0, channels = 0, bits = 0;
  if (!parse_u32(argv[3], &width) || !parse_u32(argv[4], &height) ||
      !parse_u32(argv[5], &channels) || !parse_u32(argv[6], &bits)) {
    fprintf(stderr, "invalid numeric argument\n");
    return 2;
  }
  int hdr = 0;
  uint32_t threads = 1u;
  for (int index = 7; index < argc; ++index) {
    if (strcmp(argv[index], "hdr") == 0) {
      hdr = 1;
    } else if (strncmp(argv[index], "threads=", 8u) == 0) {
      if (!parse_u32(argv[index] + 8u, &threads) || !threads) {
        fprintf(stderr, "invalid thread count\n");
        return 2;
      }
    } else {
      fprintf(stderr, "invalid option: %s\n", argv[index]);
      return 2;
    }
  }
  return strcmp(argv[1], "encode") == 0
             ? encode_file(argv[2], width, height, channels, bits, hdr)
             : decode_file(argv[2], width, height, channels, bits, hdr,
                           threads);
}
