#include <qlic/qlic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define THREAD_RESULT 0
#else
#include <pthread.h>
#define THREAD_RESULT NULL
#endif

static int api_encode_rgba_options(
    const uint8_t *pixels, uint32_t width, uint32_t height,
    const qlic_encode_options *options, uint8_t **encoded,
    size_t *encoded_size) {
  size_t stride = (size_t)width * 4u;
  return qlic_encode_rgba(pixels, stride * (size_t)height, width, height,
                          stride, options, encoded, encoded_size);
}

static int api_encode_rgba(const uint8_t *pixels, uint32_t width,
                           uint32_t height, uint8_t **encoded,
                           size_t *encoded_size) {
  return api_encode_rgba_options(pixels, width, height, NULL, encoded,
                                 encoded_size);
}

static int api_decode_rgba(const uint8_t *data, size_t size,
                           qlic_image *image) {
  return qlic_decode_rgba(data, size, NULL, image);
}

static int api_get_info(const uint8_t *data, size_t size, qlic_info *info) {
  return qlic_get_info(data, size, NULL, info);
}

static int api_encode_animation_options(
    const qlic_frame *frames, uint32_t frame_count, uint32_t loop_count,
    const qlic_encode_options *options, uint8_t **encoded,
    size_t *encoded_size) {
  qlic_frame_input *input =
      (qlic_frame_input *)calloc(frame_count, sizeof(*input));
  if (!input)
    return QLIC_OUT_OF_MEMORY;
  for (uint32_t i = 0; i < frame_count; ++i) {
    size_t stride = (size_t)frames[i].image.width * 4u;
    input[i].rgba = frames[i].image.rgba;
    input[i].rgba_size = stride * (size_t)frames[i].image.height;
    input[i].stride = stride;
    input[i].width = frames[i].image.width;
    input[i].height = frames[i].image.height;
    input[i].delay_ms = frames[i].delay_ms;
  }
  int result = qlic_encode_animation(input, frame_count, loop_count, options,
                                     encoded, encoded_size);
  free(input);
  return result;
}

static int api_encode_animation(
    const qlic_frame *frames, uint32_t frame_count, uint32_t loop_count,
    uint8_t **encoded, size_t *encoded_size) {
  return api_encode_animation_options(frames, frame_count, loop_count, NULL,
                                      encoded, encoded_size);
}

static int api_decode_animation(const uint8_t *data, size_t size,
                                qlic_animation *animation) {
  return qlic_decode_animation(data, size, NULL, animation);
}

static int check(int condition, const char *message) {
  if (condition)
    return 1;
  fprintf(stderr, "FAIL: %s", message);
  if (qlic_last_error()[0])
    fprintf(stderr, ": %s", qlic_last_error());
  fputc('\n', stderr);
  return 0;
}

static int capabilities_test(void) {
  qlic_capabilities capabilities;
  memset(&capabilities, 0, sizeof(capabilities));
  capabilities.struct_size = sizeof(capabilities);
  if (!check(qlic_get_capabilities(&capabilities) == QLIC_OK,
             "query capabilities"))
    return 0;
  const uint32_t profiles = QLIC_PROFILE_CORE_STILL |
                            QLIC_PROFILE_ANIMATION |
                            QLIC_PROFILE_WIDE_INTEGER | QLIC_PROFILE_HDR |
                            QLIC_PROFILE_LEGACY;
  if (!check(capabilities.api_version == QLIC_API_VERSION &&
                 capabilities.decode_profiles == profiles &&
                 capabilities.encode_profiles == profiles &&
                 (capabilities.features & QLIC_FEATURE_PORTABLE_LZMS) &&
                 (capabilities.features & QLIC_FEATURE_LIMITS_V2) &&
                 capabilities.max_channels == 4u &&
                 capabilities.max_bits_per_sample == 24u,
             "report exact capabilities"))
    return 0;
  capabilities.reserved[0] = 1u;
  return check(qlic_get_capabilities(&capabilities) == QLIC_BAD_ARGUMENT,
               "reject dirty capability fields");
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

static uint64_t read64le(const uint8_t *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8u; ++i)
    value |= (uint64_t)p[i] << (i * 8u);
  return value;
}

static uint32_t read32le(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8u |
         (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u;
}

static void write32le(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8u);
  p[2] = (uint8_t)(value >> 16u);
  p[3] = (uint8_t)(value >> 24u);
}

static void fill(uint8_t *rgba, uint32_t width, uint32_t height,
                 uint32_t seed) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t p = ((size_t)y * width + x) * 4u;
      rgba[p + 0u] = (uint8_t)(x * 13u + y * 3u + seed);
      rgba[p + 1u] = (uint8_t)(x * 5u + y * 17u + seed * 7u);
      rgba[p + 2u] = (uint8_t)((x ^ y) * 11u + seed * 19u);
      rgba[p + 3u] = (uint8_t)((x + y + seed) % 9u ? 255u : x + y);
    }
  }
}

static int alpha_edge_roundtrip(uint32_t width, uint32_t height,
                                unsigned alpha_pattern) {
  static const uint8_t alpha_levels[] = {0u, 1u, 127u, 254u, 255u};
  size_t row_bytes = (size_t)width * 4u;
  size_t stride = row_bytes + 7u;
  size_t source_size = (size_t)(height - 1u) * stride + row_bytes;
  uint8_t *source = (uint8_t *)malloc(source_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_image decoded = {0};
  int ok = 0;
  if (!check(source != NULL, "allocate alpha edge image"))
    goto done;
  memset(source, 0xa5, source_size);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t *row = source + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      size_t index = (size_t)y * width + x;
      uint8_t *pixel = row + (size_t)x * 4u;
      pixel[0] = (uint8_t)(index * 29u + 3u);
      pixel[1] = (uint8_t)(index * 47u + 5u);
      pixel[2] = (uint8_t)(index * 71u + 7u);
      pixel[3] = alpha_pattern == 2u
                     ? 173u
                     : alpha_pattern == 1u ? (index & 1u ? 255u : 0u)
                                           : alpha_levels[index % 5u];
    }
  }
  if (!check(qlic_encode_rgba(source, source_size, width, height, stride, NULL,
                              &encoded, &encoded_size) == QLIC_OK,
             "encode alpha edge image") ||
      !check(qlic_decode_rgba(encoded, encoded_size, NULL, &decoded) == QLIC_OK,
             "decode alpha edge image") ||
      !check(decoded.width == width && decoded.height == height &&
                 decoded.stride == row_bytes,
             "preserve alpha edge dimensions"))
    goto done;
  for (uint32_t y = 0; y < height; ++y) {
    if (!check(memcmp(decoded.rgba + (size_t)y * row_bytes,
                      source + (size_t)y * stride, row_bytes) == 0,
               "preserve alpha and hidden RGB exactly"))
      goto done;
  }
  ok = 1;
done:
  qlic_image_free(&decoded);
  qlic_free(encoded);
  free(source);
  return ok;
}

static int alpha_edge_test(void) {
  return alpha_edge_roundtrip(1u, 257u, 0u) &&
         alpha_edge_roundtrip(257u, 1u, 1u) &&
         alpha_edge_roundtrip(17u, 19u, 2u);
}

