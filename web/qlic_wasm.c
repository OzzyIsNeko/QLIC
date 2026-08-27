#include <stddef.h>
#include <stdint.h>
#include "../codec/src/lzms.h"
#include "../codec/src/qlic_core.h"
#include "../codec/src/stream.h"

#define QLIC_MAX_FILE_BYTES UINT64_C(268435456)
#define QLIC_MAX_PAYLOAD_BYTES UINT64_C(268435456)
#define QLIC_MAX_PIXELS UINT64_C(33554432)
#define QLIC_MAX_ANIMATION_BYTES UINT64_C(268435456)
#define QLIC_MAX_DECODED_BYTES UINT64_C(268435456)
#define QLIC_MAX_METADATA_BYTES UINT64_C(16777216)
#define QLIC_MAX_FRAMES 4096u
#define QLIC_MAX_CHUNKS 256u

enum {
  ANIM_FRAME_KEY,
  ANIM_FRAME_DUPLICATE,
  ANIM_FRAME_RECT,
  ANIM_FRAME_MOVE
};

enum {
  RTT_RAW = 0,
  RTT_FILT = 1,
  RTT_XD = 2,
  RTT_YD = 3,
  RTT_GRAD = 4,
  RTT_H2 = 5,
  RTT_V2 = 6,
  RTT_PLANAR = 7
};

#define RTT_MAX_MODEL RTT_PLANAR

#define BLK_SIZE 16u
#define BLK_RAW 0u
#define BLK_FLAT 1u
#define BLK_LEFT 2u
#define BLK_UP 3u
#define BLK_TWO 4u
#define BLK_FOUR 5u
#define BLK_PAT2 6u
#define BLK_REF 7u
#define BLK2_ZERO 0u
#define BLK2_FLAT 1u
#define BLK2_GRAD 2u
#define BLK2_LEFT 3u
#define BLK2_UP 4u
#define BLK2_CAUSAL 5u
#define BLK2_PDM 6u
#define PDM_SIZE 64u
#define CF_SIZE 64u

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t delay;
  uint8_t *rgba;
} Frame;

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
} Head;

/* Freestanding allocator for the missing C runtime. */
typedef struct Block Block;
struct Block {
  size_t size;
  Block *next;
  int free;
};

extern unsigned char __heap_base;

static uintptr_t heap_start;
static uintptr_t heap_pos;
static Block *heap_head;
static Block *heap_tail;
static char last_error[256];
static Frame *frames;
static uint32_t frame_count;
static uint32_t image_width;
static uint32_t image_height;
static uint32_t loop_count;
static uint32_t animated;
static uint8_t *encoded_data;
static uint32_t encoded_size;
static WideImage wide_result;
static HdrImage hdr_result;
static WideImage *sample_result;
static uint8_t hdr_metadata[68];
static uint32_t hdr_metadata_size;

static const QlicDecodeLimits decode_limits = {
    QLIC_MAX_FILE_BYTES,      QLIC_MAX_PAYLOAD_BYTES,
    QLIC_MAX_PIXELS,          QLIC_MAX_ANIMATION_BYTES,
    QLIC_MAX_DECODED_BYTES,   QLIC_MAX_METADATA_BYTES,
    QLIC_MAX_FRAMES,          QLIC_MAX_CHUNKS};

void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *dd = (uint8_t *)d;
  const uint8_t *ss = (const uint8_t *)s;
  while (n && ((uintptr_t)dd & 7u)) {
    *dd++ = *ss++;
    --n;
  }
  if ((((uintptr_t)ss & 7u) == 0)) {
    uint64_t *dw = (uint64_t *)dd;
    const uint64_t *sw = (const uint64_t *)ss;
    while (n >= 8u) {
      *dw++ = *sw++;
      n -= 8u;
    }
    dd = (uint8_t *)dw;
    ss = (const uint8_t *)sw;
  }
  while (n--)
    *dd++ = *ss++;
  return d;
}

void *memset(void *d, int c, size_t n) {
  uint8_t *p = (uint8_t *)d;
  uint8_t b = (uint8_t)c;
  while (n && ((uintptr_t)p & 7u)) {
    *p++ = b;
    --n;
  }
  uint64_t w = (uint64_t)b * 0x0101010101010101ull;
  uint64_t *wp = (uint64_t *)p;
  while (n >= 8u) {
    *wp++ = w;
    n -= 8u;
  }
  p = (uint8_t *)wp;
  while (n--)
    *p++ = b;
  return d;
}

void *memmove(void *d, const void *s, size_t n) {
  uint8_t *dd = (uint8_t *)d;
  const uint8_t *ss = (const uint8_t *)s;
  if (dd == ss || !n)
    return d;
  if (dd < ss || dd >= ss + n)
    return memcpy(d, s, n);
  if (dd > ss) {
    for (size_t i = n; i > 0; --i)
      dd[i - 1u] = ss[i - 1u];
  }
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; ++i) {
    if (x[i] != y[i])
      return (int)x[i] - (int)y[i];
  }
  return 0;
}

static size_t cstrlen(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}

static void set_error(const char *s) {
  size_t n = cstrlen(s);
  if (n >= sizeof(last_error))
    n = sizeof(last_error) - 1u;
  memcpy(last_error, s, n);
  last_error[n] = 0;
}

static uintptr_t align_up(uintptr_t x, uintptr_t a) {
  return (x + a - 1u) & ~(a - 1u);
}

static size_t block_head_size(void) {
  return (size_t)align_up((uintptr_t)sizeof(Block), 16u);
}

static size_t size_align(size_t n) {
  return (size_t)align_up((uintptr_t)(n ? n : 1u), 16u);
}

static void heap_boot(void) {
  if (!heap_start) {
    heap_start = align_up((uintptr_t)&__heap_base, 16u);
    heap_pos = heap_start;
  }
}

static int heap_grow(uintptr_t end) {
  uintptr_t pages = (uintptr_t)__builtin_wasm_memory_size(0);
  uintptr_t limit = pages << 16;
  if (end <= limit)
    return 1;
  uintptr_t need = end - limit;
  uintptr_t add = (need + 65535u) >> 16;
  return __builtin_wasm_memory_grow(0, add) != (size_t)-1;
}

static void block_link(Block *b) {
  b->next = 0;
  if (heap_tail)
    heap_tail->next = b;
  else
    heap_head = b;
  heap_tail = b;
}

static void block_split(Block *b, size_t n) {
  size_t hs = block_head_size();
  if (b->size < n + hs + 16u)
    return;
  Block *r = (Block *)((uint8_t *)b + hs + n);
  r->size = b->size - n - hs;
  r->free = 1;
  r->next = b->next;
  b->size = n;
  b->next = r;
  if (heap_tail == b)
    heap_tail = r;
}

static void block_merge(void) {
  for (Block *b = heap_head; b && b->next;) {
    Block *n = b->next;
    if (b->free && n->free) {
      b->size += block_head_size() + n->size;
      b->next = n->next;
      if (heap_tail == n)
        heap_tail = b;
    } else {
      b = b->next;
    }
  }
}

static Block *block_from_ptr(void *p) {
  return (Block *)((uint8_t *)p - block_head_size());
}

static void *block_data(Block *b) { return (uint8_t *)b + block_head_size(); }

void *malloc(size_t n) {
  heap_boot();
  n = size_align(n);
  for (Block *b = heap_head; b; b = b->next) {
    if (b->free && b->size >= n) {
      block_split(b, n);
      b->free = 0;
      return block_data(b);
    }
  }
  size_t hs = block_head_size();
  uintptr_t raw = align_up(heap_pos, 16u);
  uintptr_t user = raw + hs;
  uintptr_t end = user + (uintptr_t)n;
  if (end < user || !heap_grow(end))
    return 0;
  Block *b = (Block *)raw;
  b->size = n;
  b->free = 0;
  block_link(b);
  heap_pos = align_up(end, 16u);
  return block_data(b);
}

void free(void *p) {
  if (!p)
    return;
  Block *b = block_from_ptr(p);
  b->free = 1;
  block_merge();
}

void *calloc(size_t n, size_t s) {
  if (s && n > (size_t)-1 / s)
    return 0;
  size_t bytes = n * s;
  void *p = malloc(bytes);
  if (p)
    memset(p, 0, bytes);
  return p;
}

void *realloc(void *p, size_t n) {
  if (!p)
    return malloc(n);
  if (!n) {
    free(p);
    return 0;
  }
  n = size_align(n);
  Block *b = block_from_ptr(p);
  if (b->size >= n) {
    block_split(b, n);
    return p;
  }
  if (b->next && b->next->free &&
      b->size + block_head_size() + b->next->size >= n) {
    Block *next = b->next;
    b->size += block_head_size() + next->size;
    b->next = next->next;
    if (heap_tail == next)
      heap_tail = b;
    block_split(b, n);
    return p;
  }
  void *q = malloc(n);
  if (!q)
    return 0;
  memcpy(q, p, b->size < n ? b->size : n);
  free(p);
  return q;
}

static void clear_result(void) {
  frames = 0;
  frame_count = 0;
  image_width = 0;
  image_height = 0;
  loop_count = 0;
  animated = 0;
  encoded_data = 0;
  encoded_size = 0;
  memset(&wide_result, 0, sizeof(wide_result));
  memset(&hdr_result, 0, sizeof(hdr_result));
  sample_result = 0;
  memset(hdr_metadata, 0, sizeof(hdr_metadata));
  hdr_metadata_size = 0;
}

void qlic_reset(void) {
  /* qlic_reset rewinds the arena, returned frame pointers cannot survive it */
  heap_boot();
  heap_pos = heap_start;
  heap_head = 0;
  heap_tail = 0;
  clear_result();
}

uint32_t qlic_alloc(uint32_t n) {
  void *p = malloc((size_t)n);
  /* WebAssembly exposes pointers as offsets in its 32-bit linear memory. */
  // cppcheck-suppress [memleak, CastAddressToIntegerAtReturn]
  return (uint32_t)(uintptr_t)p;
}

static int mulok(size_t a, size_t b, size_t *out);

int qlic_encode(uint32_t ptr, uint32_t n, uint32_t width, uint32_t height) {
  encoded_data = 0;
  encoded_size = 0;
  last_error[0] = 0;
  size_t pixels = 0;
  size_t required = 0;
  uintptr_t memory_size =
      (uintptr_t)__builtin_wasm_memory_size(0) << 16;
  if (!width || !height ||
      !mulok((size_t)width, (size_t)height, &pixels) ||
      pixels > QLIC_MAX_PIXELS ||
      !mulok(pixels, 4u, &required) ||
      required != (size_t)n ||
      (uintptr_t)ptr > memory_size ||
      (uintptr_t)n > memory_size - (uintptr_t)ptr) {
    set_error("Invalid image data.");
    return 0;
  }
  Image image = {width, height, (uint8_t *)(uintptr_t)ptr};
  Buf file = {0};
  if (!enc_mem(&image, &file, 0)) {
    const char *error = qlic_core_error();
    set_error(error && error[0] ? error : "QLIC encode failed.");
    free(file.data);
    return 0;
  }
  if (!file.data || !file.size || file.size > UINT32_MAX) {
    free(file.data);
    set_error("QLIC output is too large.");
    return 0;
  }
  encoded_data = file.data;
  encoded_size = (uint32_t)file.size;
  return 1;
}

uint32_t qlic_encoded_ptr(void) {
  // cppcheck-suppress CastAddressToIntegerAtReturn
  return (uint32_t)(uintptr_t)encoded_data;
}

uint32_t qlic_encoded_size(void) { return encoded_size; }

static int mulok(size_t a, size_t b, size_t *out) {
  if (a && b > (size_t)-1 / a) {
    set_error("size overflow");
    return 0;
  }
  *out = a * b;
  return 1;
}

static int addok(size_t a, size_t b, size_t *out) {
  if (b > (size_t)-1 - a) {
    set_error("size overflow");
    return 0;
  }
  *out = a + b;
  return 1;
}

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int buf_reserve(Buf *b, size_t extra) {
  size_t need;
  if (!addok(b->size, extra, &need))
    return 0;
  if (need <= b->cap)
    return 1;
  size_t cap = b->cap ? b->cap : 256u;
  while (cap < need) {
    if (cap > (size_t)-1 / 2u) {
      cap = need;
      break;
    }
    cap *= 2u;
  }
  uint8_t *p = (uint8_t *)realloc(b->data, cap);
  if (!p) {
    set_error("out of memory");
    return 0;
  }
  b->data = p;
  b->cap = cap;
  return 1;
}

static void image_zero(Image *im) {
  im->width = 0;
  im->height = 0;
  im->rgba = 0;
}

static int is_raw(int t) {
  return t == TRANSFORM_IDENTITY_RAW || t == TRANSFORM_GDELTA_RAW;
}

static int is_gd(int t) {
  return t == TRANSFORM_GDELTA || t == TRANSFORM_GDELTA_RAW ||
         t == TRANSFORM_GDELTA_RLE;
}

static int is_rd(int t) { return t == TRANSFORM_RDELTA; }

static int is_bd(int t) { return t == TRANSFORM_BDELTA; }

static int is_planar_med(int t) {
  return t == TRANSFORM_BDELTA_PLANAR_MED;
}

static int is_rle(int t) {
  return t == TRANSFORM_IDENTITY_RLE || t == TRANSFORM_GDELTA_RLE;
}

static int mbpp(int mode) {
  switch (mode) {
  case MODE_GRAY:
    return 1;
  case MODE_GRAYA:
    return 2;
  case MODE_RGB:
    return 3;
  case MODE_RGBA:
    return 4;
  default:
    return 0;
  }
}

static int valid_index_bits(int bits) { return bits >= 1 && bits <= 16; }

static int palette_count_ok(uint32_t count, int bits) {
  if (!count || !valid_index_bits(bits))
    return 0;
  if (bits < 16 && count > (1u << bits))
    return 0;
  return count <= 65536u;
}

static int pal_bits(uint32_t count) {
  if (count <= 2u)
    return 1;
  if (count <= 4u)
    return 2;
  if (count <= 16u)
    return 4;
  if (count <= 256u)
    return 8;
  int bits = 0;
  for (uint32_t value = count - 1u; value; value >>= 1)
    ++bits;
  return bits;
}

