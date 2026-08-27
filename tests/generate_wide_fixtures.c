#include <qlic/qlic.h>

#include "wide_fixture_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_fixture(const char *directory, const WideFixture *fixture) {
  size_t storage =
      fixture->bits_per_sample <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)fixture->width * fixture->channels * storage;
  size_t pixels_size = fixture->sample_count * storage;
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  int result =
      qlic_encode_wide(fixture->pixels, pixels_size, fixture->width,
                       fixture->height, row_bytes, fixture->channels,
                       fixture->bits_per_sample, NULL, &encoded, &encoded_size);
  if (result != QLIC_OK || encoded_size < 48u || encoded[12] != 19u ||
      memcmp(encoded + 28u, "QSW1", 4u) != 0) {
    fprintf(stderr, "could not encode %s: %s\n", fixture->name,
            qlic_last_error());
    qlic_free(encoded);
    return 0;
  }

  char path[1024];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, fixture->name);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    fprintf(stderr, "fixture path is too long: %s\n", fixture->name);
    qlic_free(encoded);
    return 0;
  }
  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") || !file) {
#else
  file = fopen(path, "wb");
  if (!file) {
#endif
    fprintf(stderr, "could not open fixture output: %s\n", path);
    qlic_free(encoded);
    return 0;
  }
  int ok = fwrite(encoded, 1u, encoded_size, file) == encoded_size;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok)
    fprintf(stderr, "could not write fixture: %s\n", path);
  else
    printf("%s %zu bytes\n", fixture->name, encoded_size);
  qlic_free(encoded);
  return ok;
}

static int write_hdr_fixture(const char *directory, const HdrFixture *fixture) {
  size_t storage =
      fixture->bits_per_sample <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = (size_t)fixture->width * fixture->channels * storage;
  qlic_hdr_image image = {0};
  image.struct_size = sizeof(image);
  image.width = fixture->width;
  image.height = fixture->height;
  image.channels = fixture->channels;
  image.bits_per_sample = fixture->bits_per_sample;
  image.sample_type = QLIC_SAMPLE_UINT;
  image.alpha_mode = fixture->alpha_mode;
  image.color_authority = fixture->color_authority;
  image.pixels = (void *)fixture->pixels;
  image.pixels_size = fixture->sample_count * storage;
  image.stride = row_bytes;
  image.icc = (uint8_t *)fixture->icc;
  image.icc_size = fixture->icc_size;
  image.has_cicp = fixture->has_cicp;
  image.cicp = fixture->cicp;
  image.has_mastering_display = fixture->has_mastering_display;
  image.mastering_display = fixture->mastering_display;
  image.has_content_light = fixture->has_content_light;
  image.content_light = fixture->content_light;

  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  int result = qlic_encode_hdr(&image, NULL, &encoded, &encoded_size);
  if (result != QLIC_OK || encoded_size < 60u || encoded[12] != 20u ||
      memcmp(encoded + 28u, "QSW2", 4u) != 0) {
    fprintf(stderr, "could not encode %s: %s\n", fixture->name,
            qlic_last_error());
    qlic_free(encoded);
    return 0;
  }

  char path[1024];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, fixture->name);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    fprintf(stderr, "fixture path is too long: %s\n", fixture->name);
    qlic_free(encoded);
    return 0;
  }
  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") || !file) {
#else
  file = fopen(path, "wb");
  if (!file) {
#endif
    fprintf(stderr, "could not open fixture output: %s\n", path);
    qlic_free(encoded);
    return 0;
  }
  int ok = fwrite(encoded, 1u, encoded_size, file) == encoded_size;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok)
    fprintf(stderr, "could not write fixture: %s\n", path);
  else
    printf("%s %zu bytes\n", fixture->name, encoded_size);
  qlic_free(encoded);
  return ok;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: qlic-generate-wide-fixtures output-directory\n");
    return 2;
  }
  size_t count = sizeof(wide_fixtures) / sizeof(wide_fixtures[0]);
  for (size_t index = 0; index < count; ++index)
    if (!write_fixture(argv[1], &wide_fixtures[index]))
      return 1;
  count = sizeof(hdr_fixtures) / sizeof(hdr_fixtures[0]);
  for (size_t index = 0; index < count; ++index)
    if (!write_hdr_fixture(argv[1], &hdr_fixtures[index]))
      return 1;
  return 0;
}
