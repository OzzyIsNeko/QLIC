#ifndef QLIC_LZMS_H
#define QLIC_LZMS_H

#include <stddef.h>
#include <stdint.h>

int qlic_lzms_decompress(const uint8_t *source, size_t source_size,
                         uint8_t *destination, size_t destination_size);

#endif