static size_t row_pack(uint32_t width, int bits) {
  return ((size_t)width * (size_t)bits + 7u) >> 3;
}

static uint32_t unpack_i(const uint8_t *row, uint32_t x, int bits) {
  if (bits == 8)
    return row[x];
  if (bits == 16)
    return rd16(row + (size_t)x * 2u);
  size_t bit = (size_t)x * (size_t)bits;
  size_t byte = bit >> 3;
  int shift = (int)(bit & 7u);
  int need = (shift + bits + 7) >> 3;
  uint32_t v = 0;
  for (int i = 0; i < need; ++i)
    v |= (uint32_t)row[byte + (size_t)i] << (i * 8);
  return (v >> shift) & ((1u << bits) - 1u);
}

static int read_varint(const uint8_t *data, size_t size, size_t *pos,
                       size_t *v) {
  size_t out = 0;
  int shift = 0;
  int bits = (int)(sizeof(size_t) * 8u);
  while (*pos < size) {
    uint8_t c = data[(*pos)++];
    size_t chunk = (size_t)(c & 127u);
    if (shift >= bits || (chunk && chunk > ((size_t)-1 >> shift))) {
      set_error("corrupt file: bad run length");
      return 0;
    }
    out |= chunk << shift;
    if (!(c & 128u)) {
      *v = out;
      return 1;
    }
    shift += 7;
  }
  set_error("corrupt file: bad run length");
  return 0;
}

static int rle_decode(const uint8_t *data, size_t size, size_t expected,
                      Buf *out) {
  if (!buf_reserve(out, expected))
    return 0;
  out->size = 0;
  size_t pos = 0;
  while (pos < size) {
    size_t runm1 = 0;
    if (!read_varint(data, size, &pos, &runm1))
      return 0;
    if (pos >= size) {
      set_error("corrupt file: truncated run");
      return 0;
    }
    uint8_t v = data[pos++];
    if (runm1 == (size_t)-1) {
      set_error("corrupt file: run length overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (run > expected - out->size) {
      set_error("corrupt file: run exceeds expected size");
      return 0;
    }
    memset(out->data + out->size, v, run);
    out->size += run;
  }
  if (out->size != expected) {
    set_error("corrupt file: run payload size mismatch");
    return 0;
  }
  return 1;
}

static int pae(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc)
    return a;
  if (pb <= pc)
    return b;
  return c;
}

static int pred(int f, int left, int up, int up_left) {
  switch (f) {
  case 0:
    return 0;
  case 1:
    return left;
  case 2:
    return up;
  case 3:
    return (left + up) >> 1;
  case 4:
    return pae(left, up, up_left);
  case 5: {
    int g = left + up - up_left;
    if (g < 0)
      return 0;
    if (g > 255)
      return 255;
    return g;
  }
  default:
    return 0;
  }
}

static int unf_rows(const uint8_t *payload, size_t payload_size,
                    size_t row_bytes, uint32_t height, int bpp, Buf *samples) {
  size_t expected_row;
  size_t expected;
  if (!addok(row_bytes, 1u, &expected_row) ||
      !mulok(expected_row, (size_t)height, &expected))
    return 0;
  if (expected != payload_size) {
    set_error("corrupt file: unexpected payload size");
    return 0;
  }
  size_t sample_size;
  if (!mulok(row_bytes, (size_t)height, &sample_size) ||
      !buf_reserve(samples, sample_size))
    return 0;
  samples->size = sample_size;
  size_t in = 0;
  size_t sbpp = (size_t)bpp;
  for (uint32_t y = 0; y < height; ++y) {
    int f = payload[in++];
    if (f < 0 || f > 5) {
      set_error("corrupt file: bad filter");
      return 0;
    }
    uint8_t *row = samples->data + (size_t)y * row_bytes;
    uint8_t *prev = y ? row - row_bytes : 0;
    for (size_t x = 0; x < row_bytes; ++x) {
      int left = x >= sbpp ? row[x - sbpp] : 0;
      int up = prev ? prev[x] : 0;
      int up_left = (prev && x >= sbpp) ? prev[x - sbpp] : 0;
      row[x] = (uint8_t)(payload[in++] + pred(f, left, up, up_left));
    }
  }
  return 1;
}

static size_t file_palette_size(const Head *h) {
  return (h->mode == MODE_CPAL || h->mode == MODE_TILES ||
          h->mode == MODE_TILE_MODEL || h->mode == MODE_ANIM)
             ? 0u
             : (size_t)h->palette_count * 4u;
}

static int file_palette_mode(int mode) {
  return mode == MODE_PALETTE || mode == MODE_PSTREAM || mode == MODE_PPAL;
}

static int read_head(const uint8_t *data, size_t size, Head *h) {
  if ((uint64_t)size > QLIC_MAX_FILE_BYTES) {
    set_error("resource limit exceeded: file bytes");
    return 0;
  }
  if (size < QLIC_HEADER_SIZE || memcmp(data, "QLIC", 4u)) {
    set_error("not a QLIC file");
    return 0;
  }
  h->width = rd32(data + 4);
  h->height = rd32(data + 8);
  h->mode = data[12];
  h->transform = data[13];
  h->index_bits = data[14];
  uint8_t packed = data[15];
  h->codec = packed & ~QLIC_CODEC_CRC;
  h->palette_count = rd32(data + 16);
  h->payload_size = rd64(data + 20);
  if ((packed & QLIC_CODEC_CRC) == 0) {
    set_error("corrupt file: missing container checksum");
    return 0;
  }
  if (!h->width || !h->height) {
    set_error("corrupt file: invalid dimensions");
    return 0;
  }
  uint64_t pixels = (uint64_t)h->width * h->height;
  if (pixels > QLIC_MAX_PIXELS) {
    set_error("resource limit exceeded: pixels");
    return 0;
  }
  if (h->payload_size > QLIC_MAX_PAYLOAD_BYTES) {
    set_error("resource limit exceeded: decoded payload bytes");
    return 0;
  }
  if (h->mode < MODE_GRAY || h->mode > MODE_BLOCKS ||
      h->mode == MODE_SOURCE || h->mode == MODE_RESERVED) {
    set_error("corrupt file: invalid mode");
    return 0;
  }
  if (h->transform < TRANSFORM_IDENTITY ||
      h->transform > TRANSFORM_CPAL_PLANAR) {
    set_error("corrupt file: invalid transform");
    return 0;
  }
  if ((packed & ~(QLIC_CODEC_CRC | 3u)) ||
      (h->codec != CODEC_STORE && h->codec != CODEC_LZMS)) {
    set_error("corrupt file: invalid codec");
    return 0;
  }
  if (is_planar_med(h->transform) &&
      ((h->mode != MODE_RGB && h->mode != MODE_RGBA) ||
       h->index_bits != 0 || h->palette_count != 0 ||
       h->codec != CODEC_LZMS)) {
    set_error("corrupt file: invalid planar MED header");
    return 0;
  }
  if (h->mode == MODE_NATIVE && (h->transform != TRANSFORM_IDENTITY ||
                                 h->index_bits != 0 || h->palette_count != 0)) {
    set_error("corrupt file: invalid native stream header");
    return 0;
  }
  if (h->mode == MODE_FILTERED &&
      (h->index_bits < MODE_GRAY || h->index_bits > MODE_RGBA ||
       h->palette_count != 0)) {
    set_error("corrupt file: invalid filtered stream header");
    return 0;
  }
  if (h->mode == MODE_PSTREAM &&
      !palette_count_ok(h->palette_count, h->index_bits)) {
    set_error("corrupt file: invalid palette stream header");
    return 0;
  }
  if (h->mode == MODE_PPAL &&
      (!palette_count_ok(h->palette_count, h->index_bits) ||
       h->transform != TRANSFORM_INDEX_RLE)) {
    set_error("corrupt file: invalid ppal header");
    return 0;
  }
  if (h->mode == MODE_CPAL &&
      (!palette_count_ok(h->palette_count, h->index_bits) ||
        (h->transform != TRANSFORM_IDENTITY_RAW &&
         h->transform != TRANSFORM_INDEX_RLE &&
         h->transform != TRANSFORM_CPAL_DELTA &&
         h->transform != TRANSFORM_CPAL_TILES &&
         h->transform != TRANSFORM_CPAL_PLANAR))) {
    set_error("corrupt file: invalid cpalette header");
    return 0;
  }
  if (h->transform == TRANSFORM_CPAL_PLANAR &&
      (h->mode != MODE_CPAL || h->palette_count <= 256u ||
       h->index_bits != pal_bits(h->palette_count))) {
    set_error("corrupt file: invalid planar cpalette header");
    return 0;
  }
  if (h->mode == MODE_TILES &&
      ((h->index_bits != 1 && h->index_bits != 3 && h->index_bits != 4) ||
       h->palette_count == 0 || h->transform != TRANSFORM_IDENTITY)) {
    set_error("corrupt file: invalid tile stream header");
    return 0;
  }
  if (h->mode == MODE_TILE_MODEL &&
      ((h->index_bits != 1 && h->index_bits != 3 && h->index_bits != 4) ||
       h->palette_count == 0 || h->transform != TRANSFORM_IDENTITY ||
       h->codec != CODEC_STORE)) {
    set_error("corrupt file: invalid tile model header");
    return 0;
  }
  if (h->mode == MODE_GMODEL && (h->transform != TRANSFORM_IDENTITY ||
                                 h->index_bits != 0 || h->palette_count != 0)) {
    set_error("corrupt file: invalid gray-model header");
    return 0;
  }
  if (h->mode == MODE_ANIM && (h->transform != TRANSFORM_IDENTITY ||
                               h->index_bits != 0 || h->palette_count == 0)) {
    set_error("corrupt file: invalid animation header");
    return 0;
  }
  if (h->mode == MODE_ANIM) {
    uint64_t count = h->palette_count;
    uint64_t table_bytes = count * sizeof(Frame);
    uint64_t frame_bytes = pixels * 4u;
    if (count > QLIC_MAX_FRAMES) {
      set_error("resource limit exceeded: animation frames");
      return 0;
    }
    if (table_bytes > QLIC_MAX_ANIMATION_BYTES ||
        frame_bytes > (QLIC_MAX_ANIMATION_BYTES - table_bytes) / count) {
      set_error("resource limit exceeded: animation memory");
      return 0;
    }
  }
  if (h->mode == MODE_BLOCKS &&
      (h->transform != TRANSFORM_IDENTITY ||
       (h->index_bits != BLK_SIZE && h->index_bits != CF_SIZE) ||
       h->palette_count != 0 || h->codec != CODEC_STORE)) {
    set_error("corrupt file: invalid block stream header");
    return 0;
  }
  if (h->mode == MODE_PALETTE &&
      !palette_count_ok(h->palette_count, h->index_bits)) {
    set_error("corrupt file: invalid palette header");
    return 0;
  }
  if (!file_palette_mode(h->mode) && h->mode != MODE_CPAL &&
      h->mode != MODE_TILES && h->mode != MODE_TILE_MODEL &&
      h->mode != MODE_ANIM && h->palette_count != 0) {
    set_error("corrupt file: unexpected palette data");
    return 0;
  }
  if (size < QLIC_HEADER_SIZE + QLIC_FOOTER_SIZE) {
    set_error("corrupt file: missing integrity footer");
    return 0;
  }
  size_t palette_size = file_palette_size(h);
  size_t start;
  if (!addok(QLIC_HEADER_SIZE, palette_size, &start))
    return 0;
  size_t body_size = size - QLIC_FOOTER_SIZE;
  if (start > body_size) {
    set_error("corrupt file: palette exceeds file size");
    return 0;
  }
  if (rd32(data + body_size) != stream_crc32(data, body_size)) {
    set_error("corrupt file: container checksum mismatch");
    return 0;
  }
  h->compressed_size = (uint64_t)(body_size - start);
  return 1;
}

static int unpack_payload(const uint8_t *src, size_t src_size, int codec,
                          uint64_t expected64, Buf *out) {
  if (expected64 > QLIC_MAX_PAYLOAD_BYTES) {
    set_error("resource limit exceeded: decoded payload bytes");
    return 0;
  }
  if (expected64 > (uint64_t)(size_t)-1) {
    set_error("file is too large for this browser build");
    return 0;
  }
  size_t expected = (size_t)expected64;
  if (codec == CODEC_STORE) {
    if (src_size != expected) {
      set_error("corrupt file: stored payload size mismatch");
      return 0;
    }
    out->data = (uint8_t *)src;
    out->size = src_size;
    out->cap = src_size;
    return 1;
  }
  if (codec != CODEC_LZMS) {
    set_error("this QLIC file uses an unsupported outer codec");
    return 0;
  }
  uint8_t *decoded = (uint8_t *)malloc(expected ? expected : 1u);
  if (!decoded) {
    set_error("out of memory");
    return 0;
  }
  if (!qlic_lzms_decompress(src, src_size, decoded, expected)) {
    free(decoded);
    set_error("corrupt file: invalid LZMS payload");
    return 0;
  }
  out->data = decoded;
  out->size = expected;
  out->cap = expected;
  return 1;
}

static const uint8_t *read_rgb(uint8_t *p, const uint8_t *s, int transform,
                               int alpha) {
  uint8_t a = alpha ? s[3] : 255;
  if (is_gd(transform)) {
    uint8_t g = s[0];
    p[0] = (uint8_t)(g + s[1]);
    p[1] = g;
    p[2] = (uint8_t)(g + s[2]);
  } else if (is_rd(transform)) {
    uint8_t r = s[0];
    p[0] = r;
    p[1] = (uint8_t)(r + s[1]);
    p[2] = (uint8_t)(r + s[2]);
  } else if (is_bd(transform)) {
    uint8_t b = s[0];
    p[0] = (uint8_t)(b + s[1]);
    p[1] = (uint8_t)(b + s[2]);
    p[2] = b;
  } else {
    p[0] = s[0];
    p[1] = s[1];
    p[2] = s[2];
  }
  p[3] = a;
  return s + (alpha ? 4u : 3u);
}