static int still_image_test(void) {
  const uint32_t width = 73u;
  const uint32_t height = 61u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded_a = NULL;
  uint8_t *encoded_b = NULL;
  uint8_t *damaged = NULL;
  uint8_t *caller_pixels = NULL;
  size_t size_a = 0;
  size_t size_b = 0;
  qlic_image decoded = {0};
  qlic_info info = {0};
  qlic_encode_options options;
  int ok = 0;

  if (!check(pixels != NULL, "allocate image"))
    goto done;
  fill(pixels, width, height, 3u);
  if (!check(api_encode_rgba(pixels, width, height, &encoded_a, &size_a) ==
                 QLIC_OK,
             "encode image"))
    goto done;
  qlic_encode_options_default(&options);
  options.threads = UINT32_MAX;
  if (!check(api_encode_rgba_options(pixels, width, height, &options,
                                     &encoded_b, &size_b) == QLIC_OK,
             "encode image twice"))
    goto done;
  if (size_a != size_b || memcmp(encoded_a, encoded_b, size_a) != 0) {
    size_t first = 0;
    size_t shared = size_a < size_b ? size_a : size_b;
    while (first < shared && encoded_a[first] == encoded_b[first])
      ++first;
    fprintf(stderr,
            "determinism mismatch: %zu/%zu bytes, first difference at %zu\n",
            size_a, size_b, first);
    if (!check(0, "deterministic output"))
      goto done;
  }
  if (!check(api_get_info(encoded_a, size_a, &info) == QLIC_OK &&
                 info.width == width && info.height == height &&
                 info.frame_count == 1u && info.animated == 0u,
             "read image metadata"))
    goto done;
  if (!check(qlic_validate(encoded_a, size_a, NULL) == QLIC_OK,
             "validate still image without retaining output"))
    goto done;
  if (!check(api_decode_rgba(encoded_a, size_a, &decoded) == QLIC_OK,
             "decode image"))
    goto done;
  if (!check(decoded.width == width && decoded.height == height &&
                 decoded.stride == (size_t)width * 4u &&
                 decoded.rgba_size == pixel_bytes &&
                 memcmp(decoded.rgba, pixels, pixel_bytes) == 0,
             "lossless round trip"))
    goto done;
  caller_pixels = (uint8_t *)malloc(pixel_bytes);
  if (!check(caller_pixels != NULL, "allocate caller-owned pixels"))
    goto done;
  qlic_pixel_buffer caller = {0};
  caller.struct_size = sizeof(caller);
  caller.format = QLIC_PIXELS_RGBA8;
  caller.pixels = caller_pixels;
  caller.pixels_size = pixel_bytes;
  caller.stride = (size_t)width * 4u;
  if (!check(qlic_decode_pixels(encoded_a, size_a, NULL, &caller) == QLIC_OK &&
                 caller.width == width && caller.height == height &&
                 memcmp(caller_pixels, pixels, pixel_bytes) == 0,
             "decode into caller-owned RGBA"))
    goto done;
  caller.format = QLIC_PIXELS_RGB8;
  caller.stride = (size_t)width * 3u;
  if (!check(qlic_decode_pixels(encoded_a, size_a, NULL, &caller) ==
                 QLIC_UNSUPPORTED_FORMAT,
             "do not discard alpha in caller-owned RGB"))
    goto done;
  damaged = (uint8_t *)malloc(size_a);
  if (!check(damaged != NULL, "allocate damaged input"))
    goto done;
  memcpy(damaged, encoded_a, size_a);
  damaged[size_a / 2u] ^= 0x40u;
  qlic_image_free(&decoded);
  if (!check(api_decode_rgba(damaged, size_a, &decoded) == QLIC_BAD_DATA,
             "reject checksum mismatch"))
    goto done;
  if (!check(qlic_validate(damaged, size_a, NULL) == QLIC_BAD_DATA,
             "reject damaged input during validation"))
    goto done;
  if (!check(api_decode_rgba(encoded_a, size_a - 1u, &decoded) ==
                 QLIC_BAD_DATA,
             "reject truncated input"))
    goto done;
  ok = 1;

done:
  qlic_image_free(&decoded);
  qlic_free(encoded_a);
  qlic_free(encoded_b);
  free(damaged);
  free(caller_pixels);
  free(pixels);
  return ok;
}

static int pixel_formats_test(void) {
  const uint32_t width = 31u;
  const uint32_t height = 23u;
  const size_t rgba_size = (size_t)width * height * 4u;
  uint8_t *rgba = (uint8_t *)malloc(rgba_size);
  uint8_t *destination = (uint8_t *)malloc(rgba_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  int ok = 0;
  if (!check(rgba && destination, "allocate exact pixel format test"))
    goto done;
  for (size_t i = 0, count = (size_t)width * height; i < count; ++i) {
    uint8_t value = (uint8_t)(i * 29u + i / width * 7u);
    rgba[i * 4u] = value;
    rgba[i * 4u + 1u] = value;
    rgba[i * 4u + 2u] = value;
    rgba[i * 4u + 3u] = 255u;
  }
  if (!check(api_encode_rgba(rgba, width, height, &encoded, &encoded_size) ==
                 QLIC_OK,
             "encode exact pixel formats"))
    goto done;
  qlic_pixel_buffer output = {0};
  output.struct_size = sizeof(output);
  output.format = QLIC_PIXELS_GRAY8;
  output.pixels = destination;
  output.pixels_size = rgba_size;
  output.stride = width;
  if (!check(qlic_decode_pixels(encoded, encoded_size, NULL, &output) ==
                 QLIC_OK,
             "decode exact Gray8"))
    goto done;
  for (size_t i = 0, count = (size_t)width * height; i < count; ++i) {
    if (!check(destination[i] == rgba[i * 4u], "preserve exact Gray8"))
      goto done;
  }
  memset(destination, 0, rgba_size);
  output.width = output.height = 0;
  output.format = QLIC_PIXELS_RGB8;
  output.stride = (size_t)width * 3u;
  if (!check(qlic_decode_pixels(encoded, encoded_size, NULL, &output) ==
                 QLIC_OK && output.width == width && output.height == height,
             "decode exact RGB8"))
    goto done;
  for (size_t i = 0, count = (size_t)width * height; i < count; ++i) {
    if (!check(destination[i * 3u] == rgba[i * 4u] &&
                   destination[i * 3u + 1u] == rgba[i * 4u + 1u] &&
                   destination[i * 3u + 2u] == rgba[i * 4u + 2u],
               "preserve exact RGB8"))
      goto done;
  }
  for (size_t i = 0, count = (size_t)width * height; i < count; ++i)
    rgba[i * 4u + 3u] = (uint8_t)(i * 17u + 3u);
  qlic_free(encoded);
  encoded = NULL;
  encoded_size = 0;
  if (!check(api_encode_rgba(rgba, width, height, &encoded, &encoded_size) ==
                 QLIC_OK,
             "encode exact GrayA8"))
    goto done;
  memset(destination, 0, rgba_size);
  output.width = output.height = 0;
  output.format = QLIC_PIXELS_GRAYA8;
  output.stride = (size_t)width * 2u;
  if (!check(qlic_decode_pixels(encoded, encoded_size, NULL, &output) ==
                 QLIC_OK && output.width == width && output.height == height,
             "decode exact GrayA8"))
    goto done;
  for (size_t i = 0, count = (size_t)width * height; i < count; ++i) {
    if (!check(destination[i * 2u] == rgba[i * 4u] &&
                   destination[i * 2u + 1u] == rgba[i * 4u + 3u],
               "preserve exact GrayA8"))
      goto done;
  }
  output.format = QLIC_PIXELS_GRAY8;
  output.stride = width;
  if (!check(qlic_decode_pixels(encoded, encoded_size, NULL, &output) ==
                 QLIC_UNSUPPORTED_FORMAT,
             "do not discard alpha in caller-owned Gray8"))
    goto done;
  ok = 1;
done:
  qlic_free(encoded);
  free(destination);
  free(rgba);
  return ok;
}

static int pixel_input_test(void) {
  const uint32_t width = 3u;
  const uint32_t height = 2u;
  const uint32_t formats[] = {QLIC_PIXELS_GRAY8, QLIC_PIXELS_GRAYA8,
                              QLIC_PIXELS_RGB8, QLIC_PIXELS_RGBA8};
  uint8_t source[24] = {0};
  uint8_t expected[24] = {0};
  int ok = 1;
  for (size_t format_index = 0; format_index < 4u && ok; ++format_index) {
    uint32_t format = formats[format_index];
    size_t pixel_size = (size_t)format;
    for (size_t pixel = 0; pixel < 6u; ++pixel) {
      uint8_t red = (uint8_t)(pixel * 31u + 1u);
      uint8_t green = format < QLIC_PIXELS_RGB8
                          ? red
                          : (uint8_t)(pixel * 17u + 2u);
      uint8_t blue = format < QLIC_PIXELS_RGB8
                         ? red
                         : (uint8_t)(pixel * 13u + 3u);
      uint8_t alpha = (format == QLIC_PIXELS_GRAYA8 ||
                       format == QLIC_PIXELS_RGBA8)
                          ? (uint8_t)(pixel * 19u + 4u)
                          : 255u;
      uint8_t *input = source + pixel * pixel_size;
      if (format == QLIC_PIXELS_GRAY8) {
        input[0] = red;
      } else if (format == QLIC_PIXELS_GRAYA8) {
        input[0] = red;
        input[1] = alpha;
      } else {
        input[0] = red;
        input[1] = green;
        input[2] = blue;
        if (format == QLIC_PIXELS_RGBA8)
          input[3] = alpha;
      }
      expected[pixel * 4u] = red;
      expected[pixel * 4u + 1u] = green;
      expected[pixel * 4u + 2u] = blue;
      expected[pixel * 4u + 3u] = alpha;
    }
    qlic_pixel_input input = {0};
    input.struct_size = sizeof(input);
    input.format = format;
    input.width = width;
    input.height = height;
    input.pixels = source;
    input.pixels_size = (size_t)width * height * pixel_size;
    input.stride = (size_t)width * pixel_size;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    qlic_image decoded = {0};
    ok = check(qlic_encode_pixels(&input, NULL, &encoded, &encoded_size) ==
                   QLIC_OK,
               "encode direct pixel format") &&
         check(qlic_decode_rgba(encoded, encoded_size, NULL, &decoded) ==
                   QLIC_OK,
               "decode direct pixel format") &&
         check(decoded.rgba_size == sizeof(expected) &&
                   memcmp(decoded.rgba, expected, sizeof(expected)) == 0,
               "preserve direct pixel format");
    qlic_image_free(&decoded);
    qlic_free(encoded);
  }
  return ok;
}

static int block_reference_test(void) {
  const uint32_t width = 256u;
  const uint32_t height = 256u;
  const size_t row_bytes = (size_t)width * 4u;
  const size_t pixel_bytes = row_bytes * height;
  uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded = NULL;
  uint8_t *damaged = NULL;
  size_t encoded_size = 0;
  qlic_image decoded = {0};
  int ok = 0;

  if (!check(pixels != NULL, "allocate repeated block image"))
    goto done;
  uint32_t state = UINT32_C(0x6d2b79f5);
  for (uint32_t y = 0; y < height / 2u; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      size_t p = (size_t)y * row_bytes + (size_t)x * 4u;
      pixels[p + 0u] = (uint8_t)state;
      pixels[p + 1u] = (uint8_t)(state >> 8);
      pixels[p + 2u] = (uint8_t)(state >> 16);
      pixels[p + 3u] = 255u;
    }
  }
  memcpy(pixels + row_bytes * (height / 2u), pixels,
         row_bytes * (height / 2u));
  if (!check(api_encode_rgba(pixels, width, height, &encoded,
                              &encoded_size) == QLIC_OK,
             "encode repeated block image"))
    goto done;
  size_t qbr = SIZE_MAX;
  for (size_t i = 0; i + 4u <= encoded_size; ++i) {
    if (memcmp(encoded + i, "QBR1", 4) == 0) {
      qbr = i;
      break;
    }
  }
  if (!check(qbr != SIZE_MAX, "select block references"))
    goto done;
  if (!check(api_decode_rgba(encoded, encoded_size, &decoded) == QLIC_OK &&
                 decoded.width == width && decoded.height == height &&
                 memcmp(decoded.rgba, pixels, pixel_bytes) == 0,
             "round trip block references"))
    goto done;
  qlic_image_free(&decoded);
  size_t ref = qbr + 8u;
  for (size_t block = 0; block < 128u; ++block) {
    if (!check(ref < encoded_size && encoded[ref] == 0u,
               "parse block reference stream"))
      goto done;
    ref += 1u + 16u * 16u * 3u;
  }
  if (!check(ref + 1u < encoded_size && encoded[ref] == 7u,
             "locate first block reference"))
    goto done;
  damaged = (uint8_t *)malloc(encoded_size);
  if (!check(damaged != NULL, "allocate damaged block stream"))
    goto done;
  memcpy(damaged, encoded, encoded_size);
  damaged[ref + 1u] = 0u;
  size_t body = encoded_size - 4u;
  uint32_t checksum = crc32(damaged, body);
  damaged[body + 0u] = (uint8_t)checksum;
  damaged[body + 1u] = (uint8_t)(checksum >> 8);
  damaged[body + 2u] = (uint8_t)(checksum >> 16);
  damaged[body + 3u] = (uint8_t)(checksum >> 24);
  if (!check(api_decode_rgba(damaged, encoded_size, &decoded) ==
                 QLIC_BAD_DATA &&
                 decoded.rgba == NULL,
             "reject invalid block reference"))
    goto done;
  ok = 1;

done:
  qlic_image_free(&decoded);
  qlic_free(encoded);
  free(damaged);
  free(pixels);
  return ok;
}

