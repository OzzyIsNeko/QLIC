#ifndef QLIC_INPUT_H
#define QLIC_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef enum {
  QLIC_INPUT_WIC = 0,
  QLIC_INPUT_WEBP = 1,
  QLIC_INPUT_JXL = 2
} QlicInputDecoder;

typedef struct {
  uint8_t *data;
  size_t size;
  QlicInputDecoder decoder;
} QlicInput;

typedef struct {
  uint8_t *rgba;
  uint32_t width;
  uint32_t height;
} QlicInputImage;

int qlic_input_open(const wchar_t *path, uint64_t max_file_bytes,
                    uint64_t max_pixels, QlicInput *input, char *error,
                    size_t error_capacity);
int qlic_input_open_memory(const uint8_t *data, size_t size,
                           uint64_t max_file_bytes, uint64_t max_pixels,
                           QlicInput *input, char *error,
                           size_t error_capacity);
int qlic_input_decode(const QlicInput *input, uint64_t max_pixels,
                      QlicInputImage *image, char *error,
                      size_t error_capacity);
int qlic_input_set_runtime_directory(const wchar_t *directory);
void qlic_input_close(QlicInput *input);

#endif