static int samp_rgba(const Buf *samples, const Head *h, const uint8_t *palette,
                     Image *out) {
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  if (h->mode == MODE_PALETTE) {
    size_t row_bytes = row_pack(h->width, h->index_bits);
    size_t need;
    if (!mulok(row_bytes, (size_t)h->height, &need))
      return 0;
    if (samples->size != need) {
      set_error("corrupt file: palette payload size mismatch");
      return 0;
    }
    for (uint32_t y = 0; y < h->height; ++y) {
      const uint8_t *row = samples->data + (size_t)y * row_bytes;
      for (uint32_t x = 0; x < h->width; ++x) {
        uint32_t idx = unpack_i(row, x, h->index_bits);
        if (idx >= h->palette_count) {
          set_error("corrupt file: palette index out of range");
          return 0;
        }
        memcpy(out->rgba + ((size_t)y * h->width + x) * 4u, palette + idx * 4u,
               4u);
      }
    }
    return 1;
  }
  int bpp = mbpp(h->mode);
  size_t need;
  if (bpp <= 0 || !mulok(pixels, (size_t)bpp, &need)) {
    set_error("corrupt file: invalid sample mode");
    return 0;
  }
  if (samples->size != need) {
    set_error("corrupt file: sample payload size mismatch");
    return 0;
  }
  const uint8_t *s = samples->data;
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t *p = out->rgba + i * 4u;
    if (h->mode == MODE_GRAY) {
      uint8_t y = *s++;
      p[0] = y;
      p[1] = y;
      p[2] = y;
      p[3] = 255;
    } else if (h->mode == MODE_GRAYA) {
      uint8_t y = *s++;
      uint8_t a = *s++;
      p[0] = y;
      p[1] = y;
      p[2] = y;
      p[3] = a;
    } else if (h->mode == MODE_RGB) {
      s = read_rgb(p, s, h->transform, 0);
    } else if (h->mode == MODE_RGBA) {
      s = read_rgb(p, s, h->transform, 1);
    }
  }
  return 1;
}

static uint8_t med_predict8(uint8_t left, uint8_t up, uint8_t upper_left) {
  int low = left < up ? left : up;
  int high = left > up ? left : up;
  int gradient = (int)left + (int)up - (int)upper_left;
  gradient = gradient < low ? low : gradient;
  gradient = gradient > high ? high : gradient;
  return (uint8_t)gradient;
}

static uint8_t planar_med_step(uint8_t *plane, size_t pos, size_t width) {
  uint8_t value = (uint8_t)(plane[pos] +
                            med_predict8(plane[pos - 1u], plane[pos - width],
                                         plane[pos - width - 1u]));
  plane[pos] = value;
  return value;
}

static int planar_med_rgba(Buf *residual, const Head *h, Image *out) {
  size_t pixels = 0;
  size_t payload_bytes = 0;
  size_t rgba_bytes = 0;
  int channels = mbpp(h->mode);
  if ((h->mode != MODE_RGB && h->mode != MODE_RGBA) ||
      !mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, (size_t)channels, &payload_bytes) ||
      residual->size != payload_bytes || !mulok(pixels, 4u, &rgba_bytes)) {
    set_error("corrupt file: invalid planar MED payload");
    return 0;
  }
  out->rgba = (uint8_t *)malloc(rgba_bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  uint8_t *blue = residual->data;
  uint8_t *red_delta = blue + pixels;
  uint8_t *green_delta = red_delta + pixels;
  uint8_t *alpha = channels == 4 ? green_delta + pixels : 0;

  for (uint32_t x = 0; x < h->width; ++x) {
    size_t pos = x;
    if (x) {
      blue[pos] = (uint8_t)(blue[pos] + blue[pos - 1u]);
      red_delta[pos] = (uint8_t)(red_delta[pos] + red_delta[pos - 1u]);
      green_delta[pos] =
          (uint8_t)(green_delta[pos] + green_delta[pos - 1u]);
      if (alpha)
        alpha[pos] = (uint8_t)(alpha[pos] + alpha[pos - 1u]);
    }
    uint8_t *pixel = out->rgba + pos * 4u;
    pixel[0] = (uint8_t)(blue[pos] + red_delta[pos]);
    pixel[1] = (uint8_t)(blue[pos] + green_delta[pos]);
    pixel[2] = blue[pos];
    pixel[3] = alpha ? alpha[pos] : 255u;
  }
  for (uint32_t y = 1; y < h->height; ++y) {
    size_t row = (size_t)y * h->width;
    blue[row] = (uint8_t)(blue[row] + blue[row - h->width]);
    red_delta[row] =
        (uint8_t)(red_delta[row] + red_delta[row - h->width]);
    green_delta[row] =
        (uint8_t)(green_delta[row] + green_delta[row - h->width]);
    if (alpha)
      alpha[row] = (uint8_t)(alpha[row] + alpha[row - h->width]);
    uint8_t *first = out->rgba + row * 4u;
    first[0] = (uint8_t)(blue[row] + red_delta[row]);
    first[1] = (uint8_t)(blue[row] + green_delta[row]);
    first[2] = blue[row];
    first[3] = alpha ? alpha[row] : 255u;
    for (uint32_t x = 1; x < h->width; ++x) {
      size_t pos = row + x;
      uint8_t b = planar_med_step(blue, pos, h->width);
      uint8_t rd = planar_med_step(red_delta, pos, h->width);
      uint8_t gd = planar_med_step(green_delta, pos, h->width);
      uint8_t a = alpha ? planar_med_step(alpha, pos, h->width) : 255u;
      uint8_t *pixel = out->rgba + pos * 4u;
      pixel[0] = (uint8_t)(b + rd);
      pixel[1] = (uint8_t)(b + gd);
      pixel[2] = b;
      pixel[3] = a;
    }
  }
  return 1;
}

static int samp_size(const Head *h, size_t *out) {
  size_t pixels;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels))
    return 0;
  if (h->mode == MODE_PALETTE)
    return mulok(row_pack(h->width, h->index_bits), (size_t)h->height, out);
  int bpp = mbpp(h->mode);
  if (bpp <= 0) {
    set_error("corrupt file: invalid sample mode");
    return 0;
  }
  return mulok(pixels, (size_t)bpp, out);
}

static int dec_irun(const Buf *runs, const Head *h, const uint8_t *palette,
                    Image *out) {
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  size_t pos = 0;
  size_t pixel = 0;
  while (pos < runs->size) {
    size_t runm1 = 0;
    size_t idx = 0;
    if (!read_varint(runs->data, runs->size, &pos, &runm1) ||
        !read_varint(runs->data, runs->size, &pos, &idx))
      return 0;
    if (runm1 == (size_t)-1) {
      set_error("corrupt file: palette run overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (idx >= h->palette_count || run > pixels - pixel) {
      set_error("corrupt file: bad palette run");
      return 0;
    }
    const uint8_t *c = palette + idx * 4u;
    for (size_t i = 0; i < run; ++i)
      memcpy(out->rgba + (pixel + i) * 4u, c, 4u);
    pixel += run;
  }
  if (pixel != pixels) {
    set_error("corrupt file: palette run payload size mismatch");
    return 0;
  }
  return 1;
}

static int dec_sep(const Buf *payload, const Head *h, Image *out) {
  int base_mode = h->index_bits;
  int bpp = mbpp(base_mode);
  if (bpp <= 0) {
    set_error("corrupt file: invalid separable base mode");
    return 0;
  }
  size_t sbpp = (size_t)bpp;
  size_t row_bytes;
  if (!mulok((size_t)h->width, sbpp, &row_bytes))
    return 0;
  size_t col_bytes;
  size_t expected;
  if (!mulok((size_t)(h->height - 1u), sbpp, &col_bytes) ||
      !addok(row_bytes, col_bytes, &expected))
    return 0;
  if (payload->size != expected) {
    set_error("corrupt file: separable payload size mismatch");
    return 0;
  }
  Buf raw = {0};
  const uint8_t *table = payload->data;
  if (h->transform == TRANSFORM_SEPARABLE_DELTA) {
    if (!buf_reserve(&raw, expected))
      return 0;
    raw.size = expected;
    uint8_t *row0 = raw.data;
    const uint8_t *delta = payload->data;
    memcpy(row0, delta, sbpp);
    delta += sbpp;
    for (uint32_t x = 1; x < h->width; ++x) {
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        row0[(size_t)x * sbpp + c] =
            (uint8_t)(row0[(size_t)(x - 1u) * sbpp + c] + *delta++);
      }
    }
    uint8_t *cols = raw.data + row_bytes;
    const uint8_t *prev = row0;
    for (uint32_t y = 1; y < h->height; ++y) {
      uint8_t *cur = cols + (size_t)(y - 1u) * sbpp;
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        cur[c] = (uint8_t)(prev[c] + *delta++);
      }
      prev = cur;
    }
    table = raw.data;
  }
  Buf samples = {0};
  size_t sample_size;
  if (!mulok(row_bytes, (size_t)h->height, &sample_size) ||
      !buf_reserve(&samples, sample_size))
    return 0;
  samples.size = sample_size;
  const uint8_t *row0 = table;
  const uint8_t *cols = table + row_bytes;
  for (uint32_t y = 0; y < h->height; ++y) {
    uint8_t *row = samples.data + (size_t)y * row_bytes;
    const uint8_t *col = y == 0 ? row0 : cols + (size_t)(y - 1u) * sbpp;
    for (uint32_t x = 0; x < h->width; ++x) {
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        row[(size_t)x * sbpp + c] =
            (uint8_t)(row0[(size_t)x * sbpp + c] + col[c] - row0[c]);
      }
    }
  }
  Head sh = *h;
  sh.mode = base_mode;
  sh.transform = TRANSFORM_IDENTITY_RAW;
  return samp_rgba(&samples, &sh, 0, out);
}

static int dec_stream(const Buf *payload, const Head *h, Image *out) {
  uint8_t *pix = 0;
  uint32_t w = 0;
  uint32_t hh = 0;
  int ch = 0;
  int e = stream_decode_trusted_expected_rgba(
      payload->data, payload->size, h->width, h->height, 0, &pix, &w, &hh,
      &ch);
  if (e != STREAM_OK) {
    set_error(stream_strerror(e));
    return 0;
  }
  if (w != h->width || hh != h->height || ch != 4) {
    free(pix);
    set_error("corrupt file: native stream dimensions mismatch");
    return 0;
  }
  out->rgba = pix;
  out->width = w;
  out->height = hh;
  return 1;
}

static void tile_copy(Image *out, uint32_t y0, uint32_t th, uint32_t w, int ch,
                      const uint8_t *pix) {
  uint8_t *dst0 = out->rgba + (size_t)y0 * w * 4u;
  if (ch == 4) {
    for (uint32_t y = 0; y < th; ++y)
      memcpy(dst0 + (size_t)y * w * 4u, pix + (size_t)y * w * 4u,
             (size_t)w * 4u);
  } else if (ch == 3) {
    for (uint32_t y = 0; y < th; ++y) {
      const uint8_t *s = pix + (size_t)y * w * 3u;
      uint8_t *d = dst0 + (size_t)y * w * 4u;
      for (uint32_t x = 0; x < w; ++x) {
        d[x * 4u + 0u] = s[x * 3u + 0u];
        d[x * 4u + 1u] = s[x * 3u + 1u];
        d[x * 4u + 2u] = s[x * 3u + 2u];
        d[x * 4u + 3u] = 255;
      }
    }
  } else {
    for (uint32_t y = 0; y < th; ++y) {
      const uint8_t *s = pix + (size_t)y * w;
      uint8_t *d = dst0 + (size_t)y * w * 4u;
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t v = s[x];
        d[x * 4u + 0u] = v;
        d[x * 4u + 1u] = v;
        d[x * 4u + 2u] = v;
        d[x * 4u + 3u] = 255;
      }
    }
  }
}

static int dec_tile(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 4u) {
    set_error("corrupt file: truncated tile stream payload");
    return 0;
  }
  uint32_t count = rd32(payload->data);
  uint32_t tile_h = h->palette_count;
  uint32_t want = 1u + (h->height - 1u) / tile_h;
  if (!count || count != want || count > QLIC_MAX_CHUNKS ||
      (size_t)count > (payload->size - 4u) / 4u) {
    set_error("corrupt file: invalid tile stream table");
    return 0;
  }
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  size_t off = 4u + (size_t)count * 4u;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t size = rd32(payload->data + 4u + (size_t)i * 4u);
    if ((size_t)size > payload->size - off) {
      set_error("corrupt file: tile stream chunk exceeds payload");
      goto corrupt;
    }
    uint32_t y0 = i * tile_h;
    uint32_t th = h->height - y0 < tile_h ? h->height - y0 : tile_h;
    uint8_t *pix = 0;
    uint32_t w = 0;
    uint32_t hh = 0;
    int ch = 0;
    int e = stream_decode_trusted_expected(
        payload->data + off, size, h->width, th, h->index_bits, &pix, &w, &hh,
        &ch);
    if (e != STREAM_OK || w != h->width || hh != th || ch != h->index_bits) {
      free(pix);
      set_error(e == STREAM_OK ? "corrupt file: tile dimensions mismatch"
                               : stream_strerror(e));
      goto corrupt;
    }
    tile_copy(out, y0, th, h->width, h->index_bits, pix);
    free(pix);
    off += size;
  }
  if (off != payload->size) {
    set_error("corrupt file: trailing tile stream data");
    goto corrupt;
  }
  return 1;

corrupt:
  free(out->rgba);
  image_zero(out);
  return 0;
}

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int rtt_pred_causal(const uint8_t *p, uint32_t w, int ch, uint32_t x,
                           uint32_t y, int c, int model) {
  size_t row = (size_t)w * (size_t)ch;
  size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
  int left = x ? p[i - (size_t)ch] : 0;
  int up = y ? p[i - row] : 0;
  int up_left = (x && y) ? p[i - row - (size_t)ch] : 0;
  int left2 = x > 1 ? p[i - (size_t)ch * 2u] : left;
  int up2 = y > 1 ? p[i - row * 2u] : up;
  switch (model) {
  case RTT_XD:
    return left;
  case RTT_YD:
    return up;
  case RTT_H2:
    return x ? clamp8(2 * left - left2) : 0;
  case RTT_V2:
    return y ? clamp8(2 * up - up2) : 0;
  default:
    return pred(5, left, up, up_left);
  }
}