static int animation_test(void) {
  const uint32_t width = 17u;
  const uint32_t height = 19u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *pixels_a = (uint8_t *)malloc(pixel_bytes);
  uint8_t *pixels_b = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded = NULL;
  uint8_t *encoded_whole = NULL;
  size_t encoded_size = 0;
  size_t encoded_whole_size = 0;
  qlic_frame frames[2] = {0};
  qlic_animation decoded = {0};
  qlic_animation limited = {0};
  qlic_decode_limits limits = {0};
  qlic_encode_options options;
  qlic_info info = {0};
  int ok = 0;

  if (!check(pixels_a != NULL && pixels_b != NULL, "allocate animation"))
    goto done;
  fill(pixels_a, width, height, 5u);
  fill(pixels_b, width, height, 29u);
  frames[0].image.width = width;
  frames[0].image.height = height;
  frames[0].image.rgba = pixels_a;
  frames[0].delay_ms = 33u;
  frames[1].image.width = width;
  frames[1].image.height = height;
  frames[1].image.rgba = pixels_b;
  frames[1].delay_ms = 67u;
  if (!check(api_encode_animation(frames, 2u, 4u, &encoded, &encoded_size) ==
                 QLIC_OK,
             "encode animation"))
    goto done;
  qlic_encode_options_default(&options);
  options.threads = UINT32_MAX;
  if (!check(api_encode_animation_options(
                 frames, 2u, 4u, &options, &encoded_whole,
                 &encoded_whole_size) == QLIC_OK &&
                 encoded_whole_size == encoded_size &&
                 memcmp(encoded_whole, encoded, encoded_size) == 0,
             "deterministic threaded animation"))
    goto done;
  if (!check(api_get_info(encoded, encoded_size, &info) == QLIC_OK &&
                 info.width == width && info.height == height &&
                 info.frame_count == 2u && info.animated == 1u,
             "read animation metadata"))
    goto done;
  if (!check(qlic_validate(encoded, encoded_size, NULL) == QLIC_OK,
             "validate animation without retaining frames"))
    goto done;
  if (!check(api_decode_animation(encoded, encoded_size, &decoded) == QLIC_OK,
             "decode animation"))
    goto done;
  if (!check(decoded.width == width && decoded.height == height &&
                 decoded.frame_count == 2u && decoded.loop_count == 4u &&
                 decoded.frames[0].delay_ms == 33u &&
                 decoded.frames[1].delay_ms == 67u,
             "preserve animation metadata"))
    goto done;
  if (!check(memcmp(decoded.frames[0].image.rgba, pixels_a, pixel_bytes) == 0 &&
                 memcmp(decoded.frames[1].image.rgba, pixels_b, pixel_bytes) ==
                     0,
             "preserve animation pixels"))
    goto done;
  qlic_decode_limits_default(&limits);
  limits.max_animation_bytes =
      (uint64_t)sizeof(qlic_frame) * 2u + (uint64_t)pixel_bytes * 2u;
  if (!check(qlic_decode_animation(
                 encoded, encoded_size, &limits, &limited) ==
                 QLIC_LIMIT_EXCEEDED &&
                 limited.frames == NULL,
             "bound parallel animation scratch memory"))
    goto done;
  ok = 1;

done:
  qlic_animation_free(&decoded);
  qlic_animation_free(&limited);
  qlic_free(encoded);
  qlic_free(encoded_whole);
  free(pixels_a);
  free(pixels_b);
  return ok;
}

static int animation_delta_test(void) {
  const uint32_t width = 128u;
  const uint32_t height = 128u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *base = (uint8_t *)malloc(pixel_bytes);
  uint8_t *changed = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded = NULL;
  uint8_t *key = NULL;
  uint8_t *last = NULL;
  uint8_t *damaged = NULL;
  size_t encoded_size = 0, key_size = 0, last_size = 0;
  qlic_animation decoded = {0};
  int ok = 0;

  if (!check(base && changed, "allocate delta animation"))
    goto done;
  fill(base, width, height, 41u);
  memcpy(changed, base, pixel_bytes);
  for (uint32_t y = 43u; y < 59u; ++y) {
    for (uint32_t x = 71u; x < 87u; ++x) {
      size_t p = ((size_t)y * width + x) * 4u;
      changed[p + 0u] ^= 0x5au;
      changed[p + 1u] ^= 0xa5u;
      changed[p + 2u] ^= 0x3cu;
      changed[p + 3u] = 255u;
    }
  }
  qlic_frame frames[3] = {
      {{width, height, base, 0u, 0u}, 11u},
      {{width, height, base, 0u, 0u}, 22u},
      {{width, height, changed, 0u, 0u}, 33u}};
  if (!check(api_encode_animation(frames, 3u, 7u, &encoded,
                                   &encoded_size) == QLIC_OK,
             "encode delta animation"))
    goto done;
  if (!check(encoded_size > 64u && memcmp(encoded + 28u, "QAN2", 4u) == 0,
             "select temporal animation stream"))
    goto done;
  size_t pos = 40u;
  if (!check(pos + 16u <= encoded_size && encoded[pos + 4u] == 0u,
             "write animation key frame"))
    goto done;
  uint64_t first_size = read64le(encoded + pos + 8u);
  if (!check(first_size <= SIZE_MAX &&
                 (size_t)first_size <= encoded_size - pos - 16u,
             "parse animation key frame"))
    goto done;
  pos += 16u + (size_t)first_size;
  if (!check(pos + 8u <= encoded_size && encoded[pos + 4u] == 1u,
             "write animation duplicate"))
    goto done;
  pos += 8u;
  if (!check(pos + 32u <= encoded_size && encoded[pos + 4u] == 2u,
             "write animation rectangle"))
    goto done;
  if (!check(api_decode_animation(encoded, encoded_size, &decoded) == QLIC_OK &&
                 decoded.frame_count == 3u && decoded.loop_count == 7u &&
                 decoded.frames[0].delay_ms == 11u &&
                 decoded.frames[1].delay_ms == 22u &&
                 decoded.frames[2].delay_ms == 33u &&
                 memcmp(decoded.frames[0].image.rgba, base, pixel_bytes) == 0 &&
                 memcmp(decoded.frames[1].image.rgba, base, pixel_bytes) == 0 &&
                 memcmp(decoded.frames[2].image.rgba, changed, pixel_bytes) == 0,
             "round trip temporal animation"))
    goto done;
  if (!check(api_encode_rgba(base, width, height, &key, &key_size) == QLIC_OK &&
                 api_encode_rgba(changed, width, height, &last, &last_size) ==
                     QLIC_OK &&
                 encoded_size <
                     28u + 12u + 16u * 3u + key_size * 2u + last_size + 4u,
             "reduce independent animation frames"))
    goto done;
  damaged = (uint8_t *)malloc(encoded_size);
  if (!check(damaged != NULL, "allocate damaged animation"))
    goto done;
  memcpy(damaged, encoded, encoded_size);
  damaged[44u] = 1u;
  uint32_t checksum = crc32(damaged, encoded_size - 4u);
  damaged[encoded_size - 4u] = (uint8_t)checksum;
  damaged[encoded_size - 3u] = (uint8_t)(checksum >> 8);
  damaged[encoded_size - 2u] = (uint8_t)(checksum >> 16);
  damaged[encoded_size - 1u] = (uint8_t)(checksum >> 24);
  qlic_animation_free(&decoded);
  if (!check(api_decode_animation(damaged, encoded_size, &decoded) ==
                 QLIC_BAD_DATA &&
                 decoded.frames == NULL,
             "reject dependent animation without key frame"))
    goto done;
  ok = 1;

done:
  qlic_animation_free(&decoded);
  qlic_free(encoded);
  qlic_free(key);
  qlic_free(last);
  free(damaged);
  free(changed);
  free(base);
  return ok;
}

