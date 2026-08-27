#include <qlic/qlic.h>

#include "wide_fixture_data.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  uint32_t width;
  uint32_t height;
  uint32_t frames;
  uint32_t loop;
  uint32_t delay[2];
  uint32_t crc[2];
} Fixture;

static const Fixture fixtures[] = {
    {"animation.qlic",
     2,
     2,
     2,
     0,
     {40, 70},
     {UINT32_C(0xbc5aab1c), UINT32_C(0xd6538a42)}},
    {"blocks.qlic", 1, 17, 1, 0, {0, 0}, {UINT32_C(0x5f370f87), 0}},
    {"cpalette-lzms.qlic", 512, 512, 1, 0, {0, 0}, {UINT32_C(0xe1d7051e), 0}},
    {"gray-model-lzms.qlic", 384, 256, 1, 0, {0, 0}, {UINT32_C(0x3bf96fd4), 0}},
    {"gray-rle.qlic", 3, 31, 1, 0, {0, 0}, {UINT32_C(0xd59a4f1e), 0}},
    {"native.qlic", 64, 64, 1, 0, {0, 0}, {UINT32_C(0xb385d194), 0}},
    {"normal-map-quadratic.qlic",
     128,
     256,
     1,
     0,
     {0, 0},
     {UINT32_C(0x9122c65a), 0}},
    {"planar-med-lzms.qlic", 3, 2, 1, 0, {0, 0}, {UINT32_C(0xd04203d4), 0}},
    {"palette.qlic", 17, 1, 1, 0, {0, 0}, {UINT32_C(0x5f370f87), 0}},
    {"palette-filtered.qlic", 9, 257, 1, 0, {0, 0}, {UINT32_C(0x2b1e5d73), 0}},
    {"rgb-lzms.qlic", 257, 9, 1, 0, {0, 0}, {UINT32_C(0x10528ad2), 0}},
    {"separable.qlic", 1, 1, 1, 0, {0, 0}, {UINT32_C(0x0c463091), 0}},
    {"tile-model.qlic", 31, 3, 1, 0, {0, 0}, {UINT32_C(0xdb03a251), 0}},
    {"tile-palette-lzms.qlic",
     542,
     699,
     1,
     0,
     {0, 0},
     {UINT32_C(0x55533a44), 0}}};

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
}

static uint32_t read32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static void write32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8u);
  p[2] = (uint8_t)(v >> 16u);
  p[3] = (uint8_t)(v >> 24u);
}

static void write64le(uint8_t *p, uint64_t v) {
  write32le(p, (uint32_t)v);
  write32le(p + 4, (uint32_t)(v >> 32u));
}

static int read_file(const char *name, uint8_t **data, size_t *size) {
  char path[1024];
  int length = snprintf(path, sizeof(path), "%s/%s", QLIC_FIXTURE_DIR, name);
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
  long end = ok ? ftell(file) : -1;
  if (end <= 0 || fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *bytes = ok ? (uint8_t *)malloc((size_t)end) : NULL;
  if (!bytes)
    ok = 0;
  if (ok && fread(bytes, 1, (size_t)end, file) != (size_t)end)
    ok = 0;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok) {
    free(bytes);
    return 0;
  }
  *data = bytes;
  *size = (size_t)end;
  return 1;
}

static int run_fixture(const Fixture *fixture) {
  /* Archived streams define v1 read compatibility. */
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(fixture->name, &data, &size)) {
    fprintf(stderr, "could not read fixture: %s\n", fixture->name);
    return 0;
  }
  qlic_info info = {0};
  qlic_animation animation = {0};
  uint32_t animated = fixture->frames > 1u ? 1u : 0u;
  int ok = qlic_get_info(data, size, NULL, &info) == QLIC_OK &&
           info.width == fixture->width && info.height == fixture->height &&
           info.frame_count == fixture->frames && info.animated == animated &&
           qlic_decode_animation(data, size, NULL, &animation) == QLIC_OK &&
           animation.width == fixture->width &&
           animation.height == fixture->height &&
           animation.frame_count == fixture->frames &&
           animation.loop_count == fixture->loop;
  if (ok) {
    for (uint32_t i = 0; i < fixture->frames; ++i) {
      qlic_frame *frame = &animation.frames[i];
      size_t bytes = (size_t)fixture->width * fixture->height * 4u;
      if (frame->image.width != fixture->width ||
          frame->image.height != fixture->height ||
          frame->delay_ms != fixture->delay[i] ||
          crc32(frame->image.rgba, bytes) != fixture->crc[i]) {
        ok = 0;
        break;
      }
    }
  }
  if (!ok)
    fprintf(stderr, "compatibility failure: %s: %s\n", fixture->name,
            qlic_last_error());
  qlic_animation_free(&animation);
  free(data);
  return ok;
}

