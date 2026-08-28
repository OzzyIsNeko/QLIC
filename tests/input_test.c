#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
  const wchar_t *name;
  int accepted;
  QlicInputDecoder decoder;
} Case;

static uint32_t read32be(const uint8_t *data) {
  return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
         (uint32_t)data[2] << 8 | data[3];
}

static void write32be(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc >> 1u) ^
            (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc ^ UINT32_C(0xffffffff);
}

static int expect_bad_png(const uint8_t *data, size_t size,
                          const char *label) {
  QlicInput input = {0};
  char error[256] = {0};
  if (qlic_input_open_memory(data, size, UINT64_C(16777216),
                             UINT64_C(1048576), &input, error,
                             sizeof(error))) {
    fprintf(stderr, "%s PNG was accepted\n", label);
    qlic_input_close(&input);
    return 0;
  }
  return 1;
}

static int check_bad_pngs(const wchar_t *directory) {
  wchar_t path[32768];
  if (swprintf_s(path, 32768, L"%ls\\base.png", directory) < 0)
    return 0;
  QlicInput base = {0};
  char error[256] = {0};
  if (!qlic_input_open(path, UINT64_C(16777216), UINT64_C(1048576), &base,
                       error, sizeof(error))) {
    fprintf(stderr, "could not open base PNG: %s\n", error);
    return 0;
  }
  uint8_t *changed = (uint8_t *)malloc(base.size + 1u);
  if (!changed) {
    qlic_input_close(&base);
    return 0;
  }
  memcpy(changed, base.data, base.size);
  changed[29] ^= 1u;
  int ok = expect_bad_png(changed, base.size, "bad-checksum");

  memcpy(changed, base.data, base.size);
  size_t offset = 8u;
  while (offset + 12u <= base.size &&
         memcmp(changed + offset + 4u, "IDAT", 4u) != 0) {
    uint32_t length = read32be(changed + offset);
    if ((size_t)length > base.size - offset - 12u) {
      offset = base.size;
      break;
    }
    offset += (size_t)length + 12u;
  }
  if (offset + 12u > base.size) {
    fprintf(stderr, "base PNG has no IDAT chunk\n");
    ok = 0;
  } else {
    write32be(changed + offset, read32be(changed + offset) + 1u);
    ok &= expect_bad_png(changed, base.size, "bad-chunk-size");
  }

  memcpy(changed, base.data, base.size);
  changed[base.size] = 0;
  ok &= expect_bad_png(changed, base.size + 1u, "trailing-data");
  free(changed);
  qlic_input_close(&base);
  return ok;
}

static int check_png_metadata(const wchar_t *directory) {
  wchar_t path[32768];
  if (swprintf_s(path, 32768, L"%ls\\base.png", directory) < 0)
    return 0;
  QlicInput base = {0};
  char error[256] = {0};
  if (!qlic_input_open(path, UINT64_C(16777216), UINT64_C(1048576), &base,
                       error, sizeof(error)))
    return 0;
  size_t offset = 8u;
  while (offset + 12u <= base.size &&
         memcmp(base.data + offset + 4u, "IDAT", 4u))
    offset += (size_t)read32be(base.data + offset) + 12u;
  static const uint8_t exif[10] = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0};
  size_t chunk_size = sizeof(exif) + 12u;
  uint8_t *with_exif = (uint8_t *)malloc(base.size + chunk_size);
  if (!with_exif || offset + 12u > base.size) {
    free(with_exif);
    qlic_input_close(&base);
    return 0;
  }
  memcpy(with_exif, base.data, offset);
  write32be(with_exif + offset, sizeof(exif));
  memcpy(with_exif + offset + 4u, "eXIf", 4u);
  memcpy(with_exif + offset + 8u, exif, sizeof(exif));
  write32be(with_exif + offset + 8u + sizeof(exif),
            crc32(with_exif + offset + 4u, sizeof(exif) + 4u));
  memcpy(with_exif + offset + chunk_size, base.data + offset,
         base.size - offset);
  QlicInput inspected = {0};
  int ok = qlic_input_open_memory(with_exif, base.size + chunk_size,
                                  UINT64_C(16777216), UINT64_C(1048576),
                                  &inspected, error, sizeof(error)) &&
           inspected.metadata_count == 1u &&
           !memcmp(inspected.metadata[0].tag, "EXIF", 4u) &&
           inspected.metadata[0].size == sizeof(exif) &&
           !memcmp(inspected.metadata[0].data, exif, sizeof(exif));
  if (!ok)
    fprintf(stderr, "PNG EXIF metadata inspection failed: %s\n", error);
  qlic_input_close(&inspected);
  free(with_exif);
  qlic_input_close(&base);
  return ok;
}