static int animation_move_test(void) {
  const uint32_t width = 128u;
  const uint32_t height = 96u;
  const uint32_t sprite_width = 24u;
  const uint32_t sprite_height = 20u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *first = (uint8_t *)malloc(pixel_bytes);
  uint8_t *second = (uint8_t *)malloc(pixel_bytes);
  uint8_t *damaged = NULL;
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_animation decoded = {0};
  int ok = 0;

  if (!check(first && second, "allocate moving animation"))
    goto done;
  for (size_t p = 0; p < pixel_bytes; p += 4u) {
    first[p + 0u] = second[p + 0u] = 18u;
    first[p + 1u] = second[p + 1u] = 24u;
    first[p + 2u] = second[p + 2u] = 31u;
    first[p + 3u] = second[p + 3u] = 255u;
  }
  uint32_t state = UINT32_C(0x91e10da5);
  for (uint32_t y = 0; y < sprite_height; ++y) {
    for (uint32_t x = 0; x < sprite_width; ++x) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      uint8_t color[4] = {(uint8_t)state, (uint8_t)(state >> 8),
                          (uint8_t)(state >> 16), 255u};
      size_t a = ((size_t)(13u + y) * width + 11u + x) * 4u;
      size_t b = ((size_t)(47u + y) * width + 53u + x) * 4u;
      memcpy(first + a, color, 4u);
      memcpy(second + b, color, 4u);
    }
  }
  qlic_frame frames[2] = {
      {{width, height, first, 0u, 0u}, 17u},
      {{width, height, second, 0u, 0u}, 29u}};
  if (!check(api_encode_animation(frames, 2u, 0u, &encoded,
                                   &encoded_size) == QLIC_OK,
             "encode moving animation"))
    goto done;
  size_t pos = 40u;
  if (!check(encoded_size > pos + 16u && encoded[pos + 4u] == 0u,
             "parse moving animation key"))
    goto done;
  uint64_t first_size = read64le(encoded + pos + 8u);
  if (!check(first_size <= SIZE_MAX &&
                 (size_t)first_size <= encoded_size - pos - 16u,
             "parse moving animation key size"))
    goto done;
  pos += 16u + (size_t)first_size;
  if (!check(pos + 36u <= encoded_size && encoded[pos + 4u] == 3u,
             "select temporal move"))
    goto done;
  if (!check(api_decode_animation(encoded, encoded_size, &decoded) == QLIC_OK &&
                 decoded.frame_count == 2u &&
                 decoded.frames[0].delay_ms == 17u &&
                 decoded.frames[1].delay_ms == 29u &&
                 memcmp(decoded.frames[0].image.rgba, first, pixel_bytes) == 0 &&
                 memcmp(decoded.frames[1].image.rgba, second, pixel_bytes) == 0,
             "round trip temporal move"))
    goto done;
  damaged = (uint8_t *)malloc(encoded_size);
  if (!check(damaged != NULL, "allocate damaged move"))
    goto done;
  memcpy(damaged, encoded, encoded_size);
  memset(damaged + pos + 24u, 0xff, 4u);
  uint32_t checksum = crc32(damaged, encoded_size - 4u);
  damaged[encoded_size - 4u] = (uint8_t)checksum;
  damaged[encoded_size - 3u] = (uint8_t)(checksum >> 8);
  damaged[encoded_size - 2u] = (uint8_t)(checksum >> 16);
  damaged[encoded_size - 1u] = (uint8_t)(checksum >> 24);
  qlic_animation_free(&decoded);
  if (!check(api_decode_animation(damaged, encoded_size, &decoded) ==
                 QLIC_BAD_DATA &&
                 decoded.frames == NULL,
             "reject invalid temporal move"))
    goto done;
  ok = 1;

done:
  qlic_animation_free(&decoded);
  qlic_free(encoded);
  free(damaged);
  free(second);
  free(first);
  return ok;
}