static int run_wide_fixture(const WideFixture *fixture) {
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(fixture->name, &data, &size)) {
    fprintf(stderr, "could not read fixture: %s\n", fixture->name);
    return 0;
  }
  size_t storage =
      fixture->bits_per_sample <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)fixture->width * fixture->channels * storage;
  size_t pixels_size = fixture->sample_count * storage;
  qlic_info_ex info = {0};
  info.struct_size = sizeof(info);
  qlic_wide_image image = {0};
  qlic_image reduced = {0};
  int ok =
      qlic_get_info_ex(data, size, NULL, &info) == QLIC_OK &&
      info.width == fixture->width && info.height == fixture->height &&
      info.frame_count == 1u && info.animated == 0u &&
      info.channels == fixture->channels &&
      info.bits_per_sample == fixture->bits_per_sample &&
      qlic_decode_rgba(data, size, NULL, &reduced) == QLIC_UNSUPPORTED_FORMAT &&
      reduced.rgba == NULL &&
      qlic_decode_wide(data, size, NULL, &image) == QLIC_OK &&
      image.width == fixture->width && image.height == fixture->height &&
      image.channels == fixture->channels &&
      image.bits_per_sample == fixture->bits_per_sample &&
      image.stride == row_bytes && image.pixels_size == pixels_size &&
      memcmp(image.pixels, fixture->pixels, pixels_size) == 0;
  if (!ok)
    fprintf(stderr, "wide compatibility failure: %s: %s\n", fixture->name,
            qlic_last_error());
  qlic_image_free(&reduced);
  qlic_wide_image_free(&image);
  free(data);
  return ok;
}

static int run_hdr_fixture(const HdrFixture *fixture) {
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(fixture->name, &data, &size)) {
    fprintf(stderr, "could not read fixture: %s\n", fixture->name);
    return 0;
  }
  size_t storage =
      fixture->bits_per_sample <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)fixture->width * fixture->channels * storage;
  size_t pixels_size = fixture->sample_count * storage;
  qlic_info_v2 info = {0};
  info.struct_size = sizeof(info);
  qlic_hdr_image image = {0};
  image.struct_size = sizeof(image);
  qlic_wide_image wide = {0};
  qlic_image reduced = {0};
  int ok =
      qlic_get_info_v2(data, size, NULL, &info) == QLIC_OK &&
      info.width == fixture->width && info.height == fixture->height &&
      info.frame_count == 1u && info.animated == 0u &&
      info.channels == fixture->channels &&
      info.bits_per_sample == fixture->bits_per_sample &&
      info.sample_type == QLIC_SAMPLE_UINT &&
      info.alpha_mode == fixture->alpha_mode &&
      info.color_authority == fixture->color_authority &&
      info.has_icc == (uint32_t)(fixture->icc_size != 0u) &&
      info.has_cicp == fixture->has_cicp &&
      info.has_mastering_display == fixture->has_mastering_display &&
      info.has_content_light == fixture->has_content_light &&
      qlic_decode_rgba(data, size, NULL, &reduced) == QLIC_UNSUPPORTED_FORMAT &&
      reduced.rgba == NULL &&
      qlic_decode_wide(data, size, NULL, &wide) == QLIC_UNSUPPORTED_FORMAT &&
      wide.pixels == NULL &&
      qlic_decode_hdr(data, size, NULL, &image) == QLIC_OK &&
      image.width == fixture->width && image.height == fixture->height &&
      image.channels == fixture->channels &&
      image.bits_per_sample == fixture->bits_per_sample &&
      image.sample_type == QLIC_SAMPLE_UINT &&
      image.alpha_mode == fixture->alpha_mode &&
      image.color_authority == fixture->color_authority &&
      image.stride == row_bytes && image.pixels_size == pixels_size &&
      memcmp(image.pixels, fixture->pixels, pixels_size) == 0 &&
      image.icc_size == fixture->icc_size &&
      (!fixture->icc_size ||
       memcmp(image.icc, fixture->icc, fixture->icc_size) == 0) &&
      image.has_cicp == fixture->has_cicp &&
      (!fixture->has_cicp ||
       memcmp(&image.cicp, &fixture->cicp, sizeof(image.cicp)) == 0) &&
      image.has_mastering_display == fixture->has_mastering_display &&
      (!fixture->has_mastering_display ||
       memcmp(&image.mastering_display, &fixture->mastering_display,
              sizeof(image.mastering_display)) == 0) &&
      image.has_content_light == fixture->has_content_light &&
      (!fixture->has_content_light ||
       memcmp(&image.content_light, &fixture->content_light,
              sizeof(image.content_light)) == 0);
  if (!ok)
    fprintf(stderr, "HDR compatibility failure: %s: %s\n", fixture->name,
            qlic_last_error());
  qlic_image_free(&reduced);
  qlic_wide_image_free(&wide);
  qlic_hdr_image_free(&image);
  free(data);
  return ok;
}

