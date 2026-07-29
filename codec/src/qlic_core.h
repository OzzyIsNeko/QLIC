#ifndef QLIC_CORE_H
#define QLIC_CORE_H

#include <stddef.h>
#include <stdint.h>
#ifndef QLIC_WASM
#include "qlic_version.h"
#endif

#define QLIC_HEADER_SIZE 28
#define QLIC_MAGIC "QLIC"
#define QLIC_MAGIC_SIZE 4
#define QLIC_CODEC_CRC 0x80
#define QLIC_FOOTER_SIZE 4
#define QLIC_DEFAULT_MAX_FILE_BYTES UINT64_C(536870912)
#define QLIC_DEFAULT_MAX_PAYLOAD_BYTES UINT64_C(536870912)
#define QLIC_DEFAULT_MAX_PIXELS UINT64_C(67108864)
#define QLIC_DEFAULT_MAX_ANIMATION_BYTES UINT64_C(536870912)
#define QLIC_DEFAULT_MAX_FRAMES 100000u

/* mode numbers are stored in the file format, do not reorder them */
enum {
  MODE_GRAY = 1,
  MODE_GRAYA = 2,
  MODE_RGB = 3,
  MODE_RGBA = 4,
  MODE_PALETTE = 5,
  MODE_SOURCE = 6,
  MODE_SEPARABLE = 7,
  MODE_RESERVED = 8,
  MODE_NATIVE = 9,
  MODE_FILTERED = 10,
  MODE_PSTREAM = 11,
  MODE_PPAL = 12,
  MODE_CPAL = 13,
  MODE_TILES = 14,
  MODE_TILE_MODEL = 15,
  MODE_GMODEL = 16,
  MODE_ANIM = 17,
  MODE_BLOCKS = 18
};

enum {
  TRANSFORM_IDENTITY = 0,
  TRANSFORM_GDELTA = 1,
  TRANSFORM_IDENTITY_RAW = 2,
  TRANSFORM_GDELTA_RAW = 3,
  TRANSFORM_IDENTITY_RLE = 4,
  TRANSFORM_GDELTA_RLE = 5,
  TRANSFORM_INDEX_RLE = 6,
  TRANSFORM_SEPARABLE_DELTA = 7,
  TRANSFORM_RDELTA = 8,
  TRANSFORM_BDELTA = 9,
  TRANSFORM_CPAL_DELTA = 10
};

enum {
  CODEC_STORE = 0,
  CODEC_XPRESS = 1,
  CODEC_XPRESS_HUFF = 2,
  CODEC_LZMS = 3
};

typedef struct {
  uint8_t *data;
  size_t size;
  size_t cap;
} Buf;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint8_t *rgba;
} Image;

typedef struct {
  Image image;
  uint32_t delay_ms;
} AnimFrame;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t count;
  uint32_t loop_count;
  AnimFrame *frames;
} Anim;

typedef struct {
  uint32_t width;
  uint32_t height;
  int mode;
  int transform;
  int index_bits;
  int codec;
  uint32_t palette_count;
  uint64_t payload_size;
  uint64_t compressed_size;
} QlicHeader;

typedef struct {
  uint64_t max_file_bytes;
  uint64_t max_payload_bytes;
  uint64_t max_pixels;
  uint64_t max_animation_bytes;
  uint32_t max_frames;
} QlicDecodeLimits;

typedef struct {
  int mode;
  int transform;
  int index_bits;
  int codec;
  uint32_t palette_count;
  uint64_t payload_size;
  uint8_t *palette;
  size_t palette_size;
  uint8_t *compressed;
  size_t compressed_size;
} Candidate;

typedef enum {
  QLIC_CORE_OK = 0,
  QLIC_CORE_ERROR,
  QLIC_CORE_BAD_ARGUMENT,
  QLIC_CORE_OUT_OF_MEMORY,
  QLIC_CORE_BAD_DATA,
  QLIC_CORE_LIMIT_EXCEEDED
} QlicCoreStatus;

const char *qlic_core_error(void);
QlicCoreStatus qlic_core_status(void);
void clear_err(void);
void set_err(const char *fmt, ...);
void set_err_status(QlicCoreStatus status, const char *fmt, ...);
void buf_free(Buf *b);
void image_free(Image *im);
void anim_free(Anim *a);
void candidate_free(Candidate *c);
void qlic_core_default_limits(QlicDecodeLimits *limits);
unsigned qlic_core_hardware_threads(void);
unsigned qlic_core_thread_count(void);
int qlic_core_set_thread_count(unsigned threads);
int rd_head(const uint8_t *data, size_t size, QlicHeader *h);
int rd_head_limited(const uint8_t *data, size_t size, QlicHeader *h,
                    const QlicDecodeLimits *limits);
int dec_qlic(const uint8_t *data, size_t size, Image *out,
             QlicHeader *header_out);
int dec_qlic_limited(const uint8_t *data, size_t size, Image *out,
                     QlicHeader *header_out,
                     const QlicDecodeLimits *limits);
int dec_any_qlic(const uint8_t *data, size_t size, Anim *out,
                 QlicHeader *header_out);
int dec_any_qlic_limited(const uint8_t *data, size_t size, Anim *out,
                         QlicHeader *header_out,
                         const QlicDecodeLimits *limits);
int enc_mem(const Image *im, Buf *file, Candidate *chosen);
int enc_anim_mem(const Anim *anim, Buf *file, Candidate *chosen);

#endif