static int rtt_pred_planar(const uint8_t *par, uint32_t w, uint32_t h,
                           uint32_t x, uint32_t y, int c) {
  int base = par[(size_t)c * 3u + 0u];
  int right = par[(size_t)c * 3u + 1u];
  int bottom = par[(size_t)c * 3u + 2u];
  int64_t px =
      w > 1 ? ((int64_t)(right - base) * (int64_t)x) / (int64_t)(w - 1u) : 0;
  int64_t py =
      h > 1 ? ((int64_t)(bottom - base) * (int64_t)y) / (int64_t)(h - 1u) : 0;
  return (int)((base + px + py) & 255);
}

static int rtt_recon(uint8_t *pix, uint32_t w, uint32_t h, int ch, int model,
                     const uint8_t *par) {
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
        int pr = model == RTT_PLANAR
                     ? rtt_pred_planar(par, w, h, x, y, c)
                     : rtt_pred_causal(pix, w, ch, x, y, c, model);
        pix[i] = (uint8_t)(pix[i] + pr);
      }
    }
  }
  return 1;
}

static int dec_rtt_chunk(const uint8_t *data, size_t size, uint8_t model,
                         uint32_t y0, uint32_t th, const Head *h, Image *out) {
  if (model > RTT_MAX_MODEL) {
    set_error("corrupt file: bad tile model");
    return 0;
  }
  if (model == RTT_FILT) {
    size_t row_bytes = 0;
    if (!mulok((size_t)h->width, (size_t)h->index_bits, &row_bytes) ||
        row_bytes + 1u > 0xffffffffu)
      return 0;
    uint8_t *pix = 0;
    uint32_t w = 0;
    uint32_t hh = 0;
    int ch = 0;
    int e = stream_decode_trusted_expected(
        data, size, (uint32_t)(row_bytes + 1u), th, 1, &pix, &w, &hh, &ch);
    if (e != STREAM_OK || w != (uint32_t)(row_bytes + 1u) || hh != th ||
        ch != 1) {
      free(pix);
      set_error(e == STREAM_OK ? "corrupt file: tile filter dimensions mismatch"
                               : stream_strerror(e));
      return 0;
    }
    Buf samples = {0};
    size_t payload_size;
    if (!mulok((size_t)w, (size_t)hh, &payload_size) ||
        !unf_rows(pix, payload_size, row_bytes, th, h->index_bits, &samples)) {
      free(pix);
      free(samples.data);
      return 0;
    }
    tile_copy(out, y0, th, h->width, h->index_bits, samples.data);
    free(pix);
    free(samples.data);
    return 1;
  }
  const uint8_t *par = 0;
  const uint8_t *zdata = data;
  size_t zsize = size;
  if (model == RTT_PLANAR) {
    size_t parn = (size_t)h->index_bits * 3u;
    if (zsize <= parn) {
      set_error("corrupt file: bad planar tile");
      return 0;
    }
    par = zdata;
    zdata += parn;
    zsize -= parn;
  }
  uint8_t *pix = 0;
  uint32_t w = 0;
  uint32_t hh = 0;
  int ch = 0;
  int e = stream_decode_trusted_expected(
      zdata, zsize, h->width, th, h->index_bits, &pix, &w, &hh, &ch);
  if (e != STREAM_OK || w != h->width || hh != th || ch != h->index_bits) {
    free(pix);
    set_error(e == STREAM_OK ? "corrupt file: tile model dimensions mismatch"
                             : stream_strerror(e));
    return 0;
  }
  if (model != RTT_RAW && !rtt_recon(pix, w, hh, ch, model, par)) {
    free(pix);
    return 0;
  }
  tile_copy(out, y0, th, h->width, h->index_bits, pix);
  free(pix);
  return 1;
}

static int dec_rtt(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 4u) {
    set_error("corrupt file: truncated tile model payload");
    return 0;
  }
  uint32_t count = rd32(payload->data);
  uint32_t tile_h = h->palette_count;
  uint32_t want = (h->height + tile_h - 1u) / tile_h;
  if (!count || count != want || count > 65536u ||
      (size_t)count > (payload->size - 4u) / 5u) {
    set_error("corrupt file: invalid tile model table");
    return 0;
  }
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  size_t off = 4u + (size_t)count * 5u;
  for (uint32_t i = 0; i < count; ++i) {
    size_t ent = 4u + (size_t)i * 5u;
    uint8_t model = payload->data[ent];
    uint32_t size = rd32(payload->data + ent + 1u);
    if (model > RTT_MAX_MODEL || (size_t)size > payload->size - off) {
      set_error("corrupt file: tile model chunk exceeds payload");
      return 0;
    }
    uint32_t y0 = i * tile_h;
    uint32_t th = h->height - y0 < tile_h ? h->height - y0 : tile_h;
    if (!dec_rtt_chunk(payload->data + off, size, model, y0, th, h, out))
      return 0;
    off += size;
  }
  if (off != payload->size) {
    set_error("corrupt file: trailing tile model data");
    return 0;
  }
  return 1;
}

static int dec_gmodel(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 4u) {
    set_error("corrupt file: truncated gray-model payload");
    return 0;
  }
  int p = payload->data[0];
  int bl = payload->data[1];
  int a = payload->data[2];
  int b = payload->data[3];
  if (!((p == 8 || p == 16 || p == 32) && (bl == 0 || bl == 8 || bl == 16))) {
    set_error("corrupt file: invalid gray-model parameters");
    return 0;
  }
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  size_t pn = (size_t)p * (size_t)p;
  uint32_t bw = 0;
  uint32_t bh = 0;
  size_t blocks = 0;
  if (bl > 0) {
    bw = (h->width + (uint32_t)bl - 1u) / (uint32_t)bl;
    bh = (h->height + (uint32_t)bl - 1u) / (uint32_t)bl;
    if (!mulok((size_t)bw, (size_t)bh, &blocks))
      return 0;
  }
  size_t need = 0;
  if (!addok(4u, pn, &need) || !addok(need, blocks, &need) ||
      !addok(need, pixels, &need))
    return 0;
  if (payload->size != need) {
    set_error("corrupt file: gray-model payload size mismatch");
    return 0;
  }
  const uint8_t *phase = payload->data + 4u;
  const uint8_t *bt = phase + pn;
  const uint8_t *res = bt + blocks;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  if (bl > 0) {
    size_t ri = 0;
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t bv = bt[(size_t)by * bw + bx];
        uint32_t y1 = by * (uint32_t)bl;
        uint32_t y2 =
            y1 + (uint32_t)bl < h->height ? y1 + (uint32_t)bl : h->height;
        uint32_t x1 = bx * (uint32_t)bl;
        uint32_t x2 =
            x1 + (uint32_t)bl < h->width ? x1 + (uint32_t)bl : h->width;
        for (uint32_t y = y1; y < y2; ++y) {
          uint32_t byv = (uint32_t)b * y;
          uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
          for (uint32_t x = x1; x < x2; ++x) {
            uint8_t v =
                (uint8_t)((uint32_t)a * x + byv +
                          phase[py + (x % (uint32_t)p)] + bv + res[ri++]);
            uint8_t *d = out->rgba + ((size_t)y * h->width + x) * 4u;
            d[0] = v;
            d[1] = v;
            d[2] = v;
            d[3] = 255;
          }
        }
      }
    }
    if (ri != pixels) {
      set_error("corrupt file: gray-model residual mismatch");
      return 0;
    }
  } else {
    for (uint32_t y = 0; y < h->height; ++y) {
      uint32_t byv = (uint32_t)b * y;
      uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
      for (uint32_t x = 0; x < h->width; ++x) {
        size_t i = (size_t)y * h->width + x;
        uint8_t v = (uint8_t)((uint32_t)a * x + byv +
                              phase[py + (x % (uint32_t)p)] + res[i]);
        uint8_t *d = out->rgba + i * 4u;
        d[0] = v;
        d[1] = v;
        d[2] = v;
        d[3] = 255;
      }
    }
  }
  return 1;
}

static int dec_filtered(const Buf *payload, const Head *h, Image *out) {
  int base_mode = h->index_bits;
  int bpp = mbpp(base_mode);
  if (bpp <= 0) {
    set_error("corrupt file: invalid filtered stream base mode");
    return 0;
  }
  size_t row_bytes;
  if (!mulok((size_t)h->width, (size_t)bpp, &row_bytes) ||
      row_bytes + 1u > 0xffffffffu)
    return 0;
  uint8_t *pix = 0;
  uint32_t zw = 0;
  uint32_t zh = 0;
  int zch = 0;
  int e = stream_decode_trusted_expected(
      payload->data, payload->size, (uint32_t)(row_bytes + 1u), h->height, 1,
      &pix, &zw, &zh, &zch);
  if (e != STREAM_OK || zw != (uint32_t)(row_bytes + 1u) || zh != h->height ||
      zch != 1) {
    free(pix);
    set_error(e == STREAM_OK
                  ? "corrupt file: filtered stream dimensions mismatch"
                  : stream_strerror(e));
    return 0;
  }
  Buf samples = {0};
  size_t payload_size;
  if (!mulok((size_t)zw, (size_t)zh, &payload_size) ||
      !unf_rows(pix, payload_size, row_bytes, h->height, bpp, &samples)) {
    free(pix);
    free(samples.data);
    return 0;
  }
  free(pix);
  Head sh = *h;
  sh.mode = base_mode;
  sh.index_bits = 0;
  int ok = samp_rgba(&samples, &sh, 0, out);
  free(samples.data);
  return ok;
}

static int dec_pstream(const Buf *payload, const Head *h,
                       const uint8_t *palette, Image *out) {
  size_t row_bytes = row_pack(h->width, h->index_bits);
  size_t zrow = row_bytes;
  if (h->transform == TRANSFORM_IDENTITY)
    zrow = row_bytes + 1u;
  else if (h->transform != TRANSFORM_IDENTITY_RAW) {
    set_error("corrupt file: invalid palette stream transform");
    return 0;
  }
  if (zrow > 0xffffffffu)
    return 0;
  uint8_t *pix = 0;
  uint32_t zw = 0;
  uint32_t zh = 0;
  int zch = 0;
  int e = stream_decode_trusted_expected(
      payload->data, payload->size, (uint32_t)zrow, h->height, 1, &pix, &zw,
      &zh, &zch);
  if (e != STREAM_OK || zw != (uint32_t)zrow || zh != h->height || zch != 1) {
    free(pix);
    set_error(e == STREAM_OK
                  ? "corrupt file: palette stream dimensions mismatch"
                  : stream_strerror(e));
    return 0;
  }
  Buf samples = {0};
  int ok = 0;
  if (h->transform == TRANSFORM_IDENTITY) {
    size_t payload_size;
    ok = mulok(zrow, (size_t)zh, &payload_size) &&
         unf_rows(pix, payload_size, row_bytes, h->height,
                  h->index_bits == 16 ? 2 : 1, &samples);
  } else {
    size_t bytes;
    ok = mulok(row_bytes, (size_t)h->height, &bytes) &&
         buf_reserve(&samples, bytes);
    if (ok) {
      memcpy(samples.data, pix, bytes);
      samples.size = bytes;
    }
  }
  free(pix);
  if (!ok) {
    free(samples.data);
    return 0;
  }
  Head ph = *h;
  ph.mode = MODE_PALETTE;
  ok = samp_rgba(&samples, &ph, palette, out);
  free(samples.data);
  return ok;
}

