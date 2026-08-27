#ifndef QLIC_STATIC
#define QLIC_STATIC
#endif
#include <qlic/qlic.h>

#include <stdio.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: encode output.qlic\n");
    return 2;
  }

  static const uint8_t rgba[] = {255, 0, 0,   255, 0,   255, 0,   255,
                                 0,   0, 255, 255, 255, 255, 255, 128};
  qlic_encode_options options;
  qlic_encode_options_default(&options);
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  int status = qlic_encode_rgba(rgba, sizeof(rgba), 2, 2, 8, &options, &encoded,
                                &encoded_size);
  if (status != QLIC_OK) {
    fprintf(stderr, "encode failed: %s\n", qlic_last_error());
    return 1;
  }

  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, argv[1], "wb") != 0)
    file = NULL;
#else
  file = fopen(argv[1], "wb");
#endif
  int wrote = file && fwrite(encoded, 1, encoded_size, file) == encoded_size;
  if (file && fclose(file) != 0)
    wrote = 0;
  if (!wrote) {
    qlic_free(encoded);
    fprintf(stderr, "could not write output\n");
    return 1;
  }

  qlic_free(encoded);
  return 0;
}