static int boundary_test(void) {
  const uint32_t width = 11u;
  const uint32_t height = 7u;
  const size_t row = (size_t)width * 4u;
  const size_t stride = row + 13u;
  const size_t packed_size = row * height;
  const size_t strided_size = stride * (height - 1u) + row;
  uint8_t *packed = (uint8_t *)malloc(packed_size);
  uint8_t *strided = (uint8_t *)malloc(strided_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_image decoded = {0};
  qlic_animation decoded_animation = {0};
  qlic_decode_limits limits = {0};
  qlic_info info = {0};
  int ok = 0;

  if (!check(packed && strided, "allocate boundary image"))
    goto done;
  fill(packed, width, height, 17u);
  memset(strided, 0xa5, strided_size);
  for (uint32_t y = 0; y < height; ++y)
    memcpy(strided + (size_t)y * stride, packed + (size_t)y * row, row);
  if (!check(qlic_encode_rgba(strided, strided_size, width, height, stride,
                              NULL, &encoded, &encoded_size) == QLIC_OK,
             "encode sized strided image"))
    goto done;
  if (!check(api_decode_rgba(encoded, encoded_size, &decoded) == QLIC_OK &&
                 decoded.width == width && decoded.height == height &&
                 memcmp(decoded.rgba, packed, packed_size) == 0,
             "round trip sized strided image"))
    goto done;
  qlic_image_free(&decoded);
  uint8_t rejected_sentinel = 0;
  uint8_t *rejected = &rejected_sentinel;
  size_t rejected_size = 1u;
  if (!check(qlic_encode_rgba(strided, strided_size - 1u, width, height,
                              stride, NULL, &rejected,
                              &rejected_size) == QLIC_BAD_ARGUMENT &&
                 rejected == NULL && rejected_size == 0u,
             "reject undersized strided image"))
    goto done;
  qlic_decode_limits_default(&limits);
  if (!check(limits.struct_size == sizeof(limits) &&
                 !limits.threads &&
                 limits.max_file_bytes && limits.max_payload_bytes &&
                 limits.max_pixels && limits.max_animation_bytes &&
                 limits.max_frames,
             "provide decode limit defaults"))
    goto done;
  limits.threads = UINT32_MAX;
  if (!check(qlic_decode_rgba(encoded, encoded_size, &limits,
                              &decoded) == QLIC_OK &&
                 memcmp(decoded.rgba, packed, packed_size) == 0,
             "decode with scoped workers"))
    goto done;
  qlic_image_free(&decoded);
  limits.max_pixels = 1u;
  if (!check(qlic_decode_rgba(encoded, encoded_size, &limits,
                                          &decoded) == QLIC_LIMIT_EXCEEDED &&
                 decoded.rgba == NULL,
             "enforce decode pixel limit"))
    goto done;
  if (!check(qlic_get_info(encoded, encoded_size, &limits, &info) ==
                 QLIC_LIMIT_EXCEEDED &&
                 info.width == 0u,
             "enforce metadata pixel limit"))
    goto done;
  limits.max_pixels = 0u;
  if (!check(qlic_decode_rgba(encoded, encoded_size, &limits,
                                          &decoded) == QLIC_BAD_ARGUMENT,
             "reject invalid decode limits"))
    goto done;
  qlic_free(encoded);
  encoded = NULL;
  encoded_size = 0;
  qlic_frame_input inputs[2] = {
      {strided, strided_size, stride, width, height, 21u},
      {strided, strided_size, stride, width, height, 42u}};
  if (!check(qlic_encode_animation(inputs, 2u, 3u, NULL, &encoded,
                                   &encoded_size) == QLIC_OK,
             "encode sized strided animation"))
    goto done;
  if (!check(api_decode_animation(encoded, encoded_size,
                                   &decoded_animation) == QLIC_OK &&
                 decoded_animation.frame_count == 2u &&
                 decoded_animation.loop_count == 3u &&
                 decoded_animation.frames[0].delay_ms == 21u &&
                 decoded_animation.frames[1].delay_ms == 42u &&
                 memcmp(decoded_animation.frames[0].image.rgba, packed,
                        packed_size) == 0 &&
                 memcmp(decoded_animation.frames[1].image.rgba, packed,
                        packed_size) == 0,
             "round trip sized strided animation"))
    goto done;
  ok = 1;

done:
  qlic_animation_free(&decoded_animation);
  qlic_image_free(&decoded);
  qlic_free(encoded);
  free(strided);
  free(packed);
  return ok;
}

static int argument_test(void) {
  uint8_t sentinel = 0;
  uint8_t *data = &sentinel;
  size_t size = 1u;
  qlic_image image = {1u, 1u, &sentinel, 0u, 0u};
  qlic_info info = {1u, 1u, 1u, 1u};
  int ok = 1;
  ok &=
      check(api_encode_rgba(NULL, 1u, 1u, &data, &size) == QLIC_BAD_ARGUMENT &&
                data == NULL && size == 0u,
            "validate encoder arguments");
  ok &= check(api_decode_rgba(NULL, 0u, &image) == QLIC_BAD_ARGUMENT &&
                  image.rgba == NULL && image.width == 0u && image.height == 0u,
              "clear image output on failure");
  ok &= check(api_get_info(NULL, 0u, &info) == QLIC_BAD_ARGUMENT &&
                  info.width == 0u && info.height == 0u &&
                  info.frame_count == 0u && info.animated == 0u,
              "clear info output on failure");
  qlic_image_free(NULL);
  qlic_animation_free(NULL);
  qlic_free(NULL);
  return ok;
}

static int encode_options_test(void) {
  const uint32_t width = 23u;
  const uint32_t height = 17u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded = NULL;
  uint8_t *animation = NULL;
  size_t encoded_size = 0;
  size_t animation_size = 0;
  qlic_encode_options options;
  uint8_t rejected_sentinel = 0;
  qlic_frame frame = {{width, height, NULL, 0, 0}, 25u};
  int ok = 0;

  if (!check(pixels != NULL, "allocate options image"))
    goto done;
  fill(pixels, width, height, 73u);
  frame.image.rgba = pixels;
  qlic_encode_options_default(&options);
  options.threads = UINT32_MAX;
  if (!check(api_encode_rgba_options(pixels, width, height, &options, &encoded,
                                     &encoded_size) == QLIC_OK &&
                 encoded_size > 16u,
             "encode image with scoped workers"))
    goto done;
  if (!check(api_encode_animation_options(&frame, 1u, 0u, &options, &animation,
                                          &animation_size) == QLIC_OK &&
                 animation_size > 72u,
             "encode animation frames with scoped workers"))
    goto done;
  qlic_free(encoded);
  encoded = &rejected_sentinel;
  encoded_size = 1u;
  options.struct_size = 0u;
  if (!check(api_encode_rgba_options(pixels, width, height, &options, &encoded,
                                 &encoded_size) == QLIC_BAD_ARGUMENT &&
                 encoded == NULL && encoded_size == 0u,
             "reject short encode options"))
    goto done;
  qlic_encode_options_default(&options);
  options.flags = 1u;
  if (!check(api_encode_rgba_options(pixels, width, height, &options, &encoded,
                                 &encoded_size) == QLIC_BAD_ARGUMENT,
             "reject unknown encode option flags"))
    goto done;
  qlic_encode_options_default(&options);
  options.reserved = 1u;
  if (!check(api_encode_rgba_options(pixels, width, height, &options, &encoded,
                                     &encoded_size) == QLIC_BAD_ARGUMENT,
             "reject dirty encode option fields"))
    goto done;
  ok = 1;

done:
  qlic_free(encoded);
  qlic_free(animation);
  free(pixels);
  return ok;
}

typedef struct {
  const uint8_t *pixels;
  size_t pixel_bytes;
  uint32_t width;
  uint32_t height;
  const uint8_t *reference;
  size_t reference_size;
  int ok;
} thread_case;

#ifdef _WIN32
static DWORD WINAPI encode_thread(LPVOID parameter) {
#else
static void *encode_thread(void *parameter) {
#endif
  thread_case *test = (thread_case *)parameter;
  test->ok = 0;
  for (int pass = 0; pass < 2; ++pass) {
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    qlic_image decoded = {0};
    if (api_encode_rgba(test->pixels, test->width, test->height, &encoded,
                         &encoded_size) != QLIC_OK ||
        !encoded || encoded_size != test->reference_size ||
        memcmp(encoded, test->reference, encoded_size) != 0 ||
        api_decode_rgba(encoded, encoded_size, &decoded) != QLIC_OK ||
        !decoded.rgba || decoded.width != test->width ||
        decoded.height != test->height ||
        memcmp(decoded.rgba, test->pixels, test->pixel_bytes) != 0) {
      qlic_image_free(&decoded);
      qlic_free(encoded);
      return THREAD_RESULT;
    }
    qlic_image_free(&decoded);
    qlic_free(encoded);
  }
  test->ok = 1;
  return THREAD_RESULT;
}

static int concurrency_test(void) {
  const uint32_t width = 47u;
  const uint32_t height = 43u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
  uint8_t *reference = NULL;
  size_t reference_size = 0;
#ifdef _WIN32
  HANDLE handles[4] = {0};
#else
  pthread_t handles[4];
#endif
  thread_case cases[4] = {0};
  int created = 0;
  int joined = 0;
  int ok = 0;

  if (!check(pixels != NULL, "allocate concurrent image"))
    goto done;
  fill(pixels, width, height, 41u);
  if (!check(api_encode_rgba(pixels, width, height, &reference,
                              &reference_size) == QLIC_OK,
             "encode concurrent reference"))
    goto done;
  for (int i = 0; i < 4; ++i) {
    cases[i].pixels = pixels;
    cases[i].pixel_bytes = pixel_bytes;
    cases[i].width = width;
    cases[i].height = height;
    cases[i].reference = reference;
    cases[i].reference_size = reference_size;
#ifdef _WIN32
    handles[i] = CreateThread(NULL, 0, encode_thread, &cases[i], 0, NULL);
    if (!handles[i])
      break;
#else
    if (pthread_create(&handles[i], NULL, encode_thread, &cases[i]) != 0)
      break;
#endif
    ++created;
  }
  if (!check(created == 4, "create encoder threads"))
    goto done;
#ifdef _WIN32
  if (!check(WaitForMultipleObjects(4u, handles, TRUE, INFINITE) ==
                 WAIT_OBJECT_0,
             "wait for encoder threads"))
    goto done;
#else
  for (int i = 0; i < created; ++i) {
    if (pthread_join(handles[i], NULL) != 0)
      goto done;
    ++joined;
  }
#endif
  for (int i = 0; i < 4; ++i) {
    if (!check(cases[i].ok, "concurrent deterministic round trip"))
      goto done;
  }
  ok = 1;

done:
  for (int i = joined; i < created; ++i) {
#ifdef _WIN32
    if (handles[i]) {
      WaitForSingleObject(handles[i], INFINITE);
      CloseHandle(handles[i]);
    }
#else
    (void)pthread_join(handles[i], NULL);
#endif
  }
  qlic_free(reference);
  free(pixels);
  return ok;
}

static int thread_configuration_test(void) {
  uint32_t hardware = qlic_hardware_thread_count();
  qlic_encode_options options;
  qlic_encode_options_default(&options);
  if (!check(sizeof(options) == 16u, "keep encode options ABI size") ||
      !check(hardware >= 1u, "detect hardware threads") ||
      !check(options.struct_size == sizeof(options) && !options.flags &&
                 !options.threads && !options.reserved,
             "provide stable encode defaults") ||
      !check(strcmp(qlic_status_string(QLIC_OK), "success") == 0 &&
                 strcmp(qlic_status_string(QLIC_BAD_DATA), "bad data") == 0 &&
                 strcmp(qlic_status_string(99), "unknown status") == 0,
             "describe status values"))
    return 0;
  return 1;
}

static uint32_t wide_value(uint32_t x, uint32_t y, uint32_t channel,
                           uint32_t bits) {
  uint32_t maximum = (UINT32_C(1) << bits) - 1u;
  if (x == 0u && y == 0u)
    return channel & 1u ? maximum : 0u;
  if (x == 1u && y == 0u)
    return channel & 1u ? maximum - 1u : 1u;
  uint32_t value = x * 977u + y * 6151u + channel * 7919u +
                   (x ^ (y * 13u)) * 37u;
  return value & maximum;
}

static int wide_roundtrip_case(uint32_t bits, uint32_t channels,
                               uint32_t width, uint32_t height) {
  size_t storage = bits <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)width * channels * storage;
  size_t stride = row_bytes + storage * 3u;
  size_t pixels_size = (size_t)(height - 1u) * stride + row_bytes;
  uint8_t *pixels = (uint8_t *)malloc(pixels_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_wide_image decoded = {0};
  qlic_info ordinary_info = {0};
  qlic_info_ex info = {0};
  qlic_image reduced = {0};
  int ok = 0;
  if (!check(pixels != NULL, "allocate wide pixels"))
    goto done;
  memset(pixels, 0xa5, pixels_size);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t *row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      for (uint32_t channel = 0; channel < channels; ++channel) {
        uint32_t value = wide_value(x, y, channel, bits);
        memcpy(row + ((size_t)x * channels + channel) * storage,
               &value, storage);
      }
    }
  }
  if (!check(qlic_encode_wide(
                 pixels, pixels_size, width, height, stride, channels, bits,
                 NULL, &encoded, &encoded_size) == QLIC_OK,
             "encode wide image") ||
      !check(encoded_size > 48u && encoded[12] == 19u &&
                 encoded[14] == bits && read32le(encoded + 16u) == channels &&
                 memcmp(encoded + 28u, "QSW1", 4u) == 0,
             "write wide framing") ||
      !check(qlic_get_info(encoded, encoded_size, NULL, &ordinary_info) ==
                     QLIC_OK &&
                 ordinary_info.width == width &&
                 ordinary_info.height == height &&
                 ordinary_info.frame_count == 1u && !ordinary_info.animated,
             "read wide legacy info"))
    goto done;
  info.struct_size = sizeof(info);
  if (!check(qlic_get_info_ex(encoded, encoded_size, NULL, &info) == QLIC_OK &&
                 info.struct_size == sizeof(info) && info.width == width &&
                 info.height == height && info.channels == channels &&
                 info.bits_per_sample == bits && info.frame_count == 1u &&
                 !info.animated,
             "read extended wide info") ||
      !check(qlic_validate(encoded, encoded_size, NULL) == QLIC_OK,
             "validate wide image without retaining samples") ||
      !check(qlic_decode_rgba(encoded, encoded_size, NULL, &reduced) ==
                     QLIC_UNSUPPORTED_FORMAT &&
                 reduced.rgba == NULL,
             "reject silent wide downconversion") ||
      !check(qlic_decode_wide(encoded, encoded_size, NULL, &decoded) ==
                     QLIC_OK &&
                 decoded.width == width && decoded.height == height &&
                 decoded.channels == channels &&
                 decoded.bits_per_sample == bits &&
                 decoded.stride == row_bytes &&
                 decoded.pixels_size == row_bytes * height,
             "decode wide image"))
    goto done;
  for (uint32_t y = 0; y < height; ++y) {
    if (!check(memcmp((const uint8_t *)decoded.pixels + (size_t)y * row_bytes,
                      pixels + (size_t)y * stride, row_bytes) == 0,
               "preserve exact wide samples"))
      goto done;
  }
  qlic_wide_image_free(&decoded);

  qlic_decode_limits parallel_limits;
  qlic_decode_limits_default(&parallel_limits);
  parallel_limits.threads = 3u;
  if (!check(qlic_decode_wide(encoded, encoded_size, &parallel_limits,
                              &decoded) == QLIC_OK,
             "decode wide slices in parallel"))
    goto done;
  for (uint32_t y = 0; y < height; ++y) {
    if (!check(memcmp((const uint8_t *)decoded.pixels + (size_t)y * row_bytes,
                      pixels + (size_t)y * stride, row_bytes) == 0,
               "preserve exact parallel wide samples"))
      goto done;
  }
  qlic_wide_image_free(&decoded);

  qlic_decode_limits limits;
  qlic_decode_limits_default(&limits);
  limits.max_payload_bytes = row_bytes * height - 1u;
  if (!check(qlic_decode_wide(encoded, encoded_size, &limits, &decoded) ==
                     QLIC_LIMIT_EXCEEDED &&
                 decoded.pixels == NULL,
             "enforce wide decoded-byte limit"))
    goto done;
  uint8_t *damaged = (uint8_t *)malloc(encoded_size);
  if (!check(damaged != NULL, "allocate wide corruption probe"))
    goto done;
  memcpy(damaged, encoded, encoded_size);
  damaged[32] = 1u;
  write32le(damaged + encoded_size - 4u,
            crc32(damaged, encoded_size - 4u));
  int damaged_result =
      qlic_decode_wide(damaged, encoded_size, NULL, &decoded);
  free(damaged);
  if (!check(damaged_result == QLIC_BAD_DATA && decoded.pixels == NULL,
             "reject invalid QSW method"))
    goto done;

  if (bits != 16u) {
    uint32_t invalid = UINT32_C(1) << bits;
    memcpy(pixels, &invalid, storage);
    uint8_t *invalid_encoded = NULL;
    size_t invalid_size = 0;
    int invalid_result = qlic_encode_wide(
        pixels, pixels_size, width, height, stride, channels, bits, NULL,
        &invalid_encoded, &invalid_size);
    qlic_free(invalid_encoded);
    if (!check(invalid_result == QLIC_BAD_ARGUMENT && !invalid_size,
               "reject sample above declared precision"))
      goto done;
  }
  ok = 1;

done:
  qlic_image_free(&reduced);
  qlic_wide_image_free(&decoded);
  qlic_free(encoded);
  free(pixels);
  return ok;
}