static int dec_ppal(const Buf *payload, const Head *h, const uint8_t *palette,
                    Image *out) {
  size_t pixels;
  size_t bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  uint16_t *ids = (uint16_t *)malloc(pixels * sizeof(uint16_t));
  if (!ids) {
    set_error("out of memory");
    return 0;
  }
  size_t in = 0;
  size_t pos = 0;
  while (pos < pixels) {
    if (in >= payload->size) {
      set_error("corrupt file: truncated ppal stream");
      return 0;
    }
    uint8_t op = payload->data[in++];
    if (op > 3) {
      set_error("corrupt file: bad ppal opcode");
      return 0;
    }
    if (op == 3) {
      size_t v = 0;
      if (!read_varint(payload->data, payload->size, &in, &v) ||
          v >= h->palette_count) {
        set_error("corrupt file: bad ppal literal");
        return 0;
      }
      ids[pos++] = (uint16_t)v;
      continue;
    }
    size_t runm1 = 0;
    if (!read_varint(payload->data, payload->size, &in, &runm1))
      return 0;
    if (runm1 == (size_t)-1) {
      set_error("corrupt file: ppal run overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (run > pixels - pos) {
      set_error("corrupt file: ppal run exceeds image");
      return 0;
    }
    for (size_t i = 0; i < run; ++i) {
      size_t p = pos + i;
      uint32_t x = (uint32_t)(p % h->width);
      uint32_t y = (uint32_t)(p / h->width);
      if (op == 0) {
        if (x == 0) {
          set_error("corrupt file: bad ppal left run");
          return 0;
        }
        ids[p] = ids[p - 1u];
      } else if (op == 1) {
        if (y == 0) {
          set_error("corrupt file: bad ppal up run");
          return 0;
        }
        ids[p] = ids[p - h->width];
      } else {
        if (x == 0 || y == 0) {
          set_error("corrupt file: bad ppal diagonal run");
          return 0;
        }
        ids[p] = ids[p - h->width - 1u];
      }
    }
    pos += run;
  }
  if (in != payload->size) {
    set_error("corrupt file: trailing ppal data");
    return 0;
  }
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  for (size_t i = 0; i < pixels; ++i) {
    if (ids[i] >= h->palette_count) {
      set_error("corrupt file: palette index out of range");
      return 0;
    }
    memcpy(out->rgba + i * 4u, palette + (size_t)ids[i] * 4u, 4u);
  }
  return 1;
}

static int dec_cpal_tiles(const Buf *payload, const Head *h, Image *out) {
  enum { MIN_TILE_LOG = 3, MAX_TILE_LOG = 6 };
  size_t palette_size = (size_t)h->palette_count * 4u;
  size_t pixels = 0;
  size_t rgba_bytes = 0;
  if (payload->size < 1u + palette_size ||
      !mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &rgba_bytes)) {
    set_error("corrupt file: tile-palette payload size mismatch");
    return 0;
  }
  unsigned tile_log = payload->data[0];
  if (tile_log < MIN_TILE_LOG || tile_log > MAX_TILE_LOG) {
    set_error("corrupt file: invalid tile-palette size");
    return 0;
  }
  uint32_t tile_size = 1u << tile_log;
  size_t max_tile_pixels = (size_t)tile_size * tile_size;
  uint16_t *local_palette =
      (uint16_t *)malloc(max_tile_pixels * sizeof(uint16_t));
  out->rgba = (uint8_t *)malloc(rgba_bytes);
  if (!local_palette || !out->rgba) {
    free(local_palette);
    free(out->rgba);
    out->rgba = 0;
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  const uint8_t *palette = payload->data + 1u;
  size_t position = 1u + palette_size;

  for (uint32_t y0 = 0; y0 < h->height; y0 += tile_size) {
    uint32_t tile_height = h->height - y0 < tile_size
                               ? h->height - y0
                               : tile_size;
    for (uint32_t x0 = 0; x0 < h->width; x0 += tile_size) {
      uint32_t tile_width =
          h->width - x0 < tile_size ? h->width - x0 : tile_size;
      size_t tile_pixels = (size_t)tile_width * tile_height;
      size_t local_count_m1 = 0;
      if (!read_varint(payload->data, payload->size, &position,
                       &local_count_m1) ||
          local_count_m1 >= tile_pixels ||
          local_count_m1 >= h->palette_count) {
        set_error("corrupt file: invalid tile-local palette size");
        goto corrupt;
      }
      size_t local_count = local_count_m1 + 1u;
      size_t previous = 0;
      for (size_t i = 0; i < local_count; ++i) {
        size_t gap = 0;
        if (!read_varint(payload->data, payload->size, &position, &gap) ||
            (i && (previous == (size_t)-1 ||
                   gap > (size_t)-1 - previous - 1u))) {
          set_error("corrupt file: invalid tile-local palette gap");
          goto corrupt;
        }
        size_t global_index = i ? previous + gap + 1u : gap;
        if (global_index >= h->palette_count) {
          set_error("corrupt file: tile-local palette index out of range");
          goto corrupt;
        }
        local_palette[i] = (uint16_t)global_index;
        previous = global_index;
      }

      int local_bits = pal_bits((uint32_t)local_count);
      size_t packed_bits = 0;
      if (!mulok(tile_pixels, (size_t)local_bits, &packed_bits) ||
          packed_bits > (size_t)-1 - 7u) {
        set_error("corrupt file: tile-local index size overflow");
        goto corrupt;
      }
      size_t packed_bytes = (packed_bits + 7u) >> 3;
      if (position > payload->size ||
          packed_bytes > payload->size - position) {
        set_error("corrupt file: truncated tile-local indices");
        goto corrupt;
      }
      const uint8_t *packed = payload->data + position;
      size_t pixel_index = 0;
      for (uint32_t y = 0; y < tile_height; ++y) {
        size_t output = ((size_t)(y0 + y) * h->width + x0) * 4u;
        for (uint32_t x = 0; x < tile_width; ++x, ++pixel_index) {
          uint32_t local_index =
              unpack_i(packed, (uint32_t)pixel_index, local_bits);
          if (local_index >= local_count) {
            set_error("corrupt file: tile-local index out of range");
            goto corrupt;
          }
          size_t source = (size_t)local_palette[local_index] * 4u;
          size_t destination = output + (size_t)x * 4u;
          for (unsigned channel = 0; channel < 4u; ++channel)
            out->rgba[destination + channel] = palette[source + channel];
        }
      }
      position += packed_bytes;
    }
  }
  free(local_palette);
  if (position != payload->size) {
    set_error("corrupt file: trailing tile-palette data");
    free(out->rgba);
    image_zero(out);
    return 0;
  }
  return 1;

corrupt:
  free(local_palette);
  free(out->rgba);
  image_zero(out);
  return 0;
}

static int dec_cpal(const Buf *payload, const Head *h, Image *out) {
  if (h->transform == TRANSFORM_CPAL_TILES)
    return dec_cpal_tiles(payload, h, out);
  size_t palette_size = (size_t)h->palette_count * 4u;
  if (payload->size < palette_size) {
    set_error("corrupt file: cpalette payload size mismatch");
    return 0;
  }
  if (h->transform == TRANSFORM_CPAL_PLANAR) {
    size_t pixels = 0;
    size_t index_bytes = 0;
    size_t expected = 0;
    size_t rgba_bytes = 0;
    if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
        !mulok(pixels, 2u, &index_bytes) ||
        !addok(1u, palette_size, &expected) ||
        !addok(expected, index_bytes, &expected) ||
        payload->size != expected || !mulok(pixels, 4u, &rgba_bytes)) {
      set_error("corrupt file: planar cpalette payload size mismatch");
      return 0;
    }
    unsigned layout = payload->data[0];
    if (layout > 1u) {
      set_error("corrupt file: invalid planar cpalette layout");
      return 0;
    }
    uint8_t *palette = (uint8_t *)malloc(palette_size);
    uint8_t *rgba = (uint8_t *)malloc(rgba_bytes);
    if (!palette || !rgba) {
      free(palette);
      free(rgba);
      set_error("out of memory");
      return 0;
    }
    size_t position = 1u;
    for (unsigned channel = 0; channel < 4u; ++channel) {
      uint8_t previous = 0;
      for (uint32_t i = 0; i < h->palette_count; ++i) {
        uint8_t delta = payload->data[position++];
        uint8_t value = i ? (uint8_t)(previous + delta) : delta;
        palette[(size_t)i * 4u + channel] = value;
        previous = value;
      }
    }
    const uint8_t *indices = payload->data + position;
    const uint8_t *high = layout == 1u ? indices + pixels : NULL;
    for (size_t i = 0; i < pixels; ++i) {
      uint32_t index = layout == 0u
                           ? rd16(indices + i * 2u)
                           : (uint32_t)indices[i] |
                                 ((uint32_t)high[i] << 8);
      if (index >= h->palette_count) {
        free(palette);
        free(rgba);
        set_error("corrupt file: planar cpalette index out of range");
        return 0;
      }
      size_t source = (size_t)index * 4u;
      size_t destination = i * 4u;
      for (unsigned channel = 0; channel < 4u; ++channel)
        rgba[destination + channel] = palette[source + channel];
    }
    free(palette);
    out->rgba = rgba;
    out->width = h->width;
    out->height = h->height;
    return 1;
  }
  if (h->transform == TRANSFORM_INDEX_RLE) {
    Buf runs = {payload->data + palette_size, payload->size - palette_size,
                payload->size - palette_size};
    return dec_irun(&runs, h, payload->data, out);
  }
  if (h->transform == TRANSFORM_CPAL_DELTA) {
    uint8_t *palette = (uint8_t *)malloc(palette_size);
    if (!palette) {
      set_error("out of memory");
      return 0;
    }
    for (size_t i = 0; i < palette_size; ++i) {
      palette[i] = i < 4u ? payload->data[i]
                          : (uint8_t)(palette[i - 4u] + payload->data[i]);
    }
    Buf runs = {payload->data + palette_size, payload->size - palette_size,
                payload->size - palette_size};
    int ok = dec_irun(&runs, h, palette, out);
    free(palette);
    return ok;
  }
  size_t row_bytes = row_pack(h->width, h->index_bits);
  size_t index_size;
  if (!mulok(row_bytes, (size_t)h->height, &index_size))
    return 0;
  if (payload->size != palette_size + index_size) {
    set_error("corrupt file: cpalette payload size mismatch");
    return 0;
  }
  Buf samples = {payload->data + palette_size, index_size, index_size};
  Head ph = *h;
  ph.mode = MODE_PALETTE;
  return samp_rgba(&samples, &ph, payload->data, out);
}

typedef struct {
  uint32_t v[4];
} QBlockPat;

typedef struct {
  const uint8_t *p;
  size_t n;
  size_t pos;
} QBlockReader;

static inline uint32_t block_extent(uint32_t total, uint32_t origin,
                                    uint32_t limit) {
  uint32_t left = total - origin;
  return left < limit ? left : limit;
}

static int blk_bits(int n) {
  int b = 0;
  int v = n - 1;
  while (v > 0) {
    ++b;
    v >>= 1;
  }
  return b;
}

static uint32_t blk_color(const Image *im, uint32_t x, uint32_t y, int ch) {
  const uint8_t *p = im->rgba + ((size_t)y * im->width + x) * 4u;
  if (ch == 1)
    return p[0];
  if (ch == 3)
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void blk_set(Image *im, uint32_t x, uint32_t y, uint32_t c, int ch) {
  uint8_t *p = im->rgba + ((size_t)y * im->width + x) * 4u;
  p[0] = (uint8_t)c;
  p[1] = ch == 1 ? (uint8_t)c : (uint8_t)(c >> 8);
  p[2] = ch == 1 ? (uint8_t)c : (uint8_t)(c >> 16);
  p[3] = ch == 4 ? (uint8_t)(c >> 24) : 255;
}

static int blk2_abs(int v) { return v < 0 ? -v : v; }

static uint8_t blk2_sat(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return (uint8_t)v;
}

static int blk2_s8(uint8_t v) { return v < 128u ? (int)v : (int)v - 256; }

static int blk2_unfold(uint8_t v) {
  return (v & 1u) ? -((int)v + 1) / 2 : (int)v / 2;
}

static size_t blk2_i(uint32_t w, int ch, uint32_t x, uint32_t y, int c) {
  return ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
}

static uint8_t blk2_at(const uint8_t *pix, uint32_t w, int ch, uint32_t x,
                       uint32_t y, int c) {
  return pix[blk2_i(w, ch, x, y, c)];
}

static size_t blk2_parn(uint8_t op, int ch) {
  if (op == BLK2_FLAT || op == BLK2_LEFT || op == BLK2_UP)
    return (size_t)ch;
  if (op == BLK2_GRAD)
    return (size_t)ch * 4u;
  if (op == BLK2_PDM)
    return 3u + (size_t)ch * 7u;
  return 0;
}

static int blk2_paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = blk2_abs(p - a);
  int pb = blk2_abs(p - b);
  int pc = blk2_abs(p - c);
  if (pa <= pb && pa <= pc)
    return a;
  return pb <= pc ? b : c;
}

static uint8_t blk2_causal(const uint8_t *pix, uint32_t w, int ch, uint32_t x,
                           uint32_t y, int c) {
  if (!x && !y)
    return 0;
  int a =
      x ? blk2_at(pix, w, ch, x - 1u, y, c) : blk2_at(pix, w, ch, x, y - 1u, c);
  int b = y ? blk2_at(pix, w, ch, x, y - 1u, c) : a;
  int n = (x && y) ? blk2_at(pix, w, ch, x - 1u, y - 1u, c) : b;
  return (uint8_t)blk2_paeth(a, b, n);
}

static uint8_t blk2_linear(const uint8_t *pix, uint32_t w, int ch, uint32_t x,
                           uint32_t y, int c) {
  if (!x || !y)
    return blk2_causal(pix, w, ch, x, y, c);
  int a = blk2_at(pix, w, ch, x - 1u, y, c);
  int b = blk2_at(pix, w, ch, x, y - 1u, c);
  int n = blk2_at(pix, w, ch, x - 1u, y - 1u, c);
  return blk2_sat(a + b - n);
}

static uint8_t blk2_grad(const uint8_t *p, uint32_t bw, uint32_t bh,
                         uint32_t xx, uint32_t yy, int c) {
  const uint8_t *q = p + (size_t)c * 4u;
  if (bw == 1u && bh == 1u)
    return q[0];
  if (bh == 1u) {
    uint32_t dx = bw - 1u;
    return (
        uint8_t)(((uint32_t)q[0] * (dx - xx) + (uint32_t)q[1] * xx + dx / 2u) /
                 dx);
  }
  if (bw == 1u) {
    uint32_t dy = bh - 1u;
    return (
        uint8_t)(((uint32_t)q[0] * (dy - yy) + (uint32_t)q[2] * yy + dy / 2u) /
                 dy);
  }
  uint32_t dx = bw - 1u;
  uint32_t dy = bh - 1u;
  uint32_t top =
      ((uint32_t)q[0] * (dx - xx) + (uint32_t)q[1] * xx + dx / 2u) / dx;
  uint32_t bot =
      ((uint32_t)q[2] * (dx - xx) + (uint32_t)q[3] * xx + dx / 2u) / dx;
  return (uint8_t)((top * (dy - yy) + bot * yy + dy / 2u) / dy);
}

static uint8_t pdm_q(const uint8_t *par, int i) {
  if (i == 0)
    return par[0] & 15u;
  if (i == 1)
    return par[0] >> 4;
  if (i == 2)
    return par[1] & 15u;
  if (i == 3)
    return par[1] >> 4;
  return par[2] & 15u;
}

static uint8_t pdm_lerp(uint8_t a, uint8_t b, uint8_t q) {
  return (uint8_t)(((uint32_t)a * (15u - q) + (uint32_t)b * q + 7u) / 15u);
}

static uint8_t pdm_pred(const uint8_t *pix, uint32_t w, uint32_t x, uint32_t y,
                        uint32_t bw, uint32_t bh, uint32_t step, uint32_t xx,
                        uint32_t yy, int ch, int c, const uint8_t *par) {
  const uint8_t *flat = par + 3u;
  const uint8_t *grad = flat + (size_t)ch;
  const uint8_t *left = grad + (size_t)ch * 4u;
  const uint8_t *up = left + (size_t)ch;
  uint8_t ca = blk2_causal(pix, w, ch, x + xx, y + yy, c);
  uint8_t ln = blk2_linear(pix, w, ch, x + xx, y + yy, c);
  uint8_t lp = ca;
  uint8_t upv = ca;
  if (x >= step) {
    uint8_t v = blk2_at(pix, w, ch, x - step + xx, y + yy, c);
    lp = blk2_sat((int)v + blk2_s8(left[c]));
  }
  if (y >= step) {
    uint8_t v = blk2_at(pix, w, ch, x + xx, y - step + yy, c);
    upv = blk2_sat((int)v + blk2_s8(up[c]));
  }
  uint8_t axis = pdm_lerp(upv, lp, pdm_q(par, 1));
  uint8_t edge = pdm_lerp(ca, ln, pdm_q(par, 4));
  uint8_t nb = pdm_lerp(axis, edge, pdm_q(par, 3));
  uint8_t block =
      pdm_lerp(flat[c], blk2_grad(grad, bw, bh, xx, yy, c), pdm_q(par, 2));
  return pdm_lerp(nb, block, pdm_q(par, 0));
}

