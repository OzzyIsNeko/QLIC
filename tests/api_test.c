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

static int still_image_test(void) {
  const uint32_t width = 73u;
  const uint32_t height = 61u;
  const size_t pixel_bytes = (size_t)width * height * 4u;
  uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
  uint8_t *encoded_a = NULL;
  uint8_t *encoded_b = NULL;
  uint8_t *damaged = NULL;
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
  if (!check(api_decode_rgba(encoded_a, size_a, &decoded) == QLIC_OK,
             "decode image"))
    goto done;
  if (!check(decoded.width == width && decoded.height == height &&
                 decoded.stride == (size_t)width * 4u &&
                 decoded.rgba_size == pixel_bytes &&
                 memcmp(decoded.rgba, pixels, pixel_bytes) == 0,
             "lossless round trip"))
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
  free(pixels);
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
  uint8_t *rejected = (uint8_t *)1;
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
  uint8_t *data = (uint8_t *)1;
  size_t size = 1u;
  qlic_image image = {1u, 1u, (uint8_t *)1, 0u, 0u};
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
  encoded = (uint8_t *)1;
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
  if (!check(hardware >= 1u, "detect hardware threads") ||
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

int main(void) {
  int ok = thread_configuration_test();
  ok &= argument_test();
  ok &= encode_options_test();
  ok &= still_image_test();
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
