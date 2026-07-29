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
      {L"lossless.webp", 1, QLIC_INPUT_WEBP},
      {L"lossless.jxl", 1, QLIC_INPUT_JXL},
      {L"lossy.jpg", 0, QLIC_INPUT_WIC},
      {L"lossy.tiff", 0, QLIC_INPUT_WIC},
      {L"lossy.avif", 0, QLIC_INPUT_WIC},
      {L"lossy.webp", 0, QLIC_INPUT_WEBP},
      {L"lossy.jxl", 0, QLIC_INPUT_JXL},
      {L"high16.png", 0, QLIC_INPUT_WIC},
  };
  int ok = 1;
  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    ok &= check_case(argv[1], &cases[index], NULL);

  QlicInputImage webp = {0};
  QlicInputImage jxl = {0};
  Case webp_case = {L"lossless.webp", 1, QLIC_INPUT_WEBP};
  Case jxl_case = {L"lossless.jxl", 1, QLIC_INPUT_JXL};
  ok &= check_case(argv[1], &webp_case, &webp);
  ok &= check_case(argv[1], &jxl_case, &jxl);
  if (!webp.rgba || !jxl.rgba ||
      memcmp(webp.rgba, jxl.rgba, 16u * 16u * 4u) != 0) {
    fprintf(stderr, "lossless decoder pixels differ\n");
    ok = 0;
  }
  free(webp.rgba);
  free(jxl.rgba);
  if (ok)
    printf("input format checks passed\n");
  return ok ? 0 : 1;
}