static void cf_base(const uint8_t *pix, uint32_t w, uint32_t x, uint32_t y,
                    uint32_t bw, uint32_t bh, int ch, uint8_t *par) {
  uint8_t *flat = par + 3u;
  uint8_t *grad = flat + (size_t)ch;
  uint8_t *left = grad + (size_t)ch * 4u;
  uint8_t *up = left + (size_t)ch;
  for (int c = 0; c < ch; ++c) {
    uint32_t sum = 0;
    uint32_t n = 0;
    if (y) {
      for (uint32_t xx = 0; xx < bw; ++xx) {
        sum += blk2_at(pix, w, ch, x + xx, y - 1u, c);
        ++n;
      }
    }
    if (x) {
      for (uint32_t yy = 0; yy < bh; ++yy) {
        sum += blk2_at(pix, w, ch, x - 1u, y + yy, c);
        ++n;
      }
    }
    uint8_t f = n ? (uint8_t)((sum + n / 2u) / n) : 0;
    uint8_t tl = x && y ? blk2_at(pix, w, ch, x - 1u, y - 1u, c) : f;
    uint8_t tr = y ? blk2_at(pix, w, ch, x + bw - 1u, y - 1u, c) : f;
    uint8_t bl = x ? blk2_at(pix, w, ch, x - 1u, y + bh - 1u, c) : f;
    flat[c] = f;
    grad[(size_t)c * 4u + 0u] = tl;
    grad[(size_t)c * 4u + 1u] = tr;
    grad[(size_t)c * 4u + 2u] = bl;
    grad[(size_t)c * 4u + 3u] = blk2_sat((int)tr + (int)bl - (int)tl);
    left[c] = 0;
    up[c] = 0;
  }
}

static uint8_t blk2_pred(const uint8_t *pix, uint32_t w, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, uint32_t xx, uint32_t yy,
                         int ch, int c, uint8_t op, const uint8_t *par) {
  if (op == BLK2_FLAT)
    return par[c];
  if (op == BLK2_GRAD)
    return blk2_grad(par, bw, bh, xx, yy, c);
  if (op == BLK2_LEFT)
    return blk2_sat((int)blk2_at(pix, w, ch, x - BLK_SIZE + xx, y + yy, c) +
                    blk2_s8(par[c]));
  if (op == BLK2_UP)
    return blk2_sat((int)blk2_at(pix, w, ch, x + xx, y - BLK_SIZE + yy, c) +
                    blk2_s8(par[c]));
  if (op == BLK2_CAUSAL)
    return blk2_causal(pix, w, ch, x + xx, y + yy, c);
  if (op == BLK2_PDM)
    return pdm_pred(pix, w, x, y, bw, bh, BLK_SIZE, xx, yy, ch, c, par);
  return 0;
}

static int qr_need(QBlockReader *r, size_t n) {
  if (n > r->n - r->pos) {
    set_error("corrupt file: truncated block stream");
    return 0;
  }
  return 1;
}

static int qr_u8(QBlockReader *r, uint8_t *v) {
  if (!qr_need(r, 1u))
    return 0;
  *v = r->p[r->pos++];
  return 1;
}

static int qr_color(QBlockReader *r, int ch, uint32_t *v) {
  if (!qr_need(r, (size_t)ch))
    return 0;
  const uint8_t *p = r->p + r->pos;
  if (ch == 1)
    *v = p[0];
  else if (ch == 3)
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  else
    *v = rd32(p);
  r->pos += (size_t)ch;
  return 1;
}

static int qr_bits(QBlockReader *r, uint8_t *idx, size_t n, int bits,
                   int maxv) {
  if (bits <= 0) {
    memset(idx, 0, n);
    return 1;
  }
  size_t bytes = (n * (size_t)bits + 7u) >> 3;
  if (!qr_need(r, bytes))
    return 0;
  uint32_t acc = 0;
  int used = 0;
  size_t p = r->pos;
  for (size_t i = 0; i < n; ++i) {
    while (used < bits) {
      acc |= (uint32_t)r->p[p++] << used;
      used += 8;
    }
    uint8_t v = (uint8_t)(acc & ((1u << bits) - 1u));
    if (v >= (uint8_t)maxv) {
      set_error("corrupt file: invalid block index");
      return 0;
    }
    idx[i] = v;
    acc >>= bits;
    used -= bits;
  }
  r->pos += bytes;
  return 1;
}

static void dec_blk_fill(Image *out, uint32_t x, uint32_t y, uint32_t bw,
                         uint32_t bh, uint32_t color, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy)
    for (uint32_t xx = 0; xx < bw; ++xx)
      blk_set(out, x + xx, y + yy, color, ch);
}

static void dec_blk_copy(Image *out, uint32_t x, uint32_t y, uint32_t sx,
                         uint32_t sy, uint32_t bw, uint32_t bh, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy)
    for (uint32_t xx = 0; xx < bw; ++xx)
      blk_set(out, x + xx, y + yy, blk_color(out, sx + xx, sy + yy, ch), ch);
}

static int dec_blk_raw(QBlockReader *r, Image *out, uint32_t x, uint32_t y,
                       uint32_t bw, uint32_t bh, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint32_t c = 0;
      if (!qr_color(r, ch, &c))
        return 0;
      blk_set(out, x + xx, y + yy, c, ch);
    }
  }
  return 1;
}

static int dec_blk_map(QBlockReader *r, Image *out, uint32_t x, uint32_t y,
                       uint32_t bw, uint32_t bh, uint32_t *colors, int count,
                       int ch) {
  uint8_t idx[BLK_SIZE * BLK_SIZE];
  uint32_t area = bw * bh;
  if (!qr_bits(r, idx, area, blk_bits(count), count))
    return 0;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint8_t k = idx[(size_t)yy * bw + xx];
      blk_set(out, x + xx, y + yy, colors[k], ch);
    }
  }
  return 1;
}

static int dec_blk_pat(QBlockReader *r, Image *out, uint32_t x, uint32_t y,
                       uint32_t bw, uint32_t bh, int ch) {
  uint8_t n8 = 0;
  if (!qr_u8(r, &n8) || n8 < 2 || n8 > 16) {
    set_error("corrupt file: invalid block pattern table");
    return 0;
  }
  QBlockPat pat[16];
  for (int i = 0; i < (int)n8; ++i)
    for (int j = 0; j < 4; ++j)
      if (!qr_color(r, ch, &pat[i].v[j]))
        return 0;
  uint32_t pw = (bw + 1u) >> 1;
  uint32_t ph = (bh + 1u) >> 1;
  uint8_t idx[64];
  if (!qr_bits(r, idx, (size_t)pw * ph, blk_bits(n8), n8))
    return 0;
  for (uint32_t py = 0; py < ph; ++py) {
    for (uint32_t px = 0; px < pw; ++px) {
      QBlockPat *p = &pat[idx[(size_t)py * pw + px]];
      uint32_t x0 = x + px * 2u;
      uint32_t y0 = y + py * 2u;
      blk_set(out, x0, y0, p->v[0], ch);
      if (px * 2u + 1u < bw)
        blk_set(out, x0 + 1u, y0, p->v[1], ch);
      if (py * 2u + 1u < bh) {
        blk_set(out, x0, y0 + 1u, p->v[2], ch);
        if (px * 2u + 1u < bw)
          blk_set(out, x0 + 1u, y0 + 1u, p->v[3], ch);
      }
    }
  }
  return 1;
}

static int dec_cf_regions(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 20u || memcmp(payload->data, "QCF1", 4u) ||
      payload->data[4] != CF_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_error("corrupt file: invalid coordinate stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8u);
  uint64_t z64 = rd64(payload->data + 12u);
  if (z64 > (uint64_t)(size_t)-1) {
    set_error("corrupt file: coordinate residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t table_end = 0;
  size_t end = 0;
  if (!addok(20u, table_size, &table_end) || !addok(table_end, zsize, &end) ||
      end != payload->size) {
    set_error("corrupt file: coordinate residual size mismatch");
    return 0;
  }
  uint64_t rx = (h->width + CF_SIZE - 1u) / CF_SIZE;
  uint64_t ry = (h->height + CF_SIZE - 1u) / CF_SIZE;
  if (rx && ry && table_size != rx * ry * 3u) {
    set_error("corrupt file: coordinate table size mismatch");
    return 0;
  }
  uint8_t *res = 0;
  uint32_t w = 0;
  uint32_t hh = 0;
  int zch = 0;
  int e = stream_decode_trusted_expected(
      payload->data + table_end, zsize, h->width, h->height, ch, &res, &w,
      &hh, &zch);
  if (e != STREAM_OK) {
    set_error(stream_strerror(e));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    free(res);
    set_error("corrupt file: coordinate residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0;
  size_t compact = 0;
  size_t rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact) || !mulok(pixels, 4u, &rgba_bytes)) {
    free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)malloc(compact ? compact : 1u);
  if (!pix) {
    free(res);
    set_error("out of memory");
    return 0;
  }
  size_t tpos = 0;
  for (uint32_t y = 0; y < h->height; y += CF_SIZE) {
    for (uint32_t x = 0; x < h->width; x += CF_SIZE) {
      uint32_t bw = block_extent(h->width, x, CF_SIZE);
      uint32_t bh = block_extent(h->height, y, CF_SIZE);
      if (tpos > table_size || table_size - tpos < 3u) {
        free(pix);
        free(res);
        set_error("corrupt file: truncated coordinate table");
        return 0;
      }
      uint8_t par[64];
      memset(par, 0, sizeof(par));
      cf_base(pix, w, x, y, bw, bh, ch, par);
      memcpy(par, payload->data + 20u + tpos, 3u);
      tpos += 3u;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            size_t pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p =
                pdm_pred(pix, w, x, y, bw, bh, CF_SIZE, xx, yy, ch, c, par);
            pix[pos] = (uint8_t)((int)p + blk2_unfold(res[pos]));
          }
        }
      }
    }
  }
  free(res);
  if (tpos != table_size) {
    free(pix);
    set_error("corrupt file: trailing coordinate table data");
    return 0;
  }
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = hh;
    return 1;
  }
  out->rgba = (uint8_t *)malloc(rgba_bytes);
  if (!out->rgba) {
    free(pix);
    set_error("out of memory");
    return 0;
  }
  out->width = w;
  out->height = hh;
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      out->rgba[i * 4u + 0u] = v;
      out->rgba[i * 4u + 1u] = v;
      out->rgba[i * 4u + 2u] = v;
      out->rgba[i * 4u + 3u] = 255;
    } else {
      out->rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      out->rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      out->rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      out->rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  return 1;
}

static int dec_blocks2(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 20u || memcmp(payload->data, "QBL2", 4u) ||
      payload->data[4] != BLK_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_error("corrupt file: invalid block residual stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8u);
  uint64_t z64 = rd64(payload->data + 12u);
  if (z64 > (uint64_t)(size_t)-1) {
    set_error("corrupt file: block residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t table_end = 0;
  size_t end = 0;
  if (!addok(20u, table_size, &table_end) || !addok(table_end, zsize, &end) ||
      end != payload->size) {
    set_error("corrupt file: block residual size mismatch");
    return 0;
  }
  uint8_t *res = 0;
  uint32_t w = 0;
  uint32_t hh = 0;
  int zch = 0;
  int e = stream_decode_trusted_expected(
      payload->data + table_end, zsize, h->width, h->height, ch, &res, &w,
      &hh, &zch);
  if (e != STREAM_OK) {
    set_error(stream_strerror(e));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    free(res);
    set_error("corrupt file: block residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0;
  size_t compact = 0;
  size_t rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact) || !mulok(pixels, 4u, &rgba_bytes)) {
    free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)malloc(compact ? compact : 1u);
  if (!pix) {
    free(res);
    set_error("out of memory");
    return 0;
  }
  QBlockReader r = {payload->data + 20u, table_size, 0u};
  for (uint32_t y = 0; y < h->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < h->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(h->width, x, BLK_SIZE);
      uint32_t bh = block_extent(h->height, y, BLK_SIZE);
      uint8_t op = 0;
      if (!qr_u8(&r, &op)) {
        free(pix);
        free(res);
        return 0;
      }
      if (op > BLK2_PDM || (op == BLK2_LEFT && x < BLK_SIZE) ||
          (op == BLK2_UP && y < BLK_SIZE)) {
        free(pix);
        free(res);
        set_error("corrupt file: invalid block residual opcode");
        return 0;
      }
      size_t parn = blk2_parn(op, ch);
      if (!qr_need(&r, parn)) {
        free(pix);
        free(res);
        return 0;
      }
      const uint8_t *par = r.p + r.pos;
      r.pos += parn;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            size_t pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p = blk2_pred(pix, w, x, y, bw, bh, xx, yy, ch, c, op, par);
            pix[pos] = (uint8_t)((int)p + blk2_unfold(res[pos]));
          }
        }
      }
    }
  }
  free(res);
  if (r.pos != r.n) {
    free(pix);
    set_error("corrupt file: trailing block residual table data");
    return 0;
  }
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = hh;
    return 1;
  }
  out->rgba = (uint8_t *)malloc(rgba_bytes);
  if (!out->rgba) {
    free(pix);
    set_error("out of memory");
    return 0;
  }
  out->width = w;
  out->height = hh;
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      out->rgba[i * 4u + 0u] = v;
      out->rgba[i * 4u + 1u] = v;
      out->rgba[i * 4u + 2u] = v;
      out->rgba[i * 4u + 3u] = 255;
    } else {
      out->rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      out->rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      out->rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      out->rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  return 1;
}