static int wide_image_test(void) {
  static const struct {
    uint8_t bits;
    uint8_t channels;
    uint8_t width;
    uint8_t height;
  } cases[] = {
      {9, 1, 1, 1},   {10, 3, 1, 17}, {12, 4, 17, 1},
      {16, 3, 19, 11}, {17, 4, 9, 7}, {20, 1, 13, 15},
      {24, 4, 21, 9}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!wide_roundtrip_case(cases[i].bits, cases[i].channels,
                             cases[i].width, cases[i].height))
      return 0;
  }
  return 1;
}

static int described_sdr_test(void) {
  enum { WIDTH = 7, HEIGHT = 5, CHANNELS = 3, BITS = 8 };
  uint16_t pixels[WIDTH * HEIGHT * CHANNELS];
  for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); ++i)
    pixels[i] = (uint16_t)(i * 37u & 255u);
  qlic_hdr_image input = {0};
  input.struct_size = sizeof(input);
  input.width = WIDTH;
  input.height = HEIGHT;
  input.channels = CHANNELS;
  input.bits_per_sample = BITS;
  input.sample_type = QLIC_SAMPLE_UINT;
  input.alpha_mode = QLIC_ALPHA_NONE;
  input.color_authority = QLIC_COLOR_CICP;
  input.pixels = pixels;
  input.pixels_size = sizeof(pixels);
  input.stride = WIDTH * CHANNELS * sizeof(uint16_t);
  input.has_cicp = 1u;
  input.cicp.color_primaries = 1u;
  input.cicp.transfer_characteristics = 13u;
  input.cicp.matrix_coefficients = 0u;
  input.cicp.full_range = 1u;
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  qlic_hdr_image decoded = {0};
  uint8_t *wide = NULL;
  size_t wide_size = 0;
  int ok =
      check(qlic_encode_wide(pixels, sizeof(pixels), WIDTH, HEIGHT,
                             input.stride, CHANNELS, BITS, NULL, &wide,
                             &wide_size) == QLIC_BAD_ARGUMENT &&
                wide == NULL && wide_size == 0u,
            "keep plain wide precision at 9 to 24 bits") &&
      check(qlic_encode_hdr(&input, NULL, &encoded, &encoded_size) == QLIC_OK &&
                encoded_size > 64u && encoded[12] == 20u && encoded[14] == 8u,
            "encode self-describing 8-bit image");
  qlic_free(wide);
  if (ok) {
    decoded.struct_size = sizeof(decoded);
    ok = check(qlic_decode_hdr(encoded, encoded_size, NULL, &decoded) ==
                       QLIC_OK &&
                   decoded.bits_per_sample == BITS &&
                   decoded.channels == CHANNELS &&
                   decoded.has_cicp &&
                   decoded.cicp.color_primaries == 1u &&
                   decoded.cicp.transfer_characteristics == 13u &&
                   decoded.pixels_size == sizeof(pixels) &&
                   memcmp(decoded.pixels, pixels, sizeof(pixels)) == 0,
               "decode exact self-describing 8-bit image");
  }
  qlic_hdr_image_free(&decoded);
  qlic_free(encoded);
  return ok;
}