static uint8_t *make_tile_stream(const uint8_t *native, size_t native_size,
                                 uint32_t count, size_t *stream_size) {
  enum {
    HEADER_SIZE = 28,
    FOOTER_SIZE = 4,
    MODE_TILES = 14,
    CODEC_STORE_WITH_CRC = 0x80
  };
  if (!native || !stream_size || count == 0 || count > 65536u ||
      native_size <= 32u ||
      memcmp(native, "QLIC", 4u) != 0 || native[12] != 9u)
    return NULL;
  uint32_t width = read32le(native + 4);
  uint32_t tile_height = read32le(native + 8);
  uint64_t height64 = (uint64_t)tile_height * count;
  size_t chunk_size = native_size - HEADER_SIZE - FOOTER_SIZE;
  if (!width || !tile_height || height64 > UINT32_MAX ||
      chunk_size > UINT32_MAX)
    return NULL;
  size_t table_size = 4u + (size_t)count * 4u;
  if (chunk_size && (size_t)count > (SIZE_MAX - table_size) / chunk_size)
    return NULL;
  size_t payload_size = table_size + (size_t)count * chunk_size;
  if (payload_size > UINT64_MAX - HEADER_SIZE ||
      payload_size > SIZE_MAX - HEADER_SIZE - FOOTER_SIZE)
    return NULL;
  size_t total = HEADER_SIZE + payload_size + FOOTER_SIZE;
  uint8_t *stream = (uint8_t *)calloc(total, 1u);
  if (!stream)
    return NULL;
  memcpy(stream, "QLIC", 4u);
  write32le(stream + 4, width);
  write32le(stream + 8, (uint32_t)height64);
  stream[12] = MODE_TILES;
  stream[13] = 0u;
  stream[14] = 1u;
  stream[15] = CODEC_STORE_WITH_CRC;
  write32le(stream + 16, tile_height);
  write64le(stream + 20, (uint64_t)payload_size);
  uint8_t *payload = stream + HEADER_SIZE;
  write32le(payload, count);
  size_t offset = table_size;
  for (uint32_t i = 0; i < count; ++i) {
    write32le(payload + 4u + (size_t)i * 4u, (uint32_t)chunk_size);
    memcpy(payload + offset, native + HEADER_SIZE, chunk_size);
    offset += chunk_size;
  }
  write32le(stream + total - FOOTER_SIZE, crc32(stream, total - FOOTER_SIZE));
  *stream_size = total;
  return stream;
}

static int run_tile_stream_limits(void) {
  uint8_t *native = NULL;
  size_t native_size = 0;
  if (!read_file("native.qlic", &native, &native_size)) {
    fprintf(stderr, "could not read tile-stream source fixture\n");
    return 0;
  }
  qlic_image source = {0};
  qlic_image tiled = {0};
  qlic_image excessive = {0};
  size_t two_size = 0;
  size_t excessive_size = 0;
  uint8_t *two = make_tile_stream(native, native_size, 2u, &two_size);
  uint8_t *too_many =
      make_tile_stream(native, native_size, 257u, &excessive_size);
  int ok = two && too_many &&
           qlic_decode_rgba(native, native_size, NULL, &source) == QLIC_OK &&
           qlic_decode_rgba(two, two_size, NULL, &tiled) == QLIC_OK &&
           tiled.width == source.width && tiled.height == source.height * 2u &&
           tiled.stride == source.stride;
  size_t source_bytes = source.stride * source.height;
  if (ok)
    ok = memcmp(tiled.rgba, source.rgba, source_bytes) == 0 &&
         memcmp(tiled.rgba + source_bytes, source.rgba, source_bytes) == 0;
  int status = too_many ? qlic_decode_rgba(too_many, excessive_size, NULL,
                                           &excessive)
                        : QLIC_ERROR;
  ok = ok && status == QLIC_LIMIT_EXCEEDED && excessive.rgba == NULL;
  if (!ok)
    fprintf(stderr, "MODE_TILES compatibility/limit failure: %s\n",
            qlic_last_error());
  qlic_image_free(&source);
  qlic_image_free(&tiled);
  qlic_image_free(&excessive);
  free(two);
  free(too_many);
  free(native);
  return ok;
}

int main(void) {
  size_t count = sizeof(fixtures) / sizeof(fixtures[0]);
  for (size_t i = 0; i < count; ++i)
    if (!run_fixture(&fixtures[i]))
      return 1;
  size_t wide_count = sizeof(wide_fixtures) / sizeof(wide_fixtures[0]);
  for (size_t i = 0; i < wide_count; ++i)
    if (!run_wide_fixture(&wide_fixtures[i]))
      return 1;
  size_t hdr_count = sizeof(hdr_fixtures) / sizeof(hdr_fixtures[0]);
  for (size_t i = 0; i < hdr_count; ++i)
    if (!run_hdr_fixture(&hdr_fixtures[i]))
      return 1;
  if (!run_tile_stream_limits())
    return 1;
  printf("QLIC compatibility fixtures passed: %zu RGBA, %zu native-wide, "
         "%zu self-described HDR, MODE_TILES limits\n",
         count, wide_count, hdr_count);
  return 0;
}