static int dec_pdm_regions(const Buf *payload, const Head *h, Image *out) {
  if (payload->size < 20u || memcmp(payload->data, "QPD1", 4u) ||
      payload->data[4] != PDM_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_error("corrupt file: invalid pdm stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8u);
  uint64_t z64 = rd64(payload->data + 12u);
  if (z64 > (uint64_t)(size_t)-1) {
    set_error("corrupt file: pdm residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t table_end = 0;
  size_t end = 0;
  if (!addok(20u, table_size, &table_end) || !addok(table_end, zsize, &end) ||
      end != payload->size) {
    set_error("corrupt file: pdm residual size mismatch");
    return 0;
  }
  uint8_t *res = 0;
  uint32_t w = 0;
  uint32_t hh = 0;
  int zch = 0;
  int e = stream_decode_trusted_expected(
      payload->data + table_end, zsize, h->width, h->height, ch, &res, &w,
      &hh, &zch);
  if (e != STREAM_OK) {
    set_error(stream_strerror(e));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    free(res);
    set_error("corrupt file: pdm residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0;
  size_t compact = 0;
  size_t rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact) || !mulok(pixels, 4u, &rgba_bytes)) {
    free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)malloc(compact ? compact : 1u);
  if (!pix) {
    free(res);
    set_error("out of memory");
    return 0;
  }
  QBlockReader r = {payload->data + 20u, table_size, 0u};
  size_t parn = blk2_parn(BLK2_PDM, ch);
  for (uint32_t y = 0; y < h->height; y += PDM_SIZE) {
    for (uint32_t x = 0; x < h->width; x += PDM_SIZE) {
      uint32_t bw = block_extent(h->width, x, PDM_SIZE);
      uint32_t bh = block_extent(h->height, y, PDM_SIZE);
      if (!qr_need(&r, parn)) {
        free(pix);
        free(res);
        return 0;
      }
      const uint8_t *par = r.p + r.pos;
      r.pos += parn;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            size_t pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p =
                pdm_pred(pix, w, x, y, bw, bh, PDM_SIZE, xx, yy, ch, c, par);
            pix[pos] = (uint8_t)((int)p + blk2_unfold(res[pos]));
          }
        }
      }
    }
  }
  free(res);
  if (r.pos != r.n) {
    free(pix);
    set_error("corrupt file: trailing pdm table data");
    return 0;
  }
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = hh;
    return 1;
  }
  out->rgba = (uint8_t *)malloc(rgba_bytes);
  if (!out->rgba) {
    free(pix);
    set_error("out of memory");
    return 0;
  }
  out->width = w;
  out->height = hh;
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      out->rgba[i * 4u + 0u] = v;
      out->rgba[i * 4u + 1u] = v;
      out->rgba[i * 4u + 2u] = v;
      out->rgba[i * 4u + 3u] = 255;
    } else {
      out->rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      out->rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      out->rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      out->rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  return 1;
}