static int hdr_image_test(void) {
  enum { WIDTH = 23, HEIGHT = 11, CHANNELS = 4, BITS = 12 };
  const size_t row_bytes = WIDTH * CHANNELS * sizeof(uint16_t);
  const size_t stride = row_bytes + 10u;
  const size_t pixels_size = (HEIGHT - 1u) * stride + row_bytes;
  uint8_t *pixels = (uint8_t *)malloc(pixels_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  static uint8_t icc[] = {0x00, 0x00, 0x00, 0x10, 'a', 'c', 's', 'p',
                          'Q',  'L',  'I',  'C',  0x20, 0x26, 0x08, 0x14};
  static uint8_t exif[] = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0};
  static uint8_t xmp[] =
      "<x:xmpmeta><rdf:Description dc:creator=\"QLIC\"/></x:xmpmeta>";
  static uint8_t iptc[] = {0x1c, 0x02, 0x05, 0x00, 0x04,
                           'Q',  'L',  'I',  'C'};
  static uint8_t jumb[] = {0, 0, 0, 12, 'j', 'u', 'm', 'b', 0, 0, 0, 0};
  qlic_metadata_block metadata[4] = {0};
  qlic_hdr_image input = {0};
  qlic_hdr_image decoded = {0};
  qlic_info_v2 info = {0};
  qlic_wide_image legacy = {0};
  int ok = 0;
  if (!check(pixels != NULL, "allocate HDR pixels"))
    goto done;
  memset(pixels, 0xa5, pixels_size);
  for (uint32_t y = 0; y < HEIGHT; ++y) {
    uint8_t *row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < WIDTH; ++x) {
      for (uint32_t channel = 0; channel < CHANNELS; ++channel) {
        uint16_t value = (uint16_t)wide_value(x, y, channel, BITS);
        memcpy(row + ((size_t)x * CHANNELS + channel) * sizeof(value),
               &value, sizeof(value));
      }
    }
  }
  input.struct_size = sizeof(input);
  input.width = WIDTH;
  input.height = HEIGHT;
  input.channels = CHANNELS;
  input.bits_per_sample = BITS;
  input.sample_type = QLIC_SAMPLE_UINT;
  input.alpha_mode = QLIC_ALPHA_STRAIGHT;
  input.color_authority = QLIC_COLOR_ICC_PREFERRED;
  input.pixels = pixels;
  input.pixels_size = pixels_size;
  input.stride = stride;
  input.icc = icc;
  input.icc_size = sizeof(icc);
  input.has_cicp = 1u;
  input.cicp.color_primaries = 9u;
  input.cicp.transfer_characteristics = 16u;
  input.cicp.matrix_coefficients = 0u;
  input.cicp.full_range = 1u;
  input.has_mastering_display = 1u;
  input.mastering_display.primary_x[0] = 35400u;
  input.mastering_display.primary_y[0] = 14600u;
  input.mastering_display.primary_x[1] = 8500u;
  input.mastering_display.primary_y[1] = 39850u;
  input.mastering_display.primary_x[2] = 6550u;
  input.mastering_display.primary_y[2] = 2300u;
  input.mastering_display.white_x = 15635u;
  input.mastering_display.white_y = 16450u;
  input.mastering_display.max_luminance = 10000000u;
  input.mastering_display.min_luminance = 50u;
  input.has_content_light = 1u;
  input.content_light.max_cll = 1000u;
  input.content_light.max_fall = 400u;
  memcpy(metadata[0].tag, "EXIF", 4u);
  metadata[0].data = exif;
  metadata[0].size = sizeof(exif);
  memcpy(metadata[1].tag, "XMP_", 4u);
  metadata[1].data = xmp;
  metadata[1].size = sizeof(xmp) - 1u;
  memcpy(metadata[2].tag, "IPTC", 4u);
  metadata[2].data = iptc;
  metadata[2].size = sizeof(iptc);
  memcpy(metadata[3].tag, "JUMB", 4u);
  metadata[3].data = jumb;
  metadata[3].size = sizeof(jumb);
  input.metadata = metadata;
  input.metadata_count = 4u;
  memcpy(metadata[0].tag, "PIXL", 4u);
  if (!check(qlic_encode_hdr(&input, NULL, &encoded, &encoded_size) ==
                     QLIC_BAD_ARGUMENT &&
                 encoded == NULL && encoded_size == 0u,
             "reject reserved metadata block tags"))
    goto done;
  memcpy(metadata[0].tag, "EXIF", 4u);
  if (!check(qlic_encode_hdr(&input, NULL, &encoded, &encoded_size) ==
                     QLIC_OK,
             "encode self-describing HDR") ||
      !check(encoded_size > 100u && encoded[12] == 20u &&
                 encoded[14] == BITS && read32le(encoded + 16u) == CHANNELS &&
                 memcmp(encoded + 28u, "QSW2", 4u) == 0,
             "write QSW2 framing"))
    goto done;
  info.struct_size = sizeof(info);
  if (!check(qlic_get_info_v2(encoded, encoded_size, NULL, &info) == QLIC_OK &&
                 info.width == WIDTH && info.height == HEIGHT &&
                 info.channels == CHANNELS && info.bits_per_sample == BITS &&
                 info.sample_type == QLIC_SAMPLE_UINT &&
                 info.alpha_mode == QLIC_ALPHA_STRAIGHT &&
                 info.color_authority == QLIC_COLOR_ICC_PREFERRED &&
                 info.has_icc && info.has_cicp &&
                 info.has_mastering_display && info.has_content_light &&
                 info.metadata_count == 4u,
             "read HDR metadata info") ||
      !check(qlic_validate(encoded, encoded_size, NULL) == QLIC_OK,
             "validate HDR image without retaining samples") ||
      !check(qlic_decode_wide(encoded, encoded_size, NULL, &legacy) ==
                     QLIC_UNSUPPORTED_FORMAT,
             "reject metadata loss through legacy wide API"))
    goto done;
  decoded.struct_size = sizeof(decoded);
  if (!check(qlic_decode_hdr(encoded, encoded_size, NULL, &decoded) == QLIC_OK,
             "decode self-describing HDR") ||
      !check(decoded.width == WIDTH && decoded.height == HEIGHT &&
                 decoded.channels == CHANNELS &&
                 decoded.bits_per_sample == BITS &&
                 decoded.alpha_mode == QLIC_ALPHA_STRAIGHT &&
                 decoded.color_authority == QLIC_COLOR_ICC_PREFERRED &&
                 decoded.stride == row_bytes &&
                 decoded.pixels_size == row_bytes * HEIGHT &&
                 decoded.icc_size == sizeof(icc) &&
                 memcmp(decoded.icc, icc, sizeof(icc)) == 0 &&
                 decoded.has_cicp &&
                 decoded.cicp.color_primaries == 9u &&
                 decoded.cicp.transfer_characteristics == 16u &&
                 decoded.cicp.full_range == 1u &&
                 decoded.has_mastering_display &&
                 decoded.mastering_display.max_luminance == 10000000u &&
                 decoded.has_content_light &&
                 decoded.content_light.max_cll == 1000u &&
                 decoded.metadata_count == 4u &&
                 memcmp(decoded.metadata[0].tag, "EXIF", 4u) == 0 &&
                 decoded.metadata[0].size == sizeof(exif) &&
                 memcmp(decoded.metadata[0].data, exif, sizeof(exif)) == 0 &&
                 memcmp(decoded.metadata[1].tag, "XMP_", 4u) == 0 &&
                 decoded.metadata[1].size == sizeof(xmp) - 1u &&
                 memcmp(decoded.metadata[1].data, xmp,
                        sizeof(xmp) - 1u) == 0 &&
                 memcmp(decoded.metadata[2].tag, "IPTC", 4u) == 0 &&
                 decoded.metadata[2].size == sizeof(iptc) &&
                 memcmp(decoded.metadata[2].data, iptc, sizeof(iptc)) == 0 &&
                 memcmp(decoded.metadata[3].tag, "JUMB", 4u) == 0 &&
                 decoded.metadata[3].size == sizeof(jumb) &&
                 memcmp(decoded.metadata[3].data, jumb, sizeof(jumb)) == 0,
             "preserve HDR descriptors"))
    goto done;
  for (uint32_t y = 0; y < HEIGHT; ++y) {
    if (!check(memcmp((uint8_t *)decoded.pixels + (size_t)y * row_bytes,
                      pixels + (size_t)y * stride, row_bytes) == 0,
               "preserve exact HDR samples"))
      goto done;
  }
  uint8_t *legacy_hdr_storage =
      (uint8_t *)calloc(1u, QLIC_HDR_IMAGE_V1_SIZE);
  qlic_hdr_image *legacy_hdr = (qlic_hdr_image *)(void *)legacy_hdr_storage;
  if (!check(legacy_hdr != NULL, "allocate legacy HDR ABI probe"))
    goto done;
  const uint32_t legacy_hdr_size = QLIC_HDR_IMAGE_V1_SIZE;
  memcpy(legacy_hdr_storage + offsetof(qlic_hdr_image, struct_size),
         &legacy_hdr_size, sizeof(legacy_hdr_size));
  int legacy_hdr_result =
      qlic_decode_hdr(encoded, encoded_size, NULL, legacy_hdr);
  void *legacy_pixels = NULL;
  uint8_t *legacy_icc = NULL;
  memcpy(&legacy_pixels,
         legacy_hdr_storage + offsetof(qlic_hdr_image, pixels),
         sizeof(legacy_pixels));
  memcpy(&legacy_icc, legacy_hdr_storage + offsetof(qlic_hdr_image, icc),
         sizeof(legacy_icc));
  int legacy_hdr_ok = legacy_hdr_result == QLIC_UNSUPPORTED_FORMAT &&
                      legacy_pixels == NULL && legacy_icc == NULL;
  qlic_hdr_image_free(legacy_hdr);
  free(legacy_hdr_storage);
  if (!check(legacy_hdr_ok, "reject metadata loss through legacy HDR ABI"))
    goto done;
  qlic_hdr_image_free(&decoded);
  decoded.struct_size = sizeof(decoded);
  qlic_decode_limits_v2 limits;
  qlic_decode_limits_v2_default(&limits);
  limits.max_metadata_bytes = sizeof(icc) - 1u;
  if (!check(qlic_decode_hdr(encoded, encoded_size, &limits, &decoded) ==
                     QLIC_LIMIT_EXCEEDED &&
                 decoded.pixels == NULL && decoded.icc == NULL,
             "enforce independent HDR metadata limit"))
    goto done;

  uint8_t *damaged = (uint8_t *)malloc(encoded_size);
  if (!check(damaged != NULL, "allocate QSW2 corruption probe"))
    goto done;
  memcpy(damaged, encoded, encoded_size);
  memcpy(damaged + 28u + 32u, "UNKN", 4u);
  damaged[28u + 32u + 4u] = 1u;
  write32le(damaged + encoded_size - 4u,
            crc32(damaged, encoded_size - 4u));
  decoded.struct_size = sizeof(decoded);
  int damaged_result =
      qlic_decode_hdr(damaged, encoded_size, NULL, &decoded);
  free(damaged);
  if (!check(damaged_result == QLIC_BAD_DATA && decoded.pixels == NULL,
             "reject unknown critical QSW2 chunks"))
    goto done;
  ok = 1;

