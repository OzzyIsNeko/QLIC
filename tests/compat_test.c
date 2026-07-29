#include <qlic/qlic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    {"animation.qlic", 2, 2, 2, 0, {40, 70},
     {UINT32_C(0xbc5aab1c), UINT32_C(0xd6538a42)}},
    {"blocks.qlic", 1, 17, 1, 0, {0, 0},
     {UINT32_C(0x5f370f87), 0}},
    {"cpalette-lzms.qlic", 512, 512, 1, 0, {0, 0},
     {UINT32_C(0xe1d7051e), 0}},
    {"gray-model-lzms.qlic", 384, 256, 1, 0, {0, 0},
     {UINT32_C(0x3bf96fd4), 0}},
    {"gray-rle.qlic", 3, 31, 1, 0, {0, 0},
     {UINT32_C(0xd59a4f1e), 0}},
    {"native.qlic", 64, 64, 1, 0, {0, 0},
     {UINT32_C(0xb385d194), 0}},
    {"palette.qlic", 17, 1, 1, 0, {0, 0},
     {UINT32_C(0x5f370f87), 0}},
    {"palette-filtered.qlic", 9, 257, 1, 0, {0, 0},
     {UINT32_C(0x2b1e5d73), 0}},
    {"rgb-lzms.qlic", 257, 9, 1, 0, {0, 0},
     {UINT32_C(0x10528ad2), 0}},
    {"separable.qlic", 1, 1, 1, 0, {0, 0},
     {UINT32_C(0x0c463091), 0}},
    {"tile-model.qlic", 31, 3, 1, 0, {0, 0},
     {UINT32_C(0xdb03a251), 0}}};

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
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
  /* these archived streams define the read compatibility promised by v1 */
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
           info.frame_count == fixture->frames &&
           info.animated == animated &&
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

int main(void) {
  size_t count = sizeof(fixtures) / sizeof(fixtures[0]);
  for (size_t i = 0; i < count; ++i)
    if (!run_fixture(&fixtures[i]))
      return 1;
  printf("QLIC compatibility fixtures passed: %zu\n", count);
  return 0;
}