static int dec_blocks(const Buf *payload, const Head *h, Image *out) {
  if (payload->size >= 4u && !memcmp(payload->data, "QCF1", 4u))
    return dec_cf_regions(payload, h, out);
  if (payload->size >= 4u && !memcmp(payload->data, "QPD1", 4u))
    return dec_pdm_regions(payload, h, out);
  if (payload->size >= 4u && !memcmp(payload->data, "QBL2", 4u))
    return dec_blocks2(payload, h, out);
  int extended =
      payload->size >= 4u && !memcmp(payload->data, "QBR1", 4u);
  if (payload->size < 8u ||
      (!extended && memcmp(payload->data, "QBL1", 4u)) ||
      payload->data[4] != BLK_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_error("corrupt file: invalid block stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  out->rgba = (uint8_t *)malloc(bytes);
  if (!out->rgba) {
    set_error("out of memory");
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  QBlockReader r = {payload->data, payload->size, 8u};
  uint32_t blocks_x = (h->width + BLK_SIZE - 1u) / BLK_SIZE;
  size_t block_index = 0;
  for (uint32_t y = 0; y < h->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < h->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(h->width, x, BLK_SIZE);
      uint32_t bh = block_extent(h->height, y, BLK_SIZE);
      uint8_t op = 0;
      uint32_t colors[4];
      if (!qr_u8(&r, &op))
        return 0;
      if (op == BLK_RAW) {
        if (!dec_blk_raw(&r, out, x, y, bw, bh, ch))
          return 0;
      } else if (op == BLK_FLAT) {
        uint32_t c = 0;
        if (!qr_color(&r, ch, &c))
          return 0;
        dec_blk_fill(out, x, y, bw, bh, c, ch);
      } else if (op == BLK_LEFT) {
        if (x < BLK_SIZE) {
          set_error("corrupt file: invalid left block copy");
          return 0;
        }
        dec_blk_copy(out, x, y, x - BLK_SIZE, y, bw, bh, ch);
      } else if (op == BLK_UP) {
        if (y < BLK_SIZE) {
          set_error("corrupt file: invalid upper block copy");
          return 0;
        }
        dec_blk_copy(out, x, y, x, y - BLK_SIZE, bw, bh, ch);
      } else if (op == BLK_TWO) {
        if (!qr_color(&r, ch, &colors[0]) || !qr_color(&r, ch, &colors[1]) ||
            !dec_blk_map(&r, out, x, y, bw, bh, colors, 2, ch))
          return 0;
      } else if (op == BLK_FOUR) {
        uint8_t count = 0;
        if (!qr_u8(&r, &count) || count < 3 || count > 4) {
          set_error("corrupt file: invalid block palette");
          return 0;
        }
        for (int i = 0; i < (int)count; ++i)
          if (!qr_color(&r, ch, &colors[i]))
            return 0;
        if (!dec_blk_map(&r, out, x, y, bw, bh, colors, count, ch))
          return 0;
      } else if (op == BLK_PAT2) {
        if (!dec_blk_pat(&r, out, x, y, bw, bh, ch))
          return 0;
      } else if (op == BLK_REF && extended) {
        size_t distance = 0;
        if (!read_varint(r.p, r.n, &r.pos, &distance) || !distance ||
            distance > block_index) {
          set_error("corrupt file: invalid block reference");
          return 0;
        }
        size_t source = block_index - distance;
        uint32_t sx = (uint32_t)(source % blocks_x) * BLK_SIZE;
        uint32_t sy = (uint32_t)(source / blocks_x) * BLK_SIZE;
        if (sx > h->width - bw || sy > h->height - bh) {
          set_error("corrupt file: invalid block reference");
          return 0;
        }
        dec_blk_copy(out, x, y, sx, sy, bw, bh, ch);
      } else {
        set_error("corrupt file: invalid block opcode");
        return 0;
      }
      ++block_index;
    }
  }
  if (r.pos != r.n) {
    set_error("corrupt file: trailing block data");
    return 0;
  }
  return 1;
}

static int dec_qlic_image(const uint8_t *data, size_t size, const Head *head,
                          Image *out) {
  image_zero(out);
  Head h = *head;
  if (h.mode == MODE_ANIM) {
    set_error("animation frame expected static QLIC");
    return 0;
  }
  size_t palette_size = file_palette_size(&h);
  size_t comp_size = (size_t)h.compressed_size;
  size_t start;
  if (!addok(QLIC_HEADER_SIZE, palette_size, &start))
    return 0;
  if (start + comp_size != size - QLIC_FOOTER_SIZE) {
    set_error("corrupt file: file size does not match header");
    return 0;
  }
  const uint8_t *palette = data + QLIC_HEADER_SIZE;
  const uint8_t *comp = palette + palette_size;
  Buf payload = {0};
  if (!unpack_payload(comp, comp_size, h.codec, h.payload_size, &payload))
    return 0;
  int ok = 0;
  if (h.mode == MODE_NATIVE) {
    ok = dec_stream(&payload, &h, out);
  } else if (h.mode == MODE_BLOCKS) {
    ok = dec_blocks(&payload, &h, out);
  } else if (h.mode == MODE_TILES) {
    ok = dec_tile(&payload, &h, out);
  } else if (h.mode == MODE_TILE_MODEL) {
    ok = dec_rtt(&payload, &h, out);
  } else if (h.mode == MODE_GMODEL) {
    ok = dec_gmodel(&payload, &h, out);
  } else if (h.mode == MODE_FILTERED) {
    ok = dec_filtered(&payload, &h, out);
  } else if (h.mode == MODE_PSTREAM) {
    ok = dec_pstream(&payload, &h, palette, out);
  } else if (h.mode == MODE_PPAL) {
    ok = dec_ppal(&payload, &h, palette, out);
  } else if (h.mode == MODE_CPAL) {
    ok = dec_cpal(&payload, &h, out);
  } else if (h.mode == MODE_SEPARABLE) {
    ok = dec_sep(&payload, &h, out);
  } else if (h.mode == MODE_PALETTE && h.transform == TRANSFORM_INDEX_RLE) {
    ok = dec_irun(&payload, &h, palette, out);
  } else if (is_planar_med(h.transform)) {
    ok = planar_med_rgba(&payload, &h, out);
  } else if (is_rle(h.transform)) {
    size_t expected = 0;
    Buf samples = {0};
    ok = samp_size(&h, &expected) &&
         rle_decode(payload.data, payload.size, expected, &samples) &&
         samp_rgba(&samples, &h, palette, out);
  } else if (is_raw(h.transform)) {
    ok = samp_rgba(&payload, &h, palette, out);
  } else {
    int bpp = mbpp(h.mode);
    size_t row_bytes = 0;
    int can_filter = 1;
    if (h.mode == MODE_PALETTE) {
      row_bytes = row_pack(h.width, h.index_bits);
      bpp = h.index_bits == 16 ? 2 : 1;
    } else if (bpp <= 0 || !mulok((size_t)h.width, (size_t)bpp, &row_bytes)) {
      set_error("corrupt file: invalid sample mode");
      can_filter = 0;
    }
    if (can_filter) {
      Buf samples = {0};
      ok = unf_rows(payload.data, payload.size, row_bytes, h.height, bpp,
                    &samples) &&
           samp_rgba(&samples, &h, palette, out);
    }
  }
  if (h.codec == CODEC_LZMS)
    free(payload.data);
  return ok;
}

static void free_frames(Frame *fs, uint32_t count) {
  if (!fs)
    return;
  for (uint32_t i = 0; i < count; ++i)
    free(fs[i].rgba);
  free(fs);
}

static int dec_anim2_payload(const Buf *payload, const Head *h) {
  uint32_t count = rd32(payload->data + 4u);
  uint32_t loop = rd32(payload->data + 8u);
  if (!count || count != h->palette_count || count > QLIC_MAX_FRAMES) {
    set_error("corrupt file: invalid animation frame count");
    return 0;
  }
  uint64_t frame_bytes = (uint64_t)h->width * h->height * 4u;
  uint64_t storage = (uint64_t)count * sizeof(Frame);
  if (storage > QLIC_MAX_ANIMATION_BYTES ||
      frame_bytes >
          (QLIC_MAX_ANIMATION_BYTES - storage) / ((uint64_t)count + 1u)) {
    set_error("resource limit exceeded: animation memory");
    return 0;
  }
  size_t frame_size = (size_t)frame_bytes;
  Frame *fs = (Frame *)calloc(count, sizeof(Frame));
  if (!fs) {
    set_error("out of memory");
    return 0;
  }
  size_t pos = 12u;
  for (uint32_t i = 0; i < count; ++i) {
    if (payload->size - pos < 8u) {
      set_error("corrupt file: truncated animation table");
      free_frames(fs, count);
      return 0;
    }
    uint32_t delay = rd32(payload->data + pos);
    uint32_t type = rd32(payload->data + pos + 4u);
    pos += 8u;
    fs[i].width = h->width;
    fs[i].height = h->height;
    fs[i].delay = delay ? delay : 100u;
    if (!i && type != ANIM_FRAME_KEY) {
      set_error("corrupt file: animation must start with a key frame");
      free_frames(fs, count);
      return 0;
    }
    if (type == ANIM_FRAME_DUPLICATE) {
      fs[i].rgba = (uint8_t *)malloc(frame_size);
      if (!fs[i].rgba) {
        set_error("out of memory");
        free_frames(fs, count);
        return 0;
      }
      memcpy(fs[i].rgba, fs[i - 1u].rgba, frame_size);
      continue;
    }
    if (type == ANIM_FRAME_MOVE) {
      if (payload->size - pos < 28u) {
        set_error("corrupt file: truncated animation move");
        free_frames(fs, count);
        return 0;
      }
      uint32_t source_x = rd32(payload->data + pos);
      uint32_t source_y = rd32(payload->data + pos + 4u);
      uint32_t destination_x = rd32(payload->data + pos + 8u);
      uint32_t destination_y = rd32(payload->data + pos + 12u);
      uint32_t width = rd32(payload->data + pos + 16u);
      uint32_t height = rd32(payload->data + pos + 20u);
      uint32_t clear = rd32(payload->data + pos + 24u);
      pos += 28u;
      if (!width || !height || source_x >= h->width ||
          source_y >= h->height || destination_x >= h->width ||
          destination_y >= h->height || width > h->width - source_x ||
          height > h->height - source_y ||
          width > h->width - destination_x ||
          height > h->height - destination_y) {
        set_error("corrupt file: invalid animation move");
        free_frames(fs, count);
        return 0;
      }
      fs[i].rgba = (uint8_t *)malloc(frame_size);
      if (!fs[i].rgba) {
        set_error("out of memory");
        free_frames(fs, count);
        return 0;
      }
      memcpy(fs[i].rgba, fs[i - 1u].rgba, frame_size);
      uint8_t color[4] = {(uint8_t)clear, (uint8_t)(clear >> 8),
                          (uint8_t)(clear >> 16),
                          (uint8_t)(clear >> 24)};
      size_t stride = (size_t)h->width * 4u;
      for (uint32_t yy = 0; yy < height; ++yy) {
        uint8_t *row = fs[i].rgba + (size_t)(source_y + yy) * stride +
                       (size_t)source_x * 4u;
        for (uint32_t xx = 0; xx < width; ++xx)
          memcpy(row + (size_t)xx * 4u, color, 4u);
      }
      for (uint32_t yy = 0; yy < height; ++yy)
        memcpy(fs[i].rgba + (size_t)(destination_y + yy) * stride +
                   (size_t)destination_x * 4u,
               fs[i - 1u].rgba + (size_t)(source_y + yy) * stride +
                   (size_t)source_x * 4u,
               (size_t)width * 4u);
      continue;
    }
    uint32_t x = 0, y = 0, width = h->width, height = h->height;
    if (type == ANIM_FRAME_RECT) {
      if (payload->size - pos < 24u) {
        set_error("corrupt file: truncated animation rectangle");
        free_frames(fs, count);
        return 0;
      }
      x = rd32(payload->data + pos);
      y = rd32(payload->data + pos + 4u);
      width = rd32(payload->data + pos + 8u);
      height = rd32(payload->data + pos + 12u);
      pos += 16u;
      if (!width || !height || x >= h->width || y >= h->height ||
          width > h->width - x || height > h->height - y) {
        set_error("corrupt file: invalid animation rectangle");
        free_frames(fs, count);
        return 0;
      }
    } else if (type != ANIM_FRAME_KEY) {
      set_error("corrupt file: invalid animation frame type");
      free_frames(fs, count);
      return 0;
    }
    if (payload->size - pos < 8u) {
      set_error("corrupt file: truncated animation frame");
      free_frames(fs, count);
      return 0;
    }
    uint64_t n64 = rd64(payload->data + pos);
    pos += 8u;
    if (n64 > (uint64_t)(size_t)-1 ||
        (size_t)n64 > payload->size - pos) {
      set_error("corrupt file: invalid animation frame size");
      free_frames(fs, count);
      return 0;
    }
    Head fh;
    if (!read_head(payload->data + pos, (size_t)n64, &fh)) {
      free_frames(fs, count);
      return 0;
    }
    if (fh.mode == MODE_ANIM) {
      set_error("corrupt file: nested animation frame");
      free_frames(fs, count);
      return 0;
    }
    if (fh.width != width || fh.height != height) {
      set_error("corrupt file: animation frame dimensions mismatch");
      free_frames(fs, count);
      return 0;
    }
    Image decoded;
    if (!dec_qlic_image(payload->data + pos, (size_t)n64, &fh, &decoded)) {
      free_frames(fs, count);
      return 0;
    }
    pos += (size_t)n64;
    if (type == ANIM_FRAME_KEY) {
      fs[i].rgba = decoded.rgba;
      continue;
    }
    fs[i].rgba = (uint8_t *)malloc(frame_size);
    if (!fs[i].rgba) {
      free(decoded.rgba);
      set_error("out of memory");
      free_frames(fs, count);
      return 0;
    }
    memcpy(fs[i].rgba, fs[i - 1u].rgba, frame_size);
    size_t dst_stride = (size_t)h->width * 4u;
    size_t src_stride = (size_t)width * 4u;
    for (uint32_t yy = 0; yy < height; ++yy)
      memcpy(fs[i].rgba + (size_t)(y + yy) * dst_stride + (size_t)x * 4u,
             decoded.rgba + (size_t)yy * src_stride, src_stride);
    free(decoded.rgba);
  }
  if (pos != payload->size) {
    set_error("corrupt file: trailing animation data");
    free_frames(fs, count);
    return 0;
  }
  frames = fs;
  frame_count = count;
  image_width = h->width;
  image_height = h->height;
  loop_count = loop;
  return 1;
}

static int dec_anim_payload(const Buf *payload, const Head *h) {
  if (payload->size < 12u) {
    set_error("corrupt file: invalid animation payload");
    return 0;
  }
  if (!memcmp(payload->data, "QAN2", 4u))
    return dec_anim2_payload(payload, h);
  if (memcmp(payload->data, "QAN1", 4u)) {
    set_error("corrupt file: invalid animation payload");
    return 0;
  }
  uint32_t count = rd32(payload->data + 4u);
  uint32_t loop = rd32(payload->data + 8u);
  if (!count || count != h->palette_count || count > QLIC_MAX_FRAMES) {
    set_error("corrupt file: invalid animation frame count");
    return 0;
  }
  Frame *fs = (Frame *)calloc(count, sizeof(Frame));
  if (!fs) {
    set_error("out of memory");
    return 0;
  }
  size_t pos = 12u;
  for (uint32_t i = 0; i < count; ++i) {
    if (payload->size - pos < 16u) {
      set_error("corrupt file: truncated animation table");
      free_frames(fs, count);
      return 0;
    }
    uint32_t delay = rd32(payload->data + pos);
    uint32_t flags = rd32(payload->data + pos + 4u);
    uint64_t n64 = rd64(payload->data + pos + 8u);
    pos += 16u;
    if (flags || n64 > (uint64_t)(size_t)-1 ||
        (size_t)n64 > payload->size - pos) {
      set_error("corrupt file: invalid animation frame entry");
      free_frames(fs, count);
      return 0;
    }
    Head fh;
    if (!read_head(payload->data + pos, (size_t)n64, &fh)) {
      free_frames(fs, count);
      return 0;
    }
    if (fh.mode == MODE_ANIM) {
      set_error("corrupt file: nested animation frame");
      free_frames(fs, count);
      return 0;
    }
    if (fh.width != h->width || fh.height != h->height) {
      set_error("corrupt file: animation frame dimensions mismatch");
      free_frames(fs, count);
      return 0;
    }
    Image im;
    if (!dec_qlic_image(payload->data + pos, (size_t)n64, &fh, &im)) {
      free_frames(fs, count);
      return 0;
    }
    fs[i].width = im.width;
    fs[i].height = im.height;
    fs[i].delay = delay ? delay : 100u;
    fs[i].rgba = im.rgba;
    pos += (size_t)n64;
  }
  if (pos != payload->size) {
    set_error("corrupt file: trailing animation data");
    free_frames(fs, count);
    return 0;
  }
  frames = fs;
  frame_count = count;
  image_width = h->width;
  image_height = h->height;
  loop_count = loop;
  return 1;
}

static int input_valid(uint32_t ptr, uint32_t n) {
  uintptr_t memory_size =
      (uintptr_t)__builtin_wasm_memory_size(0) << 16;
  if (!ptr || !n || (uintptr_t)ptr > memory_size ||
      (uintptr_t)n > memory_size - (uintptr_t)ptr) {
    set_error("invalid input memory");
    return 0;
  }
  return 1;
}

static void core_error(const char *fallback) {
  const char *error = qlic_core_error();
  set_error(error && error[0] ? error : fallback);
}

static void meta16(size_t offset, uint16_t value) {
  hdr_metadata[offset] = (uint8_t)value;
  hdr_metadata[offset + 1u] = (uint8_t)(value >> 8);
}

static void meta32(size_t offset, uint32_t value) {
  hdr_metadata[offset] = (uint8_t)value;
  hdr_metadata[offset + 1u] = (uint8_t)(value >> 8);
  hdr_metadata[offset + 2u] = (uint8_t)(value >> 16);
  hdr_metadata[offset + 3u] = (uint8_t)(value >> 24);
}

static void make_hdr_metadata(void) {
  meta32(0u, hdr_result.sample_type);
  meta32(4u, hdr_result.alpha_mode);
  meta32(8u, hdr_result.color_authority);
  meta32(12u, (uint32_t)(uintptr_t)hdr_result.icc);
  meta32(16u, (uint32_t)hdr_result.icc_size);
  meta32(20u, hdr_result.has_cicp);
  meta16(24u, hdr_result.color_primaries);
  meta16(26u, hdr_result.transfer_characteristics);
  meta16(28u, hdr_result.matrix_coefficients);
  hdr_metadata[30] = hdr_result.full_range;
  meta32(32u, hdr_result.has_mastering_display);
  for (size_t i = 0; i < 3u; ++i) {
    meta16(36u + i * 4u, hdr_result.primary_x[i]);
    meta16(38u + i * 4u, hdr_result.primary_y[i]);
  }
  meta16(48u, hdr_result.white_x);
  meta16(50u, hdr_result.white_y);
  meta32(52u, hdr_result.max_luminance);
  meta32(56u, hdr_result.min_luminance);
  meta32(60u, hdr_result.has_content_light);
  meta16(64u, hdr_result.max_cll);
  meta16(66u, hdr_result.max_fall);
  hdr_metadata_size = (uint32_t)sizeof(hdr_metadata);
}

int qlic_decode_wide(uint32_t ptr, uint32_t n) {
  clear_result();
  last_error[0] = 0;
  if (!input_valid(ptr, n))
    return 0;
  if (!dec_wide_qlic_limited((const uint8_t *)(uintptr_t)ptr, (size_t)n,
                             &wide_result, 0, &decode_limits)) {
    core_error("QLIC wide decode failed.");
    return 0;
  }
  if (wide_result.pixels_size > UINT32_MAX || wide_result.stride > UINT32_MAX) {
    set_error("QLIC wide output is too large.");
    return 0;
  }
  sample_result = &wide_result;
  return 1;
}

int qlic_decode_hdr(uint32_t ptr, uint32_t n) {
  clear_result();
  last_error[0] = 0;
  if (!input_valid(ptr, n))
    return 0;
  if (!dec_hdr_qlic_limited((const uint8_t *)(uintptr_t)ptr, (size_t)n,
                            &hdr_result, 0, &decode_limits)) {
    core_error("QLIC HDR decode failed.");
    return 0;
  }
  if (hdr_result.wide.pixels_size > UINT32_MAX ||
      hdr_result.wide.stride > UINT32_MAX || hdr_result.icc_size > UINT32_MAX) {
    set_error("QLIC HDR output is too large.");
    return 0;
  }
  sample_result = &hdr_result.wide;
  make_hdr_metadata();
  return 1;
}

uint32_t qlic_sample_width(void) {
  return sample_result ? sample_result->width : 0u;
}

uint32_t qlic_sample_height(void) {
  return sample_result ? sample_result->height : 0u;
}

uint32_t qlic_sample_channels(void) {
  return sample_result ? sample_result->channels : 0u;
}

uint32_t qlic_sample_bits(void) {
  return sample_result ? sample_result->bits_per_sample : 0u;
}

uint32_t qlic_sample_ptr(void) {
  return sample_result ? (uint32_t)(uintptr_t)sample_result->pixels : 0u;
}

uint32_t qlic_sample_size(void) {
  return sample_result ? (uint32_t)sample_result->pixels_size : 0u;
}

uint32_t qlic_sample_stride(void) {
  return sample_result ? (uint32_t)sample_result->stride : 0u;
}

uint32_t qlic_hdr_metadata_ptr(void) {
  return hdr_metadata_size ? (uint32_t)(uintptr_t)hdr_metadata : 0u;
}

uint32_t qlic_hdr_metadata_size(void) { return hdr_metadata_size; }

uint32_t qlic_hdr_block_count(void) {
  return hdr_metadata_size ? hdr_result.metadata_count : 0u;
}

uint32_t qlic_hdr_block_tag(uint32_t index) {
  if (!hdr_metadata_size || index >= hdr_result.metadata_count)
    return 0u;
  const uint8_t *tag = hdr_result.metadata[index].tag;
  return (uint32_t)tag[0] | (uint32_t)tag[1] << 8u |
         (uint32_t)tag[2] << 16u | (uint32_t)tag[3] << 24u;
}

uint32_t qlic_hdr_block_ptr(uint32_t index) {
  return !hdr_metadata_size || index >= hdr_result.metadata_count
             ? 0u
             : (uint32_t)(uintptr_t)hdr_result.metadata[index].data;
}

uint32_t qlic_hdr_block_size(uint32_t index) {
  if (!hdr_metadata_size || index >= hdr_result.metadata_count ||
      hdr_result.metadata[index].size > UINT32_MAX)
    return 0u;
  return (uint32_t)hdr_result.metadata[index].size;
}

int qlic_decode(uint32_t ptr, uint32_t n) {
  clear_result();
  last_error[0] = 0;
  if (!input_valid(ptr, n)) {
    return 0;
  }
  const uint8_t *data = (const uint8_t *)(uintptr_t)ptr;
  Head h = {0};
  if (!read_head(data, (size_t)n, &h))
    return 0;
  if (h.mode == MODE_ANIM) {
    size_t comp_size = (size_t)h.compressed_size;
    if (QLIC_HEADER_SIZE + comp_size != (size_t)n - QLIC_FOOTER_SIZE) {
      set_error("corrupt file: file size does not match header");
      return 0;
    }
    Buf payload = {0};
    if (!unpack_payload(data + QLIC_HEADER_SIZE, comp_size, h.codec,
                        h.payload_size, &payload))
      return 0;
    int ok = dec_anim_payload(&payload, &h);
    if (h.codec == CODEC_LZMS)
      free(payload.data);
    if (ok)
      animated = 1;
    return ok;
  }
  Image im;
  if (!dec_qlic_image(data, (size_t)n, &h, &im))
    return 0;
  Frame *fs = (Frame *)calloc(1u, sizeof(Frame));
  if (!fs) {
    set_error("out of memory");
    return 0;
  }
  fs[0].width = im.width;
  fs[0].height = im.height;
  fs[0].delay = 0;
  fs[0].rgba = im.rgba;
  frames = fs;
  frame_count = 1;
  image_width = im.width;
  image_height = im.height;
  loop_count = 0;
  return 1;
}

int qlic_validate(uint32_t ptr, uint32_t n) {
  if (!input_valid(ptr, n) || n <= 12u)
    return 0;
  const uint8_t *input = (const uint8_t *)(uintptr_t)ptr;
  int ok = input[12] == MODE_NATIVE_WIDE
               ? qlic_decode_wide(ptr, n)
           : input[12] == MODE_HDR_WIDE ? qlic_decode_hdr(ptr, n)
                                        : qlic_decode(ptr, n);
  if (ok)
    qlic_reset();
  return ok;
}

uint32_t qlic_width(void) { return image_width; }

uint32_t qlic_height(void) { return image_height; }

uint32_t qlic_frame_count(void) { return frame_count; }

uint32_t qlic_loop_count(void) { return loop_count; }

uint32_t qlic_animated(void) { return animated; }

uint32_t qlic_frame_width(uint32_t i) {
  return i < frame_count ? frames[i].width : 0;
}

uint32_t qlic_frame_height(uint32_t i) {
  return i < frame_count ? frames[i].height : 0;
}

uint32_t qlic_frame_delay(uint32_t i) {
  return i < frame_count ? frames[i].delay : 0;
}

uint32_t qlic_frame_ptr(uint32_t i) {
  // cppcheck-suppress CastAddressToIntegerAtReturn
  return i < frame_count ? (uint32_t)(uintptr_t)frames[i].rgba : 0;
}

uint32_t qlic_frame_size(uint32_t i) {
  if (i >= frame_count)
    return 0;
  uint64_t n = (uint64_t)frames[i].width * (uint64_t)frames[i].height * 4ull;
  return n > 0xffffffffull ? 0 : (uint32_t)n;
}

uint32_t qlic_error_ptr(void) {
  // cppcheck-suppress CastAddressToIntegerAtReturn
  return (uint32_t)(uintptr_t)last_error;
}