done:
  qlic_wide_image_free(&legacy);
  qlic_hdr_image_free(&decoded);
  qlic_free(encoded);
  free(pixels);
  return ok;
}

typedef struct {
  const uint8_t *source;
  uint32_t width;
  uint32_t height;
  uint32_t rows;
  uint64_t last_progress;
  uint64_t progress_total;
  int progress_valid;
  int cancel_now;
  uint32_t cancel_after_rows;
} DeliveryProbe;

static int QLIC_CALL delivery_progress(void *user, uint64_t completed,
                                       uint64_t total) {
  DeliveryProbe *probe = (DeliveryProbe *)user;
  if (!probe || !total || completed > total ||
      (probe->progress_total && total != probe->progress_total) ||
      completed < probe->last_progress) {
    if (probe)
      probe->progress_valid = 0;
    return 0;
  }
  probe->progress_total = total;
  probe->last_progress = completed;
  return 1;
}

static int QLIC_CALL delivery_cancelled(void *user) {
  DeliveryProbe *probe = (DeliveryProbe *)user;
  return probe && probe->cancel_now;
}

static int QLIC_CALL delivery_row(void *user, uint32_t row,
                                  const uint8_t *rgba, size_t row_bytes) {
  DeliveryProbe *probe = (DeliveryProbe *)user;
  if (!probe || row != probe->rows || row >= probe->height ||
      row_bytes != (size_t)probe->width * 4u ||
      memcmp(rgba, probe->source + (size_t)row * row_bytes, row_bytes) != 0)
    return 0;
  ++probe->rows;
  return !probe->cancel_after_rows || probe->rows < probe->cancel_after_rows;
}

static int delivery_api_test(void) {
  /* Crosses 1K and power-of-two tile boundaries. */
  enum { WIDTH = 1025, HEIGHT = 769, RX = 477, RY = 353, RW = 131, RH = 97 };
  size_t source_size = (size_t)WIDTH * HEIGHT * 4u;
  uint8_t *source = (uint8_t *)malloc(source_size);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  size_t region_row = (size_t)RW * 4u;
  size_t region_stride = region_row + 11u;
  size_t region_size = (size_t)(RH - 1u) * region_stride + region_row;
  uint8_t *region_pixels = (uint8_t *)malloc(region_size);
  int ok = 0;
  if (!check(source && region_pixels, "allocate delivery API images"))
    goto done;
  fill(source, WIDTH, HEIGHT, 91u);
  memset(region_pixels, 0xa5, region_size);
  if (!check(api_encode_rgba(source, WIDTH, HEIGHT, &encoded, &encoded_size) ==
                 QLIC_OK,
             "encode delivery API source"))
    goto done;

  qlic_region region = {RX, RY, RW, RH};
  qlic_pixel_buffer destination;
  memset(&destination, 0, sizeof(destination));
  destination.struct_size = sizeof(destination);
  destination.format = QLIC_PIXELS_RGBA8;
  destination.pixels = region_pixels;
  destination.pixels_size = region_size;
  destination.stride = region_stride;
  DeliveryProbe probe;
  memset(&probe, 0, sizeof(probe));
  probe.progress_valid = 1;
  qlic_decode_observer observer;
  memset(&observer, 0, sizeof(observer));
  observer.struct_size = sizeof(observer);
  observer.progress = delivery_progress;
  observer.cancelled = delivery_cancelled;
  observer.user = &probe;
  if (!check(qlic_decode_region_rgba(encoded, encoded_size, NULL, &region,
                                     &observer, &destination) == QLIC_OK &&
                 destination.width == RW && destination.height == RH &&
                 probe.progress_valid &&
                 probe.last_progress == probe.progress_total,
             "decode exact region with progress"))
    goto done;
  for (uint32_t y = 0; y < RH; ++y) {
    const uint8_t *expected =
        source + ((size_t)(RY + y) * WIDTH + RX) * 4u;
    if (!check(memcmp(region_pixels + (size_t)y * region_stride, expected,
                      region_row) == 0,
               "preserve exact region rows"))
      goto done;
  }

  memset(&probe, 0, sizeof(probe));
  probe.source = source;
  probe.width = WIDTH;
  probe.height = HEIGHT;
  probe.progress_valid = 1;
  observer.user = &probe;
  if (!check(qlic_decode_rows_rgba(encoded, encoded_size, NULL, &observer,
                                   delivery_row, &probe) == QLIC_OK &&
                 probe.rows == HEIGHT && probe.progress_valid &&
                 probe.last_progress == probe.progress_total,
             "deliver exact validated rows"))
    goto done;

  memset(&probe, 0, sizeof(probe));
  probe.source = source;
  probe.width = WIDTH;
  probe.height = HEIGHT;
  probe.progress_valid = 1;
  probe.cancel_now = 1;
  observer.user = &probe;
  destination.width = 99u;
  destination.height = 99u;
  if (!check(qlic_decode_region_rgba(encoded, encoded_size, NULL, &region,
                                     &observer, &destination) ==
                     QLIC_CANCELLED &&
                 destination.width == 0u && destination.height == 0u,
             "cancel region before decode"))
    goto done;

  memset(&probe, 0, sizeof(probe));
  probe.source = source;
  probe.width = WIDTH;
  probe.height = HEIGHT;
  probe.progress_valid = 1;
  probe.cancel_after_rows = 3u;
  observer.user = &probe;
  if (!check(qlic_decode_rows_rgba(encoded, encoded_size, NULL, &observer,
                                   delivery_row, &probe) == QLIC_CANCELLED &&
                 probe.rows == 3u,
             "cancel row delivery cooperatively"))
    goto done;
  ok = 1;

done:
  qlic_free(encoded);
  free(region_pixels);
  free(source);
  return ok;
}

int main(void) {
  int ok = capabilities_test();
  ok &= thread_configuration_test();
  ok &= argument_test();
  ok &= encode_options_test();
  ok &= alpha_edge_test();
  ok &= still_image_test();
  ok &= pixel_formats_test();
  ok &= pixel_input_test();
  ok &= wide_image_test();
  ok &= described_sdr_test();
  ok &= hdr_image_test();
  ok &= delivery_api_test();
  ok &= block_reference_test();
  ok &= animation_test();
  ok &= animation_delta_test();
  ok &= animation_move_test();
  ok &= boundary_test();
  ok &= concurrency_test();
  if (!ok)
    return 1;
  printf("QLIC %s API tests passed\n", qlic_version());
  return 0;
}
