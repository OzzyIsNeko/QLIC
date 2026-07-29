#ifndef QLIC_STREAM_H
#define QLIC_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STREAM_OK = 0,
    STREAM_E_ARG = -1,
    STREAM_E_ALLOC = -2,
    STREAM_E_FORMAT = -3,
    STREAM_E_CORRUPT = -4,
    STREAM_E_DIM = -5
};

/* this is the same metadata view used by the decoder */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_checksum;
    uint32_t payload_size;
    uint32_t palette_count;
    int channels;
    int flags;
    int mode;
    int transform;
    int tile_log;
    int control;
    int adaptation;
    int sample_bits;
} StreamInfo;

const char *stream_strerror(int e);
uint32_t stream_crc32(const uint8_t *p, size_t n);
int stream_get_info(const uint8_t *data, size_t n, StreamInfo *info);
int stream_encode_threads(const uint8_t *pix, uint32_t w, uint32_t h, int channels,
                       int search, unsigned threads, uint8_t **out, size_t *outn);
int stream_encode_strided_threads(
    const uint8_t *pix, uint32_t w, uint32_t h, int channels,
    size_t pixel_stride, int search, unsigned threads, uint8_t **out,
    size_t *outn);
void stream_set_threads(unsigned threads);

/* expected dimensions stop a nested stream from redefining the outer image */
int stream_decode_trusted_expected_threads(
    const uint8_t *data, size_t n, unsigned threads, uint32_t expected_w,
    uint32_t expected_h, int expected_ch, uint8_t **pixout, uint32_t *pw,
    uint32_t *ph, int *pch);
int stream_decode_trusted_expected(const uint8_t *data, size_t n,
                                   uint32_t expected_w, uint32_t expected_h,
                                   int expected_ch, uint8_t **pixout,
                                   uint32_t *pw, uint32_t *ph, int *pch);
int stream_decode_trusted_expected_rgba(
    const uint8_t *data, size_t n, uint32_t expected_w, uint32_t expected_h,
    int expected_ch, uint8_t **pixout, uint32_t *pw, uint32_t *ph, int *pch);
void stream_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