static int check_tiff_jumbf(void) {
  static const uint8_t tiff[] = {
      'I', 'I', 42, 0, 8, 0, 0, 0,
      1, 0,
      0x16, 0xcd, 7, 0, 4, 0, 0, 0, 'j', 'u', 'm', 'b',
      0, 0, 0, 0};
  QlicInput input = {0};
  char error[256] = {0};
  int ok = qlic_input_open_memory(tiff, sizeof(tiff), UINT64_C(16777216),
                                  UINT64_C(1048576), &input, error,
                                  sizeof(error)) &&
           input.metadata_count == 1u &&
           !memcmp(input.metadata[0].tag, "JUMB", 4u) &&
           input.metadata[0].size == 4u &&
           !memcmp(input.metadata[0].data, "jumb", 4u);
  if (!ok)
    fprintf(stderr, "TIFF JUMBF metadata inspection failed: %s\n", error);
  qlic_input_close(&input);
  return ok;
}

static int check_case(const wchar_t *directory, const Case *test,
                      QlicInputImage *decoded) {
  wchar_t path[32768];
  if (swprintf_s(path, 32768, L"%ls\\%ls", directory, test->name) < 0)
    return 0;
  char error[256] = {0};
  QlicInput input = {0};
  int accepted = qlic_input_open(path, UINT64_C(16777216),
                                 UINT64_C(1048576), &input, error,
                                 sizeof(error));
  if (accepted != test->accepted) {
    fwprintf(stderr, L"%ls: expected %ls, got %hs\n", test->name,
             test->accepted ? L"accept" : L"reject",
             accepted ? "accept" : error);
    qlic_input_close(&input);
    return 0;
  }
  if (!accepted)
    return 1;
  QlicInput memory = {0};
  char memory_error[256] = {0};
  if (!qlic_input_open_memory(input.data, input.size, UINT64_C(16777216),
                              UINT64_C(1048576), &memory, memory_error,
                              sizeof(memory_error)) ||
      memory.decoder != test->decoder) {
    fwprintf(stderr, L"%ls: memory input failed: %hs\n", test->name,
             memory_error);
    qlic_input_close(&memory);
    qlic_input_close(&input);
    return 0;
  }
  qlic_input_close(&memory);
  if (input.decoder != test->decoder) {
    fwprintf(stderr, L"%ls: wrong decoder\n", test->name);
    qlic_input_close(&input);
    return 0;
  }
  if (decoded) {
    if (!qlic_input_decode(&input, UINT64_C(1048576), decoded, error,
                           sizeof(error)) ||
        decoded->width != 16 || decoded->height != 16) {
      fwprintf(stderr, L"%ls: decode failed: %hs\n", test->name, error);
      qlic_input_close(&input);
      return 0;
    }
  }
  qlic_input_close(&input);
  return 1;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 2)
    return 2;
  static const Case cases[] = {
      {L"base.png", 1, QLIC_INPUT_WIC},
      {L"base.bmp", 1, QLIC_INPUT_WIC},
      {L"lossless.tiff", 1, QLIC_INPUT_WIC},
      {L"lossless.avif", 1, QLIC_INPUT_WIC},
#ifdef QLIC_TEST_BUNDLED_IMAGE_CODECS
      {L"lossless.webp", 1, QLIC_INPUT_WEBP},
      {L"lossless.jxl", 1, QLIC_INPUT_JXL},
#endif
      {L"lossy.jpg", 0, QLIC_INPUT_WIC},
      {L"lossy.tiff", 0, QLIC_INPUT_WIC},
      {L"lossy.avif", 0, QLIC_INPUT_WIC},
#ifdef QLIC_TEST_BUNDLED_IMAGE_CODECS
      {L"lossy.webp", 0, QLIC_INPUT_WEBP},
      {L"lossy.jxl", 0, QLIC_INPUT_JXL},
#endif
      {L"high16.png", 1, QLIC_INPUT_WIC},
  };
  int ok = 1;
  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    ok &= check_case(argv[1], &cases[index], NULL);
  ok &= check_bad_pngs(argv[1]);
  ok &= check_png_metadata(argv[1]);
  ok &= check_tiff_jumbf();

  wchar_t high16_path[32768];
  QlicInput high16 = {0};
  char high16_error[256] = {0};
  if (swprintf_s(high16_path, 32768, L"%ls\\high16.png", argv[1]) < 0 ||
      !qlic_input_open(high16_path, UINT64_C(16777216), UINT64_C(1048576),
                       &high16, high16_error, sizeof(high16_error)) ||
      high16.channels != 1u || high16.bits_per_sample != 16u) {
    fprintf(stderr, "16-bit PNG metadata inspection failed: %s\n",
            high16_error);
    ok = 0;
  }
  qlic_input_close(&high16);

#ifdef QLIC_TEST_BUNDLED_IMAGE_CODECS
  QlicInputImage webp = {0}, jxl = {0};
  const Case webp_case = {L"lossless.webp", 1, QLIC_INPUT_WEBP};
  const Case jxl_case = {L"lossless.jxl", 1, QLIC_INPUT_JXL};
  ok &= check_case(argv[1], &webp_case, &webp);
  ok &= check_case(argv[1], &jxl_case, &jxl);
  if (!webp.rgba || !jxl.rgba ||
      memcmp(webp.rgba, jxl.rgba, 16u * 16u * 4u) != 0) {
    fprintf(stderr, "lossless decoder pixels differ\n");
    ok = 0;
  }
  free(webp.rgba);
  free(jxl.rgba);
#endif
  if (ok)
    printf("input format checks passed\n");
  return ok ? 0 : 1;
}
