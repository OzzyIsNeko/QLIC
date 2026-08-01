
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "parallel.h"
#include "stream.h"
#if defined(_MSC_VER) && defined(_M_X64)
#include "map_avx2.h"
#endif
#ifdef QLIC_WASM
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t s);
void *realloc(void *p, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#define QLIC_FORCEINLINE __forceinline
#define QLIC_NOINLINE __declspec(noinline)
#define QLIC_RESTRICT __restrict
#elif defined(__clang__) || defined(__GNUC__)
#define QLIC_FORCEINLINE inline __attribute__((always_inline))
#define QLIC_NOINLINE __attribute__((noinline))
#define QLIC_RESTRICT __restrict__
#else
#define QLIC_FORCEINLINE inline
#define QLIC_NOINLINE
#define QLIC_RESTRICT
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xffff
#endif
#elif !defined(QLIC_WASM)
#include <pthread.h>
#endif

/* native builds keep worker selection local, wasm only has one caller */
#if defined(_MSC_VER)
__declspec(thread) static unsigned stream_threads = 1;
#elif defined(__STDC_NO_THREADS__) || defined(QLIC_WASM)
static unsigned stream_threads = 1;
#else
static _Thread_local unsigned stream_threads = 1;
#endif

void stream_set_threads(unsigned threads) {
    stream_threads = threads ? threads : 1u;
}

#define STREAM_MAX_DIM    (1u << 24)
#define STREAM_MAX_PIXELS (1ull << 28)
#define STREAM_HDR        30u

const char *stream_strerror(int e) {
    switch (e) {
    case STREAM_OK:        return "ok";
    case STREAM_E_ARG:     return "bad argument";
    case STREAM_E_ALLOC:   return "out of memory";
    case STREAM_E_FORMAT:  return "bad native stream header";
    case STREAM_E_CORRUPT: return "corrupt or truncated data (checksum/stream)";
    case STREAM_E_DIM:     return "unsupported dimensions";
    default:            return "unknown error";
    }
}

void stream_free(void *p) {
    free(p);
}

static uint32_t crc_tab[8][256];

static void crc_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[0][i] = c;
    }
    for (int t = 1; t < 8; t++) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = crc_tab[t - 1][i];
            crc_tab[t][i] = crc_tab[0][c & 0xffu] ^ (c >> 8);
        }
    }
}

#ifdef _WIN32
static INIT_ONCE crc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK crc_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    crc_init_table();
    return TRUE;
}
static void crc_ensure(void) {
    InitOnceExecuteOnce(&crc_once, crc_init_once, NULL, NULL);
}
#elif !defined(QLIC_WASM)
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_ensure(void) {
    (void)pthread_once(&crc_once, crc_init_table);
}
#else
static int crc_ready;
static void crc_ensure(void) {
    if (!crc_ready) {
        crc_init_table();
        crc_ready = 1;
    }
}
#endif

static QLIC_FORCEINLINE uint32_t crc_word(uint32_t c, uint64_t v) {
    c ^= (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    return crc_tab[7][c & 0xffu] ^
           crc_tab[6][(c >> 8) & 0xffu] ^
           crc_tab[5][(c >> 16) & 0xffu] ^
           crc_tab[4][c >> 24] ^
           crc_tab[3][hi & 0xffu] ^
           crc_tab[2][(hi >> 8) & 0xffu] ^
           crc_tab[1][(hi >> 16) & 0xffu] ^
           crc_tab[0][hi >> 24];
}
static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n) {
    crc_ensure();
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        c = crc_word(c, v);
        p += 8;
        n -= 8;
    }
    for (size_t i = 0; i < n; i++) {
        c = crc_tab[0][(c ^ p[i]) & 0xffu] ^ (c >> 8);
    }
    return c;
}
uint32_t stream_crc32(const uint8_t *p, size_t n) {
    return crc_step(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}
static uint64_t pack_rgbx_pair(uint64_t value) {
    return (value & UINT64_C(0x0000000000ffffff)) |
           ((value >> 8) & UINT64_C(0x0000ffffff000000));
}
static uint32_t stream_crc32_rgbx(const uint8_t *p, size_t npix) {
    uint32_t c = UINT32_C(0xffffffff);
    crc_ensure();
    while (npix >= 8u) {
        uint64_t source[4];
        memcpy(&source[0], p, 8u);
        memcpy(&source[1], p + 8u, 8u);
        memcpy(&source[2], p + 16u, 8u);
        memcpy(&source[3], p + 24u, 8u);
        uint64_t a = pack_rgbx_pair(source[0]);
        uint64_t b = pack_rgbx_pair(source[1]);
        uint64_t d = pack_rgbx_pair(source[2]);
        uint64_t e = pack_rgbx_pair(source[3]);
        c = crc_word(c, a | ((b & UINT64_C(0xffff)) << 48));
        c = crc_word(c, (b >> 16) |
                            ((d & UINT64_C(0xffffffff)) << 32));
        c = crc_word(c, (d >> 32) | (e << 16));
        p += 32u;
        npix -= 8u;
    }
    while (npix--) {
        c = crc_tab[0][(c ^ p[0]) & 0xffu] ^ (c >> 8);
        c = crc_tab[0][(c ^ p[1]) & 0xffu] ^ (c >> 8);
        c = crc_tab[0][(c ^ p[2]) & 0xffu] ^ (c >> 8);
        p += 4u;
    }
    return c ^ UINT32_C(0xffffffff);
}
static uint32_t stream_crc32_grayx(const uint8_t *p, size_t npix) {
    uint32_t c = UINT32_C(0xffffffff);
    crc_ensure();
    while (npix >= 8u) {
        uint64_t source[4];
        memcpy(&source[0], p, 8u);
        memcpy(&source[1], p + 8u, 8u);
        memcpy(&source[2], p + 16u, 8u);
        memcpy(&source[3], p + 24u, 8u);
        uint64_t word = 0;
        for (unsigned i = 0; i < 4u; ++i) {
            uint64_t pair =
                (source[i] & UINT64_C(0xff)) |
                ((source[i] >> 24) & UINT64_C(0xff00));
            word |= pair << (i * 16u);
        }
        c = crc_word(c, word);
        p += 32u;
        npix -= 8u;
    }
    while (npix--) {
        c = crc_tab[0][(c ^ p[0]) & 0xffu] ^ (c >> 8);
        p += 4u;
    }
    return c ^ UINT32_C(0xffffffff);
}
static uint32_t stream_crc32_pixels(const uint8_t *p, size_t npix,
                                    size_t channels, size_t stride) {
    /* specialized paths avoid packing RGB or gray copies just for the checksum */
    size_t bytes = npix * channels;
    if (channels == stride) return stream_crc32(p, bytes);
    if (channels == 3u && stride == 4u)
        return stream_crc32_rgbx(p, npix);
    if (channels == 1u && stride == 4u)
        return stream_crc32_grayx(p, npix);
    uint32_t c = UINT32_C(0xffffffff);
    crc_ensure();
    for (size_t i = 0; i < npix; ++i) {
        for (size_t k = 0; k < channels; ++k)
            c = crc_tab[0][(c ^ p[k]) & 0xffu] ^ (c >> 8);
        p += stride;
    }
    return c ^ UINT32_C(0xffffffff);
}
static uint32_t container_crc32(const uint8_t *p, size_t n) {
    static const uint8_t z[4] = {0,0,0,0};
    uint32_t c = 0xFFFFFFFFu;
    if (n < STREAM_HDR) return 0;
    /* the checksum field is defined as zero while calculating its own value */
    c = crc_step(c, p, 26);
    c = crc_step(c, z, 4);
    c = crc_step(c, p + STREAM_HDR, n - STREAM_HDR);
    return c ^ 0xFFFFFFFFu;
}

#define PROB_BITS 12
#define PROB_ONE  (1u << PROB_BITS)
#define PROB_INIT (PROB_ONE >> 1)
#define ADAPT_DEFAULT 5
#define ADAPT_FAST    4
#define ADAPT_SLOW    6
#define RC_TOP    (1u << 24)
typedef uint16_t Prob;

typedef struct {
    uint8_t *buf; size_t len, cap; int oom;
    size_t max; int cut;
    int adapt;
    uint64_t low; uint32_t range; uint8_t cache; size_t cache_size;
} Enc;

static void enc_init(Enc *e, int adapt) {
    /* this scale is part of QST1 and has to match in the decoder */
    e->buf = NULL; e->len = e->cap = 0; e->oom = 0;
    e->max = SIZE_MAX; e->cut = 0;
    e->adapt = adapt == ADAPT_FAST || adapt == ADAPT_SLOW ? adapt : ADAPT_DEFAULT;
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->cache_size = 1;
}
static void enc_reserve(Enc *e, size_t capacity) {
    if (capacity > e->max)
        capacity = e->max;
    if (!capacity)
        return;
    uint8_t *buffer = malloc(capacity);
    if (!buffer)
        return;
    e->buf = buffer;
    e->cap = capacity;
}
static QLIC_NOINLINE void enc_putbyte_slow(Enc *e, uint8_t b) {
    if (e->oom || e->cut) return;
    if (e->len >= e->max) { e->cut = 1; return; }
    size_t nc;
    if (e->cap) {
        if (e->cap > SIZE_MAX / 2u) {
            e->oom = 1;
            return;
        }
        nc = e->cap * 2u;
    } else {
        nc = (size_t)1u << 16;
    }
    if (nc > e->max) nc = e->max;
    uint8_t *nb = realloc(e->buf, nc);
    if (!nb) { e->oom = 1; return; }
    e->buf = nb;
    e->cap = nc;
    e->buf[e->len++] = b;
}
static QLIC_FORCEINLINE void enc_putbyte(Enc *e, uint8_t b) {
    if (e->len < e->cap) e->buf[e->len++] = b;
    else enc_putbyte_slow(e, b);
}
static QLIC_FORCEINLINE void enc_shift(Enc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32)) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        enc_putbyte(e, (uint8_t)(e->cache + carry));
        while (--e->cache_size) enc_putbyte(e, (uint8_t)(0xFF + carry));
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low << 8) & 0xFFFFFFFFull;
}
static QLIC_FORCEINLINE void prob_update(Prob *p, int bit, int adapt) {
    if (bit) *p -= *p >> adapt;
    else *p += (PROB_ONE - *p) >> adapt;
}

_Static_assert(PROB_BITS == 12,
               "packed probability updates require 12 bit probabilities");

static QLIC_FORCEINLINE void prob_update_triplet(
    Prob *a, int adapt_a, Prob *b, Prob *c, int adapt_bc, int bit) {
    if (bit) {
        *a -= *a >> adapt_a;
        *b -= *b >> adapt_bc;
        *c -= *c >> adapt_bc;
    } else {
        *a += (PROB_ONE - *a) >> adapt_a;
        *b += (PROB_ONE - *b) >> adapt_bc;
        *c += (PROB_ONE - *c) >> adapt_bc;
    }
}

static QLIC_FORCEINLINE void prob_update_four(
    Prob *a, int adapt_a, Prob *b, int adapt_b, Prob *c, Prob *d,
    int adapt_cd, int bit) {
    if (bit) {
        *a -= *a >> adapt_a;
        *b -= *b >> adapt_b;
        *c -= *c >> adapt_cd;
        *d -= *d >> adapt_cd;
    } else {
        *a += (PROB_ONE - *a) >> adapt_a;
        *b += (PROB_ONE - *b) >> adapt_b;
        *c += (PROB_ONE - *c) >> adapt_cd;
        *d += (PROB_ONE - *d) >> adapt_cd;
    }
}

static QLIC_FORCEINLINE void prob_update_four_branchless(
    Prob *a, int adapt_a, Prob *b, int adapt_b, Prob *c, Prob *d,
    int adapt_cd, int bit) {
    uint32_t mask = 0u - (uint32_t)bit;
#define UPDATE_ONE(P, A) do { \
        uint32_t value__ = *(P); \
        uint32_t increase__ = (PROB_ONE - value__) >> (A); \
        uint32_t decrease__ = value__ >> (A); \
        *(P) = (Prob)(value__ + \
            ((increase__ & ~mask) - (decrease__ & mask))); \
    } while (0)
    UPDATE_ONE(a, adapt_a);
    UPDATE_ONE(b, adapt_b);
    UPDATE_ONE(c, adapt_cd);
    UPDATE_ONE(d, adapt_cd);
#undef UPDATE_ONE
}

static QLIC_FORCEINLINE void prob_update_five(
    Prob *a, int adapt_a, Prob *b, int adapt_b, Prob *c, int adapt_c,
    Prob *d, Prob *e, int adapt_de, int bit) {
    uint32_t mask = 0u - (uint32_t)bit;
#define UPDATE_ONE(P, A) do { \
        uint32_t value__ = *(P); \
        uint32_t increase__ = (PROB_ONE - value__) >> (A); \
        uint32_t decrease__ = value__ >> (A); \
        *(P) = (Prob)(value__ + \
            ((increase__ & ~mask) - (decrease__ & mask))); \
    } while (0)
    UPDATE_ONE(a, adapt_a);
    UPDATE_ONE(b, adapt_b);
    UPDATE_ONE(c, adapt_c);
    UPDATE_ONE(d, adapt_de);
    UPDATE_ONE(e, adapt_de);
#undef UPDATE_ONE
}

/* probabilities use 12 bits, so one masked shift can update four lanes */
static QLIC_FORCEINLINE void prob_update_quad(Prob *a, Prob *b, Prob *c,
                                               Prob *d, int bit, int adapt) {
    static const uint64_t shift_mask[9] = {
        0, 0,
        UINT64_C(0x07ff07ff07ff07ff),
        UINT64_C(0x03ff03ff03ff03ff),
        UINT64_C(0x01ff01ff01ff01ff),
        UINT64_C(0x00ff00ff00ff00ff),
        UINT64_C(0x007f007f007f007f),
        UINT64_C(0x003f003f003f003f),
        UINT64_C(0x001f001f001f001f)
    };
    uint64_t packed = (uint64_t)*a | ((uint64_t)*b << 16) |
                      ((uint64_t)*c << 32) | ((uint64_t)*d << 48);
    uint64_t delta;
    if (bit) {
        delta = (packed >> adapt) & shift_mask[adapt];
        packed -= delta;
    } else {
        delta = ((UINT64_C(0x1000100010001000) - packed) >> adapt) &
                shift_mask[adapt];
        packed += delta;
    }
    *a = (Prob)packed;
    *b = (Prob)(packed >> 16);
    *c = (Prob)(packed >> 32);
    *d = (Prob)(packed >> 48);
}

static QLIC_FORCEINLINE void enc_bit_value(Enc *QLIC_RESTRICT e,
                                           uint32_t prob, int bit) {
    uint32_t bound = (e->range >> PROB_BITS) * prob;
    uint32_t mask = 0u - (uint32_t)bit;
    uint32_t low_range = e->range - bound;
    e->low += (uint64_t)(bound & mask);
    e->range = (bound & ~mask) | (low_range & mask);
    if (e->range < RC_TOP) {
        e->range <<= 8;
        enc_shift(e);
        if (e->range < RC_TOP) {
            e->range <<= 8;
            enc_shift(e);
        }
    }
}
static QLIC_FORCEINLINE void enc_bit_rate(Enc *QLIC_RESTRICT e,
                                          Prob *QLIC_RESTRICT p, int bit,
                                          int adapt) {
    uint32_t prob = *p;
    enc_bit_value(e, prob, bit);
    uint32_t mask = 0u - (uint32_t)bit;
    uint32_t up = prob + ((PROB_ONE - prob) >> adapt);
    uint32_t down = prob - (prob >> adapt);
    *p = (Prob)((up & ~mask) | (down & mask));
}
static QLIC_FORCEINLINE void enc_bit(Enc *QLIC_RESTRICT e,
                                     Prob *QLIC_RESTRICT p, int bit) {
    enc_bit_rate(e, p, bit, e->adapt);
}
static void enc_flush(Enc *e) { for (int i = 0; i < 5; i++) enc_shift(e); }

typedef struct { const uint8_t *ptr, *end; uint32_t range, code; int truncated; int adapt; } Dec;
static QLIC_FORCEINLINE uint8_t dec_getbyte(Dec *d) {
    if (d->ptr >= d->end) { d->truncated = 1; return 0; }
    return *d->ptr++;
}
static void dec_init(Dec *d, const uint8_t *b, size_t n, int adapt) {
    d->ptr = b; d->end = b + n; d->range = 0xFFFFFFFFu; d->code = 0; d->truncated = 0;
    d->adapt = adapt == ADAPT_FAST || adapt == ADAPT_SLOW ? adapt : ADAPT_DEFAULT;
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | dec_getbyte(d);
}
static QLIC_FORCEINLINE int dec_bit_rate(Dec *QLIC_RESTRICT d,
                                         Prob *QLIC_RESTRICT p, int adapt) {
    uint32_t prob = *p;
    uint32_t bound = (d->range >> PROB_BITS) * prob;
    int bit;
    if (d->code < bound) {
        d->range = bound;
        *p = (Prob)(prob + ((PROB_ONE - prob) >> adapt));
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *p = (Prob)(prob - (prob >> adapt));
        bit = 1;
    }
    if (d->range < RC_TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | dec_getbyte(d);
        if (d->range < RC_TOP) {
            d->range <<= 8;
            d->code = (d->code << 8) | dec_getbyte(d);
        }
    }
    return (int)bit;
}
static QLIC_FORCEINLINE int dec_bit(Dec *QLIC_RESTRICT d,
                                    Prob *QLIC_RESTRICT p) {
    return dec_bit_rate(d, p, d->adapt);
}

static QLIC_FORCEINLINE void enc_bit_mix(Enc *e, Prob *child, Prob *parent,
                                         int bit) {
    Prob mixed = (Prob)((5u * *child + 3u * *parent + 4u) >> 3);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt);
    prob_update(parent, bit, e->adapt);
}

static QLIC_FORCEINLINE void enc_bit_coarse(Enc *e, Prob *child, Prob *parent,
                                            int bit) {
    Prob mixed = (Prob)((*child + *parent + 1u) >> 1);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt);
    prob_update(parent, bit, e->adapt);
}

static QLIC_FORCEINLINE void enc_bit_mix3(Enc *e, Prob *child, Prob *parent,
                                          Prob *coarse, int bit) {
    Prob parent_mix = (Prob)((*parent + *coarse + 1u) >> 1);
    Prob mixed = (Prob)((5u * *child + 3u * parent_mix + 4u) >> 3);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt);
    prob_update(parent, bit, e->adapt);
    prob_update(coarse, bit, e->adapt);
}

static QLIC_FORCEINLINE void enc_bit_root(Enc *e, Prob *fine, Prob *coarse,
                                          Prob *root, int bit, int slow) {
    Prob coarse_mix = (Prob)((*coarse + *root + 1u) >> 1);
    Prob mixed = (Prob)((*fine + coarse_mix + 1u) >> 1);
    enc_bit_value(e, mixed, bit);
    prob_update(fine, bit, e->adapt);
    prob_update(coarse, bit, e->adapt + slow);
    prob_update(root, bit, e->adapt + slow);
}

static QLIC_FORCEINLINE void enc_bit_mix4_custom(
    Enc *e, Prob *child, Prob *fine, Prob *coarse, Prob *root, int bit,
    int slow, int weight, int child_rate) {
    Prob coarse_mix = (Prob)((*coarse + *root + 1u) >> 1);
    Prob parent_mix = (Prob)((*fine + coarse_mix + 1u) >> 1);
    Prob mixed = (Prob)(((unsigned)weight * *child +
                         (unsigned)(8 - weight) * parent_mix + 4u) >> 3);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt - child_rate);
    prob_update(fine, bit, e->adapt);
    prob_update(coarse, bit, e->adapt + slow);
    prob_update(root, bit, e->adapt + slow);
}

static QLIC_FORCEINLINE void enc_bit_mix4(
    Enc *e, Prob *child, Prob *fine, Prob *coarse, Prob *root, int bit,
    int slow, int fast_child) {
    enc_bit_mix4_custom(e, child, fine, coarse, root, bit, slow,
                        fast_child ? 6 : 5, fast_child);
}

static QLIC_FORCEINLINE void enc_bit_mix4_weight(
    Enc *e, Prob *child, Prob *fine, Prob *coarse, Prob *root, int bit,
    int slow, int weight) {
    Prob coarse_mix = (Prob)((*coarse + *root + 1u) >> 1);
    Prob parent_mix = (Prob)((*fine + coarse_mix + 1u) >> 1);
    Prob mixed = (Prob)(((unsigned)weight * *child +
                         (unsigned)(8 - weight) * parent_mix + 4u) >> 3);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt + 1);
    prob_update(fine, bit, e->adapt);
    prob_update(coarse, bit, e->adapt + slow);
    prob_update(root, bit, e->adapt + slow);
}

static QLIC_FORCEINLINE void enc_bit_mix5_sign(
    Enc *e, Prob *child, Prob *exact, Prob *fine, Prob *coarse, Prob *root,
    int bit, int slow, int weight, int child_rate, int exact_rate) {
    Prob mixed = (Prob)(((unsigned)weight * *child +
                         (unsigned)(16 - weight) * *exact + 8u) >> 4);
    enc_bit_value(e, mixed, bit);
    prob_update(child, bit, e->adapt - child_rate);
    prob_update(exact, bit, e->adapt - exact_rate);
    prob_update(fine, bit, e->adapt);
    prob_update(coarse, bit, e->adapt + slow);
    prob_update(root, bit, e->adapt + slow);
}

#define NACT  12
#define NERR0 3
#define NERR  6
#define NCTX  (NACT * NERR)
#define XCTX  (NCTX * 4)
#define MAXK  10
#define RUNK  24
#define NPRED 8
#define NPREDX0 16
#define NPREDX 32
#define MAP37_REUSE_PENALTY 6
#define WEIGHTED_PROXY_BPS 100
#define WEIGHTED_MIN_GAIN_BPS 75
static const uint8_t sign53_weight[3] = {4, 8, 12};
static const uint8_t sign53_child_rate[3] = {1, 2, 3};
static const uint8_t sign53_exact_rate[3] = {0, 2, 2};
static const uint8_t mode53_zero_weight = 5;
static const int8_t mode53_zero_rate = 0;
static const uint8_t mode53_magnitude_weight = 4;
static const int8_t mode53_magnitude_rate = -1;
static const int8_t mode53_root_rate[3] = {-1, 0, 1};
static const uint8_t decode_error_context[16][2] = {
    {0, 0}, {2, 1}, {2, 1}, {4, 3},
    {4, 3}, {5, 5}, {5, 5}, {5, 5},
    {5, 5}, {5, 5}, {5, 5}, {5, 5},
    {5, 5}, {5, 5}, {5, 5}, {5, 5}
};
static const uint8_t decode_predictor_context[NPREDX] = {
    0,
    NCTX, NCTX, NCTX, NCTX,
    NCTX * 2, NCTX * 2, NCTX * 2, NCTX * 2,
    NCTX * 2, NCTX * 2,
    NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3,
    NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3,
    NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3,
    NCTX * 3, NCTX * 3, NCTX * 3, NCTX * 3
};

typedef struct {
    Prob unary[XCTX][MAXK + 1];
    Prob mant[XCTX][MAXK + 1][MAXK];
    Prob nz[XCTX];
    Prob zr[XCTX];
    Prob sg[XCTX][2];
    Prob run_unary[XCTX][RUNK + 1];
    Prob run_mant[XCTX][RUNK + 1][RUNK];
    Prob predtree[NPRED];
    Prob predtreex[NPREDX];
} Model;

typedef struct {
    Prob unary[XCTX][MAXK + 1];
    Prob mant[XCTX][MAXK + 1][MAXK];
    Prob nz[XCTX];
    Prob sg[XCTX][2];
    Prob predtreex[NPREDX];
} Model37;

typedef struct {
    Prob zero[NCTX];
    Prob mag[NCTX];
    Prob nz[NCTX];
    Prob sg[NCTX][2];
} Coarse37;

typedef struct {
    Prob unary[NCTX][MAXK + 1];
    Prob mant[NCTX][MAXK + 1][MAXK];
} Coarse37Full;

typedef struct {
    Prob zero[NACT];
    Prob mag[NACT];
    Prob nz[NACT];
    Prob sg[NACT][2];
} Root37;

typedef struct {
    Prob unary[NACT][MAXK + 1];
    Prob mant[NACT][MAXK + 1][MAXK];
} Root37Full;

typedef struct {
    Prob nz[NPREDX][NCTX][3];
    Prob sg[NPREDX][NCTX][3][2];
} Predictor37;

typedef struct {
    Model37 model;
    Coarse37 coarse;
    Coarse37Full coarse_full;
    Root37 root;
    Root37Full root_full;
    Predictor37 predictor;
} Decode37Models;

static uint8_t predictor_nbits_lut[1024];
static uint8_t predictor_cost_lut[257];
static uint8_t predictor_xzr_cost_lut[512];
static uint8_t activity_ctx_lut[1025];
static uint8_t fast55_error_context_lut[32 * 32];
static uint8_t predictor_diff_cost8[511];
static uint8_t predictor_diff_xzr8[511];
static uint8_t predictor_diff_cost9[1023];
static uint8_t predictor_diff_xzr9[1023];
static uint32_t weighted_division[64];

typedef struct {
    int16_t error;
    uint8_t error_state;
    uint8_t channel_state;
} Fast55Residual;

static Fast55Residual fast55_residual_lut[512];
#if defined(_MSC_VER) && defined(_M_X64)
static uint32_t predictor_diff_cost8_avx2[511];
static uint32_t predictor_diff_xzr8_avx2[511];
static uint32_t predictor_diff_cost9_avx2[1023];
static uint32_t predictor_diff_xzr9_avx2[1023];
#endif

static void model_init_bases(void) {
    /* predictor scoring is a major encode hot path, tables avoid repeated math */
    for (unsigned i = 0; i < 1024; ++i) {
        unsigned v = i;
        int bits = 0;
        while (v) {
            ++bits;
            v >>= 1;
        }
        predictor_nbits_lut[i] = (uint8_t)bits;
        if (i < 512u)
            predictor_xzr_cost_lut[i] =
                (uint8_t)(i ? 2u * (unsigned)bits + 2u : 1u);
        if (i <= 256u)
            predictor_cost_lut[i] =
                (uint8_t)(i ? 2u * (unsigned)bits + 1u : 1u);
    }
    for (unsigned i = 0; i <= 1024u; ++i)
        activity_ctx_lut[i] =
            (uint8_t)(i <= 2u ? i : predictor_nbits_lut[i - 1u] + 1u);
    for (unsigned left = 0; left < 32u; ++left)
        for (unsigned up = 0; up < 32u; ++up) {
            unsigned state = (up & 15u) > (left & 15u) ? up : left;
            fast55_error_context_lut[(left << 5) | up] =
                decode_error_context[state & 15u][state >> 4];
        }
    for (unsigned symbol = 0; symbol < 512u; ++symbol) {
        int error = symbol & 1u ? -(int)((symbol + 1u) >> 1) :
                                  (int)(symbol >> 1);
        unsigned magnitude = (unsigned)(error < 0 ? -error : error);
        Fast55Residual *residual = fast55_residual_lut + symbol;
        residual->error = (int16_t)error;
        residual->error_state = (uint8_t)(
            predictor_nbits_lut[magnitude] | (error > 0 ? 16 : 0));
        residual->channel_state = symbol ?
            (uint8_t)((symbol & 1u ? 2u : 1u) +
                      (magnitude > 4u ? 2u : 0u)) : 0;
    }
    for (int d = -255; d <= 255; ++d) {
        int e = ((d + 128) & 255) - 128;
        unsigned v = (unsigned)(e < 0 ? -e : e);
        unsigned z = e >= 0 ? 2u * (unsigned)e : (unsigned)(-2 * e - 1);
        predictor_diff_cost8[d + 255] = predictor_cost_lut[v];
        predictor_diff_xzr8[d + 255] = predictor_xzr_cost_lut[z];
#if defined(_MSC_VER) && defined(_M_X64)
        predictor_diff_cost8_avx2[d + 255] = predictor_cost_lut[v];
        predictor_diff_xzr8_avx2[d + 255] = predictor_xzr_cost_lut[z];
#endif
    }
    for (int d = -511; d <= 511; ++d) {
        int e = ((d + 256) & 511) - 256;
        unsigned v = (unsigned)(e < 0 ? -e : e);
        unsigned z = e >= 0 ? 2u * (unsigned)e : (unsigned)(-2 * e - 1);
        predictor_diff_cost9[d + 511] = predictor_cost_lut[v];
        predictor_diff_xzr9[d + 511] = predictor_xzr_cost_lut[z];
#if defined(_MSC_VER) && defined(_M_X64)
        predictor_diff_cost9_avx2[d + 511] = predictor_cost_lut[v];
        predictor_diff_xzr9_avx2[d + 511] = predictor_xzr_cost_lut[z];
#endif
    }
    for (unsigned i = 0; i < 64u; ++i)
        weighted_division[i] = (UINT32_C(1) << 24) / (i + 1u);
}

#ifdef _WIN32
static INIT_ONCE model_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK model_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    model_init_bases();
    return TRUE;
}
static void model_ensure(void) {
    InitOnceExecuteOnce(&model_once, model_init_once, NULL, NULL);
}
#elif !defined(QLIC_WASM)
static pthread_once_t model_once = PTHREAD_ONCE_INIT;
static void model_ensure(void) {
    (void)pthread_once(&model_once, model_init_bases);
}
#else
static int model_ready;
static void model_ensure(void) {
    if (!model_ready) {
        model_init_bases();
        model_ready = 1;
    }
}
#endif

#if defined(_MSC_VER) && defined(_M_X64)
static int map37_avx2_available(void) {
    return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
}
#endif

static void probabilities_init(Prob *p, size_t count) {
    for (size_t i = 0; i < count; ++i) p[i] = PROB_INIT;
}

static void model_init(Model *m) {
    model_ensure();
    probabilities_init((Prob *)m, sizeof(*m) / sizeof(Prob));
}

static void model37_init(Model37 *m) {
    model_ensure();
    probabilities_init((Prob *)m, sizeof(*m) / sizeof(Prob));
}

static int nbits(unsigned v) {
#ifdef _MSC_VER
    unsigned long i;
    return _BitScanReverse(&i, v) ? (int)i + 1 : 0;
#elif defined(__clang__) || defined(__GNUC__)
    return v ? (int)(sizeof(v) * 8u) - __builtin_clz(v) : 0;
#else
    int k = 0;
    while (v) { k++; v >>= 1; }
    return k;
#endif
}

typedef struct {
    int32_t prediction[4];
    int32_t combined;
    uint32_t *prediction_error[4];
    int32_t *error;
    void *storage;
    size_t row_size;
} WeightedPredictor;

static int weighted_predictor_init(WeightedPredictor *state, uint32_t width) {
    memset(state, 0, sizeof(*state));
    state->row_size = (size_t)width + 2u;
    size_t cells = state->row_size * 2u;
    if (cells > SIZE_MAX / (5u * sizeof(uint32_t))) return 0;
    state->storage = calloc(cells, 5u * sizeof(uint32_t));
    if (!state->storage) return 0;
    uint32_t *rows = state->storage;
    for (int predictor = 0; predictor < 4; ++predictor)
        state->prediction_error[predictor] =
            rows + (size_t)predictor * cells;
    state->error = (int32_t *)(rows + 4u * cells);
    return 1;
}

static void weighted_predictor_free(WeightedPredictor *state) {
    free(state->storage);
}

static uint32_t weighted_error_weight(uint64_t error, uint32_t maximum) {
    int shift = nbits((unsigned)(error + 1u)) - 6;
    if (shift < 0) shift = 0;
    return 4u +
           ((maximum * weighted_division[(unsigned)(error >> shift)]) >>
            shift);
}

static int32_t weighted_average(const int32_t prediction[4],
                                uint32_t weight[4]) {
    uint32_t weight_sum = weight[0] + weight[1] + weight[2] + weight[3];
    int shift = nbits(weight_sum) - 5;
    uint32_t reduced_sum = 0;
    for (int i = 0; i < 4; ++i) {
        weight[i] >>= shift;
        reduced_sum += weight[i];
    }
    int64_t sum = (int64_t)(reduced_sum >> 1) - 1;
    for (int i = 0; i < 4; ++i)
        sum += (int64_t)prediction[i] * weight[i];
    return (int32_t)((sum * weighted_division[reduced_sum - 1u]) >> 24);
}

static int weighted_predict(WeightedPredictor *state, uint32_t x, uint32_t y,
                            uint32_t width, int N, int W, int NE, int NW,
                            int NN) {
    size_t current = y & 1u ? 0 : state->row_size;
    size_t previous = y & 1u ? state->row_size : 0;
    size_t north = previous + x;
    size_t northeast = x + 1u < width ? north + 1u : north;
    size_t northwest = x ? north - 1u : north;
    static const uint8_t base_weight[4] = {13, 12, 12, 11};
    uint32_t weight[4];
    for (int predictor = 0; predictor < 4; ++predictor) {
        uint64_t error = state->prediction_error[predictor][north] +
                         state->prediction_error[predictor][northeast] +
                         state->prediction_error[predictor][northwest];
        weight[predictor] =
            weighted_error_weight(error, base_weight[predictor]);
    }
    N <<= 3;
    W <<= 3;
    NE <<= 3;
    NW <<= 3;
    NN <<= 3;
    int32_t west_error = x ? state->error[current + x - 1u] : 0;
    int32_t north_error = state->error[north];
    int32_t northwest_error = state->error[northwest];
    int32_t northeast_error = state->error[northeast];
    int32_t west_north_error = west_error + north_error;
    state->prediction[0] = W + NE - N;
    state->prediction[1] =
        N - ((west_north_error + northeast_error) >> 2);
    state->prediction[2] =
        W - ((west_north_error + northwest_error) >> 2);
    state->prediction[3] =
        N - ((4 * northwest_error + 3 * northeast_error +
              23 * (NN - N) + 2 * (NW - W)) >>
             5);
    state->combined = weighted_average(state->prediction, weight);
    if (((north_error ^ west_error) |
         (north_error ^ northwest_error)) <= 0) {
        int maximum = W > N ? (W > NE ? W : NE) : (N > NE ? N : NE);
        int minimum = W < N ? (W < NE ? W : NE) : (N < NE ? N : NE);
        if (state->combined < minimum) state->combined = minimum;
        if (state->combined > maximum) state->combined = maximum;
    }
    return (state->combined + 3) >> 3;
}

static void weighted_predictor_update(WeightedPredictor *state, uint32_t x,
                                      uint32_t y, int value) {
    size_t current = y & 1u ? 0 : state->row_size;
    size_t previous = y & 1u ? state->row_size : 0;
    int32_t scaled_value = value << 3;
    state->error[current + x] = state->combined - scaled_value;
    for (int predictor = 0; predictor < 4; ++predictor) {
        int32_t difference = state->prediction[predictor] - scaled_value;
        uint32_t error =
            ((uint32_t)(difference < 0 ? -difference : difference) + 3u) >>
            3;
        state->prediction_error[predictor][current + x] = error;
        state->prediction_error[predictor][previous + x + 1u] += error;
    }
}

static QLIC_FORCEINLINE int qctx(int act) {
    return (unsigned)act <= 1024u ? activity_ctx_lut[act] : 11;
}

static QLIC_FORCEINLINE int local_zero_ctx(int left, int up, int left_sign,
                                            int up_sign) {
    if (!left) return up ? 2 : 0;
    if (!up) return 1;
    return left_sign == up_sign ? 3 : 4;
}
static QLIC_FORCEINLINE int channel_state(int act, int e, int k) {
    int r = e > 0 ? (k <= 2 ? 1 : 2) : (e < 0 ? (k <= 2 ? 3 : 4) : 0);
    return (act > 0 ? 5 : 0) + r;
}
static QLIC_FORCEINLINE int channel_zero_state(int state) {
    static const uint8_t lut[10] = {0, 1, 1, 2, 2, 3, 4, 4, 5, 5};
    return lut[state];
}
static QLIC_FORCEINLINE int channel_magnitude_state(int state) {
    static const uint8_t lut[10] = {0, 1, 2, 1, 2, 0, 1, 2, 1, 2};
    return lut[state];
}
static void channel_pair_tables(uint8_t zero[256], uint8_t magnitude[256]) {
    for (int high = 0; high < 10; ++high) {
        for (int low = 0; low < 10; ++low) {
            int state = (high << 4) | low;
            zero[state] = (uint8_t)(channel_zero_state(high) * 6 +
                                    channel_zero_state(low));
            magnitude[state] =
                (uint8_t)(channel_magnitude_state(high) * 3 +
                          channel_magnitude_state(low));
        }
    }
}
static QLIC_FORCEINLINE int ebkt(int k) {
    return k == 0 ? 0 : (k <= 2 ? 1 : 2);
}
static QLIC_FORCEINLINE int ectx(int act, int prevk, int prevs, int sc) {
    if (!sc) return act * NERR0 + ebkt(prevk);
    if (prevk == 0) return act * NERR;
    if (prevk <= 2) return act * NERR + (prevs > 0 ? 1 : 2);
    if (prevk <= 4) return act * NERR + (prevs > 0 ? 3 : 4);
    return act * NERR + 5;
}
static QLIC_FORCEINLINE int pctx(int pid) {
    if (pid == 0) return 0;
    if (pid <= 4) return NCTX;
    if (pid < NPREDX0) return NCTX * 2;
    return NCTX * 3;
}
static QLIC_FORCEINLINE int pctx2(int pid) {
    if (pid == 0) return 0;
    if (pid <= 4) return NCTX;
    if (pid <= 10) return NCTX * 2;
    return NCTX * 3;
}
static QLIC_FORCEINLINE int clampi(int v, int lo, int hi) {
    uint32_t u = (uint32_t)v;
    uint32_t edge = ((u >> 31) - 1u) & (uint32_t)hi;
    (void)lo;
    return (int)(u <= (uint32_t)hi ? u : edge);
}
static QLIC_FORCEINLINE int iabs(int v) { return v < 0 ? -v : v; }
static int zmode_zr(int mode) { return mode == 2 || mode == 4 || (mode >= 8 && mode <= 23); }
static int zmode_sc(int mode) {
    return (mode >= 16 && mode <= 23) || mode == 27 ||
           (mode >= 29 && mode <= 40);
}
static int zmode_valid(int mode) {
    return (mode >= 0 && mode <= 4) ||
           (mode >= 8 && mode <= 27) || (mode >= 29 && mode <= 56);
}
static int zmode_pos(int mode, int plane) {
    if (mode == 27 || (mode >= 29 && mode <= 41)) return plane == 1;
    if (mode == 24 || mode == 25 || mode == 26) return 0;
    if (mode >= 16) return ((mode - 16) >> plane) & 1;
    if (mode >= 8) return ((mode - 8) >> plane) & 1;
    return mode == 3 || mode == 4;
}

enum {
    PLANE_BASE,
    PLANE_X,
    PLANE_XZR,
    PLANE_RULE,
    PLANE_EVENT,
    PLANE_PATTERN,
    PLANE_CONTEXT,
    PLANE_SPARSE
};

static int plane_method_for(int mode) {
    if (mode == 25) return PLANE_XZR;
    if (mode == 38) return PLANE_RULE;
    if (mode == 39) return PLANE_EVENT;
    if (mode == 40) return PLANE_PATTERN;
    if (mode == 56) return PLANE_SPARSE;
    if (mode == 37 || mode == 41 || (mode >= 43 && mode <= 54))
        return PLANE_CONTEXT;
    if (mode == 24 || mode == 26 || mode == 27 ||
        (mode >= 29 && mode <= 36))
        return PLANE_X;
    return PLANE_BASE;
}

static int plane_context_for(int mode) {
    return mode >= 44 && mode <= 54 ? mode - 43 : 0;
}

/* wrapped residuals are asymmetric at the edge, this code point keeps them reversible */
static unsigned map_res(int e, int pos, int half, int maxv) {
    if (!pos) return e >= 0 ? 2u * (unsigned)e : (unsigned)(-2 * e - 1);
    if (e == -half) return (unsigned)maxv;
    return e > 0 ? 2u * (unsigned)e - 1u : (unsigned)(-2 * e);
}
static int unmap_res(unsigned v, int pos, int half, int maxv) {
    if (!pos) return (v & 1) ? -(int)((v + 1) >> 1) : (int)(v >> 1);
    if ((int)v == maxv) return -half;
    return (v & 1) ? (int)((v + 1) >> 1) : -(int)(v >> 1);
}

static int predictr(int W, int N, int NW, int NE, int WW, int NN, int maxv);

static QLIC_FORCEINLINE int predict(int id, int W, int N, int NW, int NE,
                                    int maxv) {
    switch (id) {
    case 0: { int mx = N > W ? N : W, mn = N < W ? N : W;
              if (NW >= mx) return mn;
              if (NW <= mn) return mx;
              return N + W - NW; }
    case 1:  return W;
    case 2:  return N;
    case 3:  return (W + N + 1) >> 1;
    case 4:  return clampi(N + W - NW, 0, maxv);
    case 5:  return NE;
    case 6:  return (W + NE + 1) >> 1;
    default: return clampi((3 * (W + N) - 2 * NW + 2) >> 2, 0, maxv);
    }
}

#define SPLIT_BAND_H 128u
static int predictx(int id, int W, int N, int NW, int NE, int WW, int NN, int maxv) {
    switch (id) {
    case 0:  return predict(0, W, N, NW, NE, maxv);
    case 1:  return W;
    case 2:  return N;
    case 3:  return (W + N + 1) >> 1;
    case 4:  return clampi(N + W - NW, 0, maxv);
    case 5:  return NE;
    case 6:  return (W + NE + 1) >> 1;
    case 7:  return clampi((3 * (W + N) - 2 * NW + 2) >> 2, 0, maxv);
    case 8:  return clampi(2 * W - WW, 0, maxv);
    case 9:  return clampi(2 * N - NN, 0, maxv);
    case 10: return clampi((2 * W - WW + 2 * N - NN + 1) >> 1, 0, maxv);
    case 11: return clampi(W + ((N - NW) >> 1), 0, maxv);
    case 12: return clampi(N + ((W - NW) >> 1), 0, maxv);
    case 13: return clampi((4 * W + 2 * N - 3 * NW + NE + 2) >> 2, 0, maxv);
    case 14: return clampi((W + 2 * N + NE + 2) >> 2, 0, maxv);
    case 15: return clampi((3 * W + N + 2) >> 2, 0, maxv);
    case 16: return clampi((W + 3 * N + 2) >> 2, 0, maxv);
    case 17: return clampi((5 * N + 2 * W - 3 * NW + NE + 2) >> 2, 0, maxv);
    case 18: return clampi((2 * W + N - NW + 1) >> 1, 0, maxv);
    case 19: return clampi((W + 2 * N - NW + 1) >> 1, 0, maxv);
    case 20: return clampi(W + ((NE - NW) >> 1), 0, maxv);
    case 21: return clampi(N + ((NE - NW) >> 1), 0, maxv);
    case 22: return clampi((W + N + NE + 1) / 3, 0, maxv);
    case 23: return clampi((2 * W + N + NE + 2) >> 2, 0, maxv);
    case 24: return clampi((W + 2 * N + NW + 2) >> 2, 0, maxv);
    case 25: return clampi((3 * W + 3 * N - 2 * NW + 2) >> 2, 0, maxv);
    case 26: return clampi((4 * N + W - 2 * NW + NE + 2) >> 2, 0, maxv);
    case 27: return clampi((4 * W + N - 2 * NW + NE + 2) >> 2, 0, maxv);
    case 28: return clampi((W + N + NE - NW + 1) >> 1, 0, maxv);
    case 29: return clampi((6 * W + 2 * N - 5 * NW + NE + 2) >> 2, 0, maxv);
    case 30: return clampi((2 * W + 6 * N - 5 * NW + NE + 2) >> 2, 0, maxv);
    default: return clampi((2 * W + 6 * N - 5 * NW + NE + 2) >> 2, 0, maxv);
    }
}
static QLIC_FORCEINLINE int paethp(int W, int N, int NW) {
    int p = W + N - NW;
    int a = iabs(p - W), b = iabs(p - N), c = iabs(p - NW);
    return a <= b && a <= c ? W : (b <= c ? N : NW);
}
static QLIC_FORCEINLINE int gapp(int W, int N, int NW, int NE, int WW, int NN,
                                int maxv) {
    int dh = iabs(W - WW) + iabs(N - NW) + iabs(NE - N);
    int dv = iabs(W - NW) + iabs(N - NN) + iabs(NE - N);
    int d = dv - dh;
    int p;
    if (d > 80) p = W;
    else if (d < -80) p = N;
    else {
        p = ((W + N) >> 1) + ((NE - NW) >> 2);
        if (d > 32) p = (p + W) >> 1;
        else if (d > 8) p = (3 * p + W) >> 2;
        else if (d < -32) p = (p + N) >> 1;
        else if (d < -8) p = (3 * p + N) >> 2;
    }
    return clampi(p, 0, maxv);
}
static QLIC_FORCEINLINE int predicta_impl(int id, int W, int N, int NW, int NE,
                                          int WW, int NN, int maxv) {
    switch (id) {
    case 0:  return predict(0, W, N, NW, NE, maxv);
    case 1:  return paethp(W, N, NW);
    case 2:  return W;
    case 3:  return N;
    case 4:  return (W + N + 1) >> 1;
    case 5:  return clampi(N + W - NW, 0, maxv);
    case 6:  return NE;
    case 7:  return (W + NE + 1) >> 1;
    case 8:  return (N + NE + 1) >> 1;
    case 9:  return clampi(2 * W - WW, 0, maxv);
    case 10: return clampi(2 * N - NN, 0, maxv);
    case 11: return clampi(W + (((N - NW) * 3) >> 2), 0, maxv);
    case 12: return clampi(N + (((W - NW) * 3) >> 2), 0, maxv);
    case 13: return gapp(W, N, NW, NE, WW, NN, maxv);
    case 14: return clampi((W + N + NE + NW + 2) >> 2, 0, maxv);
    case 15: return clampi((5 * W + 2 * N - 3 * NW + NE + 2) >> 2, 0, maxv);
    case 16: return clampi((W + 3 * N + 2) >> 2, 0, maxv);
    case 17: return clampi((3 * W + N + 2) >> 2, 0, maxv);
    case 18: return clampi((5 * N + 2 * W - 3 * NW + NE + 2) >> 2, 0, maxv);
    case 19: return clampi((2 * W + N - NW + 1) >> 1, 0, maxv);
    case 20: return clampi((W + 2 * N - NW + 1) >> 1, 0, maxv);
    case 21: return clampi(W + ((NE - NW) >> 1), 0, maxv);
    case 22: return clampi(N + ((NE - NW) >> 1), 0, maxv);
    case 23: return clampi((W + N + NE + 1) / 3, 0, maxv);
    case 24: return clampi((2 * W + N + NE + 2) >> 2, 0, maxv);
    case 25: return clampi((W + 2 * N + NW + 2) >> 2, 0, maxv);
    case 26: return clampi((3 * W + 3 * N - 2 * NW + 2) >> 2, 0, maxv);
    case 27: return clampi((4 * N + W - 2 * NW + NE + 2) >> 2, 0, maxv);
    case 28: return clampi((4 * W + N - 2 * NW + NE + 2) >> 2, 0, maxv);
    case 29: return clampi((W + N + NE - NW + 1) >> 1, 0, maxv);
    case 30: return clampi((6 * W + 2 * N - 5 * NW + NE + 2) >> 2, 0, maxv);
    default: return clampi((2 * W + 6 * N - 5 * NW + NE + 2) >> 2, 0, maxv);
    }
}
static QLIC_FORCEINLINE int predicta(int id, int W, int N, int NW, int NE,
                                     int WW, int NN, int maxv) {
    return predicta_impl(id, W, N, NW, NE, WW, NN, maxv);
}
static QLIC_FORCEINLINE void predictor_costs37_one(
    uint64_t *cost, const uint8_t *diff_cost, int value, int maxv, int W,
    int N, int NW, int NE, int WW, int NN) {
#define ADD_COST37_ONE(id, prediction) do { \
    int pr = (prediction); \
    cost[id] += diff_cost[value - pr + maxv]; \
} while (0)
    ADD_COST37_ONE(0, predict(0, W, N, NW, NE, maxv));
    ADD_COST37_ONE(1, paethp(W, N, NW));
    ADD_COST37_ONE(2, W);
    ADD_COST37_ONE(3, N);
    ADD_COST37_ONE(4, (W + N + 1) >> 1);
    ADD_COST37_ONE(5, clampi(N + W - NW, 0, maxv));
    ADD_COST37_ONE(6, NE);
    ADD_COST37_ONE(7, (W + NE + 1) >> 1);
    ADD_COST37_ONE(8, (N + NE + 1) >> 1);
    ADD_COST37_ONE(9, clampi(2 * W - WW, 0, maxv));
    ADD_COST37_ONE(10, clampi(2 * N - NN, 0, maxv));
    ADD_COST37_ONE(11, clampi(W + (((N - NW) * 3) >> 2), 0, maxv));
    ADD_COST37_ONE(12, clampi(N + (((W - NW) * 3) >> 2), 0, maxv));
    ADD_COST37_ONE(13, gapp(W, N, NW, NE, WW, NN, maxv));
    ADD_COST37_ONE(14, clampi((W + N + NE + NW + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(15, clampi((5 * W + 2 * N - 3 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(16, clampi((W + 3 * N + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(17, clampi((3 * W + N + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(18, clampi((5 * N + 2 * W - 3 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(19, clampi((2 * W + N - NW + 1) >> 1, 0, maxv));
    ADD_COST37_ONE(20, clampi((W + 2 * N - NW + 1) >> 1, 0, maxv));
    ADD_COST37_ONE(21, clampi(W + ((NE - NW) >> 1), 0, maxv));
    ADD_COST37_ONE(22, clampi(N + ((NE - NW) >> 1), 0, maxv));
    ADD_COST37_ONE(23, clampi((W + N + NE + 1) / 3, 0, maxv));
    ADD_COST37_ONE(24, clampi((2 * W + N + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(25, clampi((W + 2 * N + NW + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(26, clampi((3 * W + 3 * N - 2 * NW + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(27, clampi((4 * N + W - 2 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(28, clampi((4 * W + N - 2 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(29, clampi((W + N + NE - NW + 1) >> 1, 0, maxv));
    ADD_COST37_ONE(30, clampi((6 * W + 2 * N - 5 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37_ONE(31, clampi((2 * W + 6 * N - 5 * NW + NE + 2) >> 2, 0, maxv));
#undef ADD_COST37_ONE
}

static QLIC_FORCEINLINE void predictor_costs37(
    uint64_t *cost, uint64_t *cost2, uint64_t *xzr,
    const uint8_t *diff_cost, const uint8_t *diff_xzr, int value, int maxv,
    int W, int N, int NW, int NE, int WW, int NN) {
#define ADD_COST37(id, xid, prediction) do { \
    int pr = (prediction); \
    uint8_t pc = diff_cost[value - pr + maxv]; \
    cost[id] += pc; \
    if (cost2) cost2[id] += pc; \
    if (xzr && (xid) >= 0) { \
        xzr[xid] += diff_xzr[value - pr + maxv]; \
    } \
} while (0)
    ADD_COST37(0, 0, predict(0, W, N, NW, NE, maxv));
    ADD_COST37(1, -1, paethp(W, N, NW));
    ADD_COST37(2, 1, W);
    ADD_COST37(3, 2, N);
    ADD_COST37(4, 3, (W + N + 1) >> 1);
    ADD_COST37(5, 4, clampi(N + W - NW, 0, maxv));
    ADD_COST37(6, 5, NE);
    ADD_COST37(7, 6, (W + NE + 1) >> 1);
    ADD_COST37(8, -1, (N + NE + 1) >> 1);
    ADD_COST37(9, 8, clampi(2 * W - WW, 0, maxv));
    ADD_COST37(10, 9, clampi(2 * N - NN, 0, maxv));
    ADD_COST37(11, -1, clampi(W + (((N - NW) * 3) >> 2), 0, maxv));
    ADD_COST37(12, -1, clampi(N + (((W - NW) * 3) >> 2), 0, maxv));
    ADD_COST37(13, -1, gapp(W, N, NW, NE, WW, NN, maxv));
    ADD_COST37(14, -1, clampi((W + N + NE + NW + 2) >> 2, 0, maxv));
    ADD_COST37(15, -1, clampi((5 * W + 2 * N - 3 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37(16, -1, clampi((W + 3 * N + 2) >> 2, 0, maxv));
    ADD_COST37(17, 15, clampi((3 * W + N + 2) >> 2, 0, maxv));
    ADD_COST37(18, -1, clampi((5 * N + 2 * W - 3 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37(19, -1, clampi((2 * W + N - NW + 1) >> 1, 0, maxv));
    ADD_COST37(20, -1, clampi((W + 2 * N - NW + 1) >> 1, 0, maxv));
    ADD_COST37(21, -1, clampi(W + ((NE - NW) >> 1), 0, maxv));
    ADD_COST37(22, -1, clampi(N + ((NE - NW) >> 1), 0, maxv));
    ADD_COST37(23, -1, clampi((W + N + NE + 1) / 3, 0, maxv));
    ADD_COST37(24, -1, clampi((2 * W + N + NE + 2) >> 2, 0, maxv));
    ADD_COST37(25, -1, clampi((W + 2 * N + NW + 2) >> 2, 0, maxv));
    ADD_COST37(26, 7, clampi((3 * W + 3 * N - 2 * NW + 2) >> 2, 0, maxv));
    ADD_COST37(27, -1, clampi((4 * N + W - 2 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37(28, -1, clampi((4 * W + N - 2 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37(29, -1, clampi((W + N + NE - NW + 1) >> 1, 0, maxv));
    ADD_COST37(30, -1, clampi((6 * W + 2 * N - 5 * NW + NE + 2) >> 2, 0, maxv));
    ADD_COST37(31, -1, clampi((2 * W + 6 * N - 5 * NW + NE + 2) >> 2, 0, maxv));
#undef ADD_COST37
#define ADD_XZR(id, prediction) do { \
    if (xzr) { \
        int pr = (prediction); \
        xzr[id] += diff_xzr[value - pr + maxv]; \
    } \
} while (0)
    ADD_XZR(10, clampi((2 * W - WW + 2 * N - NN + 1) >> 1, 0, maxv));
    ADD_XZR(11, clampi(W + ((N - NW) >> 1), 0, maxv));
    ADD_XZR(12, clampi(N + ((W - NW) >> 1), 0, maxv));
    ADD_XZR(13, clampi((4 * W + 2 * N - 3 * NW + NE + 2) >> 2, 0, maxv));
    ADD_XZR(14, clampi((W + 2 * N + NE + 2) >> 2, 0, maxv));
#undef ADD_XZR
}

static int sign_hint(int W, int N, int NW, int NE, int WW, int NN, int pr,
                     int maxv) {
    int ref = gapp(W, N, NW, NE, WW, NN, maxv);
    return (ref > pr) - (ref < pr);
}

static Prob *sign_prob(Model *m, int ctx, int hint, int hc, int hd) {
    if (!hc || !hint) return &m->nz[ctx];
    return hd ? &m->sg[ctx][hint < 0] : &m->zr[ctx];
}
static int predictr(int W, int N, int NW, int NE, int WW, int NN, int maxv) {
    int edge = iabs(W - N) + iabs(W - NW) + iabs(N - NW);
    int dh = iabs(W - WW) + iabs(N - NW) + iabs(NE - N);
    int dv = iabs(W - NW) + iabs(N - NN) + iabs(NE - N);
    int lo = maxv >> 5, hi = maxv >> 1, gap = maxv >> 4;
    if (lo < 4) lo = 4;
    if (gap < 8) gap = 8;
    if (edge <= lo) return (W + N + 1) >> 1;
    if (dh + gap < dv) return W;
    if (dv + gap < dh) return N;
    if (edge >= hi) return paethp(W, N, NW);
    return gapp(W, N, NW, NE, WW, NN, maxv);
}
static uint32_t imod32(int64_t v, uint32_t m) {
    int64_t r = v % (int64_t)m;
    return (uint32_t)(r < 0 ? r + (int64_t)m : r);
}
static size_t order_pos(uint64_t rank, uint32_t w, uint32_t h, int order) {
    switch (order) {
    case 1: {
        uint32_t x = (uint32_t)(rank / h);
        uint32_t y = (uint32_t)(rank - (uint64_t)x * h);
        return (size_t)y * w + x;
    }
    case 2:
    case 3:
    case 4:
    case 5: {
        static const int ktab[4] = {1, -1, 2, -2};
        uint32_t band = (uint32_t)(rank / h);
        uint32_t y = (uint32_t)(rank - (uint64_t)band * h);
        uint32_t x = imod32((int64_t)band + (int64_t)ktab[order - 2] * y, w);
        return (size_t)y * w + x;
    }
    case 6: {
        uint32_t y = (uint32_t)(rank / w);
        uint32_t x = (uint32_t)(rank - (uint64_t)y * w);
        if (y & 1u) x = w - 1u - x;
        return (size_t)y * w + x;
    }
    case 7: {
        uint32_t x = (uint32_t)(rank / h);
        uint32_t y = (uint32_t)(rank - (uint64_t)x * h);
        if (x & 1u) y = h - 1u - y;
        return (size_t)y * w + x;
    }
    default:
        return (size_t)rank;
    }
}
static unsigned run_cost(unsigned v) {
    int k = nbits(v);
    return k ? (unsigned)(k * 2) : 1u;
}
static void enc_tree3(Enc *e, Prob *t, int sym) {
    int c = 1;
    for (int i = 2; i >= 0; i--) { int b = (sym >> i) & 1; enc_bit(e, &t[c], b); c = (c << 1) | b; }
}
static int dec_tree3(Dec *d, Prob *t) {
    int c = 1;
    for (int i = 0; i < 3; i++) c = (c << 1) | dec_bit(d, &t[c]);
    return c & 7;
}
static void enc_tree4(Enc *e, Prob *t, int sym) {
    int c = 1;
    for (int i = 3; i >= 0; i--) { int b = (sym >> i) & 1; enc_bit(e, &t[c], b); c = (c << 1) | b; }
}
static int dec_tree4(Dec *d, Prob *t) {
    int c = 1;
    for (int i = 0; i < 4; i++) c = (c << 1) | dec_bit(d, &t[c]);
    return c & 15;
}
static void enc_tree5(Enc *e, Prob *t, int sym) {
    int c = 1;
    for (int i = 4; i >= 0; i--) { int b = (sym >> i) & 1; enc_bit(e, &t[c], b); c = (c << 1) | b; }
}
static int dec_tree5(Dec *d, Prob *t) {
    int c = 1;
    for (int i = 0; i < 5; i++) c = (c << 1) | dec_bit(d, &t[c]);
    return c & 31;
}
static void enc_run_uint(Enc *e, Model *m, int ctx, unsigned v) {
    int k = nbits(v);
    if (k > RUNK) { e->cut = 1; return; }
    for (int i = 0; i < k; i++) enc_bit(e, &m->run_unary[ctx][i], 1);
    if (k < RUNK) enc_bit(e, &m->run_unary[ctx][k], 0);
    for (int i = k - 2; i >= 0; i--) enc_bit(e, &m->run_mant[ctx][k][i], (v >> i) & 1);
}

static unsigned dec_run_uint(Dec *d, Model *m, int ctx) {
    int k = 0;
    while (k < RUNK && dec_bit(d, &m->run_unary[ctx][k])) k++;
    unsigned v = 0;
    if (k) {
        v = 1u << (k - 1);
        for (int i = k - 2; i >= 0; i--) v |= (unsigned)dec_bit(d, &m->run_mant[ctx][k][i]) << i;
    }
    return v;
}

#define NEIGHBORS() do { \
    Wv  = x ? row[x-1] : (y ? up[x] : half); \
    Nv  = y ? up[x] : Wv; \
    NWv = (x && y) ? up[x-1] : Nv; \
    NEv = (y && x + 1 < w) ? up[x+1] : Nv; \
} while (0)

static int encode_plane(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        uint64_t *cost = calloc(ntiles * NPRED, sizeof *cost);
        tp = malloc(ntiles);
        if (!cost || !tp) { free(cost); free(tp); free(m); return STREAM_E_ALLOC; }
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row;
            for (uint32_t x = 0; x < w; x++) {
                int Wv, Nv, NWv, NEv; NEIGHBORS();
                uint64_t *cc = cost + ((size_t)(y >> tlog) * ntx + (x >> tlog)) * NPRED;
                for (int p = 0; p < NPRED; p++) {
                    int e = ((row[x] - predict(p, Wv, Nv, NWv, NEv, maxv) + half) & maxv) - half;
                    unsigned v = map_res(e, pos, half, maxv);
                    cc[p] += v ? 2u * predictor_nbits_lut[v] : 1u;
                }
            }
        }
        for (size_t i = 0; i < ntiles; i++) {
            uint64_t *cc = cost + i * NPRED; int best = 0;
            for (int p = 1; p < NPRED; p++) if (cc[p] < cc[best]) best = p;
            tp[i] = (uint8_t)best;
            enc_tree3(enc, m->predtree, best);
        }
        free(cost);
    }
    for (uint32_t y = 0; y < h && !enc->cut; y++) {
        const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row;
        int prevk = 0;
        for (uint32_t x = 0; x < w && !enc->cut; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
            int e = ((row[x] - predict(pid, Wv, Nv, NWv, NEv, maxv) + half) & maxv) - half;
            unsigned v = map_res(e, pos, half, maxv);
            int ctx = ectx(qctx(iabs(Wv-NWv) + iabs(NWv-Nv) + iabs(Nv-NEv)), prevk, 0, 0);
            int k = predictor_nbits_lut[v];
            for (int i = 0; i < k; i++) enc_bit(enc, &m->unary[ctx][i], 1);
            if (k < depth) enc_bit(enc, &m->unary[ctx][k], 0);
            for (int i = k - 2; i >= 0; i--) enc_bit(enc, &m->mant[ctx][k][i], (v >> i) & 1);
            prevk = k;
        }
    }
    free(tp); free(m);
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int encode_plane_x(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos, int sc, int alt, int hist, int sgn, int sp, int wide, int pc, int pg, int sh, int hc, int hd, const uint8_t *preset) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    int npred = wide ? NPREDX : NPREDX0;
    uint32_t ntx = 1;
    uint8_t *tp = (uint8_t *)preset;
    int owned_map = 0;
    uint8_t *upk = NULL;
    int8_t *ups = NULL;
    if (hist) {
        upk = calloc(w, sizeof(*upk));
        ups = calloc(w, sizeof(*ups));
        if (!upk || !ups) {
            free(upk);
            free(ups);
            free(m);
            return STREAM_E_ALLOC;
        }
    }
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        if (!tp) {
            uint64_t *cost =
                calloc(ntiles * (size_t)npred, sizeof *cost);
            tp = malloc(ntiles);
            owned_map = 1;
            if (!cost || !tp) {
                free(cost); free(tp); free(upk); free(ups); free(m);
                return STREAM_E_ALLOC;
            }
            for (uint32_t y = 0; y < h; y++) {
                const uint16_t *row = pl + (size_t)y * w;
                const uint16_t *up = y ? row - w : row;
                const uint16_t *up2 =
                    y > 1 ? row - (size_t)w * 2u : up;
                for (uint32_t x = 0; x < w; x++) {
                    int Wv, Nv, NWv, NEv;
                    NEIGHBORS();
                    int WWv = x > 1 ? row[x - 2] : Wv;
                    int NNv = y > 1 ? up2[x] : Nv;
                    size_t ti =
                        (size_t)(y >> tlog) * ntx + (x >> tlog);
                    uint64_t *cc = cost + ti * (size_t)npred;
                    for (int p = 0; p < npred; p++) {
                        int pr =
                            alt ? predicta(p, Wv, Nv, NWv, NEv, WWv,
                                           NNv, maxv)
                                : predictx(p, Wv, Nv, NWv, NEv, WWv,
                                           NNv, maxv);
                        int e = ((row[x] - pr + half) & maxv) - half;
                        unsigned v =
                            sgn ? (unsigned)iabs(e)
                                : map_res(e, pos, half, maxv);
                        cc[p] +=
                            v ? 2u * predictor_nbits_lut[v] +
                                    (hc ? 1u : 0u)
                              : 1u;
                    }
                }
            }
            for (size_t i = 0; i < ntiles; i++) {
                uint64_t *cc = cost + i * (size_t)npred;
                int best = 0;
                for (int p = 1; p < npred; p++)
                    if (cc[p] < cc[best]) best = p;
                tp[i] = (uint8_t)best;
            }
            free(cost);
        }
        for (size_t i = 0; i < ntiles; i++) {
            int best = tp[i];
            if (wide) enc_tree5(enc, m->predtreex, best);
            else enc_tree4(enc, m->predtreex, best);
        }
    }
    for (uint32_t y = 0; y < h && !enc->cut; y++) {
        const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w && !enc->cut; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            size_t ti = tlog ? (size_t)(y >> tlog) * ntx + (x >> tlog) : 0;
            int pid = tlog ? tp[ti] : 0;
            int pr = alt ? predicta(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv)
                         : predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            if (hist) {
                int uk = upk[x];
                act += uk <= 2 ? uk : 3;
                if (uk > ck) { ck = uk; cs = ups[x]; }
            }
            int ctx = ectx(qctx(act), ck, cs, sc);
            if (pc) ctx += pg ? pctx2(pid) : pctx(pid);
            int e = ((row[x] - pr + half) & maxv) - half;
            unsigned v = sgn ? (unsigned)iabs(e) : map_res(e, pos, half, maxv);
            int k = predictor_nbits_lut[v];
            for (int i = 0; i < k; i++) enc_bit(enc, &m->unary[ctx][i], 1);
            if (k < depth) enc_bit(enc, &m->unary[ctx][k], 0);
            for (int i = k - 2; i >= 0; i--) enc_bit(enc, &m->mant[ctx][k][i], (v >> i) & 1);
            if (sgn && k) {
                int neg = e < 0;
                int hint = sh ? sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv) : 0;
                if (sp && cs) neg ^= cs < 0;
                if (sh) neg ^= hint < 0;
                enc_bit(enc, sign_prob(m, ctx, hint, hc, hd), neg);
            }
            prevk = k;
            prevs = (e > 0) - (e < 0);
            if (hist) {
                upk[x] = (uint8_t)(k > 15 ? 15 : k);
                ups[x] = (int8_t)prevs;
            }
        }
    }
    free(upk); free(ups);
    if (owned_map) free(tp);
    free(m);
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static uint8_t predictor_map37_choose(const uint64_t *cost, size_t i,
                                      uint32_t ntx, int map_penalty,
                                      const uint8_t *tp) {
    /* changing predictors also costs map bits, the penalty favors useful reuse */
    int best = 0;
    uint64_t best_cost = UINT64_MAX;
    uint32_t tx = (uint32_t)(i % ntx);
    for (int p = 0; p < NPREDX; p++) {
        uint64_t pcost = cost[p];
        if (map_penalty) {
            if (tx) {
                int left = tp[i - 1];
                if (p != left) {
                    pcost += (uint64_t)map_penalty;
                    if (i < ntx || tp[i - ntx] == left || p != tp[i - ntx])
                        pcost += (uint64_t)map_penalty * 4u;
                }
            } else if (i >= ntx && p != tp[i - ntx]) {
                pcost += (uint64_t)map_penalty * 5u;
            }
        }
        if (pcost < best_cost) {
            best_cost = pcost;
            best = p;
        }
    }
    return (uint8_t)best;
}

static int predictor_map37(const uint16_t *pl, uint32_t w, uint32_t h,
                           int depth, int tlog, int map_penalty,
                           int paired_penalty, uint8_t **out,
                           uint8_t **paired_out, uint8_t **xzr_out) {
    model_ensure();
    int half = 1 << (depth - 1), maxv = (1 << depth) - 1;
    const uint8_t *diff_cost = depth == 8 ? predictor_diff_cost8
                                           : predictor_diff_cost9;
    const uint8_t *diff_xzr = depth == 8 ? predictor_diff_xzr8
                                          : predictor_diff_xzr9;
#if defined(_MSC_VER) && defined(_M_X64)
    const uint32_t *diff_cost_avx2 =
        depth == 8 ? predictor_diff_cost8_avx2
                   : predictor_diff_cost9_avx2;
    const uint32_t *diff_xzr_avx2 =
        depth == 8 ? predictor_diff_xzr8_avx2
                   : predictor_diff_xzr9_avx2;
    int use_avx2 =
        (tlog == 3 || tlog == 4) && map37_avx2_available();
#endif
    uint32_t ts = 1u << tlog;
    uint32_t ntx = (w + ts - 1) >> tlog;
    uint32_t nty = (h + ts - 1) >> tlog;
    size_t ntiles = (size_t)ntx * nty;
    uint8_t *tp = malloc(ntiles);
    uint8_t *paired = paired_out ? malloc(ntiles) : NULL;
    uint8_t *xzr_map = xzr_out ? malloc(ntiles) : NULL;
    if (!tp || (paired_out && !paired) || (xzr_out && !xzr_map)) {
        free(tp); free(paired); free(xzr_map);
        return STREAM_E_ALLOC;
    }
    for (uint32_t ty = 0; ty < nty; ++ty) {
        uint32_t y0 = ty << tlog;
        uint32_t y1 = y0 + ts < h ? y0 + ts : h;
        for (uint32_t tx = 0; tx < ntx; ++tx) {
            uint64_t cost[NPREDX] = {0};
            uint64_t xzr_cost[NPREDX0];
            if (xzr_map) memset(xzr_cost, 0, sizeof(xzr_cost));
            uint32_t x0 = tx << tlog;
            uint32_t x1 = x0 + ts < w ? x0 + ts : w;
            int vectorized = 0;
#if defined(_MSC_VER) && defined(_M_X64)
            if (use_avx2 && x0 >= 2 && x1 - x0 == ts && x1 < w &&
                y0 >= 2 && y1 - y0 == ts) {
                /* edge tiles keep the scalar boundary rules */
                qlic_map37_cost_tile_avx2(
                    pl, w, x0, y0, maxv, diff_cost_avx2,
                    diff_xzr_avx2, cost, xzr_map ? xzr_cost : NULL, ts);
                vectorized = 1;
            }
#endif
            if (!vectorized) {
                for (uint32_t y = y0; y < y1; ++y) {
                    const uint16_t *row = pl + (size_t)y * w;
                    const uint16_t *up = y ? row - w : row;
                    const uint16_t *up2 =
                        y > 1 ? row - (size_t)w * 2u : up;
                    for (uint32_t x = x0; x < x1; ++x) {
                        int Wv, Nv, NWv, NEv; NEIGHBORS();
                        int WWv = x > 1 ? row[x - 2] : Wv;
                        int NNv = y > 1 ? up2[x] : Nv;
                        if (xzr_map)
                            predictor_costs37(
                                cost, NULL, xzr_cost, diff_cost,
                                diff_xzr, row[x], maxv, Wv, Nv,
                                NWv, NEv, WWv, NNv);
                        else
                            predictor_costs37_one(
                                cost, diff_cost, row[x], maxv, Wv,
                                Nv, NWv, NEv, WWv, NNv);
                    }
                }
            }
            size_t ti = (size_t)ty * ntx + tx;
            tp[ti] = predictor_map37_choose(cost, ti, ntx, map_penalty, tp);
            if (paired)
                paired[ti] = predictor_map37_choose(cost, ti, ntx,
                                                    paired_penalty, paired);
            if (xzr_map) {
                int best = 0;
                for (int p = 1; p < NPREDX0; ++p)
                    if (xzr_cost[p] < xzr_cost[best]) best = p;
                xzr_map[ti] = (uint8_t)best;
            }
        }
    }
    *out = tp;
    if (paired_out) *paired_out = paired;
    if (xzr_out) *xzr_out = xzr_map;
    return STREAM_OK;
}

static int predictor_map37_weighted(
    const uint16_t *plane, uint32_t w, uint32_t h, int depth, int tlog,
    int map_penalty, const uint8_t *baseline_map, uint8_t **out,
    uint64_t *baseline_cost_out, uint64_t *candidate_cost_out) {
    /* reuse the ordinary map here, rescoring every predictor erased most of the speed gain */
    model_ensure();
    *baseline_cost_out = 0;
    *candidate_cost_out = 0;
    int half = 1 << (depth - 1);
    int maxv = (1 << depth) - 1;
    uint32_t tile_size = 1u << tlog;
    uint32_t ntx = (w + tile_size - 1u) >> tlog;
    uint32_t nty = (h + tile_size - 1u) >> tlog;
    size_t ntiles = (size_t)ntx * nty;
    uint8_t *map = malloc(ntiles);
    uint64_t *baseline_cost = calloc(ntiles, sizeof(*baseline_cost));
    uint64_t *weighted_cost = calloc(ntiles, sizeof(*weighted_cost));
    if (!map || !baseline_cost || !weighted_cost) {
        free(weighted_cost);
        free(baseline_cost);
        free(map);
        return STREAM_E_ALLOC;
    }
    WeightedPredictor weighted;
    if (!weighted_predictor_init(&weighted, w)) {
        free(weighted_cost);
        free(baseline_cost);
        free(map);
        return STREAM_E_ALLOC;
    }
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t *row = plane + (size_t)y * w;
        const uint16_t *up = y ? row - w : row;
        const uint16_t *up2 = y > 1 ? row - (size_t)w * 2u : up;
        for (uint32_t x = 0; x < w; ++x) {
            int Wv, Nv, NWv, NEv;
            NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            size_t tile =
                (size_t)(y >> tlog) * ntx + (x >> tlog);
            int pid = baseline_map[tile];
            int baseline_prediction;
            if (pid == 23)
                baseline_prediction =
                    clampi((Wv + Nv + NEv + 1) / 3, 0, maxv);
            else if (pid == 14)
                baseline_prediction =
                    clampi((Wv + Nv + NEv + NWv + 2) >> 2, 0, maxv);
            else if (pid == 0)
                baseline_prediction =
                    predict(0, Wv, Nv, NWv, NEv, maxv);
            else if (pid == 2)
                baseline_prediction = Wv;
            else if (pid == 1)
                baseline_prediction = paethp(Wv, Nv, NWv);
            else
                baseline_prediction =
                    predicta(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int prediction = weighted_predict(
                &weighted, x, y, w, Nv, Wv, NEv, NWv, NNv);
            int residual =
                ((row[x] - baseline_prediction + half) & maxv) - half;
            baseline_cost[tile] += predictor_cost_lut[iabs(residual)];
            residual = ((row[x] - prediction + half) & maxv) - half;
            weighted_cost[tile] += predictor_cost_lut[iabs(residual)];
            weighted_predictor_update(&weighted, x, y, row[x]);
        }
    }
    weighted_predictor_free(&weighted);
    for (size_t tile = 0; tile < ntiles; ++tile) {
        uint64_t cost[NPREDX];
        for (int predictor = 0; predictor < NPREDX; ++predictor)
            cost[predictor] = UINT64_MAX / 8u;
        int baseline = baseline_map[tile];
        cost[baseline] = baseline_cost[tile];
        cost[31] = weighted_cost[tile];
        map[tile] = predictor_map37_choose(
            cost, tile, ntx, map_penalty, map);
        *baseline_cost_out += baseline_cost[tile];
        *candidate_cost_out +=
            map[tile] == 31 ? weighted_cost[tile] : baseline_cost[tile];
    }
    free(weighted_cost);
    free(baseline_cost);
    *out = map;
    return STREAM_OK;
}

static int predictor_map37_pair(const uint16_t *pl, uint32_t w, uint32_t h,
                                int depth, int map_penalty,
                                int paired_penalty, uint8_t **out3,
                                uint8_t **paired3, uint8_t **out4,
                                uint8_t **paired4, uint8_t **xzr4) {
    model_ensure();
    int half = 1 << (depth - 1), maxv = (1 << depth) - 1;
    const uint8_t *diff_cost = depth == 8 ? predictor_diff_cost8
                                           : predictor_diff_cost9;
    const uint8_t *diff_xzr = depth == 8 ? predictor_diff_xzr8
                                          : predictor_diff_xzr9;
#if defined(_MSC_VER) && defined(_M_X64)
    const uint32_t *diff_cost_avx2 =
        depth == 8 ? predictor_diff_cost8_avx2
                   : predictor_diff_cost9_avx2;
    const uint32_t *diff_xzr_avx2 =
        depth == 8 ? predictor_diff_xzr8_avx2
                   : predictor_diff_xzr9_avx2;
    int use_avx2 = map37_avx2_available();
#endif
    uint32_t ntx3 = (w + 7u) >> 3;
    uint32_t nty3 = (h + 7u) >> 3;
    uint32_t ntx4 = (w + 15u) >> 4;
    uint32_t nty4 = (h + 15u) >> 4;
    size_t ntiles3 = (size_t)ntx3 * nty3;
    size_t ntiles4 = (size_t)ntx4 * nty4;
    uint64_t *cost4 = calloc((size_t)ntx4 * NPREDX, sizeof(*cost4));
    uint64_t *xzr_cost = xzr4
                             ? calloc((size_t)ntx4 * NPREDX0,
                                      sizeof(*xzr_cost))
                             : NULL;
    uint8_t *map3 = malloc(ntiles3);
    uint8_t *reuse3 = malloc(ntiles3);
    uint8_t *map4 = malloc(ntiles4);
    uint8_t *reuse4 = malloc(ntiles4);
    uint8_t *xzr_map = xzr4 ? malloc(ntiles4) : NULL;
    if (!cost4 || (xzr4 && !xzr_cost) || !map3 || !reuse3 ||
        !map4 || !reuse4 || (xzr4 && !xzr_map)) {
        free(cost4); free(map3); free(reuse3);
        free(map4); free(reuse4); free(xzr_cost); free(xzr_map);
        return STREAM_E_ALLOC;
    }
    for (uint32_t ty = 0; ty < nty3; ++ty) {
        uint32_t y0 = ty << 3;
        uint32_t y1 = y0 + 8u < h ? y0 + 8u : h;
        for (uint32_t tx = 0; tx < ntx3; ++tx) {
            uint64_t cost3[NPREDX] = {0};
            uint64_t xzr3[NPREDX0];
            if (xzr_cost) memset(xzr3, 0, sizeof(xzr3));
            uint32_t x0 = tx << 3;
            uint32_t x1 = x0 + 8u < w ? x0 + 8u : w;
            int vectorized = 0;
#if defined(_MSC_VER) && defined(_M_X64)
            if (use_avx2 && x0 >= 2 && x1 - x0 == 8 && x1 < w &&
                y0 >= 2 && y1 - y0 == 8) {
                /* edge tiles keep the scalar boundary rules */
                qlic_map37_cost_tile_avx2(
                    pl, w, x0, y0, maxv, diff_cost_avx2,
                    diff_xzr_avx2, cost3, xzr_cost ? xzr3 : NULL, 8u);
                vectorized = 1;
            }
#endif
            if (!vectorized) {
                for (uint32_t y = y0; y < y1; ++y) {
                    const uint16_t *row = pl + (size_t)y * w;
                    const uint16_t *up = y ? row - w : row;
                    const uint16_t *up2 =
                        y > 1 ? row - (size_t)w * 2u : up;
                    for (uint32_t x = x0; x < x1; ++x) {
                        int Wv, Nv, NWv, NEv; NEIGHBORS();
                        int WWv = x > 1 ? row[x - 2] : Wv;
                        int NNv = y > 1 ? up2[x] : Nv;
                        if (xzr_cost)
                            predictor_costs37(
                                cost3, NULL, xzr3, diff_cost,
                                diff_xzr, row[x], maxv, Wv, Nv,
                                NWv, NEv, WWv, NNv);
                        else
                            predictor_costs37_one(
                                cost3, diff_cost, row[x], maxv, Wv,
                                Nv, NWv, NEv, WWv, NNv);
                    }
                }
            }
            size_t i3 = (size_t)ty * ntx3 + tx;
            map3[i3] = predictor_map37_choose(cost3, i3, ntx3, map_penalty,
                                              map3);
            reuse3[i3] = predictor_map37_choose(cost3, i3, ntx3,
                                                paired_penalty, reuse3);
            uint64_t *parent = cost4 + (size_t)(tx >> 1) * NPREDX;
            for (int p = 0; p < NPREDX; ++p) parent[p] += cost3[p];
            if (xzr_cost) {
                uint64_t *xparent = xzr_cost +
                                    (size_t)(tx >> 1) * NPREDX0;
                for (int p = 0; p < NPREDX0; ++p)
                    xparent[p] += xzr3[p];
            }
        }
        if ((ty & 1u) || ty + 1u == nty3) {
            uint32_t ty4 = ty >> 1;
            for (uint32_t tx4 = 0; tx4 < ntx4; ++tx4) {
                size_t i4 = (size_t)ty4 * ntx4 + tx4;
                const uint64_t *parent = cost4 + (size_t)tx4 * NPREDX;
                map4[i4] = predictor_map37_choose(parent, i4, ntx4,
                                                   map_penalty, map4);
                reuse4[i4] = predictor_map37_choose(parent, i4, ntx4,
                                                     paired_penalty, reuse4);
                if (xzr_map) {
                    const uint64_t *xparent = xzr_cost +
                                              (size_t)tx4 * NPREDX0;
                    int best = 0;
                    for (int p = 1; p < NPREDX0; ++p)
                        if (xparent[p] < xparent[best]) best = p;
                    xzr_map[i4] = (uint8_t)best;
                }
            }
            memset(cost4, 0, (size_t)ntx4 * NPREDX * sizeof(*cost4));
            if (xzr_cost)
                memset(xzr_cost, 0,
                       (size_t)ntx4 * NPREDX0 * sizeof(*xzr_cost));
        }
    }
    free(cost4);
    free(xzr_cost);
    *out3 = map3;
    *paired3 = reuse3;
    *out4 = map4;
    *paired4 = reuse4;
    if (xzr4) *xzr4 = xzr_map;
    return STREAM_OK;
}

static QLIC_FORCEINLINE int encode_plane37_impl(
                          Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h,
                          int depth, int tlog, int cmap, int context_mode,
                          uint8_t *state_out, const uint8_t *state_in,
                          const uint8_t *preset, int map_penalty) {
    Model37 *m = malloc(sizeof(*m));
    if (!m) return STREAM_E_ALLOC;
    model37_init(m);
    int half = 1 << (depth - 1), maxv = (1 << depth) - 1;
    uint32_t ntx = 1;
    uint8_t *owned = NULL;
    const uint8_t *tp = preset;
    Prob same[2][4];
    for (int i = 0; i < 8; i++) ((Prob *)same)[i] = PROB_INIT;
    int local_ctx = context_mode != 0;
    int spatial_ctx = context_mode == 2;
    int mix_ctx = context_mode >= 3;
    int coarse_ctx = context_mode >= 4;
    int full_coarse = context_mode >= 5;
    int root_ctx = context_mode >= 6;
    int slow_root = context_mode >= 7;
    int exact_ctx = context_mode >= 8;
    int exact_sign = exact_ctx;
    int exact_sign_k = context_mode >= 9;
    int refined_sign = context_mode >= 10;
    int weighted_mode = context_mode >= 11;
    int slow_zero = 0;
    int slow_mag = refined_sign ? 2 : slow_root;
    int slow_sign = refined_sign ? 2 : slow_root;
    int slow_unary = 0;
    int slow_mant = slow_root;
    int fast_mag = 0, fast_sign = slow_root;
    Coarse37 coarse;
    Coarse37Full coarse_full;
    Root37 root;
    Root37Full root_full;
    Predictor37 predictor;
    if (coarse_ctx)
        for (size_t i = 0; i < sizeof(coarse) / sizeof(Prob); i++)
            ((Prob *)&coarse)[i] = PROB_INIT;
    if (full_coarse)
        for (size_t i = 0; i < sizeof(coarse_full) / sizeof(Prob); i++)
            ((Prob *)&coarse_full)[i] = PROB_INIT;
    if (root_ctx) {
        for (size_t i = 0; i < sizeof(root) / sizeof(Prob); i++)
            ((Prob *)&root)[i] = PROB_INIT;
        for (size_t i = 0; i < sizeof(root_full) / sizeof(Prob); i++)
            ((Prob *)&root_full)[i] = PROB_INIT;
    }
    if (exact_ctx)
        for (size_t i = 0; i < sizeof(predictor) / sizeof(Prob); i++)
            ((Prob *)&predictor)[i] = PROB_INIT;
    int local_states = spatial_ctx ? 135 : local_ctx ? 5 : 0;
    int cross_states = state_in ? (local_ctx ? local_states * (state_out ? 6 : 36) : 4) : local_states;
    size_t context_stride = tlog ? (size_t)XCTX : (size_t)NCTX;
    Prob *cross = cross_states ? malloc((size_t)cross_states * context_stride * sizeof(*cross)) : NULL;
    if (cross_states && !cross) {
        free(m);
        return STREAM_E_ALLOC;
    }
    int sign_base_states = local_ctx && state_in ? (state_out ? 30 : 180) : 0;
    int sign_states = sign_base_states * 3;
    Prob *cross_sign = sign_states ? malloc((size_t)sign_states * context_stride * sizeof(*cross_sign)) : NULL;
    if (sign_states && !cross_sign) {
        free(cross);
        free(m);
        return STREAM_E_ALLOC;
    }
    int mag_states = local_ctx && state_in ? (state_out ? 3 : 9) : 0;
    Prob *cross_mag = mag_states ? malloc((size_t)mag_states * context_stride * sizeof(*cross_mag)) : NULL;
    if (mag_states && !cross_mag) {
        free(cross_sign);
        free(cross);
        free(m);
        return STREAM_E_ALLOC;
    }
    for (size_t i = 0; i < (size_t)cross_states * context_stride; i++)
        cross[i] = 0;
    for (size_t i = 0; i < (size_t)sign_states * context_stride; i++)
        cross_sign[i] = 0;
    for (size_t i = 0; i < (size_t)mag_states * context_stride; i++)
        cross_mag[i] = 0;
    uint8_t *upk = calloc(w, sizeof(*upk));
    int8_t *ups = calloc(w, sizeof(*ups));
    uint8_t zero_pair[256];
    uint8_t magnitude_pair[256];
    if (!upk || !ups) {
        free(upk); free(ups); free(cross_mag); free(cross_sign); free(cross); free(m);
        return STREAM_E_ALLOC;
    }
    if (state_in && !state_out)
        channel_pair_tables(zero_pair, magnitude_pair);
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        if (!tp) {
            int err = predictor_map37(pl, w, h, depth, tlog, map_penalty, 0,
                                      &owned, NULL, NULL);
            if (err != STREAM_OK) {
                free(upk); free(ups); free(cross_mag); free(cross_sign); free(cross); free(m);
                return err;
            }
            tp = owned;
        }
        for (size_t i = 0; i < ntiles; i++) {
            int best = tp[i];
            int tree = 1;
            if (cmap) {
                uint32_t tx = (uint32_t)(i % ntx);
                if (tx) {
                    int ref = tp[i - 1];
                    int group = ref ? (ref <= 4 ? 1 : (ref <= 10 ? 2 : 3)) : 0;
                    int diff = best != ref;
                    enc_bit(enc, &same[0][group], diff);
                    if (!diff) tree = 0;
                }
                if (tree && i >= ntx && (!tx || tp[i - ntx] != tp[i - 1])) {
                    int ref = tp[i - ntx];
                    int group = ref ? (ref <= 4 ? 1 : (ref <= 10 ? 2 : 3)) : 0;
                    int diff = best != ref;
                    enc_bit(enc, &same[1][group], diff);
                    if (!diff) tree = 0;
                }
            }
            if (tree) enc_tree5(enc, m->predtreex, best);
        }
    }
    WeightedPredictor weighted;
    memset(&weighted, 0, sizeof(weighted));
    if (weighted_mode && !weighted_predictor_init(&weighted, w)) {
        free(upk);
        free(ups);
        free(owned);
        free(cross_mag);
        free(cross_sign);
        free(cross);
        free(m);
        return STREAM_E_ALLOC;
    }
    for (uint32_t y = 0; y < h && !enc->cut; y++) {
        const uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row;
        const uint16_t *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0, prev_uk = 0, prev_us = 0, wwk = 0, wws = 0;
        for (uint32_t x = 0; x < w && !enc->cut; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            size_t ti = tlog ? (size_t)(y >> tlog) * ntx + (x >> tlog) : 0;
            int pid = tlog ? tp[ti] : 0;
            int weighted_prediction =
                weighted_mode
                    ? weighted_predict(
                          &weighted, x, y, w, Nv, Wv, NEv, NWv, NNv)
                    : 0;
            int pr;
            if (weighted_mode && pid == 31)
                pr = weighted_prediction;
            else if (pid == 23)
                pr = clampi((Wv + Nv + NEv + 1) / 3, 0, maxv);
            else if (pid == 14)
                pr = clampi((Wv + Nv + NEv + NWv + 2) >> 2, 0, maxv);
            else if (pid == 0)
                pr = predict(0, Wv, Nv, NWv, NEv, maxv);
            else if (pid == 2)
                pr = Wv;
            else if (pid == 1)
                pr = paethp(Wv, Nv, NWv);
            else if (pid == 3)
                pr = Nv;
            else if (pid == 4)
                pr = (Wv + Nv + 1) >> 1;
            else if (pid == 5)
                pr = clampi(Nv + Wv - NWv, 0, maxv);
            else pr = predicta(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) +
                      ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            int uk = upk[x];
            int us = ups[x];
            act += uk <= 2 ? uk : 3;
            if (uk > ck) { ck = uk; cs = us; }
            int aq = qctx(act);
            int base_ctx = ectx(aq, ck, cs, 1);
            int ctx = base_ctx + pctx2(pid);
            int e = ((row[x] - pr + half) & maxv) - half;
            unsigned v = (unsigned)iabs(e);
            int k = predictor_nbits_lut[v];
            size_t pi = (size_t)y * w + x;
            int channel = state_in ? state_in[pi] : 0;
            int zero_state = local_ctx ? local_zero_ctx(prevk, uk, prevs, us) : 0;
            if (spatial_ctx) {
                int nek = x + 1 < w ? upk[x + 1] : 0;
                int nes = x + 1 < w ? ups[x + 1] : 0;
                int ne_ref = uk ? us : prevs;
                int ne_state = nek ? ((!uk && !prevk) || nes == ne_ref ? 1 : 2) : 0;
                int nw_ref = uk ? us : prevs;
                int nw_state = prev_uk ? ((!uk && !prevk) || prev_us == nw_ref ? 1 : 2) : 0;
                zero_state += 5 * ne_state + 15 * nw_state +
                              45 * (wwk ? (!prevk || wws == prevs ? 1 : 2) : 0);
            }
            if (state_in) {
                if (local_ctx) {
                    int z = state_out ? channel_zero_state(channel) :
                        zero_pair[channel];
                    zero_state += local_states * z;
                } else {
                    zero_state += channel;
                }
            }
            Prob *zero_parent = &m->unary[ctx][0];
            Prob *zero = cross_states
                             ? &cross[(size_t)zero_state * context_stride +
                                      (size_t)ctx]
                             : zero_parent;
            if (!*zero) *zero = *zero_parent;
            if (root_ctx) {
                Prob *cp = &coarse.zero[base_ctx];
                Prob *rp = &root.zero[aq];
                if (zero != zero_parent) {
                    if (refined_sign)
                        enc_bit_mix4_custom(enc, zero, zero_parent, cp, rp,
                                            k != 0, slow_zero,
                                            mode53_zero_weight,
                                            mode53_zero_rate);
                    else
                        enc_bit_mix4(enc, zero, zero_parent, cp, rp, k != 0,
                                     slow_zero, 0);
                } else
                    enc_bit_root(enc, zero_parent, cp, rp, k != 0, slow_zero);
            } else if (coarse_ctx) {
                Prob *cp = &coarse.zero[base_ctx];
                if (zero != zero_parent) enc_bit_mix3(enc, zero, zero_parent, cp, k != 0);
                else enc_bit_coarse(enc, zero_parent, cp, k != 0);
            } else if (mix_ctx && zero != zero_parent)
                enc_bit_mix(enc, zero, zero_parent, k != 0);
            else {
                enc_bit(enc, zero, k != 0);
                if (zero != zero_parent)
                    prob_update(zero_parent, k != 0, enc->adapt);
            }
            if (k) {
                Prob *u1 = &m->unary[ctx][1];
                if (cross_mag) {
                    int base = state_out ? channel_magnitude_state(channel) :
                        magnitude_pair[channel];
                    u1 = &cross_mag[(size_t)base * context_stride + (size_t)ctx];
                    if (!*u1) *u1 = m->unary[ctx][1];
                }
                if (k == 1) {
                    if (root_ctx) {
                        Prob *parent = &m->unary[ctx][1];
                        Prob *cp = &coarse.mag[base_ctx];
                        Prob *rp = &root.mag[aq];
                        if (cross_mag) {
                            if (refined_sign)
                                enc_bit_mix4_custom(enc, u1, parent, cp, rp,
                                                    0, slow_mag,
                                                    mode53_magnitude_weight,
                                                    mode53_magnitude_rate);
                            else
                                enc_bit_mix4(enc, u1, parent, cp, rp, 0,
                                             slow_mag, fast_mag);
                        } else
                            enc_bit_root(enc, parent, cp, rp, 0, slow_mag);
                    } else if (coarse_ctx) {
                        Prob *parent = &m->unary[ctx][1];
                        Prob *cp = &coarse.mag[base_ctx];
                        if (cross_mag) enc_bit_mix3(enc, u1, parent, cp, 0);
                        else enc_bit_coarse(enc, parent, cp, 0);
                    } else if (mix_ctx && cross_mag)
                        enc_bit_mix(enc, u1, &m->unary[ctx][1], 0);
                    else {
                        enc_bit(enc, u1, 0);
                        if (cross_mag) prob_update(&m->unary[ctx][1], 0, enc->adapt);
                    }
                } else {
                    if (root_ctx) {
                        Prob *parent = &m->unary[ctx][1];
                        Prob *cp = &coarse.mag[base_ctx];
                        Prob *rp = &root.mag[aq];
                        if (cross_mag) {
                            if (refined_sign)
                                enc_bit_mix4_custom(enc, u1, parent, cp, rp,
                                                    1, slow_mag,
                                                    mode53_magnitude_weight,
                                                    mode53_magnitude_rate);
                            else
                                enc_bit_mix4(enc, u1, parent, cp, rp, 1,
                                             slow_mag, fast_mag);
                        } else
                            enc_bit_root(enc, parent, cp, rp, 1, slow_mag);
                    } else if (coarse_ctx) {
                        Prob *parent = &m->unary[ctx][1];
                        Prob *cp = &coarse.mag[base_ctx];
                        if (cross_mag) enc_bit_mix3(enc, u1, parent, cp, 1);
                        else enc_bit_coarse(enc, parent, cp, 1);
                    } else if (mix_ctx && cross_mag)
                        enc_bit_mix(enc, u1, &m->unary[ctx][1], 1);
                    else {
                        enc_bit(enc, u1, 1);
                        if (cross_mag) prob_update(&m->unary[ctx][1], 1, enc->adapt);
                    }
                    for (int i = 2; i < k; i++) {
                        if (root_ctx)
                            enc_bit_root(enc, &m->unary[ctx][i],
                                         &coarse_full.unary[base_ctx][i],
                                         &root_full.unary[aq][i], 1,
                                         slow_unary);
                        else if (full_coarse)
                            enc_bit_coarse(enc, &m->unary[ctx][i],
                                           &coarse_full.unary[base_ctx][i], 1);
                        else enc_bit(enc, &m->unary[ctx][i], 1);
                    }
                    if (k < depth) {
                        if (root_ctx)
                            enc_bit_root(enc, &m->unary[ctx][k],
                                         &coarse_full.unary[base_ctx][k],
                                         &root_full.unary[aq][k], 0,
                                         slow_unary);
                        else if (full_coarse)
                            enc_bit_coarse(enc, &m->unary[ctx][k],
                                           &coarse_full.unary[base_ctx][k], 0);
                        else enc_bit(enc, &m->unary[ctx][k], 0);
                    }
                }
            }
            for (int i = k - 2; i >= 0; i--) {
                int bit = (v >> i) & 1u;
                if (root_ctx)
                    enc_bit_root(enc, &m->mant[ctx][k][i],
                                 &coarse_full.mant[base_ctx][k][i],
                                 &root_full.mant[aq][k][i], bit, slow_mant);
                else if (full_coarse)
                    enc_bit_coarse(enc, &m->mant[ctx][k][i],
                                   &coarse_full.mant[base_ctx][k][i], bit);
                else enc_bit(enc, &m->mant[ctx][k][i], bit);
            }
            if (k) {
                int neg = e < 0;
                int hint = sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv);
                if (cs) neg ^= cs < 0;
                neg ^= hint < 0;
                Prob *p;
                Prob *parent = hint ? &m->sg[ctx][hint < 0] : &m->nz[ctx];
                if (cross_sign) {
                    int cross_base = state_out ? channel_zero_state(channel) :
                        zero_pair[channel];
                    int base = cross_base * 5 +
                               local_zero_ctx(prevk, uk, prevs, us);
                    int hc = hint < 0 ? 2 : hint > 0;
                    p = &cross_sign[(size_t)(base + sign_base_states * hc) * context_stride + (size_t)ctx];
                    if (!*p) *p = *parent;
                } else {
                    p = parent;
                }
                if (root_ctx) {
                    Prob *cp = hint ? &coarse.sg[base_ctx][hint < 0]
                                    : &coarse.nz[base_ctx];
                    Prob *rp = hint ? &root.sg[aq][hint < 0] : &root.nz[aq];
                    int kb = exact_sign_k ? (k <= 1 ? 0 : k <= 3 ? 1 : 2) : 0;
                    Prob *ep = hint ? &predictor.sg[pid][base_ctx][kb][hint < 0]
                                    : &predictor.nz[pid][base_ctx][kb];
                    if (p != parent) {
                        if (exact_sign)
                            enc_bit_mix5_sign(enc, p, ep, parent, cp, rp, neg,
                                              slow_sign,
                                              refined_sign ? sign53_weight[kb] : 12,
                                              refined_sign ? sign53_child_rate[kb] : 1,
                                              refined_sign ? sign53_exact_rate[kb] : 2);
                        else
                            enc_bit_mix4(enc, p, parent, cp, rp, neg,
                                         slow_sign, fast_sign);
                    }
                    else if (exact_sign) {
                        if (refined_sign) {
                            enc_bit_mix4_custom(enc, ep, parent, cp, rp, neg,
                                                slow_sign, 5,
                                                mode53_root_rate[kb]);
                        } else
                            enc_bit_mix4_weight(enc, ep, parent, cp, rp, neg,
                                                slow_sign, 5);
                    }
                    else
                        enc_bit_root(enc, parent, cp, rp, neg, slow_sign);
                } else if (coarse_ctx) {
                    Prob *cp = hint ? &coarse.sg[base_ctx][hint < 0] : &coarse.nz[base_ctx];
                    if (p != parent) enc_bit_mix3(enc, p, parent, cp, neg);
                    else enc_bit_coarse(enc, parent, cp, neg);
                } else if (mix_ctx && p != parent)
                    enc_bit_mix(enc, p, parent, neg);
                else {
                    enc_bit(enc, p, neg);
                    if (p != parent) prob_update(parent, neg, enc->adapt);
                }
            }
            wwk = prevk;
            wws = prevs;
            prevk = k;
            prevs = (e > 0) - (e < 0);
            upk[x] = (uint8_t)(k > 15 ? 15 : k);
            ups[x] = (int8_t)prevs;
            if (state_out) {
                uint8_t next_state = local_ctx
                    ? (uint8_t)channel_state(act, e, k)
                    : (uint8_t)((act > 16 ? 2 : 0) | (k != 0));
                if (local_ctx && state_out == state_in)
                    next_state = (uint8_t)((channel << 4) | next_state);
                state_out[pi] = next_state;
            }
            if (weighted_mode)
                weighted_predictor_update(&weighted, x, y, row[x]);
            prev_uk = uk;
            prev_us = us;
        }
    }
    weighted_predictor_free(&weighted);
    free(upk); free(ups); free(owned); free(cross_mag); free(cross_sign); free(cross); free(m);
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int encode_plane37_base(Enc *enc, const uint16_t *pl, uint32_t w,
                               uint32_t h, int depth, int tlog, int cmap,
                               const uint8_t *preset, int map_penalty) {
    return encode_plane37_impl(enc, pl, w, h, depth, tlog, cmap, 0,
                               NULL, NULL, preset, map_penalty);
}

static int encode_plane37_spatial(Enc *enc, const uint16_t *pl, uint32_t w,
                                  uint32_t h, int depth, int tlog, int cmap,
                                  uint8_t *state_out, const uint8_t *state_in,
                                  const uint8_t *preset, int map_penalty) {
    return encode_plane37_impl(enc, pl, w, h, depth, tlog, cmap, 2,
                               state_out, state_in, preset, map_penalty);
}

static int encode_plane37_exact(Enc *enc, const uint16_t *pl, uint32_t w,
                                uint32_t h, int depth, int tlog, int cmap,
                                uint8_t *state_out, const uint8_t *state_in,
                                const uint8_t *preset, int map_penalty) {
    return encode_plane37_impl(enc, pl, w, h, depth, tlog, cmap, 9,
                               state_out, state_in, preset, map_penalty);
}

static int encode_plane37_refined(Enc *enc, const uint16_t *pl, uint32_t w,
                                  uint32_t h, int depth, int tlog, int cmap,
                                  uint8_t *state_out, const uint8_t *state_in,
                                  const uint8_t *preset, int map_penalty) {
    return encode_plane37_impl(enc, pl, w, h, depth, tlog, cmap, 10,
                               state_out, state_in, preset, map_penalty);
}

static int encode_plane37(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h,
                          int depth, int tlog, int cmap, int context_mode,
                          uint8_t *state_out, const uint8_t *state_in,
                          const uint8_t *preset, int map_penalty) {
    if (context_mode == 0)
        return encode_plane37_base(enc, pl, w, h, depth, tlog, cmap,
                                   preset, map_penalty);
    if (context_mode == 2)
        return encode_plane37_spatial(enc, pl, w, h, depth, tlog, cmap,
                                      state_out, state_in, preset, map_penalty);
    if (context_mode == 9)
        return encode_plane37_exact(enc, pl, w, h, depth, tlog, cmap,
                                    state_out, state_in, preset, map_penalty);
    if (context_mode == 10)
        return encode_plane37_refined(enc, pl, w, h, depth, tlog, cmap,
                                      state_out, state_in, preset, map_penalty);
    return encode_plane37_impl(enc, pl, w, h, depth, tlog, cmap, context_mode,
                               state_out, state_in, preset, map_penalty);
}

static int encode_plane_rule(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint8_t *upk = calloc(w, sizeof(*upk));
    int8_t *ups = calloc(w, sizeof(*ups));
    if (!upk || !ups) {
        free(upk);
        free(ups);
        free(m);
        return STREAM_E_ALLOC;
    }
    for (uint32_t y = 0; y < h && !enc->cut; y++) {
        const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w && !enc->cut; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pr = predictr(Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            int uk = upk[x];
            act += uk <= 2 ? uk : 3;
            if (uk > ck) { ck = uk; cs = ups[x]; }
            int ctx = ectx(qctx(act), ck, cs, 1);
            int e = ((row[x] - pr + half) & maxv) - half;
            unsigned v = (unsigned)iabs(e);
            int k = predictor_nbits_lut[v];
            for (int i = 0; i < k; i++) enc_bit(enc, &m->unary[ctx][i], 1);
            if (k < depth) enc_bit(enc, &m->unary[ctx][k], 0);
            for (int i = k - 2; i >= 0; i--) enc_bit(enc, &m->mant[ctx][k][i], (v >> i) & 1);
            if (k) {
                int hint = sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv);
                int neg = e < 0;
                if (cs) neg ^= cs < 0;
                if (hint) neg ^= hint < 0;
                enc_bit(enc, sign_prob(m, ctx, hint, 1, 1), neg);
            }
            prevk = k;
            prevs = (e > 0) - (e < 0);
            upk[x] = (uint8_t)(k > 15 ? 15 : k);
            ups[x] = (int8_t)prevs;
        }
    }
    free(upk); free(ups); free(m);
    (void)pos;
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int make_residuals(int16_t *res, const uint16_t *pl, uint32_t w, uint32_t h, int depth, unsigned *nz) {
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    unsigned n = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pr = predictr(Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int e = ((row[x] - pr + half) & maxv) - half;
            res[(size_t)y * w + x] = (int16_t)e;
            if (e) n++;
        }
    }
    *nz = n;
    return 1;
}

static int choose_order(const int16_t *res, uint32_t w, uint32_t h, unsigned *events) {
    /* sparse residuals get cheaper when nonzero events are closer in the chosen order */
    uint64_t total = (uint64_t)w * h;
    uint64_t best = UINT64_MAX;
    int best_order = 0;
    unsigned best_events = 0;
    for (int order = 0; order < 8; order++) {
        uint64_t cost = 4u;
        uint64_t last = UINT64_MAX;
        unsigned n = 0;
        for (uint64_t r = 0; r < total; r++) {
            int e = res[order_pos(r, w, h, order)];
            if (!e) continue;
            uint64_t skip = last == UINT64_MAX ? r : r - last - 1u;
            if (skip > 0xffffffu || n == 0xffffffu) {
                cost = UINT64_MAX;
                break;
            }
            unsigned a = (unsigned)iabs(e);
            cost += run_cost((unsigned)skip) + run_cost(a - 1u) + 1u;
            last = r;
            n++;
        }
        if (cost != UINT64_MAX) cost += run_cost(n);
        if (cost < best) {
            best = cost;
            best_order = order;
            best_events = n;
        }
    }
    *events = best_events;
    return best_order;
}

static int encode_plane_event(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    size_t npix = (size_t)w * h;
    if (npix > 0xffffffu) return STREAM_E_DIM;
    int16_t *res = malloc(npix * sizeof(*res));
    Model *m = malloc(sizeof *m);
    if (!res || !m) {
        free(res);
        free(m);
        return STREAM_E_ALLOC;
    }
    model_init(m);
    unsigned nz = 0;
    if (!make_residuals(res, pl, w, h, depth, &nz)) {
        free(res);
        free(m);
        return STREAM_E_ALLOC;
    }
    if ((uint64_t)nz * 2u > (uint64_t)npix) {
        free(res);
        free(m);
        return STREAM_E_FORMAT;
    }
    unsigned events = 0;
    int order = choose_order(res, w, h, &events);
    enc_tree3(enc, m->predtree, order);
    enc_run_uint(enc, m, 0, events);
    uint64_t total = (uint64_t)w * h;
    uint64_t last = UINT64_MAX;
    int prevk = 0, prevs = 0;
    for (uint64_t r = 0; r < total && !enc->cut; r++) {
        int e = res[order_pos(r, w, h, order)];
        if (!e) continue;
        unsigned skip = (unsigned)(last == UINT64_MAX ? r : r - last - 1u);
        int skip_ctx = ectx(qctx(0), prevk, prevs, 1);
        enc_run_uint(enc, m, skip_ctx, skip);
        int ctx = ectx(qctx(skip > 512u ? 512 : (int)skip), prevk, prevs, 1);
        unsigned a = (unsigned)iabs(e);
        unsigned v = a - 1u;
        int k = predictor_nbits_lut[v];
        for (int i = 0; i < k; i++) enc_bit(enc, &m->unary[ctx][i], 1);
        if (k < depth) enc_bit(enc, &m->unary[ctx][k], 0);
        for (int i = k - 2; i >= 0; i--) enc_bit(enc, &m->mant[ctx][k][i], (v >> i) & 1);
        int neg = e < 0;
        if (prevs) neg ^= prevs < 0;
        enc_bit(enc, sign_prob(m, ctx, prevs, 1, 1), neg);
        prevk = predictor_nbits_lut[a];
        prevs = (e > 0) - (e < 0);
        last = r;
    }
    free(res);
    free(m);
    (void)pos;
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int encode_plane_xzr(Enc *enc, const uint16_t *pl, uint32_t w,
                            uint32_t h, int depth, int tlog, int pos,
                            const uint8_t *preset) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *owned = NULL;
    const uint8_t *tp = preset;
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        if (!tp) {
            uint64_t *cost = calloc(ntiles * NPREDX0, sizeof *cost);
            owned = malloc(ntiles);
            if (!cost || !owned) {
                free(cost); free(owned); free(m); return STREAM_E_ALLOC;
            }
            tp = owned;
            for (uint32_t y = 0; y < h; y++) {
                const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
                for (uint32_t x = 0; x < w; x++) {
                    int Wv, Nv, NWv, NEv; NEIGHBORS();
                    int WWv = x > 1 ? row[x - 2] : Wv;
                    int NNv = y > 1 ? up2[x] : Nv;
                    uint64_t *cc = cost + ((size_t)(y >> tlog) * ntx + (x >> tlog)) * NPREDX0;
                    for (int p = 0; p < NPREDX0; p++) {
                        int e = ((row[x] - predictx(p, Wv, Nv, NWv, NEv, WWv, NNv, maxv) + half) & maxv) - half;
                        unsigned v = map_res(e, pos, half, maxv);
                        cc[p] += predictor_xzr_cost_lut[v];
                    }
                }
            }
            for (size_t i = 0; i < ntiles; i++) {
                uint64_t *cc = cost + i * NPREDX0; int best = 0;
                for (int p = 1; p < NPREDX0; p++) if (cc[p] < cc[best]) best = p;
                owned[i] = (uint8_t)best;
            }
            free(cost);
        }
        for (size_t i = 0; i < ntiles; i++) {
            enc_tree4(enc, m->predtreex, tp[i]);
        }
    }
    for (uint32_t y = 0; y < h && !enc->cut; y++) {
        const uint16_t *row = pl + (size_t)y * w, *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w && !enc->cut;) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
            int e = ((row[x] - predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv) + half) & maxv) - half;
            unsigned v = map_res(e, pos, half, maxv);
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ctx = ectx(qctx(act), prevk, prevs, 1);
            if (!v) {
                uint32_t run = 1;
                while (x + run < w) {
                    uint32_t xx = x + run;
                    Wv = xx ? row[xx - 1] : (y ? up[xx] : half);
                    Nv = y ? up[xx] : Wv;
                    NWv = (xx && y) ? up[xx - 1] : Nv;
                    NEv = (y && xx + 1 < w) ? up[xx + 1] : Nv;
                    WWv = xx > 1 ? row[xx - 2] : Wv;
                    NNv = y > 1 ? up2[xx] : Nv;
                    pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (xx >> tlog)] : 0;
                    e = ((row[xx] - predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv) + half) & maxv) - half;
                    v = map_res(e, pos, half, maxv);
                    if (v) break;
                    run++;
                }
                enc_bit(enc, &m->nz[ctx], 0);
                if (run == 1) enc_bit(enc, &m->zr[ctx], 0);
                else { enc_bit(enc, &m->zr[ctx], 1); enc_run_uint(enc, m, ctx, run - 2u); }
                x += run;
                prevk = 0;
                prevs = 0;
            } else {
                unsigned u = v - 1u;
                int k = predictor_nbits_lut[u];
                enc_bit(enc, &m->nz[ctx], 1);
                for (int i = 0; i < k; i++) enc_bit(enc, &m->unary[ctx][i], 1);
                if (k < depth) enc_bit(enc, &m->unary[ctx][k], 0);
                for (int i = k - 2; i >= 0; i--) enc_bit(enc, &m->mant[ctx][k][i], (u >> i) & 1);
                prevk = predictor_nbits_lut[v];
                prevs = (e > 0) - (e < 0);
                x++;
            }
        }
    }
    free(owned); free(m);
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int decode_plane(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        tp = malloc(ntiles);
        if (!tp) { free(m); return STREAM_E_ALLOC; }
        for (size_t i = 0; i < ntiles; i++) tp[i] = (uint8_t)dec_tree3(dec, m->predtree);
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w; const uint16_t *up = y ? row - w : row;
        int prevk = 0;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
            int ctx = ectx(qctx(iabs(Wv-NWv) + iabs(NWv-Nv) + iabs(Nv-NEv)), prevk, 0, 0);
            int k = 0;
            while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
            unsigned v = 0;
            if (k) {
                v = 1u << (k - 1);
                for (int i = k - 2; i >= 0; i--) v |= (unsigned)dec_bit(dec, &m->mant[ctx][k][i]) << i;
            }
            int e = unmap_res(v, pos, half, maxv);
            row[x] = (uint16_t)((predict(pid, Wv, Nv, NWv, NEv, maxv) + e) & maxv);
            prevk = k;
        }
    }
    free(tp); free(m);
    return STREAM_OK;
}

static int decode_plane_x(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos, int sc, int alt, int hist, int sgn, int sp, int wide, int pc, int pg, int sh, int hc, int hd) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    uint8_t *upk = NULL;
    int8_t *ups = NULL;
    if (hist) {
        upk = calloc(w, sizeof(*upk));
        ups = calloc(w, sizeof(*ups));
        if (!upk || !ups) {
            free(upk);
            free(ups);
            free(m);
            return STREAM_E_ALLOC;
        }
    }
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        tp = malloc(ntiles);
        if (!tp) { free(upk); free(ups); free(m); return STREAM_E_ALLOC; }
        for (size_t i = 0; i < ntiles; i++) {
            tp[i] = (uint8_t)(wide ? dec_tree5(dec, m->predtreex) : dec_tree4(dec, m->predtreex));
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            size_t ti = tlog ? (size_t)(y >> tlog) * ntx + (x >> tlog) : 0;
            int pid = tlog ? tp[ti] : 0;
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            if (hist) {
                int uk = upk[x];
                act += uk <= 2 ? uk : 3;
                if (uk > ck) { ck = uk; cs = ups[x]; }
            }
            int ctx = ectx(qctx(act), ck, cs, sc);
            if (pc) ctx += pg ? pctx2(pid) : pctx(pid);
            int k = 0;
            while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
            unsigned v = 0;
            if (k) {
                v = 1u << (k - 1);
                for (int i = k - 2; i >= 0; i--) v |= (unsigned)dec_bit(dec, &m->mant[ctx][k][i]) << i;
            }
            int pr = alt ? predicta_impl(pid, Wv, Nv, NWv, NEv, WWv, NNv,
                                         maxv)
                         : predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int e;
            if (sgn) {
                int neg = 0;
                if (v) {
                    int hint = sh ? sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv) : 0;
                    neg = dec_bit(dec, sign_prob(m, ctx, hint, hc, hd));
                    if (sp && cs) neg ^= cs < 0;
                    if (sh) neg ^= hint < 0;
                }
                e = v ? (neg ? -(int)v : (int)v) : 0;
            } else {
                e = unmap_res(v, pos, half, maxv);
            }
            row[x] = (uint16_t)((pr + e) & maxv);
            prevk = k;
            prevs = (e > 0) - (e < 0);
            if (hist) {
                upk[x] = (uint8_t)(k > 15 ? 15 : k);
                ups[x] = (int8_t)prevs;
            }
        }
    }
    free(upk); free(ups); free(tp); free(m);
    return STREAM_OK;
}

static int decode_plane45_mapfree(Dec *dec, uint16_t *pl, uint32_t w,
                                  uint32_t h, int depth, uint8_t *state_out,
                                  const uint8_t *state_in) {
    Model37 *m = malloc(sizeof(*m));
    if (!m) return STREAM_E_ALLOC;
    model37_init(m);
    int half = 1 << (depth - 1), maxv = (1 << depth) - 1;
    int cross_states = state_in ? 135 * (state_out ? 6 : 36) : 135;
    int sign_base_states = state_in ? (state_out ? 30 : 180) : 0;
    int sign_states = sign_base_states * 3;
    int mag_states = state_in ? (state_out ? 3 : 9) : 0;
    Prob *cross = calloc((size_t)cross_states * NCTX, sizeof(*cross));
    Prob *cross_sign = sign_states
                           ? calloc((size_t)sign_states * NCTX,
                                    sizeof(*cross_sign))
                           : NULL;
    Prob *cross_mag = mag_states
                          ? calloc((size_t)mag_states * NCTX,
                                   sizeof(*cross_mag))
                          : NULL;
    uint8_t *upk = calloc(w, sizeof(*upk));
    int8_t *ups = calloc(w, sizeof(*ups));
    uint8_t zero_pair[256];
    uint8_t magnitude_pair[256];
    Prob *cross_ctx[NCTX];
    Prob *sign_ctx[NCTX];
    Prob *magnitude_ctx[NCTX];
    if (!cross || (sign_states && !cross_sign) ||
        (mag_states && !cross_mag) || !upk || !ups) {
        free(upk);
        free(ups);
        free(cross_mag);
        free(cross_sign);
        free(cross);
        free(m);
        return STREAM_E_ALLOC;
    }
    if (state_in && !state_out)
        channel_pair_tables(zero_pair, magnitude_pair);
    for (int i = 0; i < NCTX; ++i) {
        cross_ctx[i] = cross + (size_t)i * (size_t)cross_states;
        sign_ctx[i] = cross_sign ? cross_sign + (size_t)i * (size_t)sign_states : NULL;
        magnitude_ctx[i] = cross_mag ? cross_mag + (size_t)i * (size_t)mag_states : NULL;
    }
    const uint8_t *ptr = dec->ptr;
    const uint8_t *end = dec->end;
    uint32_t range = dec->range;
    uint32_t code = dec->code;
    int truncated = dec->truncated;
    int adapt = dec->adapt;
#define DEC45_BIT(P, B) do { \
        Prob *prob__ = (P); \
        uint32_t bound__ = (range >> PROB_BITS) * *prob__; \
        if (code < bound__) { \
            range = bound__; \
            *prob__ += (PROB_ONE - *prob__) >> adapt; \
            (B) = 0; \
        } else { \
            code -= bound__; \
            range -= bound__; \
            *prob__ -= *prob__ >> adapt; \
            (B) = 1; \
        } \
        if (range < RC_TOP) { \
            range <<= 8; \
            if (ptr < end) code = (code << 8) | *ptr++; \
            else { code <<= 8; truncated = 1; } \
            if (range < RC_TOP) { \
                range <<= 8; \
                if (ptr < end) code = (code << 8) | *ptr++; \
                else { code <<= 8; truncated = 1; } \
            } \
        } \
    } while (0)
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row;
        const uint16_t *up2 = y > 1 ? up - w : up;
        uint8_t *state_row_out = state_out ? state_out + (size_t)y * w : NULL;
        const uint8_t *state_row_in = state_in ? state_in + (size_t)y * w : NULL;
        int prevk = 0, prevs = 0, prev_uk = 0, prev_us = 0;
        int wwk = 0, wws = 0;
        for (uint32_t x = 0; x < w; x++) {
            int Wv = x ? row[x - 1] : (y ? up[x] : half);
            int Nv = y ? up[x] : Wv;
            int NWv = x && y ? up[x - 1] : Nv;
            int NEv = y && x + 1 < w ? up[x + 1] : Nv;
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) +
                      ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            int uk = upk[x];
            int us = ups[x];
            act += uk <= 2 ? uk : 3;
            if (uk > ck) {
                ck = uk;
                cs = us;
            }
            int ctx = ectx(qctx(act), ck, cs, 1);
            int nek = x + 1 < w ? upk[x + 1] : 0;
            int nes = x + 1 < w ? ups[x + 1] : 0;
            int ne_ref = uk ? us : prevs;
            int ne_state = nek ? ((!uk && !prevk) || nes == ne_ref ? 1 : 2) : 0;
            int nw_ref = uk ? us : prevs;
            int nw_state = prev_uk
                               ? ((!uk && !prevk) || prev_us == nw_ref ? 1 : 2)
                               : 0;
            int local_state = local_zero_ctx(prevk, uk, prevs, us);
            int zero_state = local_state + 5 * ne_state + 15 * nw_state +
                             45 * (wwk ? (!prevk || wws == prevs ? 1 : 2) : 0);
            int state = 0;
            int cross_zero = 0;
            int cross_magnitude = 0;
            if (state_row_in) {
                state = state_row_in[x];
                if (state_row_out) {
                    cross_zero = channel_zero_state(state);
                    cross_magnitude = channel_magnitude_state(state);
                } else {
                    cross_zero = zero_pair[state];
                    cross_magnitude = magnitude_pair[state];
                }
                zero_state += 135 * cross_zero;
            }
            Prob *zero_parent = &m->unary[ctx][0];
            Prob *zero = cross_ctx[ctx] + zero_state;
            if (!*zero) *zero = *zero_parent;
            int nonzero;
            DEC45_BIT(zero, nonzero);
            prob_update(zero_parent, nonzero, adapt);
            int k = 0;
            if (nonzero) {
                k = 1;
                Prob *parent = &m->unary[ctx][1];
                Prob *u1 = parent;
                if (cross_mag) {
                    u1 = magnitude_ctx[ctx] + cross_magnitude;
                    if (!*u1) *u1 = *parent;
                }
                int more;
                DEC45_BIT(u1, more);
                if (u1 != parent) prob_update(parent, more, adapt);
                if (more) {
                    k = 2;
                    while (k < depth) {
                        DEC45_BIT(&m->unary[ctx][k], more);
                        if (!more) break;
                        k++;
                    }
                }
            }
            unsigned v = 0;
            if (k) {
                v = 1u << (k - 1);
                for (int i = k - 2; i >= 0; i--) {
                    int bit;
                    DEC45_BIT(&m->mant[ctx][k][i], bit);
                    v |= (unsigned)bit << i;
                }
            }
            int mx = Nv > Wv ? Nv : Wv;
            int mn = Nv < Wv ? Nv : Wv;
            int pr = NWv >= mx ? mn : NWv <= mn ? mx : Nv + Wv - NWv;
            int e = 0;
            if (v) {
                int hint = sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv);
                Prob *parent = hint ? &m->sg[ctx][hint < 0] : &m->nz[ctx];
                Prob *p = parent;
                if (cross_sign) {
                    int base = cross_zero * 5 + local_state;
                    int hc = hint < 0 ? 2 : hint > 0;
                    p = sign_ctx[ctx] + base + sign_base_states * hc;
                    if (!*p) *p = *parent;
                }
                int neg;
                DEC45_BIT(p, neg);
                if (p != parent) prob_update(parent, neg, adapt);
                if (cs) neg ^= cs < 0;
                neg ^= hint < 0;
                e = neg ? -(int)v : (int)v;
            }
            row[x] = (uint16_t)((pr + e) & maxv);
            wwk = prevk;
            wws = prevs;
            prevk = k;
            prevs = (e > 0) - (e < 0);
            upk[x] = (uint8_t)k;
            ups[x] = (int8_t)prevs;
            if (state_row_out) {
                uint8_t next = (uint8_t)channel_state(act, e, k);
                if (state_row_out == state_row_in)
                    next = (uint8_t)((state << 4) | next);
                state_row_out[x] = next;
            }
            prev_uk = uk;
            prev_us = us;
        }
    }
    dec->ptr = ptr;
    dec->range = range;
    dec->code = code;
    dec->truncated = truncated;
#undef DEC45_BIT
    free(upk);
    free(ups);
    free(cross_mag);
    free(cross_sign);
    free(cross);
    free(m);
    return STREAM_OK;
}

static QLIC_FORCEINLINE int decode_plane37_impl(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    int cmap, int context_mode, uint8_t *state_out, const uint8_t *state_in) {
    if (context_mode == 2 && !tlog)
        return decode_plane45_mapfree(dec, pl, w, h, depth, state_out,
                                      state_in);
    int half = 1 << (depth - 1), maxv = (1 << depth) - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    Prob same[2][4];
    for (int i = 0; i < 8; i++) ((Prob *)same)[i] = PROB_INIT;
    int local_ctx = context_mode != 0;
    int spatial_ctx = context_mode == 2;
    int mix_ctx = context_mode >= 3;
    int coarse_ctx = context_mode >= 4;
    int full_coarse = context_mode >= 5;
    int root_ctx = context_mode >= 6;
    int slow_root = context_mode >= 7;
    int exact_ctx = context_mode >= 8;
    int exact_sign = exact_ctx;
    int exact_sign_k = context_mode >= 9;
    int refined_sign = context_mode >= 10;
    int weighted_mode = context_mode >= 11;
    int slow_zero = 0;
    int slow_mag = refined_sign ? 2 : slow_root;
    int slow_sign = refined_sign ? 2 : slow_root;
    int slow_unary = 0;
    int slow_mant = slow_root;
    int fast_mag = 0, fast_sign = slow_root;
    int local_states = spatial_ctx ? 135 : local_ctx ? 5 : 0;
    int cross_states = state_in ? (local_ctx ? local_states * (state_out ? 6 : 36) : 4) : local_states;
    size_t context_stride = tlog ? (size_t)XCTX : (size_t)NCTX;
    int sign_base_states = local_ctx && state_in ? (state_out ? 30 : 180) : 0;
    int sign_states = sign_base_states * 3;
    int mag_states = local_ctx && state_in ? (state_out ? 3 : 9) : 0;
    size_t cross_count = (size_t)cross_states * context_stride;
    size_t sign_count = (size_t)sign_states * context_stride;
    size_t mag_count = (size_t)mag_states * context_stride;
    size_t prob_count = cross_count;
    if (sign_count > SIZE_MAX - prob_count) return STREAM_E_ALLOC;
    prob_count += sign_count;
    if (mag_count > SIZE_MAX - prob_count ||
        prob_count + mag_count > SIZE_MAX / sizeof(Prob))
        return STREAM_E_ALLOC;
    prob_count += mag_count;
    size_t ntiles = 0;
    if (tlog && !tp) {
        uint32_t ts = 1u << tlog;
        uint32_t nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        if ((size_t)nty > SIZE_MAX / ntx) return STREAM_E_ALLOC;
        ntiles = (size_t)ntx * nty;
    }
    size_t workspace_size = prob_count * sizeof(Prob);
    if ((size_t)w > (SIZE_MAX - workspace_size) / 2u)
        return STREAM_E_ALLOC;
    workspace_size += (size_t)w * 2u;
    if (ntiles > SIZE_MAX - workspace_size)
        return STREAM_E_ALLOC;
    workspace_size += ntiles;
    if (workspace_size > SIZE_MAX - sizeof(Decode37Models))
        return STREAM_E_ALLOC;
    Decode37Models *models =
        malloc(sizeof(*models) + workspace_size);
    if (!models) return STREAM_E_ALLOC;
    Model37 *m = &models->model;
    Coarse37 *coarse = &models->coarse;
    Coarse37Full *coarse_full = &models->coarse_full;
    Root37 *root = &models->root;
    Root37Full *root_full = &models->root_full;
    Predictor37 *predictor = &models->predictor;
    model37_init(m);
    if (coarse_ctx)
        for (size_t i = 0; i < sizeof(*coarse) / sizeof(Prob); i++)
            ((Prob *)coarse)[i] = PROB_INIT;
    if (full_coarse)
        for (size_t i = 0; i < sizeof(*coarse_full) / sizeof(Prob); i++)
            ((Prob *)coarse_full)[i] = PROB_INIT;
    if (root_ctx) {
        for (size_t i = 0; i < sizeof(*root) / sizeof(Prob); i++)
            ((Prob *)root)[i] = PROB_INIT;
        for (size_t i = 0; i < sizeof(*root_full) / sizeof(Prob); i++)
            ((Prob *)root_full)[i] = PROB_INIT;
    }
    if (exact_ctx)
        for (size_t i = 0; i < sizeof(*predictor) / sizeof(Prob); i++)
            ((Prob *)predictor)[i] = PROB_INIT;
    uint8_t *workspace = (uint8_t *)(models + 1);
    memset(workspace, 0, workspace_size);
    uint8_t *cursor = workspace;
    Prob *cross = cross_count ? (Prob *)cursor : NULL;
    cursor += cross_count * sizeof(Prob);
    Prob *cross_sign = sign_count ? (Prob *)cursor : NULL;
    cursor += sign_count * sizeof(Prob);
    Prob *cross_mag = mag_count ? (Prob *)cursor : NULL;
    cursor += mag_count * sizeof(Prob);
    uint8_t *upk = cursor;
    cursor += w;
    int8_t *ups = (int8_t *)cursor;
    cursor += w;
    tp = ntiles ? cursor : NULL;
    uint8_t zero_pair[256];
    uint8_t magnitude_pair[256];
    if (state_in && !state_out)
        channel_pair_tables(zero_pair, magnitude_pair);
    if (tlog) {
        for (size_t i = 0; i < ntiles; i++) {
            int pid = -1;
            if (cmap) {
                uint32_t tx = (uint32_t)(i % ntx);
                if (tx) {
                    int ref = tp[i - 1];
                    int group = ref ? (ref <= 4 ? 1 : (ref <= 10 ? 2 : 3)) : 0;
                    if (!dec_bit(dec, &same[0][group])) pid = ref;
                }
                if (pid < 0 && i >= ntx && (!tx || tp[i - ntx] != tp[i - 1])) {
                    int ref = tp[i - ntx];
                    int group = ref ? (ref <= 4 ? 1 : (ref <= 10 ? 2 : 3)) : 0;
                    if (!dec_bit(dec, &same[1][group])) pid = ref;
                }
            }
            if (pid < 0) pid = dec_tree5(dec, m->predtreex);
            tp[i] = (uint8_t)pid;
        }
    }
    WeightedPredictor weighted;
    memset(&weighted, 0, sizeof(weighted));
    if (weighted_mode && !weighted_predictor_init(&weighted, w)) {
        free(models);
        return STREAM_E_ALLOC;
    }
    const uint8_t *bit_cursor = dec->ptr;
    const uint8_t *bit_end = dec->end;
    uint32_t bit_range = dec->range;
    uint32_t bit_code = dec->code;
    int bit_truncated = dec->truncated;
    int bit_adapt = dec->adapt;
#define DEC37_VALUE(V, B) do { \
        uint32_t probability__ = (V); \
        uint32_t bound__ = (bit_range >> PROB_BITS) * probability__; \
        if (bit_code < bound__) { \
            bit_range = bound__; \
            (B) = 0; \
        } else { \
            bit_code -= bound__; \
            bit_range -= bound__; \
            (B) = 1; \
        } \
        if (bit_range < RC_TOP) { \
            bit_range <<= 8; \
            if (bit_cursor < bit_end) \
                bit_code = (bit_code << 8) | *bit_cursor++; \
            else { \
                bit_code <<= 8; \
                bit_truncated = 1; \
            } \
            if (bit_range < RC_TOP) { \
                bit_range <<= 8; \
                if (bit_cursor < bit_end) \
                    bit_code = (bit_code << 8) | *bit_cursor++; \
                else { \
                    bit_code <<= 8; \
                    bit_truncated = 1; \
                } \
            } \
        } \
    } while (0)
#define DEC37_BIT(P, B) do { \
        Prob *prob__ = (P); \
        DEC37_VALUE(*prob__, B); \
        prob_update(prob__, B, bit_adapt); \
    } while (0)
#define DEC37_ROOT(F, C, R, S, B) do { \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob *root__ = (R); \
        int slow__ = (S); \
        Prob coarse_mix__ = (Prob)((*coarse__ + *root__ + 1u) >> 1); \
        Prob mixed__ = (Prob)((*fine__ + coarse_mix__ + 1u) >> 1); \
        DEC37_VALUE(mixed__, B); \
        prob_update_triplet(fine__, bit_adapt, coarse__, root__, \
                            bit_adapt + slow__, B); \
    } while (0)
#define DEC37_COARSE(F, C, B) do { \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob mixed__ = (Prob)((*fine__ + *coarse__ + 1u) >> 1); \
        DEC37_VALUE(mixed__, B); \
        prob_update(fine__, B, bit_adapt); \
        prob_update(coarse__, B, bit_adapt); \
    } while (0)
#define DEC37_MIX(CH, P, B) do { \
        Prob *child__ = (CH); \
        Prob *parent__ = (P); \
        Prob mixed__ = (Prob)((5u * *child__ + 3u * *parent__ + 4u) >> 3); \
        DEC37_VALUE(mixed__, B); \
        prob_update(child__, B, bit_adapt); \
        prob_update(parent__, B, bit_adapt); \
    } while (0)
#define DEC37_MIX3(CH, P, C, B) do { \
        Prob *child__ = (CH); \
        Prob *parent__ = (P); \
        Prob *coarse__ = (C); \
        Prob parent_mix__ = (Prob)((*parent__ + *coarse__ + 1u) >> 1); \
        Prob mixed__ = (Prob)((5u * *child__ + 3u * parent_mix__ + 4u) >> 3); \
        DEC37_VALUE(mixed__, B); \
        prob_update(child__, B, bit_adapt); \
        prob_update(parent__, B, bit_adapt); \
        prob_update(coarse__, B, bit_adapt); \
    } while (0)
#define DEC37_MIX4_CUSTOM(CH, F, C, R, S, W, CR, B) do { \
        Prob *child__ = (CH); \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob *root__ = (R); \
        int slow__ = (S); \
        int weight__ = (W); \
        int child_rate__ = (CR); \
        Prob coarse_mix__ = (Prob)((*coarse__ + *root__ + 1u) >> 1); \
        Prob parent_mix__ = (Prob)((*fine__ + coarse_mix__ + 1u) >> 1); \
        Prob mixed__ = (Prob)(((unsigned)weight__ * *child__ + \
                               (unsigned)(8 - weight__) * parent_mix__ + 4u) >> 3); \
        DEC37_VALUE(mixed__, B); \
        if (!child_rate__ && !slow__) \
            prob_update_quad(child__, fine__, coarse__, root__, B, bit_adapt); \
        else \
            prob_update_four(child__, bit_adapt - child_rate__, fine__, \
                             bit_adapt, coarse__, root__, \
                             bit_adapt + slow__, B); \
    } while (0)
#define DEC37_MIX4(CH, F, C, R, S, FC, B) \
    DEC37_MIX4_CUSTOM(CH, F, C, R, S, (FC) ? 6 : 5, FC, B)
#define DEC37_MIX4_SIGN_CUSTOM(CH, F, C, R, S, W, CR, B) do { \
        Prob *child__ = (CH); \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob *root__ = (R); \
        int slow__ = (S); \
        int weight__ = (W); \
        int child_rate__ = (CR); \
        Prob coarse_mix__ = (Prob)((*coarse__ + *root__ + 1u) >> 1); \
        Prob parent_mix__ = (Prob)((*fine__ + coarse_mix__ + 1u) >> 1); \
        Prob mixed__ = (Prob)(((unsigned)weight__ * *child__ + \
                               (unsigned)(8 - weight__) * parent_mix__ + 4u) >> 3); \
        DEC37_VALUE(mixed__, B); \
        prob_update_four_branchless( \
            child__, bit_adapt - child_rate__, fine__, bit_adapt, coarse__, \
            root__, bit_adapt + slow__, B); \
    } while (0)
#define DEC37_MIX4_WEIGHT(CH, F, C, R, S, W, B) do { \
        Prob *child__ = (CH); \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob *root__ = (R); \
        int slow__ = (S); \
        int weight__ = (W); \
        Prob coarse_mix__ = (Prob)((*coarse__ + *root__ + 1u) >> 1); \
        Prob parent_mix__ = (Prob)((*fine__ + coarse_mix__ + 1u) >> 1); \
        Prob mixed__ = (Prob)(((unsigned)weight__ * *child__ + \
                               (unsigned)(8 - weight__) * parent_mix__ + 4u) >> 3); \
        DEC37_VALUE(mixed__, B); \
        prob_update_four_branchless( \
            child__, bit_adapt + 1, fine__, bit_adapt, coarse__, root__, \
            bit_adapt + slow__, B); \
    } while (0)
#define DEC37_MIX5_SIGN(CH, E, F, C, R, S, W, CR, ER, B) do { \
        Prob *child__ = (CH); \
        Prob *exact__ = (E); \
        Prob *fine__ = (F); \
        Prob *coarse__ = (C); \
        Prob *root__ = (R); \
        int weight__ = (W); \
        Prob mixed__ = (Prob)(((unsigned)weight__ * *child__ + \
                               (unsigned)(16 - weight__) * *exact__ + 8u) >> 4); \
        DEC37_VALUE(mixed__, B); \
        prob_update_five(child__, bit_adapt - (CR), exact__, \
                         bit_adapt - (ER), fine__, bit_adapt, coarse__, \
                         root__, bit_adapt + (S), B); \
    } while (0)
    for (uint32_t y = 0; y < h; y++) {
        size_t row_offset = (size_t)y * w;
        uint16_t *row = pl + row_offset;
        const uint16_t *up = y ? row - w : row;
        const uint16_t *up2 = y > 1 ? row - (size_t)w * 2u : up;
        uint8_t *state_row_out = state_out ? state_out + row_offset : NULL;
        const uint8_t *state_row_in =
            state_in ? state_in + row_offset : NULL;
        const uint8_t *tile_row =
            tlog ? tp + (size_t)(y >> tlog) * ntx : NULL;
        int prevk = 0, prevs = 0, prev_uk = 0, prev_us = 0, wwk = 0, wws = 0;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pid = tile_row ? tile_row[x >> tlog] : 0;
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) +
                      ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            int uk = upk[x];
            int us = ups[x];
            act += uk <= 2 ? uk : 3;
            if (uk > ck) { ck = uk; cs = us; }
            int aq = qctx(act);
            int base_ctx =
                aq * NERR + decode_error_context[ck][cs > 0];
            int ctx = base_ctx + decode_predictor_context[pid];
            int channel_context = state_row_in ? state_row_in[x] : 0;
            int local_state =
                local_ctx ? local_zero_ctx(prevk, uk, prevs, us) : 0;
            int zero_state = local_state;
            int cross_zero = 0;
            int cross_magnitude = 0;
            if (spatial_ctx) {
                int nek = x + 1 < w ? upk[x + 1] : 0;
                int nes = x + 1 < w ? ups[x + 1] : 0;
                int ne_ref = uk ? us : prevs;
                int ne_state = nek ? ((!uk && !prevk) || nes == ne_ref ? 1 : 2) : 0;
                int nw_ref = uk ? us : prevs;
                int nw_state = prev_uk ? ((!uk && !prevk) || prev_us == nw_ref ? 1 : 2) : 0;
                zero_state += 5 * ne_state + 15 * nw_state +
                              45 * (wwk ? (!prevk || wws == prevs ? 1 : 2) : 0);
            }
            if (state_row_in) {
                if (local_ctx) {
                    if (state_row_out) {
                        cross_zero = channel_zero_state(channel_context);
                        cross_magnitude =
                            channel_magnitude_state(channel_context);
                    } else {
                        cross_zero = zero_pair[channel_context];
                        cross_magnitude = magnitude_pair[channel_context];
                    }
                    zero_state += local_states * cross_zero;
                } else {
                    zero_state += channel_context;
                }
            }
            Prob *zero_parent = &m->unary[ctx][0];
            Prob *zero = cross_states
                             ? &cross[(size_t)ctx * (size_t)cross_states +
                                      (size_t)zero_state]
                             : zero_parent;
            if (!*zero) *zero = *zero_parent;
            int k = 0;
            int nonzero;
            if (root_ctx) {
                Prob *cp = &coarse->zero[base_ctx];
                Prob *rp = &root->zero[aq];
                if (zero != zero_parent) {
                    if (refined_sign)
                        DEC37_MIX4_CUSTOM(zero, zero_parent, cp, rp,
                                          slow_zero, mode53_zero_weight,
                                          mode53_zero_rate, nonzero);
                    else
                        DEC37_MIX4(zero, zero_parent, cp, rp, slow_zero, 0,
                                   nonzero);
                } else
                    DEC37_ROOT(zero_parent, cp, rp, slow_zero, nonzero);
            } else if (coarse_ctx) {
                Prob *cp = &coarse->zero[base_ctx];
                if (zero != zero_parent)
                    DEC37_MIX3(zero, zero_parent, cp, nonzero);
                else DEC37_COARSE(zero_parent, cp, nonzero);
            } else if (mix_ctx && zero != zero_parent)
                DEC37_MIX(zero, zero_parent, nonzero);
            else {
                DEC37_BIT(zero, nonzero);
                if (zero != zero_parent)
                    prob_update(zero_parent, nonzero, bit_adapt);
            }
            if (nonzero) {
                k = 1;
                Prob *u1 = &m->unary[ctx][1];
                if (cross_mag) {
                    u1 = &cross_mag[(size_t)ctx * (size_t)mag_states +
                                    (size_t)cross_magnitude];
                    if (!*u1) *u1 = m->unary[ctx][1];
                }
                int more;
                if (root_ctx) {
                    Prob *parent = &m->unary[ctx][1];
                    Prob *cp = &coarse->mag[base_ctx];
                    Prob *rp = &root->mag[aq];
                    if (cross_mag) {
                        if (refined_sign)
                            DEC37_MIX4_CUSTOM(u1, parent, cp, rp, slow_mag,
                                              mode53_magnitude_weight,
                                              mode53_magnitude_rate, more);
                        else
                            DEC37_MIX4(u1, parent, cp, rp, slow_mag, fast_mag,
                                       more);
                    } else
                        DEC37_ROOT(parent, cp, rp, slow_mag, more);
                } else if (coarse_ctx) {
                    Prob *parent = &m->unary[ctx][1];
                    Prob *cp = &coarse->mag[base_ctx];
                    if (cross_mag) DEC37_MIX3(u1, parent, cp, more);
                    else DEC37_COARSE(parent, cp, more);
                } else if (mix_ctx && cross_mag)
                    DEC37_MIX(u1, &m->unary[ctx][1], more);
                else {
                    DEC37_BIT(u1, more);
                    if (cross_mag)
                        prob_update(&m->unary[ctx][1], more, bit_adapt);
                }
                if (more) {
                    k = 2;
                    while (k < depth) {
                        int bit;
                        if (root_ctx)
                            DEC37_ROOT(&m->unary[ctx][k],
                                       &coarse_full->unary[base_ctx][k],
                                       &root_full->unary[aq][k], slow_unary,
                                       bit);
                        else if (full_coarse)
                            DEC37_COARSE(&m->unary[ctx][k],
                                         &coarse_full->unary[base_ctx][k], bit);
                        else
                            DEC37_BIT(&m->unary[ctx][k], bit);
                        if (!bit) break;
                        k++;
                    }
                }
            }
            unsigned v = 0;
            if (k) {
                v = 1u << (k - 1);
                for (int i = k - 2; i >= 0; i--) {
                    int bit;
                    if (root_ctx)
                        DEC37_ROOT(&m->mant[ctx][k][i],
                                   &coarse_full->mant[base_ctx][k][i],
                                   &root_full->mant[aq][k][i], slow_mant,
                                   bit);
                    else if (full_coarse)
                        DEC37_COARSE(&m->mant[ctx][k][i],
                                     &coarse_full->mant[base_ctx][k][i], bit);
                    else
                        DEC37_BIT(&m->mant[ctx][k][i], bit);
                    v |= (unsigned)bit << i;
                }
            }
            int weighted_prediction =
                weighted_mode
                    ? weighted_predict(
                          &weighted, x, y, w, Nv, Wv, NEv, NWv, NNv)
                    : 0;
            int pr;
            if (weighted_mode && pid == 31)
                pr = weighted_prediction;
            else if (pid == 0)
                pr = predict(0, Wv, Nv, NWv, NEv, maxv);
            else if (pid == 1)
                pr = paethp(Wv, Nv, NWv);
            else if (pid == 2) pr = Wv;
            else if (pid == 3) pr = Nv;
            else if (pid == 4) pr = (Wv + Nv + 1) >> 1;
            else if (pid == 5)
                pr = clampi(Nv + Wv - NWv, 0, maxv);
            else if (pid == 6) pr = NEv;
            else if (pid == 14)
                pr = clampi((Wv + Nv + NEv + NWv + 2) >> 2, 0, maxv);
            else if (pid == 23)
                pr = clampi((Wv + Nv + NEv + 1) / 3, 0, maxv);
            else
                pr = predicta_impl(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int e = 0;
            if (v) {
                int hint = sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv);
                Prob *p;
                Prob *parent = hint ? &m->sg[ctx][hint < 0] : &m->nz[ctx];
                if (cross_sign) {
                    int base = cross_zero * 5 + local_state;
                    int hc = hint < 0 ? 2 : hint > 0;
                    p = &cross_sign[
                        (size_t)ctx * (size_t)sign_states +
                        (size_t)(base + sign_base_states * hc)];
                    if (!*p) *p = *parent;
                } else {
                    p = parent;
                }
                int neg;
                if (root_ctx) {
                    Prob *cp = hint ? &coarse->sg[base_ctx][hint < 0]
                                    : &coarse->nz[base_ctx];
                    Prob *rp = hint ? &root->sg[aq][hint < 0] : &root->nz[aq];
                    int kb = exact_sign_k ? (k <= 1 ? 0 : k <= 3 ? 1 : 2) : 0;
                    Prob *ep = hint ? &predictor->sg[pid][base_ctx][kb][hint < 0]
                                    : &predictor->nz[pid][base_ctx][kb];
                    if (p != parent) {
                        if (exact_sign)
                            DEC37_MIX5_SIGN(
                                p, ep, parent, cp, rp, slow_sign,
                                refined_sign ? sign53_weight[kb] : 12,
                                refined_sign ? sign53_child_rate[kb] : 1,
                                refined_sign ? sign53_exact_rate[kb] : 2,
                                neg);
                        else
                            DEC37_MIX4(p, parent, cp, rp, slow_sign,
                                       fast_sign, neg);
                    }
                    else if (exact_sign) {
                        if (refined_sign) {
                            DEC37_MIX4_SIGN_CUSTOM(
                                ep, parent, cp, rp, slow_sign, 5,
                                mode53_root_rate[kb], neg);
                        } else
                            DEC37_MIX4_WEIGHT(ep, parent, cp, rp, slow_sign,
                                              5, neg);
                    }
                    else
                        DEC37_ROOT(parent, cp, rp, slow_sign, neg);
                } else if (coarse_ctx) {
                    Prob *cp = hint ? &coarse->sg[base_ctx][hint < 0] : &coarse->nz[base_ctx];
                    if (p != parent) DEC37_MIX3(p, parent, cp, neg);
                    else DEC37_COARSE(parent, cp, neg);
                } else if (mix_ctx && p != parent)
                    DEC37_MIX(p, parent, neg);
                else {
                    DEC37_BIT(p, neg);
                    if (p != parent) prob_update(parent, neg, bit_adapt);
                }
                if (cs) neg ^= cs < 0;
                neg ^= hint < 0;
                e = neg ? -(int)v : (int)v;
            }
            row[x] = (uint16_t)((pr + e) & maxv);
            if (weighted_mode)
                weighted_predictor_update(&weighted, x, y, row[x]);
            wwk = prevk;
            wws = prevs;
            prevk = k;
            prevs = (e > 0) - (e < 0);
            upk[x] = (uint8_t)(k > 15 ? 15 : k);
            ups[x] = (int8_t)prevs;
            if (state_row_out) {
                uint8_t next_state = local_ctx
                    ? (uint8_t)channel_state(act, e, k)
                    : (uint8_t)((act > 16 ? 2 : 0) | (k != 0));
                if (local_ctx && state_row_out == state_row_in)
                    next_state =
                        (uint8_t)((channel_context << 4) | next_state);
                state_row_out[x] = next_state;
            }
            prev_uk = uk;
            prev_us = us;
        }
    }
    dec->ptr = bit_cursor;
    dec->range = bit_range;
    dec->code = bit_code;
    dec->truncated = bit_truncated;
#undef DEC37_MIX5_SIGN
#undef DEC37_MIX4_WEIGHT
#undef DEC37_MIX4_SIGN_CUSTOM
#undef DEC37_MIX4
#undef DEC37_MIX4_CUSTOM
#undef DEC37_MIX3
#undef DEC37_MIX
#undef DEC37_COARSE
#undef DEC37_ROOT
#undef DEC37_BIT
#undef DEC37_VALUE
    weighted_predictor_free(&weighted);
    free(models);
    return STREAM_OK;
}

static QLIC_NOINLINE int decode_plane37(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    int cmap, int context_mode, uint8_t *state_out, const uint8_t *state_in) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, cmap, context_mode,
                               state_out, state_in);
}

/* fixed wrappers keep mode checks out of the pixel loop */
static QLIC_NOINLINE int decode_plane52_independent(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 9, NULL, NULL);
}

static QLIC_NOINLINE int decode_plane52_first(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 9, state, NULL);
}

static QLIC_NOINLINE int decode_plane52_middle(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 9, state,
                               state);
}

static QLIC_NOINLINE int decode_plane52_last(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    const uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 9, NULL, state);
}

static QLIC_NOINLINE int decode_plane53_independent(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 10, NULL, NULL);
}

static QLIC_NOINLINE int decode_plane53_first(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 10, state, NULL);
}

static QLIC_NOINLINE int decode_plane53_middle(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 10, state,
                               state);
}

static QLIC_NOINLINE int decode_plane53_last(
    Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog,
    const uint8_t *state) {
    return decode_plane37_impl(dec, pl, w, h, depth, tlog, 1, 10, NULL, state);
}

static int decode_plane_rule(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint8_t *upk = calloc(w, sizeof(*upk));
    int8_t *ups = calloc(w, sizeof(*ups));
    if (!upk || !ups) {
        free(upk);
        free(ups);
        free(m);
        return STREAM_E_ALLOC;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pr = predictr(Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ck = prevk, cs = prevs;
            int uk = upk[x];
            act += uk <= 2 ? uk : 3;
            if (uk > ck) { ck = uk; cs = ups[x]; }
            int ctx = ectx(qctx(act), ck, cs, 1);
            int k = 0;
            while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
            unsigned v = 0;
            if (k) {
                v = 1u << (k - 1);
                for (int i = k - 2; i >= 0; i--) v |= (unsigned)dec_bit(dec, &m->mant[ctx][k][i]) << i;
            }
            int e = 0;
            if (v) {
                int hint = sign_hint(Wv, Nv, NWv, NEv, WWv, NNv, pr, maxv);
                int neg = dec_bit(dec, sign_prob(m, ctx, hint, 1, 1));
                if (cs) neg ^= cs < 0;
                if (hint) neg ^= hint < 0;
                e = neg ? -(int)v : (int)v;
            }
            row[x] = (uint16_t)((pr + e) & maxv);
            prevk = k;
            prevs = (e > 0) - (e < 0);
            upk[x] = (uint8_t)(k > 15 ? 15 : k);
            ups[x] = (int8_t)prevs;
        }
    }
    free(upk); free(ups); free(m);
    (void)pos;
    return STREAM_OK;
}

static int recon_residuals(uint16_t *pl, const int16_t *res, uint32_t w, uint32_t h, int depth) {
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        for (uint32_t x = 0; x < w; x++) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pr = predictr(Wv, Nv, NWv, NEv, WWv, NNv, maxv);
            int e = res[(size_t)y * w + x];
            row[x] = (uint16_t)((pr + e) & maxv);
        }
    }
    return 1;
}

static int decode_plane_event(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    size_t npix = (size_t)w * h;
    if (npix > 0xffffffu) return STREAM_E_FORMAT;
    Model *m = malloc(sizeof *m);
    int16_t *res = calloc(npix, sizeof(*res));
    if (!m || !res) {
        free(m);
        free(res);
        return STREAM_E_ALLOC;
    }
    model_init(m);
    int order = dec_tree3(dec, m->predtree);
    unsigned events = dec_run_uint(dec, m, 0);
    if (events > npix) {
        free(m);
        free(res);
        return STREAM_E_CORRUPT;
    }
    uint64_t rank = UINT64_MAX;
    int prevk = 0, prevs = 0;
    for (unsigned i = 0; i < events; i++) {
        int skip_ctx = ectx(qctx(0), prevk, prevs, 1);
        unsigned skip = dec_run_uint(dec, m, skip_ctx);
        uint64_t nrank = rank == UINT64_MAX ? (uint64_t)skip : rank + (uint64_t)skip + 1u;
        if (nrank >= (uint64_t)npix) {
            free(m);
            free(res);
            return STREAM_E_CORRUPT;
        }
        int ctx = ectx(qctx(skip > 512u ? 512 : (int)skip), prevk, prevs, 1);
        int k = 0;
        while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
        unsigned v = 0;
        if (k) {
            v = 1u << (k - 1);
            for (int j = k - 2; j >= 0; j--) v |= (unsigned)dec_bit(dec, &m->mant[ctx][k][j]) << j;
        }
        unsigned a = v + 1u;
        if (a > (unsigned)(1 << (depth - 1))) {
            free(m);
            free(res);
            return STREAM_E_CORRUPT;
        }
        int neg = dec_bit(dec, sign_prob(m, ctx, prevs, 1, 1));
        if (prevs) neg ^= prevs < 0;
        int e = neg ? -(int)a : (int)a;
        res[order_pos(nrank, w, h, order)] = (int16_t)e;
        prevk = nbits(a);
        prevs = (e > 0) - (e < 0);
        rank = nrank;
    }
    int ok = recon_residuals(pl, res, w, h, depth);
    free(m);
    free(res);
    (void)pos;
    return ok ? STREAM_OK : STREAM_E_CORRUPT;
}

#define PAT_MAX 127
#define PAT_STORE 4096
#define PAT_HASH 8192

typedef struct {
    uint16_t v[4];
    uint32_t n;
} Pat;

static void enc_bits(Enc *e, Prob *p, unsigned v, int bits) {
    for (int i = bits - 1; i >= 0; i--) enc_bit(e, &p[i], (v >> i) & 1u);
}

static unsigned dec_bits(Dec *d, Prob *p, int bits) {
    unsigned v = 0;
    for (int i = bits - 1; i >= 0; i--) v |= (unsigned)dec_bit(d, &p[i]) << i;
    return v;
}

static uint32_t pat_hashv(const uint16_t v[4]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h ^= v[i] & 255u;
        h *= 16777619u;
        h ^= v[i] >> 8;
        h *= 16777619u;
    }
    return h & (PAT_HASH - 1u);
}

static int pat_eq(const uint16_t a[4], const uint16_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void pat_block(const uint16_t *pl, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint16_t v[4]) {
    uint32_t x = bx << 1, y = by << 1;
    uint32_t x1 = x + 1u < w ? x + 1u : x;
    uint32_t y1 = y + 1u < h ? y + 1u : y;
    const uint16_t *r0 = pl + (size_t)y * w;
    const uint16_t *r1 = pl + (size_t)y1 * w;
    v[0] = r0[x];
    v[1] = r0[x1];
    v[2] = r1[x];
    v[3] = r1[x1];
}

static void pat_put(uint16_t *pl, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, const uint16_t v[4]) {
    uint32_t x = bx << 1, y = by << 1;
    pl[(size_t)y * w + x] = v[0];
    if (x + 1u < w) pl[(size_t)y * w + x + 1u] = v[1];
    if (y + 1u < h) {
        pl[(size_t)(y + 1u) * w + x] = v[2];
        if (x + 1u < w) pl[(size_t)(y + 1u) * w + x + 1u] = v[3];
    }
}

static int pat_slot(const Pat *pat, const int *tab, const uint16_t v[4]) {
    uint32_t h = pat_hashv(v);
    for (uint32_t i = 0; i < PAT_HASH; i++) {
        int k = tab[(h + i) & (PAT_HASH - 1u)];
        if (k < 0 || pat_eq(pat[k].v, v)) return (int)((h + i) & (PAT_HASH - 1u));
    }
    return -1;
}

static int pat_token_at(const Pat *pat, const int *tab, const uint8_t *map, const uint16_t v[4]) {
    int s = pat_slot(pat, tab, v);
    if (s < 0 || tab[s] < 0) return 0;
    return map[tab[s]];
}

static int encode_plane_pattern(Enc *enc, const uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    uint32_t bw = (w + 1u) >> 1, bh = (h + 1u) >> 1;
    size_t blocks = (size_t)bw * bh;
    if (!blocks || blocks > 0xffffffu) return STREAM_E_FORMAT;
    Pat *pat = calloc(PAT_STORE, sizeof(*pat));
    int *tab = malloc(PAT_HASH * sizeof(*tab));
    uint8_t *map = calloc(PAT_STORE, 1);
    uint8_t *tok = malloc(blocks);
    Model *m = malloc(sizeof *m);
    if (!pat || !tab || !map || !tok || !m) {
        free(pat); free(tab); free(map); free(tok); free(m);
        return STREAM_E_ALLOC;
    }
    for (int i = 0; i < PAT_HASH; i++) tab[i] = -1;
    int pn = 0;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint16_t v[4];
            pat_block(pl, w, h, bx, by, v);
            int s = pat_slot(pat, tab, v);
            if (s < 0) { free(pat); free(tab); free(map); free(tok); free(m); return STREAM_E_FORMAT; }
            int k = tab[s];
            if (k < 0) {
                if (pn == PAT_STORE) { free(pat); free(tab); free(map); free(tok); free(m); return STREAM_E_FORMAT; }
                k = pn++;
                tab[s] = k;
                memcpy(pat[k].v, v, sizeof(v));
            }
            pat[k].n++;
        }
    }
    int sel[PAT_MAX];
    int sn = 0;
    for (;;) {
        int best = -1;
        uint32_t bn = 1;
        for (int i = 0; i < pn; i++) {
            if (!map[i] && pat[i].n > bn) {
                best = i;
                bn = pat[i].n;
            }
        }
        if (best < 0 || sn == PAT_MAX) break;
        sel[sn++] = best;
        map[best] = (uint8_t)sn;
    }
    if (sn < 1) { free(pat); free(tab); free(map); free(tok); free(m); return STREAM_E_FORMAT; }
    size_t matched = 0;
    for (int i = 0; i < sn; i++) matched += pat[sel[i]].n;
    if (matched * 4u < blocks) { free(pat); free(tab); free(map); free(tok); free(m); return STREAM_E_FORMAT; }
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint16_t v[4];
            pat_block(pl, w, h, bx, by, v);
            tok[(size_t)by * bw + bx] = (uint8_t)pat_token_at(pat, tab, map, v);
        }
    }
    model_init(m);
    enc_run_uint(enc, m, 0, (unsigned)(sn - 1));
    for (int i = 0; i < sn; i++)
        for (int j = 0; j < 4; j++) enc_bits(enc, m->mant[0][0], pat[sel[i]].v[j], depth);
    int tb = nbits((unsigned)sn);
    for (size_t bi = 0; bi < blocks && !enc->cut;) {
        unsigned t = tok[bi];
        size_t run = 1;
        while (bi + run < blocks && tok[bi + run] == t && run < 0xffffffu) run++;
        enc_bits(enc, m->mant[1][0], t, tb);
        enc_run_uint(enc, m, 1, (unsigned)(run - 1u));
        if (!t) {
            for (size_t i = 0; i < run; i++) {
                size_t b = bi + i;
                uint32_t bx = (uint32_t)(b % bw), by = (uint32_t)(b / bw);
                uint16_t v[4];
                pat_block(pl, w, h, bx, by, v);
                for (int j = 0; j < 4; j++) enc_bits(enc, m->mant[2][0], v[j], depth);
            }
        }
        bi += run;
    }
    free(pat); free(tab); free(map); free(tok); free(m);
    (void)pos;
    return enc->oom ? STREAM_E_ALLOC : STREAM_OK;
}

static int decode_plane_pattern(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int pos) {
    uint32_t bw = (w + 1u) >> 1, bh = (h + 1u) >> 1;
    size_t blocks = (size_t)bw * bh;
    if (!blocks || blocks > 0xffffffu) return STREAM_E_FORMAT;
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    unsigned sn0 = dec_run_uint(dec, m, 0);
    if (sn0 >= PAT_MAX) { free(m); return STREAM_E_CORRUPT; }
    unsigned sn = sn0 + 1u;
    uint16_t dict[PAT_MAX][4];
    for (unsigned i = 0; i < sn; i++)
        for (int j = 0; j < 4; j++) dict[i][j] = (uint16_t)dec_bits(dec, m->mant[0][0], depth);
    int tb = nbits(sn);
    for (size_t bi = 0; bi < blocks;) {
        unsigned t = dec_bits(dec, m->mant[1][0], tb);
        unsigned run = dec_run_uint(dec, m, 1) + 1u;
        if (t > sn || (size_t)run > blocks - bi) { free(m); return STREAM_E_CORRUPT; }
        if (t) {
            for (unsigned i = 0; i < run; i++) {
                size_t b = bi + i;
                pat_put(pl, w, h, (uint32_t)(b % bw), (uint32_t)(b / bw), dict[t - 1u]);
            }
        } else {
            for (unsigned i = 0; i < run; i++) {
                size_t b = bi + i;
                uint16_t v[4];
                for (int j = 0; j < 4; j++) v[j] = (uint16_t)dec_bits(dec, m->mant[2][0], depth);
                pat_put(pl, w, h, (uint32_t)(b % bw), (uint32_t)(b / bw), v);
            }
        }
        bi += run;
    }
    free(m);
    (void)pos;
    return STREAM_OK;
}

static int decode_plane_xzr(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        tp = malloc(ntiles);
        if (!tp) { free(m); return STREAM_E_ALLOC; }
        for (size_t i = 0; i < ntiles; i++) tp[i] = (uint8_t)dec_tree4(dec, m->predtreex);
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w;
        const uint16_t *up = y ? row - w : row, *up2 = y > 1 ? row - (size_t)w * 2u : up;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w;) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
            int act = iabs(Wv - NWv) + iabs(NWv - Nv) + iabs(Nv - NEv) + ((iabs(Wv - WWv) + iabs(Nv - NNv)) >> 1);
            int ctx = ectx(qctx(act), prevk, prevs, 1);
            if (!dec_bit(dec, &m->nz[ctx])) {
                uint32_t run = 1;
                if (dec_bit(dec, &m->zr[ctx])) run = dec_run_uint(dec, m, ctx) + 2u;
                if (run > w - x) { free(tp); free(m); return STREAM_E_CORRUPT; }
                for (uint32_t r = 0; r < run; r++, x++) {
                    NEIGHBORS();
                    WWv = x > 1 ? row[x - 2] : Wv;
                    NNv = y > 1 ? up2[x] : Nv;
                    pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
                    row[x] = (uint16_t)predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
                }
                prevk = 0;
                prevs = 0;
            } else {
                int k = 0;
                while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
                unsigned u = 0;
                if (k) {
                    u = 1u << (k - 1);
                    for (int i = k - 2; i >= 0; i--) u |= (unsigned)dec_bit(dec, &m->mant[ctx][k][i]) << i;
                }
                if (u >= (unsigned)(M - 1)) { free(tp); free(m); return STREAM_E_CORRUPT; }
                unsigned v = u + 1u;
                int e = unmap_res(v, pos, half, maxv);
                row[x] = (uint16_t)((predictx(pid, Wv, Nv, NWv, NEv, WWv, NNv, maxv) + e) & maxv);
                prevk = nbits(v);
                prevs = (e > 0) - (e < 0);
                x++;
            }
        }
    }
    free(tp); free(m);
    return STREAM_OK;
}

static int decode_plane_zr(Dec *dec, uint16_t *pl, uint32_t w, uint32_t h, int depth, int tlog, int pos, int sc) {
    Model *m = malloc(sizeof *m);
    if (!m) return STREAM_E_ALLOC;
    model_init(m);
    int M = 1 << depth, half = M >> 1, maxv = M - 1;
    uint32_t ntx = 1;
    uint8_t *tp = NULL;
    if (tlog) {
        uint32_t ts = 1u << tlog, nty = (h + ts - 1) >> tlog;
        ntx = (w + ts - 1) >> tlog;
        size_t ntiles = (size_t)ntx * nty;
        tp = malloc(ntiles);
        if (!tp) { free(m); return STREAM_E_ALLOC; }
        for (size_t i = 0; i < ntiles; i++) tp[i] = (uint8_t)dec_tree3(dec, m->predtree);
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = pl + (size_t)y * w; const uint16_t *up = y ? row - w : row;
        int prevk = 0, prevs = 0;
        for (uint32_t x = 0; x < w;) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
            int ctx = ectx(qctx(iabs(Wv-NWv) + iabs(NWv-Nv) + iabs(Nv-NEv)), prevk, prevs, sc);
            if (!dec_bit(dec, &m->nz[ctx])) {
                uint32_t run = 1;
                if (dec_bit(dec, &m->zr[ctx])) run = dec_run_uint(dec, m, ctx) + 2u;
                if (run > w - x) { free(tp); free(m); return STREAM_E_CORRUPT; }
                for (uint32_t r = 0; r < run; r++, x++) {
                    NEIGHBORS();
                    pid = tlog ? tp[(size_t)(y >> tlog) * ntx + (x >> tlog)] : 0;
                    row[x] = (uint16_t)predict(pid, Wv, Nv, NWv, NEv, maxv);
                }
                prevk = 0;
                prevs = 0;
            } else {
                int k = 0;
                while (k < depth && dec_bit(dec, &m->unary[ctx][k])) k++;
                unsigned u = 0;
                if (k) {
                    u = 1u << (k - 1);
                    for (int i = k - 2; i >= 0; i--) u |= (unsigned)dec_bit(dec, &m->mant[ctx][k][i]) << i;
                }
                if (u >= (unsigned)(M - 1)) { free(tp); free(m); return STREAM_E_CORRUPT; }
                unsigned v = u + 1u;
                int e = unmap_res(v, pos, half, maxv);
                row[x] = (uint16_t)((predict(pid, Wv, Nv, NWv, NEv, maxv) + e) & maxv);
                prevk = nbits(v);
                prevs = (e > 0) - (e < 0);
                x++;
            }
        }
    }
    free(tp); free(m);
    return STREAM_OK;
}

static int floor_shift(int value, unsigned shift) {
    /* C right shift rules vary here, use explicit floor rounding for the inverse */
    if (value >= 0) return value >> shift;
    return -(int)(((unsigned)(-value) + (1u << shift) - 1u) >> shift);
}

static QLIC_FORCEINLINE int rg_blend(int r, int g, int t) {
    static const uint8_t weight[18] = {
        8, 16, 24, 40, 48, 56, 64, 12, 20, 28, 18, 22, 26, 19, 21, 23,
        25, 27
    };
    int rw = weight[t - 11];
    return floor_shift(rw * r + (64 - rw) * g, 6);
}

static QLIC_FORCEINLINE int rg_luma_blend(int r, int g, int t) {
    static const uint8_t weight[7] = {0, 16, 20, 22, 24, 32, 22};
    int rw = weight[t - 29];
    return floor_shift(rw * r + (64 - rw) * g, 6);
}

static int rg_luma_lift(int u, int v) {
    return floor_shift(u + v, 2);
}
static void fwd_transform(const uint8_t *pix, size_t npix, int stride, int t, uint16_t *P[3]) {
    size_t s = (size_t)stride;
    for (size_t i = 0; i < npix; i++) {
        size_t o = i * s;
        int R = pix[o], G = pix[o + 1u], B = pix[o + 2u];
        if      (t == 0) { P[0][i] = (uint16_t)R; P[1][i] = (uint16_t)G;           P[2][i] = (uint16_t)B; }
        else if (t == 1) { P[0][i] = (uint16_t)G; P[1][i] = (uint16_t)(R - G + 256); P[2][i] = (uint16_t)(B - G + 256); }
        else if (t == 2) {
            int Co = R - B, tt = B + (Co >> 1), Cg = G - tt, Y = tt + (Cg >> 1);
            P[0][i] = (uint16_t)Y; P[1][i] = (uint16_t)(Co + 256); P[2][i] = (uint16_t)(Cg + 256);
        } else if (t == 3) {
            P[0][i] = (uint16_t)R; P[1][i] = (uint16_t)(G - R + 256); P[2][i] = (uint16_t)(B - R + 256);
        } else if (t == 4) {
            P[0][i] = (uint16_t)B; P[1][i] = (uint16_t)(R - B + 256); P[2][i] = (uint16_t)(G - B + 256);
        } else if (t == 5) {
            P[0][i] = (uint16_t)G; P[1][i] = (uint16_t)(R - G + 256); P[2][i] = (uint16_t)(B - ((R + G) >> 1) + 256);
        } else if (t == 6) {
            P[0][i] = (uint16_t)G; P[1][i] = (uint16_t)(B - G + 256); P[2][i] = (uint16_t)(R - ((B + G) >> 1) + 256);
        } else if (t == 7) {
            P[0][i] = (uint16_t)R; P[1][i] = (uint16_t)(B - R + 256); P[2][i] = (uint16_t)(G - ((R + B) >> 1) + 256);
        } else if (t == 8) {
            P[0][i] = (uint16_t)B; P[1][i] = (uint16_t)(R - B + 256); P[2][i] = (uint16_t)(G - ((R + B) >> 1) + 256);
        } else if (t == 9) {
            P[0][i] = (uint16_t)R; P[1][i] = (uint16_t)(G - R + 256); P[2][i] = (uint16_t)(B - G + 256);
        } else if (t == 10) {
            P[0][i] = (uint16_t)B; P[1][i] = (uint16_t)(G - B + 256); P[2][i] = (uint16_t)(R - G + 256);
        } else if (t <= 28) {
            P[0][i] = (uint16_t)G;
            P[1][i] = (uint16_t)(R - G + 256);
            P[2][i] = (uint16_t)(B - rg_blend(R, G, t) + 256);
        } else if (t <= 34) {
            int u = R - G;
            int v = B - G;
            P[0][i] = (uint16_t)(G + rg_luma_lift(u, v));
            P[1][i] = (uint16_t)(u + 256);
            P[2][i] = (uint16_t)(B - rg_luma_blend(R, G, t) + 256);
        } else {
            P[0][i] = (uint16_t)R;
            P[1][i] = (uint16_t)(G - R + 256);
            P[2][i] = (uint16_t)(B - floor_shift(G + R, 1) + 256);
        }
    }
}
static QLIC_FORCEINLINE void inv_transform_pixel(
    uint16_t *P[3], size_t i, int t, int *R, int *G, int *B) {
    if      (t == 0) { *R = P[0][i]; *G = P[1][i]; *B = P[2][i]; }
    else if (t == 1) { *G = P[0][i]; *R = (int)P[1][i] - 256 + *G; *B = (int)P[2][i] - 256 + *G; }
    else if (t == 2) {
        int Y = P[0][i], Co = (int)P[1][i] - 256, Cg = (int)P[2][i] - 256;
        int tt = Y - (Cg >> 1); *G = Cg + tt; *B = tt - (Co >> 1); *R = *B + Co;
    } else if (t == 3) {
        *R = P[0][i]; *G = (int)P[1][i] - 256 + *R; *B = (int)P[2][i] - 256 + *R;
    } else if (t == 4) {
        *B = P[0][i]; *R = (int)P[1][i] - 256 + *B; *G = (int)P[2][i] - 256 + *B;
    } else if (t == 5) {
        *G = P[0][i]; *R = (int)P[1][i] - 256 + *G; *B = (int)P[2][i] - 256 + ((*R + *G) >> 1);
    } else if (t == 6) {
        *G = P[0][i]; *B = (int)P[1][i] - 256 + *G; *R = (int)P[2][i] - 256 + ((*B + *G) >> 1);
    } else if (t == 7) {
        *R = P[0][i]; *B = (int)P[1][i] - 256 + *R; *G = (int)P[2][i] - 256 + ((*R + *B) >> 1);
    } else if (t == 8) {
        *B = P[0][i]; *R = (int)P[1][i] - 256 + *B; *G = (int)P[2][i] - 256 + ((*R + *B) >> 1);
    } else if (t == 9) {
        *R = P[0][i]; *G = (int)P[1][i] - 256 + *R; *B = (int)P[2][i] - 256 + *G;
    } else if (t == 10) {
        *B = P[0][i]; *G = (int)P[1][i] - 256 + *B; *R = (int)P[2][i] - 256 + *G;
    } else if (t <= 28) {
        *G = P[0][i];
        *R = (int)P[1][i] - 256 + *G;
        *B = (int)P[2][i] - 256 + rg_blend(*R, *G, t);
    } else if (t <= 34) {
        int u = (int)P[1][i] - 256;
        int d = rg_luma_blend(u, 0, t);
        int v = (int)P[2][i] - 256 + d;
        *G = (int)P[0][i] - rg_luma_lift(u, v);
        *R = *G + u;
        *B = *G + v;
    } else {
        *R = P[0][i];
        *G = (int)P[1][i] - 256 + *R;
        *B = (int)P[2][i] - 256 + floor_shift(*G + *R, 1);
    }
}

static void inv_transform(uint16_t *P[3], size_t npix, int stride, int t,
                          uint8_t *pix) {
    size_t s = (size_t)stride;
    for (size_t i = 0; i < npix; i++) {
        int R, G, B;
        inv_transform_pixel(P, i, t, &R, &G, &B);
        size_t o = i * s;
        pix[o] = (uint8_t)R;
        pix[o + 1u] = (uint8_t)G;
        pix[o + 2u] = (uint8_t)B;
    }
}

static void inv_transform_reverse(uint16_t *P[3], size_t npix, int stride,
                                  int t, uint8_t *pix, int opaque) {
    size_t s = (size_t)stride;
    for (size_t i = npix; i-- > 0;) {
        int R, G, B;
        inv_transform_pixel(P, i, t, &R, &G, &B);
        size_t o = i * s;
        pix[o] = (uint8_t)R;
        pix[o + 1u] = (uint8_t)G;
        pix[o + 2u] = (uint8_t)B;
        if (opaque) pix[o + 3u] = 255;
    }
}

static int candidates(int search, int cand[][2]) {
    int n = 0;
    #define ADD(t,L) (cand[n][0] = (t), cand[n][1] = (L), n++)
    if (search <= 0) {
        ADD(2,4);
    } else if (search == 1) {
        static const int ts[] = {9,10,5,6,2,3,1,7,8};
        for (int i = 0; i < 9; i++) ADD(ts[i],4);
    } else {
        ADD(2,0);
        ADD(2,6);
        for (int t = 0; t < 5; t++) {
            ADD(t,4);
            ADD(t,5);
        }
        ADD(9,4);
        ADD(10,4);
        ADD(2,3);
    }
    #undef ADD
    return n;
}

static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static int dims_ok(uint32_t w, uint32_t h, int ch, size_t *npix, size_t *nbytes) {
    if (ch != 1 && ch != 3 && ch != 4) return 0;
    if (!w || !h || w > STREAM_MAX_DIM || h > STREAM_MAX_DIM) return 0;
    uint64_t p = (uint64_t)w * h;
    if (p > STREAM_MAX_PIXELS || p * (uint64_t)ch > SIZE_MAX) return 0;
    if (npix) *npix = (size_t)p;
    if (nbytes) *nbytes = (size_t)p * (size_t)ch;
    return 1;
}

typedef struct {
    int transform, tlog, plane;
    uint8_t *map, *reuse_map, *weighted_map, *weighted_reuse_map, *xzr_map;
    uint64_t weighted_baseline[2], weighted_candidate[2];
} Map37Entry;

typedef struct {
    Map37Entry entries[64];
    int count;
} Map37Cache;

typedef struct {
    uint8_t *map[3];
    int ready;
} GrayMapCache;

typedef struct {
    uint16_t *planes[36];
} TransformPlaneCache;

typedef struct {
    const uint8_t *pix; uint32_t w, h; int ch;
    size_t stride;
    int gray, const_alpha; uint8_t alpha_val;
    int pal_n; uint8_t pal[256][4]; uint8_t *pal_idx;
    uint32_t crc;
    Map37Cache *map37_cache;
    GrayMapCache *gray_map_cache;
    TransformPlaneCache *transform_plane_cache;
    uint8_t map37_penalty[4];
    int map37_override;
    uint64_t xzr_map_mask;
    uint64_t map37_pair_mask;
    uint64_t zero_run_known_mask;
    uint16_t zero_run_rate[36];
} EncCtx;

static int cached_transform_planes(const EncCtx *c, int transform,
                                   uint16_t **planes) {
    /* mode trials read the same transformed values, keep one plane per transform */
    TransformPlaneCache *cache = c->transform_plane_cache;
    if (!cache) return STREAM_E_ARG;
    size_t npix = (size_t)c->w * c->h;
    uint16_t *data = cache->planes[transform];
    if (!data) {
        if (npix > SIZE_MAX / (3u * sizeof(*data))) return STREAM_E_DIM;
        data = malloc(npix * 3u * sizeof(*data));
        if (!data) return STREAM_E_ALLOC;
        uint16_t *dst[3] = {data, data + npix, data + npix * 2u};
        fwd_transform(c->pix, npix, (int)c->stride, transform, dst);
        cache->planes[transform] = data;
    }
    planes[0] = data;
    planes[1] = data + npix;
    planes[2] = data + npix * 2u;
    return STREAM_OK;
}

static void free_transform_plane_cache(TransformPlaneCache *cache) {
    for (int i = 0; i < 36; ++i) free(cache->planes[i]);
}

static int cached_map37(const EncCtx *c, int transform, int tlog, int plane,
                         const uint16_t *samples, int depth, int kind,
                         const uint8_t **out) {
    *out = NULL;
    /* map construction scans the full image, paired trials should reuse it */
    if (!c->map37_cache || !tlog) return STREAM_OK;
    Map37Cache *cache = c->map37_cache;
    int penalty = kind;
    int key_transform = plane == 3 ? -1 : transform;
    int want_xzr = plane == 3
                        ? c->xzr_map_mask != 0
                        : key_transform >= 0 &&
                              (c->xzr_map_mask &
                               (UINT64_C(1) << key_transform)) != 0;
    int want_pair = plane == 3
                         ? c->map37_pair_mask != 0
                         : key_transform >= 0 &&
                               (c->map37_pair_mask &
                                (UINT64_C(1) << key_transform)) != 0;
    if ((tlog == 3 || tlog == 4) && want_pair) {
        Map37Entry *entry3 = NULL;
        Map37Entry *entry4 = NULL;
        for (int i = 0; i < cache->count; ++i) {
            Map37Entry *candidate = &cache->entries[i];
            if (candidate->transform != key_transform ||
                candidate->plane != plane)
                continue;
            if (candidate->tlog == 3) entry3 = candidate;
            else if (candidate->tlog == 4) entry4 = candidate;
        }
        int capacity = (int)(sizeof(cache->entries) / sizeof(cache->entries[0]));
        if (!entry3 && !entry4 && cache->count <= capacity - 2) {
            entry3 = &cache->entries[cache->count++];
            entry4 = &cache->entries[cache->count++];
            memset(entry3, 0, sizeof(*entry3));
            memset(entry4, 0, sizeof(*entry4));
            entry3->transform = entry4->transform = key_transform;
            entry3->plane = entry4->plane = plane;
            entry3->tlog = 3;
            entry4->tlog = 4;
            int err = predictor_map37_pair(
                samples, c->w, c->h, depth, 0, MAP37_REUSE_PENALTY,
                &entry3->map, &entry3->reuse_map, &entry4->map,
                &entry4->reuse_map,
                want_xzr ? &entry4->xzr_map : NULL);
            if (err != STREAM_OK) {
                cache->count -= 2;
                return err;
            }
            Map37Entry *selected = tlog == 3 ? entry3 : entry4;
            *out = penalty ? selected->reuse_map : selected->map;
            return STREAM_OK;
        }
    }
    Map37Entry *entry = NULL;
    for (int i = 0; i < cache->count; i++) {
        Map37Entry *candidate = &cache->entries[i];
        if (candidate->transform == key_transform && candidate->tlog == tlog &&
            candidate->plane == plane) {
            entry = candidate;
            break;
        }
    }
    if (!entry) {
        if (cache->count == (int)(sizeof(cache->entries) / sizeof(cache->entries[0])))
            return STREAM_OK;
        entry = &cache->entries[cache->count++];
        memset(entry, 0, sizeof(*entry));
        entry->transform = key_transform;
        entry->tlog = tlog;
        entry->plane = plane;
    }
    uint8_t **slot = penalty ? &entry->reuse_map : &entry->map;
    if (!*slot) {
        int err;
        uint8_t **xzr_slot = !kind && tlog == 4 && want_xzr &&
                                     !entry->xzr_map
                                 ? &entry->xzr_map
                                 : NULL;
        if (!penalty && !entry->reuse_map) {
            err = predictor_map37(samples, c->w, c->h, depth, tlog, 0,
                                  MAP37_REUSE_PENALTY, &entry->map,
                                  &entry->reuse_map, xzr_slot);
        } else {
            err = predictor_map37(samples, c->w, c->h, depth, tlog, kind, 0,
                                  slot, NULL, xzr_slot);
        }
        if (err != STREAM_OK) return err;
    }
    *out = *slot;
    return STREAM_OK;
}

static int cached_weighted_map37(
    const EncCtx *c, int transform, int tlog, int plane,
    const uint16_t *samples, int depth, int penalty, const uint8_t **out,
    uint64_t *baseline_cost, uint64_t *candidate_cost) {
    *out = NULL;
    if (!c->map37_cache || !tlog) return STREAM_E_ARG;
    Map37Cache *cache = c->map37_cache;
    int key_transform = plane == 3 ? -1 : transform;
    Map37Entry *entry = NULL;
    for (int i = 0; i < cache->count; ++i) {
        Map37Entry *candidate = &cache->entries[i];
        if (candidate->transform == key_transform &&
            candidate->tlog == tlog && candidate->plane == plane) {
            entry = candidate;
            break;
        }
    }
    if (!entry) {
        if (cache->count ==
            (int)(sizeof(cache->entries) / sizeof(cache->entries[0])))
            return STREAM_E_ALLOC;
        entry = &cache->entries[cache->count++];
        memset(entry, 0, sizeof(*entry));
        entry->transform = key_transform;
        entry->tlog = tlog;
        entry->plane = plane;
    }
    int variant = penalty != 0;
    uint8_t **slot =
        variant ? &entry->weighted_reuse_map : &entry->weighted_map;
    if (!*slot) {
        const uint8_t *baseline_map =
            variant ? entry->reuse_map : entry->map;
        if (!baseline_map) {
            int err = cached_map37(
                c, transform, tlog, plane, samples, depth, penalty,
                &baseline_map);
            if (err != STREAM_OK) return err;
        }
        int err = predictor_map37_weighted(
            samples, c->w, c->h, depth, tlog, penalty, baseline_map, slot,
            &entry->weighted_baseline[variant],
            &entry->weighted_candidate[variant]);
        if (err != STREAM_OK) return err;
    }
    *out = *slot;
    if (baseline_cost)
        *baseline_cost = entry->weighted_baseline[variant];
    if (candidate_cost)
        *candidate_cost = entry->weighted_candidate[variant];
    return STREAM_OK;
}

static const uint8_t *cached_xzr_map(const EncCtx *c, int transform,
                                      int tlog, int plane) {
    if (!c->map37_cache || tlog != 4) return NULL;
    int key_transform = plane == 3 ? -1 : transform;
    for (int i = 0; i < c->map37_cache->count; ++i) {
        const Map37Entry *entry = &c->map37_cache->entries[i];
        if (entry->transform == key_transform && entry->tlog == tlog &&
            entry->plane == plane)
            return entry->xzr_map;
    }
    return NULL;
}

static uint8_t predictor_cost_min(const uint64_t *cost, int count) {
    int best = 0;
    for (int i = 1; i < count; ++i)
        if (cost[i] < cost[best]) best = i;
    return (uint8_t)best;
}

static QLIC_NOINLINE int build_gray_map_cache(const EncCtx *c,
                                              const uint16_t *samples,
                                              int depth) {
    GrayMapCache *gray = c->gray_map_cache;
    size_t npix = (size_t)c->w * c->h;
    if (!gray || gray->ready || npix > 1000000u)
        return STREAM_OK;

    uint32_t ntx3 = (c->w + 7u) >> 3;
    uint32_t nty3 = (c->h + 7u) >> 3;
    uint32_t ntx4 = (c->w + 15u) >> 4;
    uint32_t nty4 = (c->h + 15u) >> 4;
    uint32_t ntx5 = (c->w + 31u) >> 5;
    uint32_t nty5 = (c->h + 31u) >> 5;
    size_t ntiles3 = (size_t)ntx3 * nty3;
    size_t ntiles4 = (size_t)ntx4 * nty4;
    size_t ntiles5 = (size_t)ntx5 * nty5;

    uint8_t *map33_3 = malloc(ntiles3);
    uint8_t *map33_4 = malloc(ntiles4);
    uint8_t *map33_5 = malloc(ntiles5);
    uint64_t *cost33_3 =
        calloc(ntiles3 * NPREDX, sizeof(*cost33_3));
    uint64_t *cost33_4 =
        calloc(ntiles4 * NPREDX, sizeof(*cost33_4));
    uint64_t *cost33_5 =
        calloc(ntiles5 * NPREDX, sizeof(*cost33_5));
    if (!map33_3 || !map33_4 || !map33_5 || !cost33_3 || !cost33_4 ||
        !cost33_5) {
        free(map33_3); free(map33_4); free(map33_5);
        free(cost33_3); free(cost33_4); free(cost33_5);
        return STREAM_E_ALLOC;
    }

    model_ensure();
    int half = 1 << (depth - 1);
    int maxv = (1 << depth) - 1;
    uint32_t w = c->w;
    for (uint32_t y = 0; y < c->h; ++y) {
        const uint16_t *row = samples + (size_t)y * c->w;
        const uint16_t *up = y ? row - c->w : row;
        const uint16_t *up2 =
            y > 1 ? row - (size_t)c->w * 2u : up;
        for (uint32_t x = 0; x < c->w; ++x) {
            int Wv, Nv, NWv, NEv;
            NEIGHBORS();
            int WWv = x > 1 ? row[x - 2] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            uint64_t *cost =
                cost33_3 +
                ((size_t)(y >> 3) * ntx3 + (x >> 3)) * NPREDX;
            for (int p = 0; p < NPREDX; ++p) {
                int pr =
                    predicta(p, Wv, Nv, NWv, NEv, WWv, NNv, maxv);
                int e = ((row[x] - pr + half) & maxv) - half;
                unsigned v = (unsigned)iabs(e);
                cost[p] += v ? 2u * predictor_nbits_lut[v] : 1u;
            }
        }
    }
    for (uint32_t ty3 = 0; ty3 < nty3; ++ty3) {
        for (uint32_t tx3 = 0; tx3 < ntx3; ++tx3) {
            size_t i3 = (size_t)ty3 * ntx3 + tx3;
            uint64_t *cost = cost33_3 + i3 * NPREDX;
            map33_3[i3] = predictor_cost_min(cost, NPREDX);
            uint64_t *parent4 =
                cost33_4 +
                ((size_t)(ty3 >> 1) * ntx4 + (tx3 >> 1)) * NPREDX;
            uint64_t *parent5 =
                cost33_5 +
                ((size_t)(ty3 >> 2) * ntx5 + (tx3 >> 2)) * NPREDX;
            /* aligned larger tiles are exact sums of the 8 by 8 costs */
            for (int p = 0; p < NPREDX; ++p) {
                parent4[p] += cost[p];
                parent5[p] += cost[p];
            }
        }
    }
    for (size_t i4 = 0; i4 < ntiles4; ++i4)
        map33_4[i4] =
            predictor_cost_min(cost33_4 + i4 * NPREDX, NPREDX);
    for (size_t i5 = 0; i5 < ntiles5; ++i5)
        map33_5[i5] =
            predictor_cost_min(cost33_5 + i5 * NPREDX, NPREDX);
    free(cost33_3);
    free(cost33_4);
    free(cost33_5);
    gray->map[0] = map33_3;
    gray->map[1] = map33_4;
    gray->map[2] = map33_5;
    gray->ready = 1;
    return STREAM_OK;
}

static void free_gray_map_cache(GrayMapCache *cache) {
    for (int i = 0; i < 3; ++i) free(cache->map[i]);
    memset(cache, 0, sizeof(*cache));
}

static void free_map37_cache(Map37Cache *cache) {
    for (int i = 0; i < cache->count; i++) {
        free(cache->entries[i].map);
        free(cache->entries[i].reuse_map);
        free(cache->entries[i].weighted_map);
        free(cache->entries[i].weighted_reuse_map);
        free(cache->entries[i].xzr_map);
    }
    cache->count = 0;
}

static int split37_enc_one(const uint16_t *pl, uint32_t w, uint32_t h, int depth,
                           int tlog, size_t limit, uint8_t **out,
                           size_t *outn) {
    Enc e;
    enc_init(&e, ADAPT_DEFAULT);
    e.max = limit;
    int err = encode_plane37(&e, pl, w, h, depth, tlog, 0, 0, NULL, NULL,
                             NULL, 0);
    enc_flush(&e);
    if (err == STREAM_OK && e.cut) err = STREAM_E_FORMAT;
    if (err == STREAM_OK && e.oom) err = STREAM_E_ALLOC;
    if (err != STREAM_OK) {
        free(e.buf);
        return err;
    }
    *out = e.buf;
    *outn = e.len;
    return STREAM_OK;
}

static int split37_enc_append(const uint16_t *pl, uint32_t w, uint32_t h,
                              int depth, int tlog, size_t limit, size_t *used,
                              uint8_t **out, size_t *outn) {
    size_t remaining = limit == SIZE_MAX
                           ? SIZE_MAX
                           : *used < limit ? limit - *used : 0;
    int err =
        split37_enc_one(pl, w, h, depth, tlog, remaining, out, outn);
    if (err != STREAM_OK) return err;
    if (*outn > SIZE_MAX - *used) {
        free(*out);
        *out = NULL;
        *outn = 0;
        return STREAM_E_DIM;
    }
    *used += *outn;
    return STREAM_OK;
}

static int split37_stream_encode(const EncCtx *c, int t, int tlog, size_t limit,
                                 uint8_t **out, size_t *outn) {
    /* band boundaries trade some context for streams that can run independently */
    size_t npix = (size_t)c->w * c->h;
    size_t sch = c->stride;
    int planes = c->ch == 4 && !c->const_alpha ? 4 : (c->ch == 1 || c->gray ? 1 : 3);
    uint32_t bands = (c->h + SPLIT_BAND_H - 1u) / SPLIT_BAND_H;
    size_t streams = (size_t)planes * bands;
    if (streams > (SIZE_MAX - STREAM_HDR) / 4u) return STREAM_E_DIM;
    size_t meta = streams * 4u;
    size_t used = STREAM_HDR + meta;
    uint8_t **chunk = calloc(streams, sizeof(*chunk));
    size_t *clen = calloc(streams, sizeof(*clen));
    if (!chunk || !clen) {
        free(chunk);
        free(clen);
        return STREAM_E_ALLOC;
    }
    int err = STREAM_OK;
    int planes_cached = c->transform_plane_cache && c->ch == 3 && !c->gray;
    uint16_t *cached_planes[3] = {0};
    uint16_t *P = NULL;
    if (planes_cached) {
        err = cached_transform_planes(c, t, cached_planes);
        if (err == STREAM_OK) P = cached_planes[0];
    } else {
        P = malloc(npix * sizeof(*P));
        if (!P) err = STREAM_E_ALLOC;
    }
    if (err != STREAM_OK) {
        free(chunk);
        free(clen);
        return err;
    }
    if (c->ch == 1 || c->gray) {
        for (size_t i = 0; i < npix; i++) P[i] = c->pix[i * sch];
        for (uint32_t by = 0; by < bands && err == STREAM_OK; by++) {
            uint32_t y0 = by * SPLIT_BAND_H;
            uint32_t bh = c->h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : c->h - y0;
            size_t si = by;
            err = split37_enc_append(
                P + (size_t)y0 * c->w, c->w, bh, 8, tlog, limit, &used,
                &chunk[si], &clen[si]);
        }
    } else {
        uint16_t *Q =
            planes_cached ? cached_planes[1] : malloc(npix * sizeof(*Q));
        uint16_t *R =
            planes_cached ? cached_planes[2] : malloc(npix * sizeof(*R));
        if (!Q || !R) {
            if (!planes_cached) {
                free(Q);
                free(R);
                free(P);
            }
            free(chunk);
            free(clen);
            return STREAM_E_ALLOC;
        }
        uint16_t *pls[3] = {P, Q, R};
        int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
        if (!planes_cached)
            fwd_transform(c->pix, npix, (int)c->stride, t, pls);
        for (int p = 0; p < 3 && err == STREAM_OK; p++) {
            for (uint32_t by = 0; by < bands && err == STREAM_OK; by++) {
                uint32_t y0 = by * SPLIT_BAND_H;
                uint32_t bh = c->h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : c->h - y0;
                size_t si = (size_t)p * bands + by;
                err = split37_enc_append(
                    pls[p] + (size_t)y0 * c->w, c->w, bh, depth[p], tlog,
                    limit, &used, &chunk[si], &clen[si]);
            }
        }
        if (!planes_cached) {
            free(Q);
            free(R);
        }
    }
    if (err == STREAM_OK && c->ch == 4 && !c->const_alpha) {
        for (size_t i = 0; i < npix; i++) P[i] = c->pix[i * sch + 3u];
        for (uint32_t by = 0; by < bands && err == STREAM_OK; by++) {
            uint32_t y0 = by * SPLIT_BAND_H;
            uint32_t bh = c->h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : c->h - y0;
            size_t si = (size_t)3 * bands + by;
            err = split37_enc_append(
                P + (size_t)y0 * c->w, c->w, bh, 8, tlog, limit, &used,
                &chunk[si], &clen[si]);
        }
    }
    if (!planes_cached) free(P);
    if (err != STREAM_OK) {
        for (size_t i = 0; i < streams; i++) free(chunk[i]);
        free(chunk);
        free(clen);
        return err;
    }
    size_t payload = meta;
    for (size_t i = 0; i < streams; i++) {
        if (clen[i] > 0xFFFFFFFFu || payload > SIZE_MAX - clen[i]) {
            for (size_t j = 0; j < streams; j++) free(chunk[j]);
            free(chunk);
            free(clen);
            return STREAM_E_DIM;
        }
        payload += clen[i];
    }
    if (payload > 0xFFFFFFFFu || payload > limit - (limit < STREAM_HDR ? 0 : STREAM_HDR)) {
        for (size_t i = 0; i < streams; i++) free(chunk[i]);
        free(chunk);
        free(clen);
        return STREAM_E_FORMAT;
    }
    size_t total = STREAM_HDR + payload;
    uint8_t *f = malloc(total);
    if (!f) {
        for (size_t i = 0; i < streams; i++) free(chunk[i]);
        free(chunk);
        free(clen);
        return STREAM_E_ALLOC;
    }
    int flags = 0;
    if (c->gray) flags |= 1;
    if (c->ch == 4 && c->const_alpha) flags |= 2;
    memcpy(f, "QST1", 4);
    put32(f + 4, c->w);
    put32(f + 8, c->h);
    f[12] = (uint8_t)c->ch;
    f[13] = (uint8_t)flags;
    f[14] = 42;
    f[15] = (uint8_t)t;
    f[16] = (uint8_t)tlog;
    f[17] = (uint8_t)((flags & 2) ? c->alpha_val : 0);
    put32(f + 18, c->crc);
    put32(f + 22, (uint32_t)payload);
    put32(f + 26, 0);
    size_t off = STREAM_HDR;
    for (size_t i = 0; i < streams; i++) put32(f + off + i * 4u, (uint32_t)clen[i]);
    off += meta;
    for (size_t i = 0; i < streams; i++) {
        memcpy(f + off, chunk[i], clen[i]);
        off += clen[i];
        free(chunk[i]);
    }
    free(chunk);
    free(clen);
    put32(f + 26, container_crc32(f, total));
    *out = f;
    *outn = total;
    return STREAM_OK;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t *plane;
    uint32_t w, h;
    int depth, tlog, err;
} SplitDecTask;

static void split37_dec_one(SplitDecTask *t) {
    Dec d;
    dec_init(&d, t->data, t->size, ADAPT_DEFAULT);
    t->err = decode_plane37(&d, t->plane, t->w, t->h, t->depth, t->tlog, 0, 0,
                            NULL, NULL);
    if (t->err == STREAM_OK && d.truncated) t->err = STREAM_E_CORRUPT;
}

typedef struct {
    const uint8_t *data[4];
    size_t size[4];
    uint8_t *pix;
    uint32_t w, h;
    int ch, t, tlog, calpha, err;
    uint8_t aval;
} SplitBandTask;

static void split37_dec_band(SplitBandTask *b) {
    size_t npix = (size_t)b->w * b->h;
    size_t count = (b->ch == 4 && !b->calpha) ? 4u : 3u;
    if (npix > SIZE_MAX / (sizeof(uint16_t) * count)) {
        b->err = STREAM_E_DIM;
        return;
    }
    uint16_t *buf = malloc(npix * sizeof(*buf) * count);
    if (!buf) {
        b->err = STREAM_E_ALLOC;
        return;
    }
    uint16_t *pl[3] = {buf, buf + npix, buf + npix * 2u};
    int depth[3] = {8, b->t ? 9 : 8, b->t ? 9 : 8};
    b->err = STREAM_OK;
    for (int p = 0; p < 3 && b->err == STREAM_OK; p++) {
        SplitDecTask t;
        memset(&t, 0, sizeof(t));
        t.data = b->data[p];
        t.size = b->size[p];
        t.plane = pl[p];
        t.w = b->w;
        t.h = b->h;
        t.depth = depth[p];
        t.tlog = b->tlog;
        split37_dec_one(&t);
        b->err = t.err;
    }
    if (b->err == STREAM_OK) {
        inv_transform(pl, npix, b->ch, b->t, b->pix);
        if (b->ch == 4) {
            if (b->calpha) {
                for (size_t i = 0; i < npix; i++) b->pix[i * 4u + 3u] = b->aval;
            } else {
                uint16_t *a = buf + npix * 3u;
                SplitDecTask t;
                memset(&t, 0, sizeof(t));
                t.data = b->data[3];
                t.size = b->size[3];
                t.plane = a;
                t.w = b->w;
                t.h = b->h;
                t.depth = 8;
                split37_dec_one(&t);
                b->err = t.err;
                if (b->err == STREAM_OK)
                    for (size_t i = 0; i < npix; i++) b->pix[i * 4u + 3u] = (uint8_t)a[i];
            }
        }
    }
    free(buf);
}

static void split37_dec_item(void *context, unsigned index) {
    split37_dec_one(&((SplitDecTask *)context)[index]);
}

static void split37_band_item(void *context, unsigned index) {
    split37_dec_band(&((SplitBandTask *)context)[index]);
}

static int split37_decode_tasks(SplitDecTask *tasks, int n, size_t npix) {
    if (n > 1 && npix >= 262144u && stream_threads > 1u)
        qlic_parallel_for((unsigned)n, stream_threads, split37_dec_item, tasks);
    else
        for (int i = 0; i < n; i++) split37_dec_one(&tasks[i]);
    for (int i = 0; i < n; i++) if (tasks[i].err != STREAM_OK) return tasks[i].err;
    return STREAM_OK;
}

static int split37_decode_bands(SplitBandTask *tasks, int n, size_t npix) {
    if (n > 1 && npix >= 262144u && stream_threads > 1u)
        qlic_parallel_for((unsigned)n, stream_threads, split37_band_item, tasks);
    else
        for (int i = 0; i < n; i++) split37_dec_band(&tasks[i]);
    for (int i = 0; i < n; i++) if (tasks[i].err != STREAM_OK) return tasks[i].err;
    return STREAM_OK;
}

static int split37_stream_decode(const uint8_t *payload, size_t plen, uint32_t w,
                                 uint32_t h, int ch, int gray, int calpha,
                                 uint8_t aval, int t, int tlog, uint32_t crc,
                                 uint8_t **pixout, uint32_t *pw, uint32_t *ph,
                                 int *pch) {
    size_t npix = 0, nbytes = 0;
    if (!dims_ok(w, h, ch, &npix, &nbytes)) return STREAM_E_DIM;
    int planes = ch == 4 && !calpha ? 4 : (ch == 1 || gray ? 1 : 3);
    uint32_t bands = (h + SPLIT_BAND_H - 1u) / SPLIT_BAND_H;
    size_t streams = (size_t)planes * bands;
    size_t meta = streams * 4u;
    if (plen < meta) return STREAM_E_CORRUPT;
    size_t off = meta;
    SplitDecTask *tasks = calloc(streams, sizeof(*tasks));
    if (!tasks) return STREAM_E_ALLOC;
    for (size_t i = 0; i < streams; i++) {
        uint32_t n = get32(payload + i * 4u);
        if (off > plen || (size_t)n > plen - off) {
            free(tasks);
            return STREAM_E_CORRUPT;
        }
        tasks[i].data = payload + off;
        tasks[i].size = n;
        tasks[i].w = w;
        tasks[i].tlog = tlog;
        off += n;
    }
    if (off != plen) {
        free(tasks);
        return STREAM_E_CORRUPT;
    }
    uint8_t *pix = malloc(nbytes);
    uint16_t *P = NULL;
    if (!pix) {
        free(pix);
        free(tasks);
        return STREAM_E_ALLOC;
    }
    int err = STREAM_OK;
    size_t sch = (size_t)ch;
    if (ch == 1 || gray) {
        P = malloc(npix * sizeof(*P));
        if (!P) {
            free(pix);
            free(tasks);
            return STREAM_E_ALLOC;
        }
        for (uint32_t by = 0; by < bands; by++) {
            uint32_t y0 = by * SPLIT_BAND_H;
            uint32_t bh = h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : h - y0;
            tasks[by].plane = P + (size_t)y0 * w;
            tasks[by].h = bh;
            tasks[by].depth = 8;
        }
        err = split37_decode_tasks(tasks, (int)bands, npix);
        if (err == STREAM_OK) {
            for (size_t i = 0; i < npix; i++) {
                uint8_t v = (uint8_t)P[i];
                for (int k = 0; k < (ch < 3 ? 1 : 3); k++) pix[i * sch + (size_t)k] = v;
            }
        }
    } else if (ch == 3) {
        P = malloc(npix * sizeof(*P));
        uint16_t *Q = malloc(npix * sizeof(*Q));
        uint16_t *R = malloc(npix * sizeof(*R));
        if (!P || !Q || !R) {
            free(P);
            free(Q);
            free(R);
            free(pix);
            free(tasks);
            return STREAM_E_ALLOC;
        }
        uint16_t *pls[3] = {P, Q, R};
        int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
        for (int i = 0; i < 3; i++) {
            for (uint32_t by = 0; by < bands; by++) {
                uint32_t y0 = by * SPLIT_BAND_H;
                uint32_t bh = h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : h - y0;
                size_t si = (size_t)i * bands + by;
                tasks[si].plane = pls[i] + (size_t)y0 * w;
                tasks[si].h = bh;
                tasks[si].depth = depth[i];
            }
        }
        err = split37_decode_tasks(tasks, (int)((size_t)3 * bands), npix);
        if (err == STREAM_OK) inv_transform(pls, npix, ch, t, pix);
        free(Q);
        free(R);
    } else if (ch == 4 && !calpha) {
        P = malloc(npix * sizeof(*P));
        uint16_t *Q = malloc(npix * sizeof(*Q));
        uint16_t *R = malloc(npix * sizeof(*R));
        if (!P || !Q || !R) {
            free(P);
            free(Q);
            free(R);
            free(pix);
            free(tasks);
            return STREAM_E_ALLOC;
        }
        uint16_t *pls[3] = {P, Q, R};
        int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
        for (int i = 0; i < 3; i++) {
            for (uint32_t by = 0; by < bands; by++) {
                uint32_t y0 = by * SPLIT_BAND_H;
                uint32_t bh = h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : h - y0;
                size_t si = (size_t)i * bands + by;
                tasks[si].plane = pls[i] + (size_t)y0 * w;
                tasks[si].h = bh;
                tasks[si].depth = depth[i];
            }
        }
        err = split37_decode_tasks(tasks, (int)((size_t)3 * bands), npix);
        if (err == STREAM_OK) inv_transform(pls, npix, ch, t, pix);
        if (err == STREAM_OK) {
            SplitDecTask *alpha = tasks + (size_t)3 * bands;
            for (uint32_t by = 0; by < bands; by++) {
                uint32_t y0 = by * SPLIT_BAND_H;
                uint32_t bh = h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : h - y0;
                alpha[by].plane = P + (size_t)y0 * w;
                alpha[by].h = bh;
                alpha[by].depth = 8;
            }
            err = split37_decode_tasks(alpha, (int)bands, npix);
            if (err == STREAM_OK)
                for (size_t i = 0; i < npix; i++) pix[i * 4u + 3] = (uint8_t)P[i];
        }
        free(Q);
        free(R);
    } else {
        SplitBandTask *bands2 = calloc(bands, sizeof(*bands2));
        if (!bands2) {
            free(pix);
            free(tasks);
            return STREAM_E_ALLOC;
        }
        for (uint32_t by = 0; by < bands; by++) {
            uint32_t y0 = by * SPLIT_BAND_H;
            uint32_t bh = h - y0 > SPLIT_BAND_H ? SPLIT_BAND_H : h - y0;
            bands2[by].pix = pix + (size_t)y0 * w * sch;
            bands2[by].w = w;
            bands2[by].h = bh;
            bands2[by].ch = ch;
            bands2[by].t = t;
            bands2[by].tlog = tlog;
            bands2[by].calpha = calpha;
            bands2[by].aval = aval;
            for (int p = 0; p < 3; p++) {
                size_t si = (size_t)p * bands + by;
                bands2[by].data[p] = tasks[si].data;
                bands2[by].size[p] = tasks[si].size;
            }
            if (ch == 4 && !calpha) {
                size_t si = (size_t)3 * bands + by;
                bands2[by].data[3] = tasks[si].data;
                bands2[by].size[3] = tasks[si].size;
            }
        }
        err = split37_decode_bands(bands2, (int)bands, npix);
        free(bands2);
    }
    free(P);
    free(tasks);
    if (err == STREAM_OK && stream_crc32(pix, nbytes) != crc) err = STREAM_E_CORRUPT;
    if (err != STREAM_OK) {
        free(pix);
        return err;
    }
    *pixout = pix;
    *pw = w;
    *ph = h;
    *pch = ch;
    return STREAM_OK;
}

static void transform_px(const uint8_t *p, int t, int v[3]) {
    int R = p[0], G = p[1], B = p[2];
    if      (t == 0) { v[0] = R; v[1] = G; v[2] = B; }
    else if (t == 1) { v[0] = G; v[1] = R - G + 256; v[2] = B - G + 256; }
    else if (t == 2) {
        int Co = R - B, tt = B + (Co >> 1), Cg = G - tt, Y = tt + (Cg >> 1);
        v[0] = Y; v[1] = Co + 256; v[2] = Cg + 256;
    } else if (t == 3) {
        v[0] = R; v[1] = G - R + 256; v[2] = B - R + 256;
    } else if (t == 4) {
        v[0] = B; v[1] = R - B + 256; v[2] = G - B + 256;
    } else if (t == 5) {
        v[0] = G; v[1] = R - G + 256; v[2] = B - ((R + G) >> 1) + 256;
    } else if (t == 6) {
        v[0] = G; v[1] = B - G + 256; v[2] = R - ((B + G) >> 1) + 256;
    } else if (t == 7) {
        v[0] = R; v[1] = B - R + 256; v[2] = G - ((R + B) >> 1) + 256;
    } else if (t == 8) {
        v[0] = B; v[1] = R - B + 256; v[2] = G - ((R + B) >> 1) + 256;
    } else if (t == 9) {
        v[0] = R; v[1] = G - R + 256; v[2] = B - G + 256;
    } else if (t == 10) {
        v[0] = B; v[1] = G - B + 256; v[2] = R - G + 256;
    } else if (t <= 28) {
        v[0] = G;
        v[1] = R - G + 256;
        v[2] = B - rg_blend(R, G, t) + 256;
    } else if (t <= 34) {
        int u = R - G;
        int b = B - G;
        v[0] = G + rg_luma_lift(u, b);
        v[1] = u + 256;
        v[2] = B - rg_luma_blend(R, G, t) + 256;
    } else {
        v[0] = R;
        v[1] = G - R + 256;
        v[2] = B - floor_shift(G + R, 1) + 256;
    }
}

static unsigned zero_run_rate_for(EncCtx *c, int t) {
    uint64_t bit = UINT64_C(1) << t;
    if (c->zero_run_known_mask & bit)
        return c->zero_run_rate[t];
    uint32_t xs = c->w > 512u ? (c->w + 511u) / 512u : 1u;
    uint32_t ys = c->h > 128u ? (c->h + 127u) / 128u : 1u;
    uint64_t zero = 0, total = 0;
    int stride = (int)c->stride;
    int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
    for (uint32_t y = 0; y < c->h; y += ys) {
        for (uint32_t x = 0; x < c->w; x += xs) {
            int C[3], W[3], N[3], NW[3], NE[3];
            const uint8_t *p = c->pix + ((size_t)y * c->w + x) * (size_t)stride;
            const uint8_t *wp = x ? p - stride : p;
            const uint8_t *np = y ? p - (size_t)c->w * (size_t)stride : wp;
            const uint8_t *nwp = (x && y) ? np - stride : np;
            const uint8_t *nep = (y && x + 1u < c->w) ? np + stride : np;
            transform_px(p, t, C);
            transform_px(wp, t, W);
            transform_px(np, t, N);
            transform_px(nwp, t, NW);
            transform_px(nep, t, NE);
            for (int k = 0; k < 3; k++) {
                int maxv = (1 << depth[k]) - 1;
                int half = 1 << (depth[k] - 1);
                int wv = x ? W[k] : (y ? N[k] : half);
                int nv = y ? N[k] : wv;
                int nwv = (x && y) ? NW[k] : nv;
                int nev = (y && x + 1u < c->w) ? NE[k] : nv;
                int pr = predict(0, wv, nv, nwv, nev, maxv);
                int e = ((C[k] - pr + half) & maxv) - half;
                zero += e == 0;
                total++;
            }
        }
    }
    unsigned rate = total
                        ? (unsigned)((zero * 10000u) / total)
                        : 0u;
    c->zero_run_known_mask |= bit;
    c->zero_run_rate[t] = (uint16_t)rate;
    return rate;
}

static int zero_run_candidate_for(EncCtx *c, int t) {
    return zero_run_rate_for(c, t) >= 2500u;
}

static int mapfree_candidate_for(const EncCtx *c, int transform,
                                 int tile_log) {
    if (!c->map37_cache || !tile_log) return 0;
    uint32_t tiles_x = (c->w + (1u << tile_log) - 1u) >> tile_log;
    const uint8_t *maps[3] = {0};
    for (int plane = 0; plane < 3; ++plane) {
        for (int index = 0; index < c->map37_cache->count; ++index) {
            const Map37Entry *entry = &c->map37_cache->entries[index];
            if (entry->transform == transform && entry->tlog == tile_log &&
                entry->plane == plane) {
                maps[plane] = entry->reuse_map;
                break;
            }
        }
        if (!maps[plane]) return 0;
    }
    uint64_t fixed_cost = 0;
    uint64_t mapped_cost = 0;
    uint32_t xs = c->w > 192u ? (c->w + 191u) / 192u : 1u;
    uint32_t ys = c->h > 128u ? (c->h + 127u) / 128u : 1u;
    size_t row_stride = (size_t)c->w * c->stride;
    for (uint32_t y = 0; y < c->h; y += ys) {
        for (uint32_t x = 0; x < c->w; x += xs) {
            const uint8_t *pixel =
                c->pix + ((size_t)y * c->w + x) * c->stride;
            const uint8_t *west = x ? pixel - c->stride : pixel;
            const uint8_t *north = y ? pixel - row_stride : west;
            const uint8_t *northwest = x && y ? north - c->stride : north;
            const uint8_t *northeast =
                y && x + 1u < c->w ? north + c->stride : north;
            const uint8_t *west2 = x > 1u ? west - c->stride : west;
            const uint8_t *north2 = y > 1u ? north - row_stride : north;
            int value[3], wv[3], nv[3], nwv[3], nev[3], ww[3], nn[3];
            transform_px(pixel, transform, value);
            transform_px(west, transform, wv);
            transform_px(north, transform, nv);
            transform_px(northwest, transform, nwv);
            transform_px(northeast, transform, nev);
            transform_px(west2, transform, ww);
            transform_px(north2, transform, nn);
            size_t tile = (size_t)(y >> tile_log) * tiles_x +
                          (x >> tile_log);
            for (int plane = 0; plane < 3; ++plane) {
                int maximum = transform && plane ? 511 : 255;
                int half = (maximum + 1) >> 1;
                int fixed = predict(0, wv[plane], nv[plane], nwv[plane],
                                    nev[plane], maximum);
                int mapped = predicta_impl(
                    maps[plane][tile], wv[plane], nv[plane], nwv[plane],
                    nev[plane], ww[plane], nn[plane], maximum);
                int fixed_error =
                    ((value[plane] - fixed + half) & maximum) - half;
                int mapped_error =
                    ((value[plane] - mapped + half) & maximum) - half;
                fixed_cost += predictor_cost_lut[iabs(fixed_error)];
                mapped_cost += predictor_cost_lut[iabs(mapped_error)];
            }
        }
    }
    return fixed_cost <= mapped_cost + mapped_cost / 50u;
}

static int gray_unique_count(const EncCtx *c, int limit) {
    uint64_t seen[4] = {0};
    int count = 0;
    size_t npix = (size_t)c->w * c->h;
    for (size_t i = 0; i < npix; ++i) {
        unsigned value = c->pix[i * c->stride];
        uint64_t bit = UINT64_C(1) << (value & 63u);
        uint64_t *word = &seen[value >> 6];
        if (!(*word & bit)) {
            *word |= bit;
            if (++count > limit) return count;
        }
    }
    return count;
}

static const uint8_t scored_transforms[36] = {
    2, 9, 10, 5, 6, 3, 1, 7, 8, 0, 4,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35
};
static void ranked_transform_scores(const EncCtx *c, uint64_t cost[36],
                                    int include_reversible);

static int fast_transform_for(const EncCtx *c, int *ordinary_out,
                              int *alternate_out, int *alternate_close,
                              int *dense_out) {
    uint64_t cost2 = 0, best_cost = UINT64_MAX;
    uint64_t cost[36];
    int best = 2;
    ranked_transform_scores(c, cost, 1);
    for (size_t i = 0;
         i < sizeof(scored_transforms) / sizeof(scored_transforms[0]); i++) {
        int transform = scored_transforms[i];
        uint64_t score = cost[transform];
        if (transform == 2) cost2 = score;
        if (score < best_cost) {
            best_cost = score;
            best = transform;
        }
    }
    int ordinary = scored_transforms[0];
    for (int i = 1; i < 11; ++i)
        if (cost[scored_transforms[i]] < cost[ordinary])
            ordinary = scored_transforms[i];
    int reversible = scored_transforms[11];
    for (int i = 12; i < 36; ++i)
        if (cost[scored_transforms[i]] < cost[reversible])
            reversible = scored_transforms[i];
    if (ordinary_out) *ordinary_out = ordinary;
    if (best != 2 && cost2 && best_cost * 200u >= cost2 * 199u) best = 2;
    int alternate = best <= 10 ? reversible : ordinary;
    if (alternate_out) *alternate_out = alternate;
    if (alternate_close)
        *alternate_close =
            cost[alternate] <= cost[best] + cost[best] / 10u;
    if (dense_out) {
        uint32_t xs = c->w > 192u ? (c->w + 191u) / 192u : 1u;
        uint32_t ys = c->h > 128u ? (c->h + 127u) / 128u : 1u;
        uint64_t samples =
            (uint64_t)((c->w + xs - 1u) / xs) *
            ((c->h + ys - 1u) / ys);
        *dense_out = best_cost >= samples * 6u;
    }
    return best;
}

static int ranked_transforms_for(const EncCtx *c, int cand[][2], int maxn) {
    uint64_t transform_cost[36];
    ranked_transform_scores(c, transform_cost, 1);
    int ordinary = scored_transforms[0];
    for (int i = 1; i < 11; ++i)
        if (transform_cost[scored_transforms[i]] <
            transform_cost[ordinary])
            ordinary = scored_transforms[i];
    int reversible = scored_transforms[11];
    for (int i = 12; i < 36; ++i)
        if (transform_cost[scored_transforms[i]] <
            transform_cost[reversible])
            reversible = scored_transforms[i];
    int selected[2] = {ordinary, reversible};
    if (transform_cost[selected[1]] < transform_cost[selected[0]]) {
        int swap = selected[0];
        selected[0] = selected[1];
        selected[1] = swap;
    }
    if (maxn > 2) maxn = 2;
    for (int i = 0; i < maxn; i++) {
        cand[i][0] = selected[i];
        cand[i][1] = 4;
    }
    return maxn;
}

static QLIC_FORCEINLINE uint32_t transform_component_cost(
    const int v[5], int depth, int has_w, int has_n, int has_ne) {
    int maxv = (1 << depth) - 1;
    int half = 1 << (depth - 1);
    int W = has_w ? v[1] : has_n ? v[2] : half;
    int N = has_n ? v[2] : W;
    int NW = has_w && has_n ? v[3] : N;
    int NE = has_ne ? v[4] : N;
    int pr = predict(0, W, N, NW, NE, maxv);
    int e = ((v[0] - pr + half) & maxv) - half;
    unsigned a = (unsigned)iabs(e);
    return predictor_cost_lut[a];
}

static void ranked_transform_scores(const EncCtx *c, uint64_t cost[36],
                                    int include_reversible) {
    /* sampling keeps transform search cheap, full encoding still decides final size */
    model_ensure();
    memset(cost, 0, 36u * sizeof(*cost));
    uint32_t xs = c->w > 192u ? (c->w + 191u) / 192u : 1u;
    uint32_t ys = c->h > 128u ? (c->h + 127u) / 128u : 1u;
    size_t stride = c->stride;
    size_t row_stride = (size_t)c->w * stride;
    for (uint32_t y = 0; y < c->h; y += ys) {
        for (uint32_t x = 0; x < c->w; x += xs) {
            const uint8_t *p =
                c->pix + ((size_t)y * c->w + x) * stride;
            const uint8_t *wp = x ? p - stride : p;
            const uint8_t *np = y ? p - row_stride : wp;
            const uint8_t *nwp = x && y ? np - stride : np;
            const uint8_t *nep =
                y && x + 1u < c->w ? np + stride : np;
            const uint8_t *source[5] = {p, wp, np, nwp, nep};
            int red[5], green[5], blue[5], rg[5], bg[5], gr[5], br[5];
            int rb[5], gb[5], ycg[5], co[5], cg[5], luma[5], third[5];
            int b_rg[5], r_bg[5], g_rb[5];
            for (int i = 0; i < 5; ++i) {
                int r = source[i][0];
                int g = source[i][1];
                int b = source[i][2];
                int cov = r - b;
                int tmp = b + (cov >> 1);
                int cgv = g - tmp;
                int u = r - g;
                int v = b - g;
                red[i] = r;
                green[i] = g;
                blue[i] = b;
                rg[i] = r - g + 256;
                bg[i] = b - g + 256;
                gr[i] = g - r + 256;
                br[i] = b - r + 256;
                rb[i] = r - b + 256;
                gb[i] = g - b + 256;
                ycg[i] = tmp + (cgv >> 1);
                co[i] = cov + 256;
                cg[i] = cgv + 256;
                b_rg[i] = b - ((r + g) >> 1) + 256;
                r_bg[i] = r - ((b + g) >> 1) + 256;
                g_rb[i] = g - ((r + b) >> 1) + 256;
                if (include_reversible)
                    luma[i] = g + rg_luma_lift(u, v);
            }
            int has_w = x != 0;
            int has_n = y != 0;
            int has_ne = y != 0 && x + 1u < c->w;
            uint32_t sr = transform_component_cost(
                red, 8, has_w, has_n, has_ne);
            uint32_t sg = transform_component_cost(
                green, 8, has_w, has_n, has_ne);
            uint32_t sb = transform_component_cost(
                blue, 8, has_w, has_n, has_ne);
            uint32_t srg = transform_component_cost(
                rg, 9, has_w, has_n, has_ne);
            uint32_t sbg = transform_component_cost(
                bg, 9, has_w, has_n, has_ne);
            uint32_t sgr = transform_component_cost(
                gr, 9, has_w, has_n, has_ne);
            uint32_t sbr = transform_component_cost(
                br, 9, has_w, has_n, has_ne);
            uint32_t srb = transform_component_cost(
                rb, 9, has_w, has_n, has_ne);
            uint32_t sgb = transform_component_cost(
                gb, 9, has_w, has_n, has_ne);
            uint32_t sycg = transform_component_cost(
                ycg, 8, has_w, has_n, has_ne);
            uint32_t sco = transform_component_cost(
                co, 9, has_w, has_n, has_ne);
            uint32_t scg = transform_component_cost(
                cg, 9, has_w, has_n, has_ne);
            uint32_t sb_rg = transform_component_cost(
                b_rg, 9, has_w, has_n, has_ne);
            uint32_t sr_bg = transform_component_cost(
                r_bg, 9, has_w, has_n, has_ne);
            uint32_t sg_rb = transform_component_cost(
                g_rb, 9, has_w, has_n, has_ne);
            cost[0] += sr + sg + sb;
            cost[1] += sg + srg + sbg;
            cost[2] += sycg + sco + scg;
            cost[3] += sr + sgr + sbr;
            cost[4] += sb + srb + sgb;
            cost[5] += sg + srg + sb_rg;
            cost[6] += sg + sbg + sr_bg;
            cost[7] += sr + sbr + sg_rb;
            cost[8] += sb + srb + sg_rb;
            cost[9] += sr + sgr + sbg;
            cost[10] += sb + sgb + srg;
            if (include_reversible) {
                uint32_t sluma = transform_component_cost(
                    luma, 8, has_w, has_n, has_ne);
                if (include_reversible == 1) {
                    for (int t = 11; t <= 28; ++t) {
                        for (int i = 0; i < 5; ++i)
                            third[i] =
                                blue[i] - rg_blend(red[i], green[i], t) + 256;
                        cost[t] += sg + srg + transform_component_cost(
                            third, 9, has_w, has_n, has_ne);
                    }
                }
                for (int t = 29; t <= 34; ++t) {
                    for (int i = 0; i < 5; ++i)
                        third[i] =
                            blue[i] -
                            rg_luma_blend(red[i], green[i], t) + 256;
                    cost[t] += sluma + srg + transform_component_cost(
                        third, 9, has_w, has_n, has_ne);
                }
                for (int i = 0; i < 5; ++i)
                    third[i] =
                        blue[i] - floor_shift(green[i] + red[i], 1) + 256;
                cost[35] += sr + sgr + transform_component_cost(
                    third, 9, has_w, has_n, has_ne);
            }
        }
    }
}

static int build_palette(EncCtx *c) {
    size_t npix = (size_t)c->w * c->h; int ch = c->ch;
    size_t sch = c->stride;
    c->pal_idx = malloc(npix);
    if (!c->pal_idx) return STREAM_E_ALLOC;
    uint32_t keys[512] = {0};
    uint16_t ids[512] = {0};
    uint8_t used[512] = {0};
    int n = 0, last = 0;
    for (size_t i = 0; i < npix; i++) {
        const uint8_t *p = c->pix + i * sch;
        int j = (n && !memcmp(c->pal[last], p, (size_t)ch)) ? last : -1;
        uint32_t key = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) |
                       (ch == 4 ? (uint32_t)p[3] << 24 : 0u);
        size_t slot = (size_t)((key * UINT32_C(2654435761)) >> 23);
        if (j < 0) {
            while (used[slot] && keys[slot] != key) slot = (slot + 1u) & 511u;
            if (used[slot]) j = ids[slot];
        }
        if (j < 0) {
            if (n == 256) { free(c->pal_idx); c->pal_idx = NULL; c->pal_n = 0; return STREAM_OK; }
            used[slot] = 1;
            keys[slot] = key;
            ids[slot] = (uint16_t)n;
            memcpy(c->pal[n], p, (size_t)ch);
            j = n++;
        }
        c->pal_idx[i] = (uint8_t)j; last = j;
    }
    uint8_t ord[256], map[256], pal2[256][4];
    for (int i = 0; i < n; i++) ord[i] = (uint8_t)i;
    for (int i = 1; i < n; i++) {
        uint8_t o = ord[i]; int j = i - 1;
        int lo = 2*c->pal[o][0] + 5*c->pal[o][1] + c->pal[o][2];
        while (j >= 0 && 2*c->pal[ord[j]][0] + 5*c->pal[ord[j]][1] + c->pal[ord[j]][2] > lo) {
            ord[j+1] = ord[j]; j--;
        }
        ord[j+1] = o;
    }
    for (int i = 0; i < n; i++) { memcpy(pal2[i], c->pal[ord[i]], 4); map[ord[i]] = (uint8_t)i; }
    memcpy(c->pal, pal2, (size_t)n * sizeof pal2[0]);
    for (size_t i = 0; i < npix; i++) c->pal_idx[i] = map[c->pal_idx[i]];
    c->pal_n = n;
    return STREAM_OK;
}

static int encode_plane_sparse56(Enc *encoder, const uint16_t *plane,
                                 uint32_t width, uint32_t height, int depth,
                                 int tile_log, const uint8_t *map);
static int decode_plane_sparse56(Dec *decoder, uint16_t *plane,
                                 uint32_t width, uint32_t height, int depth,
                                 int tile_log);

static int encode_mode_plane(const EncCtx *c, Enc *e, const uint16_t *plane,
                             int depth, int mode, int transform, int tlog,
                             int plane_index, uint8_t *state_out,
                             const uint8_t *state_in, const uint8_t *map,
                             int map_kind) {
    int pos = zmode_pos(mode, plane_index);
    switch (plane_method_for(mode)) {
    case PLANE_X:
        return encode_plane_x(
            e, plane, c->w, c->h, depth, tlog, pos, mode >= 26, mode >= 27,
            mode >= 29, mode >= 30, mode >= 31, mode >= 32, mode >= 33, 0,
            mode >= 34, mode >= 35, mode >= 36, map);
    case PLANE_XZR:
        return encode_plane_xzr(e, plane, c->w, c->h, depth, tlog, pos,
                                cached_xzr_map(c, transform, tlog,
                                               plane_index));
    case PLANE_RULE:
        return encode_plane_rule(e, plane, c->w, c->h, depth, pos);
    case PLANE_EVENT:
        return encode_plane_event(e, plane, c->w, c->h, depth, pos);
    case PLANE_PATTERN:
        return encode_plane_pattern(e, plane, c->w, c->h, depth, pos);
    case PLANE_SPARSE:
        return encode_plane_sparse56(e, plane, c->w, c->h, depth, tlog,
                                     map);
    case PLANE_CONTEXT:
        return encode_plane37(
            e, plane, c->w, c->h, depth, tlog,
            mode == 41 || (mode >= 44 && mode <= 54),
            plane_context_for(mode), state_out, state_in, map, map_kind);
    default:
        return encode_plane(e, plane, c->w, c->h, depth, tlog, pos);
    }
}

static int decode_mode_plane(Dec *d, uint16_t *plane, uint32_t w, uint32_t h,
                             int depth, int mode, int tlog, int plane_index,
                             uint8_t *state_out, const uint8_t *state_in) {
    int pos = zmode_pos(mode, plane_index);
    switch (plane_method_for(mode)) {
    case PLANE_X:
        return decode_plane_x(
            d, plane, w, h, depth, tlog, pos, mode >= 26, mode >= 27,
            mode >= 29, mode >= 30, mode >= 31, mode >= 32, mode >= 33, 0,
            mode >= 34, mode >= 35, mode >= 36);
    case PLANE_XZR:
        return decode_plane_xzr(d, plane, w, h, depth, tlog, pos);
    case PLANE_RULE:
        return decode_plane_rule(d, plane, w, h, depth, pos);
    case PLANE_EVENT:
        return decode_plane_event(d, plane, w, h, depth, pos);
    case PLANE_PATTERN:
        return decode_plane_pattern(d, plane, w, h, depth, pos);
    case PLANE_SPARSE:
        return decode_plane_sparse56(d, plane, w, h, depth, tlog);
    case PLANE_CONTEXT:
        if (mode == 52) {
            if (!state_in)
                return state_out
                           ? decode_plane52_first(d, plane, w, h, depth, tlog,
                                                  state_out)
                           : decode_plane52_independent(d, plane, w, h, depth,
                                                        tlog);
            if (!state_out)
                return decode_plane52_last(d, plane, w, h, depth, tlog,
                                           state_in);
            if (state_out == state_in)
                return decode_plane52_middle(d, plane, w, h, depth, tlog,
                                             state_out);
        }
        if (mode == 53) {
            if (!state_in)
                return state_out
                           ? decode_plane53_first(d, plane, w, h, depth, tlog,
                                                  state_out)
                           : decode_plane53_independent(d, plane, w, h, depth,
                                                        tlog);
            if (!state_out)
                return decode_plane53_last(d, plane, w, h, depth, tlog,
                                           state_in);
            if (state_out == state_in)
                return decode_plane53_middle(d, plane, w, h, depth, tlog,
                                             state_out);
        }
        return decode_plane37(
            d, plane, w, h, depth, tlog,
            mode == 41 || (mode >= 44 && mode <= 54),
            plane_context_for(mode), state_out, state_in);
    default:
        return zmode_zr(mode)
                   ? decode_plane_zr(d, plane, w, h, depth, tlog, pos,
                                     zmode_sc(mode))
                   : decode_plane(d, plane, w, h, depth, tlog, pos);
    }
}

#define FAST55_BASE_CONTEXTS 4320
#define FAST55_PAIRED_CONTEXTS 7776
#define FAST55_CONTEXTS_MAX FAST55_PAIRED_CONTEXTS
#define FAST55_CLUSTERS 32
#define FAST55_DIRECT 64
#define FAST55_ESCAPE FAST55_DIRECT
#define FAST55_RUN (FAST55_DIRECT + 1)
#define FAST55_PRIMARY (FAST55_DIRECT + 2)
#define FAST55_RUN_MIN 32u
#define FAST55_REGIONS 2
#define FAST55_SCALE_BITS 12
#define FAST55_SCALE (1u << FAST55_SCALE_BITS)
#define FAST55_RANS_L (1u << 23)
#define FAST55_LANES 4
#define FAST55_SIMPLE_CONTEXT_FLAG 16
#define FAST55_PAIRED_CONTEXT_FLAG 32

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t limit;
} Fast55Buffer;

typedef struct {
    uint16_t start;
    uint16_t frequency;
} Fast55Symbol;

static int fast55_reserve(Fast55Buffer *buffer, size_t extra) {
    if (extra > buffer->limit - buffer->size) return 0;
    size_t needed = buffer->size + extra;
    if (needed <= buffer->capacity) return 1;
    size_t capacity = buffer->capacity ? buffer->capacity : 65536u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > buffer->limit) capacity = buffer->limit;
    uint8_t *data = realloc(buffer->data, capacity);
    if (!data) return 0;
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int fast55_append(Fast55Buffer *buffer, const void *data, size_t size) {
    if (!size) return 1;
    if (!fast55_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int fast55_u32(Fast55Buffer *buffer, uint32_t value) {
    uint8_t bytes[4];
    put32(bytes, value);
    return fast55_append(buffer, bytes, sizeof(bytes));
}

static int fast55_uint(Fast55Buffer *buffer, uint32_t value) {
    uint8_t bytes[5];
    size_t count = 0;
    do {
        uint8_t byte = (uint8_t)(value & 127u);
        value >>= 7;
        if (value) byte |= 128u;
        bytes[count++] = byte;
    } while (value);
    return fast55_append(buffer, bytes, count);
}

static QLIC_FORCEINLINE int fast55_activity(int value) {
    return qctx(value);
}

static QLIC_FORCEINLINE int fast55_error_state(uint8_t left, uint8_t up) {
    return fast55_error_context_lut[(unsigned)left * 32u + up];
}

static QLIC_FORCEINLINE int fast55_predictor_group(int predictor) {
    return !predictor ? 0 : predictor <= 4 ? 1 : predictor <= 10 ? 2 : 3;
}

static QLIC_FORCEINLINE int fast55_activity_reference(
    int W, int N, int NW, int NE, int WW, int NN, int maximum,
    int *activity) {
    int west_west = iabs(W - WW);
    int north_northwest = iabs(N - NW);
    int northeast_north = iabs(NE - N);
    int west_northwest = iabs(W - NW);
    int north_north = iabs(N - NN);
    *activity = west_northwest + north_northwest + northeast_north +
                ((west_west + north_north) >> 1);
    int direction = west_northwest + north_north - west_west -
                    north_northwest;
    int reference;
    if (direction > 80) reference = W;
    else if (direction < -80) reference = N;
    else {
        reference = ((W + N) >> 1) + ((NE - NW) >> 2);
        if (direction > 32) reference = (reference + W) >> 1;
        else if (direction > 8) reference = (3 * reference + W) >> 2;
        else if (direction < -32) reference = (reference + N) >> 1;
        else if (direction < -8) reference = (3 * reference + N) >> 2;
    }
    reference = clampi(reference, 0, maximum);
    return reference;
}

static QLIC_FORCEINLINE int fast55_activity_hint_reference(
    int W, int N, int NW, int NE, int WW, int NN, int *activity) {
    int west_west = iabs(W - WW);
    int north_northwest = iabs(N - NW);
    int northeast_north = iabs(NE - N);
    int west_northwest = iabs(W - NW);
    int north_north = iabs(N - NN);
    *activity = west_northwest + north_northwest + northeast_north +
                ((west_west + north_north) >> 1);
    int lower = W < N ? W : N;
    int upper = W > N ? W : N;
    int reference = clampi(
        ((W + N) >> 1) + ((NE - NW) >> 3), lower, upper);
    return reference;
}

static QLIC_FORCEINLINE uint8_t fast55_channel_state(unsigned symbol) {
    return fast55_residual_lut[symbol].channel_state;
}

static QLIC_FORCEINLINE unsigned fast55_log2_q12(uint32_t value) {
    if (value <= 1u) return 0;
    unsigned exponent = (unsigned)nbits(value) - 1u;
    uint32_t base = 1u << exponent;
    uint32_t fraction = (uint32_t)(((uint64_t)(value - base) << 12) / base);
    uint32_t curve = (uint32_t)(((uint64_t)fraction * (4096u - fraction)) >> 13);
    return exponent * 4096u + fraction + curve;
}

typedef struct {
    uint16_t context;
    uint32_t mean;
} Fast55ActiveContext;

static int fast55_context_compare(const void *left, const void *right) {
    const Fast55ActiveContext *a = left;
    const Fast55ActiveContext *b = right;
    if (a->mean != b->mean) return a->mean < b->mean ? -1 : 1;
    return (int)a->context - (int)b->context;
}

static void fast55_context_sift(Fast55ActiveContext *active, int root,
                                int end) {
    while (root * 2 + 1 <= end) {
        int child = root * 2 + 1;
        int swap = root;
        if (fast55_context_compare(active + swap, active + child) < 0)
            swap = child;
        if (child + 1 <= end &&
            fast55_context_compare(active + swap, active + child + 1) < 0)
            swap = child + 1;
        if (swap == root) return;
        Fast55ActiveContext temporary = active[root];
        active[root] = active[swap];
        active[swap] = temporary;
        root = swap;
    }
}

static void fast55_context_sort(Fast55ActiveContext *active, int count) {
    if (count < 2) return;
    for (int start = (count - 2) / 2; start >= 0; --start)
        fast55_context_sift(active, start, count - 1);
    for (int end = count - 1; end > 0; --end) {
        Fast55ActiveContext temporary = active[end];
        active[end] = active[0];
        active[0] = temporary;
        fast55_context_sift(active, 0, end - 1);
    }
}

static void fast55_cluster(const uint32_t *histogram,
                           uint8_t mapping[FAST55_CONTEXTS_MAX],
                           int cluster_count, int context_count) {
    uint64_t tables[FAST55_CLUSTERS][FAST55_PRIMARY];
    Fast55ActiveContext active[FAST55_CONTEXTS_MAX];
    uint8_t support_count[FAST55_CONTEXTS_MAX];
    uint8_t *support = malloc((size_t)context_count * FAST55_PRIMARY);
    int active_count = 0;
    memset(mapping, 0, (size_t)context_count);
    memset(support_count, 0, (size_t)context_count);
    for (int context = 0; context < context_count; ++context) {
        const uint32_t *source =
            histogram + (size_t)context * FAST55_PRIMARY;
        uint64_t total = 0;
        uint64_t weighted = 0;
        for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol) {
            uint64_t count = source[symbol];
            total += count;
            weighted += count * (uint64_t)symbol;
            if (support && count)
                support[(size_t)context * FAST55_PRIMARY +
                        support_count[context]++] = (uint8_t)symbol;
        }
        if (!total) continue;
        active[active_count].context = (uint16_t)context;
        active[active_count].mean =
            (uint32_t)((weighted << 16) / total);
        ++active_count;
    }
    if (!active_count) {
        free(support);
        return;
    }
    fast55_context_sort(active, active_count);
    for (int index = 0; index < active_count; ++index)
        mapping[active[index].context] = (uint8_t)(
            (uint64_t)index * (uint64_t)cluster_count /
            (uint64_t)active_count);
    for (int iteration = 0; iteration < 8; ++iteration) {
        memset(tables, 0, sizeof(tables));
        uint64_t totals[FAST55_CLUSTERS] = {0};
        for (int index = 0; index < active_count; ++index) {
            int context = active[index].context;
            int cluster = mapping[context];
            const uint32_t *source =
                histogram + (size_t)context * FAST55_PRIMARY;
            uint64_t *table = tables[cluster];
            uint64_t source_total = 0;
            if (support) {
                const uint8_t *symbols =
                    support + (size_t)context * FAST55_PRIMARY;
                for (int item = 0; item < support_count[context]; ++item) {
                    int symbol = symbols[item];
                    table[symbol] += source[symbol];
                    source_total += source[symbol];
                }
            } else {
                for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol) {
                    table[symbol] += source[symbol];
                    source_total += source[symbol];
                }
            }
            totals[cluster] += source_total;
        }
        uint32_t symbol_cost[FAST55_CLUSTERS][FAST55_PRIMARY];
        for (int cluster = 0; cluster < cluster_count; ++cluster) {
            uint64_t table_total = FAST55_PRIMARY + totals[cluster];
            unsigned total_cost = fast55_log2_q12(
                table_total > UINT32_MAX ? UINT32_MAX :
                                           (uint32_t)table_total);
            for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol) {
                uint64_t count = tables[cluster][symbol] + 1u;
                symbol_cost[cluster][symbol] =
                    total_cost - fast55_log2_q12(
                        count > UINT32_MAX ? UINT32_MAX :
                                             (uint32_t)count);
            }
        }
        int changed = 0;
        for (int index = 0; index < active_count; ++index) {
            int context = active[index].context;
            const uint32_t *source =
                histogram + (size_t)context * FAST55_PRIMARY;
            uint64_t best_cost = UINT64_MAX;
            int best = 0;
            for (int cluster = 0; cluster < cluster_count; ++cluster) {
                uint64_t cost = 0;
                if (support) {
                    const uint8_t *symbols =
                        support + (size_t)context * FAST55_PRIMARY;
                    for (int item = 0; item < support_count[context];
                         ++item) {
                        int symbol = symbols[item];
                        cost += (uint64_t)source[symbol] *
                                symbol_cost[cluster][symbol];
                    }
                } else {
                    for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol)
                        if (source[symbol])
                            cost += (uint64_t)source[symbol] *
                                    symbol_cost[cluster][symbol];
                }
                if (cost < best_cost) {
                    best_cost = cost;
                    best = cluster;
                }
            }
            changed |= mapping[context] != best;
            mapping[context] = (uint8_t)best;
        }
        if (!changed) break;
    }
    free(support);
}

static void fast55_normalize(const uint64_t *counts, int alphabet,
                             Fast55Symbol *symbols, uint16_t *frequencies);

static void fast55_partition_costs(
    const uint64_t *quarter_counts, int cluster_count,
    uint64_t *half_cost, uint64_t *quarter_cost) {
    uint64_t costs[2] = {0, 0};
    for (int choice = 0; choice < 2; ++choice) {
        int split = choice ? 1 : 2;
        for (int region = 0; region < FAST55_REGIONS; ++region) {
            for (int cluster = 0; cluster < cluster_count; ++cluster) {
                uint64_t counts[FAST55_PRIMARY] = {0};
                for (int source_region = 0; source_region < 4;
                     ++source_region) {
                    if ((source_region >= split) != region) continue;
                    for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol)
                        counts[symbol] += quarter_counts[
                            ((size_t)source_region * (size_t)cluster_count +
                             (size_t)cluster) * FAST55_PRIMARY +
                            (size_t)symbol];
                }
                Fast55Symbol symbols[FAST55_PRIMARY];
                uint16_t frequencies[FAST55_PRIMARY];
                fast55_normalize(counts, FAST55_PRIMARY, symbols,
                                 frequencies);
                for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol) {
                    if (!counts[symbol]) continue;
                    unsigned frequency_log =
                        fast55_log2_q12(frequencies[symbol]);
                    costs[choice] += counts[symbol] *
                        (uint64_t)(FAST55_SCALE_BITS * 4096u -
                                   frequency_log);
                }
            }
        }
    }
    *half_cost = costs[0];
    *quarter_cost = costs[1];
}

static void fast55_normalize(const uint64_t *counts, int alphabet,
                             Fast55Symbol *symbols, uint16_t *frequencies) {
    uint64_t total = 0;
    for (int symbol = 0; symbol < alphabet; ++symbol) total += counts[symbol];
    if (!total) {
        memset(frequencies, 0, (size_t)alphabet * sizeof(*frequencies));
        frequencies[0] = FAST55_SCALE;
    } else {
        unsigned sum = 0;
        for (int symbol = 0; symbol < alphabet; ++symbol) {
            unsigned frequency = counts[symbol]
                                     ? (unsigned)((counts[symbol] * FAST55_SCALE) / total)
                                     : 0u;
            if (counts[symbol] && !frequency) frequency = 1;
            frequencies[symbol] = (uint16_t)frequency;
            sum += frequency;
        }
        while (sum < FAST55_SCALE) {
            int best = -1;
            uint64_t best_gain = 0;
            for (int symbol = 0; symbol < alphabet; ++symbol) {
                unsigned frequency = frequencies[symbol];
                if (!counts[symbol] || frequency >= FAST55_SCALE) continue;
                uint64_t gain = counts[symbol] *
                    (fast55_log2_q12(frequency + 1u) -
                     fast55_log2_q12(frequency));
                if (best < 0 || gain > best_gain) {
                    best = symbol;
                    best_gain = gain;
                }
            }
            if (best < 0) break;
            ++frequencies[best];
            ++sum;
        }
        while (sum > FAST55_SCALE) {
            int best = -1;
            uint64_t best_loss = UINT64_MAX;
            for (int symbol = 0; symbol < alphabet; ++symbol)
                if (frequencies[symbol] > 1u) {
                    unsigned frequency = frequencies[symbol];
                    uint64_t loss = counts[symbol] *
                        (fast55_log2_q12(frequency) -
                         fast55_log2_q12(frequency - 1u));
                    if (loss < best_loss) {
                    best = symbol;
                        best_loss = loss;
                    }
                }
            if (best < 0) break;
            --frequencies[best];
            --sum;
        }
    }
    unsigned start = 0;
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        symbols[symbol].start = (uint16_t)start;
        symbols[symbol].frequency = frequencies[symbol];
        start += frequencies[symbol];
    }
}

static void fast55_refine_normalized(
    const uint32_t *histogram, uint8_t *mapping, int cluster_count,
    int context_count) {
    uint64_t tables[FAST55_CLUSTERS][FAST55_PRIMARY];
    uint32_t costs[FAST55_CLUSTERS][FAST55_PRIMARY];
    uint16_t active[FAST55_CONTEXTS_MAX];
    uint8_t support_count[FAST55_CONTEXTS_MAX];
    uint8_t *support = malloc((size_t)context_count * FAST55_PRIMARY);
    if (!support) return;
    int active_count = 0;
    memset(support_count, 0, (size_t)context_count);
    for (int context = 0; context < context_count; ++context) {
        const uint32_t *source =
            histogram + (size_t)context * FAST55_PRIMARY;
        for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol)
            if (source[symbol])
                support[(size_t)context * FAST55_PRIMARY +
                        support_count[context]++] = (uint8_t)symbol;
        if (support_count[context]) active[active_count++] =
            (uint16_t)context;
    }
    for (int iteration = 0; iteration < 1; ++iteration) {
        memset(tables, 0, sizeof(tables));
        for (int index = 0; index < active_count; ++index) {
            int context = active[index];
            uint64_t *table = tables[mapping[context]];
            const uint32_t *source =
                histogram + (size_t)context * FAST55_PRIMARY;
            const uint8_t *symbols =
                support + (size_t)context * FAST55_PRIMARY;
            for (int item = 0; item < support_count[context]; ++item) {
                int symbol = symbols[item];
                table[symbol] += source[symbol];
            }
        }
        for (int cluster = 0; cluster < cluster_count; ++cluster) {
            Fast55Symbol symbols[FAST55_PRIMARY];
            uint16_t frequencies[FAST55_PRIMARY];
            fast55_normalize(tables[cluster], FAST55_PRIMARY, symbols,
                             frequencies);
            for (int symbol = 0; symbol < FAST55_PRIMARY; ++symbol)
                costs[cluster][symbol] = frequencies[symbol]
                    ? FAST55_SCALE_BITS * 4096u -
                          fast55_log2_q12(frequencies[symbol])
                    : UINT32_MAX;
        }
        int changed = 0;
        for (int index = 0; index < active_count; ++index) {
            int context = active[index];
            const uint32_t *source =
                histogram + (size_t)context * FAST55_PRIMARY;
            const uint8_t *symbols =
                support + (size_t)context * FAST55_PRIMARY;
            int current = mapping[context];
            uint64_t best_cost = 0;
            for (int item = 0; item < support_count[context]; ++item) {
                int symbol = symbols[item];
                uint32_t count = source[symbol];
                best_cost += (uint64_t)count * costs[current][symbol];
            }
            int best = current;
            for (int cluster = 0; cluster < cluster_count; ++cluster) {
                if (cluster == current) continue;
                uint64_t cost = 0;
                int valid = 1;
                for (int item = 0; item < support_count[context]; ++item) {
                    int symbol = symbols[item];
                    uint32_t count = source[symbol];
                    if (costs[cluster][symbol] == UINT32_MAX) {
                        valid = 0;
                        break;
                    }
                    cost += (uint64_t)count * costs[cluster][symbol];
                }
                if (valid && cost < best_cost) {
                    best_cost = cost;
                    best = cluster;
                }
            }
            changed |= best != current;
            mapping[context] = (uint8_t)best;
        }
        if (!changed) break;
    }
    free(support);
}

static QLIC_FORCEINLINE void fast55_rans_put(uint32_t *state,
                                              Fast55Symbol symbol,
                                              uint8_t **output) {
    uint32_t value = *state;
    uint32_t maximum = (uint32_t)(((FAST55_RANS_L >> FAST55_SCALE_BITS) << 8) *
                                  (uint32_t)symbol.frequency);
    while (value >= maximum) {
        *--*output = (uint8_t)value;
        value >>= 8;
    }
    *state = (value / symbol.frequency << FAST55_SCALE_BITS) +
             value % symbol.frequency + symbol.start;
}

static int fast55_encode_map(Enc *encoder, const uint8_t *map,
                             size_t tiles, uint32_t tiles_x) {
    Prob tree[NPREDX];
    Prob same[2][4];
    probabilities_init(tree, NPREDX);
    probabilities_init((Prob *)same, 8);
    for (size_t index = 0; index < tiles && !encoder->cut; ++index) {
        int predictor = map[index];
        int write_tree = 1;
        uint32_t tile_x = (uint32_t)(index % tiles_x);
        if (tile_x) {
            int reference = map[index - 1u];
            int group = fast55_predictor_group(reference);
            int different = predictor != reference;
            enc_bit(encoder, &same[0][group], different);
            if (!different) write_tree = 0;
        }
        if (write_tree && index >= tiles_x &&
            (!tile_x || map[index - tiles_x] != map[index - 1u])) {
            int reference = map[index - tiles_x];
            int group = fast55_predictor_group(reference);
            int different = predictor != reference;
            enc_bit(encoder, &same[1][group], different);
            if (!different) write_tree = 0;
        }
        if (write_tree) enc_tree5(encoder, tree, predictor);
    }
    return encoder->oom ? STREAM_E_ALLOC :
           encoder->cut ? STREAM_E_FORMAT : STREAM_OK;
}

static int fast55_decode_map(Dec *decoder, uint8_t *map, size_t tiles,
                             uint32_t tiles_x) {
    Prob tree[NPREDX];
    Prob same[2][4];
    probabilities_init(tree, NPREDX);
    probabilities_init((Prob *)same, 8);
    for (size_t index = 0; index < tiles; ++index) {
        int predictor = -1;
        uint32_t tile_x = (uint32_t)(index % tiles_x);
        if (tile_x) {
            int reference = map[index - 1u];
            int group = fast55_predictor_group(reference);
            if (!dec_bit(decoder, &same[0][group])) predictor = reference;
        }
        if (predictor < 0 && index >= tiles_x &&
            (!tile_x || map[index - tiles_x] != map[index - 1u])) {
            int reference = map[index - tiles_x];
            int group = fast55_predictor_group(reference);
            if (!dec_bit(decoder, &same[1][group])) predictor = reference;
        }
        if (predictor < 0) predictor = dec_tree5(decoder, tree);
        if ((unsigned)predictor >= NPREDX) return STREAM_E_CORRUPT;
        map[index] = (uint8_t)predictor;
    }
    return decoder->truncated ? STREAM_E_CORRUPT : STREAM_OK;
}

#define SPARSE56_RUN_CONTEXTS 9
#define SPARSE56_RUN_BITS 29

typedef struct {
    Prob run_unary[SPARSE56_RUN_CONTEXTS][SPARSE56_RUN_BITS + 1];
    Prob run_mant[SPARSE56_RUN_CONTEXTS][SPARSE56_RUN_BITS + 1]
                 [SPARSE56_RUN_BITS];
    Prob unary[NCTX][MAXK + 1];
    Prob mant[NCTX][MAXK + 1][MAXK];
    Prob sign[NCTX];
} Sparse56Model;

typedef struct {
    uint32_t position;
    int16_t residual;
} Sparse56Event;

static QLIC_FORCEINLINE int sparse56_run_context(int magnitude, int sign) {
    int group = magnitude > 3 ? 3 : magnitude;
    return 1 + group * 2 + (sign < 0);
}

static QLIC_FORCEINLINE void sparse56_encode_uint(Enc *encoder,
                                                   Sparse56Model *model,
                                                   int context,
                                                   uint32_t value) {
    int bits = nbits(value);
    if (bits > SPARSE56_RUN_BITS) {
        encoder->cut = 1;
        return;
    }
    for (int index = 0; index < bits; ++index)
        enc_bit(encoder, &model->run_unary[context][index], 1);
    if (bits < SPARSE56_RUN_BITS)
        enc_bit(encoder, &model->run_unary[context][bits], 0);
    for (int index = bits - 2; index >= 0; --index)
        enc_bit(encoder, &model->run_mant[context][bits][index],
                (value >> index) & 1u);
}

static QLIC_FORCEINLINE uint32_t sparse56_decode_uint(Dec *decoder,
                                                       Sparse56Model *model,
                                                       int context) {
    int bits = 0;
    while (bits < SPARSE56_RUN_BITS &&
           dec_bit(decoder, &model->run_unary[context][bits]))
        ++bits;
    uint32_t value = bits ? 1u << (bits - 1) : 0u;
    for (int index = bits - 2; index >= 0; --index)
        value |= (uint32_t)dec_bit(
                     decoder, &model->run_mant[context][bits][index])
                 << index;
    return value;
}

typedef struct {
    Dec *decoder;
    Sparse56Model *model;
    size_t pixels;
    size_t next;
    uint32_t remaining;
    uint32_t skip;
    int previous_magnitude;
    int previous_sign;
} Sparse56Cursor;

static QLIC_NOINLINE int sparse56_decode_event(Sparse56Cursor *cursor,
                                                size_t position, int depth,
                                                int half, int *residual) {
    int context = ectx(
        qctx(cursor->skip > 512u ? 512 : (int)cursor->skip),
        cursor->previous_magnitude, cursor->previous_sign, 1);
    int bits = 0;
    while (bits < depth &&
           dec_bit(cursor->decoder, &cursor->model->unary[context][bits]))
        ++bits;
    unsigned value = bits ? 1u << (bits - 1) : 0u;
    for (int index = bits - 2; index >= 0; --index)
        value |= (unsigned)dec_bit(
                     cursor->decoder,
                     &cursor->model->mant[context][bits][index])
                 << index;
    unsigned magnitude = value + 1u;
    if (magnitude > (unsigned)half) return 0;
    int negative = dec_bit(cursor->decoder, &cursor->model->sign[context]);
    if (cursor->previous_sign) negative ^= cursor->previous_sign < 0;
    *residual = negative ? -(int)magnitude : (int)magnitude;
    cursor->previous_magnitude = predictor_nbits_lut[magnitude];
    cursor->previous_sign = (*residual > 0) - (*residual < 0);
    --cursor->remaining;
    if (!cursor->remaining) {
        cursor->next = SIZE_MAX;
        return !cursor->decoder->truncated;
    }
    cursor->skip = sparse56_decode_uint(
        cursor->decoder, cursor->model,
        sparse56_run_context(cursor->previous_magnitude,
                             cursor->previous_sign));
    if ((size_t)cursor->skip >= cursor->pixels - position - 1u) return 0;
    cursor->next = position + 1u + cursor->skip;
    return !cursor->decoder->truncated;
}

static int encode_plane_sparse56(Enc *encoder, const uint16_t *plane,
                                 uint32_t width, uint32_t height, int depth,
                                 int tile_log, const uint8_t *map) {
    if (!tile_log || !map) return STREAM_E_FORMAT;
    model_ensure();
    uint32_t tiles_x =
        (width + (1u << tile_log) - 1u) >> tile_log;
    uint32_t tiles_y =
        (height + (1u << tile_log) - 1u) >> tile_log;
    size_t tiles = (size_t)tiles_x * tiles_y;
    int error = fast55_encode_map(encoder, map, tiles, tiles_x);
    if (error != STREAM_OK) return error;

    int half = 1 << (depth - 1);
    int maximum = (1 << depth) - 1;
    uint32_t w = width;
    size_t pixels = (size_t)width * height;
    size_t event_limit = pixels / 2u + 1u;
    size_t event_capacity = pixels / 16u + 1u;
    if (event_capacity > 65536u) event_capacity = 65536u;
    if (event_capacity > event_limit) event_capacity = event_limit;
    Sparse56Event *events = malloc(event_capacity * sizeof(*events));
    if (!events) return STREAM_E_ALLOC;
    size_t event_count = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t *row = plane + (size_t)y * width;
        const uint16_t *up = y ? row - width : row;
        const uint16_t *up2 = y > 1 ? up - width : up;
        const uint8_t *tile_row =
            map + (size_t)(y >> tile_log) * tiles_x;
        for (uint32_t x = 0; x < width; ++x) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2u] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int predictor = tile_row[x >> tile_log];
            int prediction = predicta_impl(
                predictor, Wv, Nv, NWv, NEv, WWv, NNv, maximum);
            int residual =
                ((row[x] - prediction + half) & maximum) - half;
            if (!residual) continue;
            if (event_count == event_limit) {
                free(events);
                return STREAM_E_FORMAT;
            }
            if (event_count == event_capacity) {
                size_t capacity = event_capacity * 2u;
                if (capacity > event_limit) capacity = event_limit;
                Sparse56Event *grown =
                    realloc(events, capacity * sizeof(*events));
                if (!grown) {
                    free(events);
                    return STREAM_E_ALLOC;
                }
                events = grown;
                event_capacity = capacity;
            }
            events[event_count].position = (uint32_t)((size_t)y * width + x);
            events[event_count].residual = (int16_t)residual;
            ++event_count;
        }
    }
    if (event_count * 2u > pixels) {
        free(events);
        return STREAM_E_FORMAT;
    }

    Sparse56Model *model = malloc(sizeof(*model));
    if (!model) {
        free(events);
        return STREAM_E_ALLOC;
    }
    probabilities_init((Prob *)model, sizeof(*model) / sizeof(Prob));
    sparse56_encode_uint(encoder, model, 0, (uint32_t)event_count);
    size_t previous = SIZE_MAX;
    int previous_magnitude = 0;
    int previous_sign = 0;
    for (size_t index = 0; index < event_count && !encoder->cut; ++index) {
        size_t position = events[index].position;
        int residual = events[index].residual;
        uint32_t skip = (uint32_t)(
            previous == SIZE_MAX ? position : position - previous - 1u);
        sparse56_encode_uint(
            encoder, model,
            sparse56_run_context(previous_magnitude, previous_sign), skip);
        int context = ectx(qctx(skip > 512u ? 512 : (int)skip),
                           previous_magnitude, previous_sign, 1);
        unsigned magnitude = (unsigned)iabs(residual);
        unsigned value = magnitude - 1u;
        int bits = predictor_nbits_lut[value];
        for (int bit = 0; bit < bits; ++bit)
            enc_bit(encoder, &model->unary[context][bit], 1);
        if (bits < depth)
            enc_bit(encoder, &model->unary[context][bits], 0);
        for (int bit = bits - 2; bit >= 0; --bit)
            enc_bit(encoder, &model->mant[context][bits][bit],
                    (value >> bit) & 1u);
        int negative = residual < 0;
        if (previous_sign) negative ^= previous_sign < 0;
        enc_bit(encoder, &model->sign[context], negative);
        previous = position;
        previous_magnitude = predictor_nbits_lut[magnitude];
        previous_sign = (residual > 0) - (residual < 0);
    }
    free(model);
    free(events);
    return encoder->oom ? STREAM_E_ALLOC :
           encoder->cut ? STREAM_E_FORMAT : STREAM_OK;
}

static int decode_plane_sparse56(Dec *decoder, uint16_t *plane,
                                 uint32_t width, uint32_t height, int depth,
                                 int tile_log) {
    if (!tile_log) return STREAM_E_FORMAT;
    model_ensure();
    uint32_t tiles_x =
        (width + (1u << tile_log) - 1u) >> tile_log;
    uint32_t tiles_y =
        (height + (1u << tile_log) - 1u) >> tile_log;
    size_t tiles = (size_t)tiles_x * tiles_y;
    uint8_t *map = malloc(tiles);
    Sparse56Model *model = malloc(sizeof(*model));
    if (!map || !model) {
        free(model);
        free(map);
        return STREAM_E_ALLOC;
    }
    int error = fast55_decode_map(decoder, map, tiles, tiles_x);
    if (error != STREAM_OK) {
        free(model);
        free(map);
        return error;
    }
    probabilities_init((Prob *)model, sizeof(*model) / sizeof(Prob));
    size_t pixels = (size_t)width * height;
    uint32_t events = sparse56_decode_uint(decoder, model, 0);
    if (events > pixels || decoder->truncated) {
        free(model);
        free(map);
        return STREAM_E_CORRUPT;
    }
    Sparse56Cursor cursor = {
        decoder, model, pixels, SIZE_MAX, events, 0, 0, 0
    };
    if (cursor.remaining) {
        cursor.skip = sparse56_decode_uint(
            decoder, model, sparse56_run_context(0, 0));
        if ((size_t)cursor.skip >= pixels) {
            free(model);
            free(map);
            return STREAM_E_CORRUPT;
        }
        cursor.next = cursor.skip;
    }
    int half = 1 << (depth - 1);
    int maximum = (1 << depth) - 1;
    uint32_t w = width;
    size_t position = 0;
#define SPARSE56_DECODE_RANGE(prediction_expression) do { \
    while (x < end) { \
        int Wv, Nv, NWv, NEv; NEIGHBORS(); \
        int WWv = x > 1 ? row[x - 2u] : Wv; \
        int NNv = y > 1 ? up2[x] : Nv; \
        (void)Wv; \
        (void)Nv; \
        (void)NWv; \
        (void)NEv; \
        (void)WWv; \
        (void)NNv; \
        int prediction = (prediction_expression); \
        int residual = 0; \
        if (position == cursor.next && \
            !sparse56_decode_event(&cursor, position, depth, half, \
                                   &residual)) \
            goto sparse56_corrupt; \
        row[x] = (uint16_t)((prediction + residual) & maximum); \
        ++x; \
        ++position; \
    } \
} while (0)
#define SPARSE56_WEST_RANGE() do { \
    size_t span = end - x; \
    if (cursor.next >= position + span) { \
        uint16_t value = x ? row[x - 1u] : (y ? up[x] : (uint16_t)half); \
        position += span; \
        while (x < end) row[x++] = value; \
    } else { \
        SPARSE56_DECODE_RANGE(Wv); \
    } \
} while (0)
#define SPARSE56_NORTH_RANGE() do { \
    size_t span = end - x; \
    if (cursor.next >= position + span) { \
        if (y) memcpy(row + x, up + x, span * sizeof(*row)); \
        else { \
            uint16_t value = x ? row[x - 1u] : (uint16_t)half; \
            for (uint32_t fill = x; fill < end; ++fill) row[fill] = value; \
        } \
        x = end; \
        position += span; \
    } else { \
        SPARSE56_DECODE_RANGE(Nv); \
    } \
} while (0)
#define SPARSE56_NORTHEAST_RANGE() do { \
    size_t span = end - x; \
    if (cursor.next >= position + span) { \
        if (y) { \
            size_t copied = x + span < width ? span : span - 1u; \
            if (copied) \
                memcpy(row + x, up + x + 1u, copied * sizeof(*row)); \
            if (copied < span) row[x + copied] = up[x + copied]; \
        } else { \
            uint16_t value = x ? row[x - 1u] : (uint16_t)half; \
            for (uint32_t fill = x; fill < end; ++fill) row[fill] = value; \
        } \
        x = end; \
        position += span; \
    } else { \
        SPARSE56_DECODE_RANGE(NEv); \
    } \
} while (0)
    for (uint32_t y = 0; y < height; ++y) {
        uint16_t *row = plane + (size_t)y * width;
        const uint16_t *up = y ? row - width : row;
        const uint16_t *up2 = y > 1 ? up - width : up;
        const uint8_t *tile_row =
            map + (size_t)(y >> tile_log) * tiles_x;
        uint32_t x = 0;
        for (uint32_t tile = 0; tile < tiles_x; ++tile) {
            uint32_t end = (tile + 1u) << tile_log;
            if (end > width) end = width;
            /* predictor choice stays outside the pixel loop because a tile only has one */
            switch (tile_row[tile]) {
            case 0: SPARSE56_DECODE_RANGE(
                        predict(0, Wv, Nv, NWv, NEv, maximum)); break;
            case 1: SPARSE56_DECODE_RANGE(paethp(Wv, Nv, NWv)); break;
            case 2: SPARSE56_WEST_RANGE(); break;
            case 3: SPARSE56_NORTH_RANGE(); break;
            case 4: SPARSE56_DECODE_RANGE((Wv + Nv + 1) >> 1); break;
            case 5: SPARSE56_DECODE_RANGE(
                        clampi(Nv + Wv - NWv, 0, maximum)); break;
            case 6: SPARSE56_NORTHEAST_RANGE(); break;
            case 7: SPARSE56_DECODE_RANGE((Wv + NEv + 1) >> 1); break;
            case 8: SPARSE56_DECODE_RANGE((Nv + NEv + 1) >> 1); break;
            case 9: SPARSE56_DECODE_RANGE(
                        clampi(2 * Wv - WWv, 0, maximum)); break;
            case 10: SPARSE56_DECODE_RANGE(
                         clampi(2 * Nv - NNv, 0, maximum)); break;
            case 11: SPARSE56_DECODE_RANGE(
                         clampi(Wv + (((Nv - NWv) * 3) >> 2),
                                0, maximum)); break;
            case 12: SPARSE56_DECODE_RANGE(
                         clampi(Nv + (((Wv - NWv) * 3) >> 2),
                                0, maximum)); break;
            case 13: SPARSE56_DECODE_RANGE(
                         gapp(Wv, Nv, NWv, NEv, WWv, NNv, maximum)); break;
            case 14: SPARSE56_DECODE_RANGE(
                         clampi((Wv + Nv + NEv + NWv + 2) >> 2,
                                0, maximum)); break;
            case 15: SPARSE56_DECODE_RANGE(
                         clampi((5 * Wv + 2 * Nv - 3 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            case 16: SPARSE56_DECODE_RANGE(
                         clampi((Wv + 3 * Nv + 2) >> 2, 0, maximum)); break;
            case 17: SPARSE56_DECODE_RANGE(
                         clampi((3 * Wv + Nv + 2) >> 2, 0, maximum)); break;
            case 18: SPARSE56_DECODE_RANGE(
                         clampi((5 * Nv + 2 * Wv - 3 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            case 19: SPARSE56_DECODE_RANGE(
                         clampi((2 * Wv + Nv - NWv + 1) >> 1,
                                0, maximum)); break;
            case 20: SPARSE56_DECODE_RANGE(
                         clampi((Wv + 2 * Nv - NWv + 1) >> 1,
                                0, maximum)); break;
            case 21: SPARSE56_DECODE_RANGE(
                         clampi(Wv + ((NEv - NWv) >> 1), 0, maximum)); break;
            case 22: SPARSE56_DECODE_RANGE(
                         clampi(Nv + ((NEv - NWv) >> 1), 0, maximum)); break;
            case 23: SPARSE56_DECODE_RANGE(
                         clampi((Wv + Nv + NEv + 1) / 3, 0, maximum)); break;
            case 24: SPARSE56_DECODE_RANGE(
                         clampi((2 * Wv + Nv + NEv + 2) >> 2,
                                0, maximum)); break;
            case 25: SPARSE56_DECODE_RANGE(
                         clampi((Wv + 2 * Nv + NWv + 2) >> 2,
                                0, maximum)); break;
            case 26: SPARSE56_DECODE_RANGE(
                         clampi((3 * Wv + 3 * Nv - 2 * NWv + 2) >> 2,
                                0, maximum)); break;
            case 27: SPARSE56_DECODE_RANGE(
                         clampi((4 * Nv + Wv - 2 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            case 28: SPARSE56_DECODE_RANGE(
                         clampi((4 * Wv + Nv - 2 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            case 29: SPARSE56_DECODE_RANGE(
                         clampi((Wv + Nv + NEv - NWv + 1) >> 1,
                                0, maximum)); break;
            case 30: SPARSE56_DECODE_RANGE(
                         clampi((6 * Wv + 2 * Nv - 5 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            default: SPARSE56_DECODE_RANGE(
                         clampi((2 * Wv + 6 * Nv - 5 * NWv + NEv + 2) >> 2,
                                0, maximum)); break;
            }
        }
    }
#undef SPARSE56_DECODE_RANGE
#undef SPARSE56_WEST_RANGE
#undef SPARSE56_NORTH_RANGE
#undef SPARSE56_NORTHEAST_RANGE
    int ok = !cursor.remaining && !decoder->truncated;
    free(model);
    free(map);
    return ok ? STREAM_OK : STREAM_E_CORRUPT;
sparse56_corrupt:
#undef SPARSE56_DECODE_RANGE
#undef SPARSE56_WEST_RANGE
#undef SPARSE56_NORTH_RANGE
#undef SPARSE56_NORTHEAST_RANGE
    free(model);
    free(map);
    return STREAM_E_CORRUPT;
}

static int fast55_frequency(Fast55Buffer *buffer, uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)value;
    if (value < 128u) return fast55_append(buffer, bytes, 1u);
    bytes[0] |= 128u;
    bytes[1] = (uint8_t)(value >> 7);
    return fast55_append(buffer, bytes, 2u);
}

static int fast55_frequency_table(Fast55Buffer *buffer,
                                  const uint16_t *frequencies,
                                  int alphabet) {
    int symbol = 0;
    int stored = alphabet - 1;
    while (symbol < stored) {
        if (frequencies[symbol]) {
            if (!fast55_frequency(buffer, frequencies[symbol])) return 0;
            ++symbol;
            continue;
        }
        int run = 1;
        while (run < 255 && symbol + run < stored &&
               !frequencies[symbol + run])
            ++run;
        uint8_t bytes[2] = {0, (uint8_t)run};
        if (!fast55_append(buffer, bytes, sizeof(bytes))) return 0;
        symbol += run;
    }
    return 1;
}

#define FAST55_FREQ_CONTEXTS 8
#define FAST55_FREQ_BITS 14

typedef struct {
    Prob unary[FAST55_FREQ_CONTEXTS][FAST55_FREQ_BITS + 1];
    Prob mantissa[FAST55_FREQ_CONTEXTS][FAST55_FREQ_BITS + 1]
                 [FAST55_FREQ_BITS];
} Fast55FrequencyModel;

static QLIC_FORCEINLINE int fast55_frequency_context(int symbol) {
    int context = nbits((unsigned)symbol);
    return context < FAST55_FREQ_CONTEXTS ? context :
                                            FAST55_FREQ_CONTEXTS - 1;
}

static void fast55_encode_frequency_value(Enc *encoder,
                                          Fast55FrequencyModel *model,
                                          int context, unsigned value) {
    int bits = nbits(value);
    for (int index = 0; index < bits; ++index)
        enc_bit(encoder, &model->unary[context][index], 1);
    if (bits < FAST55_FREQ_BITS)
        enc_bit(encoder, &model->unary[context][bits], 0);
    for (int index = bits - 2; index >= 0; --index)
        enc_bit(encoder, &model->mantissa[context][bits][index],
                (value >> index) & 1u);
}

static void fast55_encode_frequency_range(
    Enc *encoder, Fast55FrequencyModel *model,
    const uint16_t *frequencies, const uint16_t *previous, int alphabet) {
    for (int symbol = 0; symbol < alphabet - 1; ++symbol) {
        int prediction = previous ? previous[symbol] : 0;
        int delta = (int)frequencies[symbol] - prediction;
        unsigned value = delta < 0 ? (unsigned)(-2 * delta - 1) :
                                     (unsigned)(2 * delta);
        fast55_encode_frequency_value(
            encoder, model, fast55_frequency_context(symbol), value);
    }
}

static size_t fast55_pack_mapping(const uint8_t *mapping, uint8_t *packed,
                                  size_t context_count) {
    size_t input = 0;
    size_t output = 0;
    while (input < context_count) {
        size_t run = 1;
        if (!mapping[input]) {
            while (run < 128u && input + run < context_count &&
                   !mapping[input + run])
                ++run;
            packed[output++] = (uint8_t)(128u | (run - 1u));
        } else {
            while (run < 128u && input + run < context_count &&
                   mapping[input + run])
                ++run;
            packed[output++] = (uint8_t)(run - 1u);
            for (size_t index = 0; index < run; index += 2u) {
                uint8_t value = mapping[input + index];
                if (index + 1u < run)
                    value |= (uint8_t)(mapping[input + index + 1u] << 4);
                packed[output++] = value;
            }
        }
        input += run;
    }
    return output;
}

static int fast55_encode_context_mapping(const uint8_t *mapping,
                                         int cluster_count,
                                         size_t context_count,
                                         uint8_t **data, size_t *size) {
    Enc encoder;
    enc_init(&encoder, ADAPT_DEFAULT);
    enc_reserve(&encoder, context_count);
    Prob tree[64];
    Prob same[5];
    probabilities_init(tree, 64);
    probabilities_init(same, 5);
    for (size_t context = 0; context < context_count; ++context) {
        size_t reference = 0;
        int dimension = -1;
        if (context % 12u) {
            reference = context - 1u;
            dimension = 0;
        } else if ((context / 12u) % 6u) {
            reference = context - 12u;
            dimension = 1;
        } else if ((context / 72u) % 4u) {
            reference = context - 72u;
            dimension = 2;
        } else if ((context / 288u) % 3u) {
            reference = context - 288u;
            dimension = 3;
        } else if (context >= 864u) {
            reference = context - 864u;
            dimension = 4;
        }
        if (dimension >= 0) {
            int different = mapping[context] != mapping[reference];
            enc_bit(&encoder, &same[dimension], different);
            if (!different) continue;
        }
        if (cluster_count <= 16)
            enc_tree4(&encoder, tree, mapping[context]);
        else
            enc_tree5(&encoder, tree, mapping[context]);
    }
    enc_flush(&encoder);
    if (encoder.oom || encoder.cut) {
        free(encoder.buf);
        return 0;
    }
    *data = encoder.buf;
    *size = encoder.len;
    return 1;
}

static int fast55_encode_plane_mapping(
    Fast55Buffer *output, const uint16_t *symbols, const uint16_t *contexts,
    const uint8_t *regions, size_t events, const uint64_t *cluster_counts,
    const uint64_t *tail_counts, int tail_alphabet,
    const uint8_t mapping[FAST55_CONTEXTS_MAX], int cluster_count,
    int context_count, int paired_context,
    int region_count, int region_split,
    const Fast55Buffer *run_stream, int use_runs, size_t pixels, int depth) {
    Fast55Symbol *tail_symbols = malloc(
        (size_t)tail_alphabet * sizeof(*tail_symbols));
    uint16_t *tail_frequencies = malloc(
        (size_t)tail_alphabet * sizeof(*tail_frequencies));
    if (!tail_symbols || !tail_frequencies) {
        free(tail_frequencies);
        free(tail_symbols);
        return STREAM_E_ALLOC;
    }
    Fast55Symbol primary_symbols[FAST55_REGIONS][FAST55_CLUSTERS]
                                      [FAST55_PRIMARY];
    uint16_t primary_frequencies[FAST55_REGIONS][FAST55_CLUSTERS]
                                        [FAST55_PRIMARY];
    for (int region = 0; region < region_count; ++region)
        for (int cluster = 0; cluster < cluster_count; ++cluster)
            fast55_normalize(
                             cluster_counts +
                                 ((size_t)region * (size_t)cluster_count +
                                  (size_t)cluster) * FAST55_PRIMARY,
                             FAST55_PRIMARY,
                             primary_symbols[region][cluster],
                             primary_frequencies[region][cluster]);
    fast55_normalize(tail_counts, tail_alphabet, tail_symbols,
                     tail_frequencies);
    if (pixels > (SIZE_MAX - 64u) / 5u) {
        free(tail_frequencies);
        free(tail_symbols);
        return STREAM_E_DIM;
    }
    size_t scratch_size = pixels * 5u + 64u;
    uint8_t *scratch = malloc(scratch_size);
    if (!scratch) {
        free(tail_frequencies);
        free(tail_symbols);
        return STREAM_E_ALLOC;
    }
    uint8_t *cursor = scratch + scratch_size;
    uint32_t states[FAST55_LANES];
    for (int lane = 0; lane < FAST55_LANES; ++lane)
        states[lane] = FAST55_RANS_L;
    for (size_t event = events; event-- > 0;) {
        unsigned symbol = symbols[event];
        uint32_t *state = &states[event & (FAST55_LANES - 1u)];
        int cluster = mapping[contexts[event]];
        int region = region_count == 1 ? 0 : regions[event] >= region_split;
        if (symbol >= FAST55_DIRECT && symbol != UINT16_MAX)
            fast55_rans_put(state, tail_symbols[symbol - FAST55_DIRECT],
                            &cursor);
        unsigned primary = symbol == UINT16_MAX
                               ? FAST55_RUN
                               : symbol < FAST55_DIRECT ? symbol
                                                        : FAST55_ESCAPE;
        fast55_rans_put(state, primary_symbols[region][cluster][primary],
                        &cursor);
    }
    Fast55Buffer frequency_bytes = {0};
    frequency_bytes.limit = SIZE_MAX;
    int frequency_ok = 1;
    for (int region = 0; region < region_count && frequency_ok; ++region)
        for (int cluster = 0; cluster < cluster_count && frequency_ok;
             ++cluster)
            frequency_ok = fast55_frequency_table(
                &frequency_bytes, primary_frequencies[region][cluster],
                FAST55_PRIMARY);
    if (frequency_ok)
        frequency_ok = fast55_frequency_table(
            &frequency_bytes, tail_frequencies, tail_alphabet);
    Enc frequency_encoder;
    enc_init(&frequency_encoder, ADAPT_DEFAULT);
    enc_reserve(&frequency_encoder, frequency_bytes.size + 64u);
    Fast55FrequencyModel frequency_model;
    probabilities_init((Prob *)&frequency_model,
                       sizeof(frequency_model) / sizeof(Prob));
    const uint16_t *previous_frequencies = NULL;
    for (int region = 0; region < region_count; ++region)
        for (int cluster = 0; cluster < cluster_count; ++cluster) {
            fast55_encode_frequency_range(
                &frequency_encoder, &frequency_model,
                primary_frequencies[region][cluster], previous_frequencies,
                FAST55_PRIMARY);
            previous_frequencies = primary_frequencies[region][cluster];
        }
    fast55_encode_frequency_range(
        &frequency_encoder, &frequency_model, tail_frequencies, NULL,
        tail_alphabet);
    enc_flush(&frequency_encoder);
    int range_frequencies = !frequency_encoder.oom &&
                            !frequency_encoder.cut &&
                            frequency_encoder.len + 4u <
                                frequency_bytes.size;
    Fast55Buffer block = {0};
    block.limit = output->limit - output->size;
    uint8_t packed_mapping[FAST55_CONTEXTS_MAX];
    size_t packed_mapping_size = cluster_count <= 16
                                     ? fast55_pack_mapping(
                                           mapping, packed_mapping,
                                           (size_t)context_count)
                                     : SIZE_MAX;
    uint8_t *context_mapping = NULL;
    size_t context_mapping_size = 0;
    int context_mapping_ok = fast55_encode_context_mapping(
        mapping, cluster_count, (size_t)context_count, &context_mapping,
        &context_mapping_size);
    size_t mapping_size = cluster_count <= 16
                              ? (size_t)context_count / 2u : SIZE_MAX;
    int mapping_method = cluster_count <= 16 ? 0 : 2;
    if (cluster_count <= 16 && packed_mapping_size < mapping_size) {
        mapping_size = packed_mapping_size;
        mapping_method = 1;
    }
    if (context_mapping_ok && context_mapping_size + 4u < mapping_size) {
        mapping_method = 2;
    }
    uint8_t header[8] = {'R','5', region_count == 1 ? '5' : '7','P',
                         (uint8_t)depth,
                         (uint8_t)cluster_count,
                         (uint8_t)mapping_method,
                         (uint8_t)(use_runs | (range_frequencies ? 2 : 0) |
                                    ((region_count - 1) << 2) |
                                     (region_split == 1 ? 8 : 0) |
                                     FAST55_SIMPLE_CONTEXT_FLAG |
                                     (paired_context
                                          ? FAST55_PAIRED_CONTEXT_FLAG
                                          : 0))};
    int ok = frequency_ok &&
             (context_mapping_ok || cluster_count <= 16);
    if (ok) ok = fast55_append(&block, header, sizeof(header));
    if (mapping_method == 2) {
        if (context_mapping_size > UINT32_MAX) ok = 0;
        if (ok) ok = fast55_u32(&block, (uint32_t)context_mapping_size) &&
                     fast55_append(&block, context_mapping,
                                   context_mapping_size);
    } else if (mapping_method == 1) {
        if (ok) ok = fast55_append(&block, packed_mapping,
                                   packed_mapping_size);
    } else {
        for (int context = 0; context < context_count && ok;
             context += 2) {
            uint8_t value = (uint8_t)(mapping[context] |
                                      (mapping[context + 1] << 4));
            ok = fast55_append(&block, &value, 1);
        }
    }
    if (range_frequencies) {
        if (frequency_encoder.len > UINT32_MAX) ok = 0;
        if (ok) ok = fast55_u32(&block, (uint32_t)frequency_encoder.len) &&
                     fast55_append(&block, frequency_encoder.buf,
                                   frequency_encoder.len);
    } else if (ok) {
        ok = fast55_append(&block, frequency_bytes.data,
                           frequency_bytes.size);
    }
    if (run_stream->size > UINT32_MAX) ok = 0;
    if (ok) ok = fast55_u32(&block, (uint32_t)run_stream->size) &&
                 fast55_append(&block, run_stream->data, run_stream->size);
    size_t ans_bytes = (size_t)(scratch + scratch_size - cursor) +
                       FAST55_LANES * sizeof(uint32_t);
    if (ans_bytes > UINT32_MAX) ok = 0;
    if (ok) ok = fast55_u32(&block, (uint32_t)ans_bytes);
    for (int lane = 0; lane < FAST55_LANES && ok; ++lane)
        ok = fast55_u32(&block, states[lane]);
    if (ok) ok = fast55_append(
        &block, cursor, ans_bytes - FAST55_LANES * sizeof(uint32_t));
    if (ok && block.size <= UINT32_MAX)
        ok = fast55_u32(output, (uint32_t)block.size) &&
             fast55_append(output, block.data, block.size);
    free(context_mapping);
    free(frequency_encoder.buf);
    free(frequency_bytes.data);
    free(block.data);
    free(scratch);
    free(tail_frequencies);
    free(tail_symbols);
    return ok ? STREAM_OK : STREAM_E_FORMAT;
}

static int fast55_encode_plane(Fast55Buffer *output, const uint16_t *plane,
                               const uint8_t *map, uint32_t width,
                               uint32_t height, int depth, int tile_log,
                               uint8_t *state_out, const uint8_t *state_in) {
    size_t pixels = (size_t)width * height;
    int paired_context = state_in != NULL;
    int paired_final = paired_context && !state_out;
    int context_count = paired_final ? FAST55_PAIRED_CONTEXTS
                                     : FAST55_BASE_CONTEXTS;
    if (pixels > SIZE_MAX / (sizeof(uint16_t) * 2u)) return STREAM_E_DIM;
    uint16_t *storage = malloc(pixels * sizeof(uint16_t) * 2u);
    uint8_t *regions = malloc(pixels);
    uint8_t *up_error = calloc(width, 1u);
    uint32_t *histogram = calloc(
        (size_t)context_count * FAST55_PRIMARY, sizeof(uint32_t));
    if (!storage || !regions || !up_error || !histogram) {
        free(histogram);
        free(up_error);
        free(regions);
        free(storage);
        return STREAM_E_ALLOC;
    }
    uint16_t *symbols = storage;
    uint16_t *contexts = storage + pixels;
    uint32_t w = width;
    int half = 1 << (depth - 1);
    int maximum = (1 << depth) - 1;
    uint32_t tiles_x = (width + (1u << tile_log) - 1u) >> tile_log;
    size_t zero_run = 0;
    size_t run_samples = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t *row = plane + (size_t)y * width;
        const uint16_t *up = y ? row - width : row;
        const uint16_t *up2 = y > 1 ? up - width : up;
        uint8_t left_error = 0;
        for (uint32_t x = 0; x < width; ++x) {
            int Wv, Nv, NWv, NEv; NEIGHBORS();
            int WWv = x > 1 ? row[x - 2u] : Wv;
            int NNv = y > 1 ? up2[x] : Nv;
            int predictor = map[(size_t)(y >> tile_log) * tiles_x +
                                (x >> tile_log)];
            int prediction = predicta_impl(predictor, Wv, Nv, NWv,
                                           NEv, WWv, NNv, maximum);
            int error = ((row[x] - prediction + half) & maximum) - half;
            unsigned symbol = error < 0 ? (unsigned)(-2 * error - 1) :
                                          (unsigned)(2 * error);
            if (!symbol) {
                ++zero_run;
            } else {
                if (zero_run >= FAST55_RUN_MIN) run_samples += zero_run;
                zero_run = 0;
            }
            size_t index = (size_t)y * width + x;
            int channel_context = state_in ? state_in[index] : 0;
            int cross = channel_context;
            if (paired_final) {
                int current = channel_context & 15;
                int previous = channel_context >> 4;
                cross = current ? current : previous ? previous + 4 : 0;
            }
            int activity;
            int reference = fast55_activity_hint_reference(
                Wv, Nv, NWv, NEv, WWv, NNv, &activity);
            int hint = predictor == 13 ? 1 :
                       (reference > prediction) - (reference < prediction) + 1;
            int context = fast55_activity(activity) +
                          12 * fast55_error_state(
                              left_error, up_error[x]) +
                          decode_predictor_context[predictor] +
                          288 * hint + 864 * cross;
            symbols[index] = (uint16_t)symbol;
            contexts[index] = (uint16_t)context;
            if (state_out) {
                uint8_t next = fast55_channel_state(symbol);
                if (state_out == state_in)
                    next = (uint8_t)((channel_context << 4) | next);
                state_out[index] = next;
            }
            left_error = fast55_residual_lut[symbol].error_state;
            up_error[x] = left_error;
        }
    }
    if (zero_run >= FAST55_RUN_MIN) run_samples += zero_run;

    Fast55Buffer run_stream = {0};
    run_stream.limit = SIZE_MAX;
    int use_runs = run_samples > pixels / 2u;
    int tail_alphabet = (1 << depth) - FAST55_DIRECT;
    uint64_t tail_counts[512] = {0};
    size_t events = 0;
    for (size_t input = 0; input < pixels;) {
        if (use_runs && !symbols[input]) {
            size_t end = input + 1u;
            while (end < pixels && !symbols[end]) ++end;
            size_t run = end - input;
            if (run >= FAST55_RUN_MIN) {
                symbols[events] = UINT16_MAX;
                contexts[events] = contexts[input];
                regions[events] = (uint8_t)(
                    (uint64_t)(input / width) * 4u / height);
                if (run > UINT32_MAX ||
                    !fast55_uint(&run_stream,
                                 (uint32_t)run - FAST55_RUN_MIN)) {
                    free(run_stream.data);
                    free(histogram);
                    free(up_error);
                    free(regions);
                    free(storage);
                    return STREAM_E_ALLOC;
                }
                ++events;
                input = end;
                continue;
            }
        }
        symbols[events] = symbols[input];
        contexts[events] = contexts[input];
        regions[events] = (uint8_t)(
            (uint64_t)(input / width) * 4u / height);
        if (symbols[events] >= FAST55_DIRECT) {
            unsigned tail = symbols[events] - FAST55_DIRECT;
            ++tail_counts[tail];
        }
        ++events;
        ++input;
    }
    for (size_t event = 0; event < events; ++event) {
        unsigned primary = symbols[event] == UINT16_MAX
                               ? FAST55_RUN
                               : symbols[event] < FAST55_DIRECT
                                     ? symbols[event] : FAST55_ESCAPE;
        size_t offset = (size_t)contexts[event] * FAST55_PRIMARY + primary;
        ++histogram[offset];
    }

    uint8_t mapping[FAST55_CONTEXTS_MAX];
    int cluster_count = FAST55_CLUSTERS;
    fast55_cluster(histogram, mapping, cluster_count, context_count);
    fast55_refine_normalized(histogram, mapping, cluster_count,
                             context_count);
    size_t cluster_stride =
        (size_t)cluster_count * FAST55_PRIMARY;
    uint64_t *cluster_storage = calloc(
        7u * cluster_stride, sizeof(*cluster_storage));
    if (!cluster_storage) {
        free(run_stream.data);
        free(histogram);
        free(up_error);
        free(regions);
        free(storage);
        return STREAM_E_ALLOC;
    }
    uint64_t *global_counts = cluster_storage;
    uint64_t *quarter_counts = global_counts + cluster_stride;
    uint64_t *regional_counts = quarter_counts + 4u * cluster_stride;
    for (size_t event = 0; event < events; ++event) {
        unsigned primary = symbols[event] == UINT16_MAX
                               ? FAST55_RUN
                               : symbols[event] < FAST55_DIRECT
                                     ? symbols[event] : FAST55_ESCAPE;
        size_t offset =
            (size_t)mapping[contexts[event]] * FAST55_PRIMARY + primary;
        ++global_counts[offset];
        ++quarter_counts[(size_t)regions[event] * cluster_stride + offset];
    }
    uint64_t half_cost;
    uint64_t quarter_cost;
    fast55_partition_costs(quarter_counts, cluster_count,
                           &half_cost, &quarter_cost);
    int region_split = quarter_cost < half_cost ? 1 : 2;
    for (int source_region = 0; source_region < 4; ++source_region) {
        int region = source_region >= region_split;
        uint64_t *destination =
            regional_counts + (size_t)region * cluster_stride;
        const uint64_t *source =
            quarter_counts + (size_t)source_region * cluster_stride;
        for (size_t entry = 0; entry < cluster_stride; ++entry)
            destination[entry] += source[entry];
    }
    size_t remaining = output->limit - output->size;
    Fast55Buffer global_output = {0};
    Fast55Buffer regional_output = {0};
    global_output.limit = remaining;
    regional_output.limit = remaining;
    int result = fast55_encode_plane_mapping(
        &global_output, symbols, contexts, regions, events, global_counts,
        tail_counts, tail_alphabet, mapping, cluster_count, context_count,
        paired_context, 1, 0, &run_stream, use_runs, pixels, depth);
    int regional_result = fast55_encode_plane_mapping(
        &regional_output, symbols, contexts, regions, events,
        regional_counts, tail_counts, tail_alphabet, mapping,
        cluster_count, context_count, paired_context, 2, region_split,
        &run_stream,
        use_runs, pixels, depth);
    Fast55Buffer *selected = &global_output;
    if (regional_result == STREAM_OK &&
        (result != STREAM_OK || regional_output.size < selected->size)) {
        selected = &regional_output;
        result = STREAM_OK;
    }
    if (result == STREAM_OK &&
        !fast55_append(output, selected->data, selected->size))
        result = STREAM_E_FORMAT;
    free(regional_output.data);
    free(global_output.data);
    free(cluster_storage);
    free(run_stream.data);
    free(histogram);
    free(up_error);
    free(regions);
    free(storage);
    return result;
}

static int fast55_stream_encode(const EncCtx *context, int transform,
                                int tile_log, size_t limit, uint8_t **out,
                                size_t *out_size) {
    if (context->ch != 3 || context->gray || !tile_log)
        return STREAM_E_FORMAT;
    model_ensure();
    size_t pixels = (size_t)context->w * context->h;
    uint16_t *owned_planes = NULL;
    uint16_t *planes[3];
    int error;
    if (context->transform_plane_cache) {
        error = cached_transform_planes(context, transform, planes);
        if (error != STREAM_OK) return error;
    } else {
        if (pixels > SIZE_MAX / (6u * sizeof(uint8_t))) return STREAM_E_DIM;
        owned_planes = malloc(pixels * 3u * sizeof(uint16_t));
        if (!owned_planes) return STREAM_E_ALLOC;
        planes[0] = owned_planes;
        planes[1] = owned_planes + pixels;
        planes[2] = owned_planes + pixels * 2u;
        fwd_transform(context->pix, pixels, (int)context->stride, transform,
                      planes);
    }
    int depth[3] = {8, transform ? 9 : 8, transform ? 9 : 8};
    int map_kind = (transform != 5 || tile_log != 3)
                       ? MAP37_REUSE_PENALTY : 0;
    if (transform == 35) map_kind = 4;
    const uint8_t *maps[3] = {0};
    uint8_t *owned_maps[3] = {0};
    uint32_t tiles_x = (context->w + (1u << tile_log) - 1u) >> tile_log;
    uint32_t tiles_y = (context->h + (1u << tile_log) - 1u) >> tile_log;
    size_t tiles = (size_t)tiles_x * tiles_y;
    for (int plane = 0; plane < 3; ++plane) {
        error = cached_map37(context, transform, tile_log, plane,
                             planes[plane], depth[plane], map_kind,
                             &maps[plane]);
        if (error != STREAM_OK) break;
        if (!maps[plane]) {
            error = predictor_map37(planes[plane], context->w, context->h,
                                    depth[plane], tile_log, map_kind, 0,
                                    &owned_maps[plane], NULL, NULL);
            maps[plane] = owned_maps[plane];
            if (error != STREAM_OK) break;
        }
    }
    if (error != STREAM_OK) {
        for (int plane = 0; plane < 3; ++plane) free(owned_maps[plane]);
        free(owned_planes);
        return error;
    }

    Enc map_encoder;
    enc_init(&map_encoder, ADAPT_DEFAULT);
    map_encoder.max = limit;
    enc_reserve(&map_encoder, tiles * 2u + 64u);
    for (int plane = 0; plane < 3 && !map_encoder.cut; ++plane)
        fast55_encode_map(&map_encoder, maps[plane], tiles, tiles_x);
    enc_flush(&map_encoder);
    if (map_encoder.oom || map_encoder.cut || map_encoder.len > UINT32_MAX) {
        for (int plane = 0; plane < 3; ++plane) free(owned_maps[plane]);
        free(owned_planes);
        free(map_encoder.buf);
        return map_encoder.oom ? STREAM_E_ALLOC : STREAM_E_FORMAT;
    }

    Fast55Buffer output = {0};
    output.limit = limit;
    uint8_t native_header[STREAM_HDR] = {0};
    int ok = fast55_append(&output, native_header, sizeof(native_header)) &&
             fast55_append(&output, "Q55A", 4) &&
             fast55_u32(&output, (uint32_t)map_encoder.len) &&
             fast55_append(&output, map_encoder.buf, map_encoder.len);
    uint8_t *state = ok ? malloc(pixels) : NULL;
    if (ok && !state) {
        ok = 0;
        error = STREAM_E_ALLOC;
    }
    if (ok) {
        error = fast55_encode_plane(&output, planes[0], maps[0], context->w,
                                    context->h, depth[0], tile_log, state,
                                    NULL);
        if (error == STREAM_OK)
            error = fast55_encode_plane(&output, planes[1], maps[1],
                                        context->w, context->h, depth[1],
                                        tile_log, state, state);
        if (error == STREAM_OK)
            error = fast55_encode_plane(&output, planes[2], maps[2],
                                        context->w, context->h, depth[2],
                                        tile_log, NULL, state);
        ok = error == STREAM_OK;
    }
    free(state);
    free(map_encoder.buf);
    for (int plane = 0; plane < 3; ++plane) free(owned_maps[plane]);
    free(owned_planes);
    if (!ok) {
        free(output.data);
        return error == STREAM_E_ALLOC ? error : STREAM_E_FORMAT;
    }
    if (output.size < STREAM_HDR || output.size - STREAM_HDR > UINT32_MAX) {
        free(output.data);
        return STREAM_E_DIM;
    }
    uint8_t *header = output.data;
    memcpy(header, "QST1", 4);
    put32(header + 4, context->w);
    put32(header + 8, context->h);
    header[12] = 3;
    header[13] = 0;
    header[14] = 55;
    header[15] = (uint8_t)transform;
    header[16] = (uint8_t)tile_log;
    header[17] = 0;
    put32(header + 18, context->crc);
    put32(header + 22, (uint32_t)(output.size - STREAM_HDR));
    put32(header + 26, 0);
    put32(header + 26, container_crc32(header, output.size));
    *out = output.data;
    *out_size = output.size;
    return STREAM_OK;
}

typedef struct {
    uint16_t symbol;
    uint16_t start;
    uint16_t frequency;
} Fast55DecodeEntry;

typedef struct {
    const uint8_t *cursor;
    const uint8_t *end;
} Fast55Reader;

static int fast55_read(Fast55Reader *reader, void *destination, size_t size) {
    if (size > (size_t)(reader->end - reader->cursor)) return 0;
    memcpy(destination, reader->cursor, size);
    reader->cursor += size;
    return 1;
}

static int fast55_read_mapping(Fast55Reader *reader, int packed,
                               int cluster_count, size_t context_count,
                               uint8_t *mapping) {
    if (packed == 2) {
        uint8_t bytes[4];
        if (!fast55_read(reader, bytes, sizeof(bytes))) return 0;
        uint32_t size = get32(bytes);
        if (size > (size_t)(reader->end - reader->cursor)) return 0;
        Dec decoder;
        dec_init(&decoder, reader->cursor, size, ADAPT_DEFAULT);
        reader->cursor += size;
        Prob tree[64];
        Prob same[5];
        probabilities_init(tree, 64);
        probabilities_init(same, 5);
        for (size_t context = 0; context < context_count; ++context) {
            size_t reference = 0;
            int dimension = -1;
            if (context % 12u) {
                reference = context - 1u;
                dimension = 0;
            } else if ((context / 12u) % 6u) {
                reference = context - 12u;
                dimension = 1;
            } else if ((context / 72u) % 4u) {
                reference = context - 72u;
                dimension = 2;
            } else if ((context / 288u) % 3u) {
                reference = context - 288u;
                dimension = 3;
            } else if (context >= 864u) {
                reference = context - 864u;
                dimension = 4;
            }
            if (dimension >= 0 &&
                !dec_bit(&decoder, &same[dimension])) {
                mapping[context] = mapping[reference];
                continue;
            }
            mapping[context] = (uint8_t)(
                cluster_count <= 16 ? dec_tree4(&decoder, tree) :
                                      dec_tree5(&decoder, tree));
        }
        return !decoder.truncated;
    }
    if (cluster_count > 16) return 0;
    if (!packed) {
        for (size_t context = 0; context < context_count; context += 2) {
            uint8_t value;
            if (!fast55_read(reader, &value, 1u)) return 0;
            mapping[context] = value & 15u;
            mapping[context + 1] = value >> 4;
        }
        return 1;
    }
    size_t context = 0;
    while (context < context_count) {
        uint8_t token;
        if (!fast55_read(reader, &token, 1u)) return 0;
        size_t run = (token & 127u) + 1u;
        if (run > context_count - context) return 0;
        if (token & 128u) {
            memset(mapping + context, 0, run);
        } else {
            for (size_t index = 0; index < run; index += 2u) {
                uint8_t value;
                if (!fast55_read(reader, &value, 1u)) return 0;
                mapping[context + index] = value & 15u;
                if (index + 1u < run)
                    mapping[context + index + 1u] = value >> 4;
            }
        }
        context += run;
    }
    return 1;
}

static int fast55_read_u32(Fast55Reader *reader, uint32_t *value) {
    uint8_t bytes[4];
    if (!fast55_read(reader, bytes, sizeof(bytes))) return 0;
    *value = get32(bytes);
    return 1;
}

static int fast55_read_uint(Fast55Reader *reader, uint32_t *value) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 35u; shift += 7u) {
        uint8_t byte;
        if (!fast55_read(reader, &byte, 1u) ||
            (shift == 28u && (byte & 240u)))
            return 0;
        result |= (uint32_t)(byte & 127u) << shift;
        if (!(byte & 128u)) {
            *value = result;
            return 1;
        }
    }
    return 0;
}

static int fast55_decode_frequency_value(Dec *decoder,
                                         Fast55FrequencyModel *model,
                                         int context, unsigned *value) {
    int bits = 0;
    while (bits < FAST55_FREQ_BITS &&
           dec_bit(decoder, &model->unary[context][bits]))
        ++bits;
    unsigned result = bits ? 1u << (bits - 1) : 0u;
    for (int index = bits - 2; index >= 0; --index)
        result |= (unsigned)dec_bit(
                      decoder, &model->mantissa[context][bits][index])
                  << index;
    *value = result;
    return !decoder->truncated;
}

static int fast55_decode_frequency_range(
    Dec *decoder, Fast55FrequencyModel *model, uint16_t *frequencies,
    const uint16_t *previous, int alphabet) {
    unsigned remaining = FAST55_SCALE;
    for (int symbol = 0; symbol < alphabet - 1; ++symbol) {
        unsigned value;
        if (!fast55_decode_frequency_value(
                decoder, model, fast55_frequency_context(symbol), &value))
            return 0;
        int prediction = previous ? previous[symbol] : 0;
        int delta = value & 1u ? -(int)((value + 1u) >> 1) :
                                (int)(value >> 1);
        int frequency = prediction + delta;
        if (frequency < 0 || (unsigned)frequency > remaining) return 0;
        frequencies[symbol] = (uint16_t)frequency;
        remaining -= (unsigned)frequency;
    }
    frequencies[alphabet - 1] = (uint16_t)remaining;
    return 1;
}

static int fast55_fill_decode_table(const uint16_t *frequencies,
                                    int alphabet,
                                    Fast55DecodeEntry *table) {
    unsigned start = 0;
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        unsigned frequency = frequencies[symbol];
        if (frequency > FAST55_SCALE - start) return 0;
        for (unsigned slot = 0; slot < frequency; ++slot) {
            Fast55DecodeEntry *entry = table + start + slot;
            entry->symbol = (uint16_t)symbol;
            entry->start = (uint16_t)start;
            entry->frequency = (uint16_t)frequency;
        }
        start += frequency;
    }
    return start == FAST55_SCALE;
}

static int fast55_fill_primary_decode_table(
    const uint16_t *frequencies, int alphabet, uint8_t *table,
    Fast55Symbol *symbols) {
    unsigned start = 0;
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        unsigned frequency = frequencies[symbol];
        if (frequency > FAST55_SCALE - start) return 0;
        symbols[symbol].start = (uint16_t)start;
        symbols[symbol].frequency = (uint16_t)frequency;
        memset(table + start, symbol, frequency);
        start += frequency;
    }
    return start == FAST55_SCALE;
}

static int fast55_read_frequency_table(Fast55Reader *reader, int alphabet,
                                       uint16_t *frequencies) {
    unsigned start = 0;
    int zero_run = 0;
    for (int symbol = 0; symbol < alphabet; ++symbol) {
        uint16_t frequency = 0;
        if (symbol == alphabet - 1) {
            frequency = (uint16_t)(FAST55_SCALE - start);
        } else if (zero_run) {
            --zero_run;
        } else {
            uint8_t first;
            if (!fast55_read(reader, &first, 1u)) return 0;
            if (!first) {
                uint8_t run;
                if (!fast55_read(reader, &run, 1u) || !run ||
                    run > alphabet - 1 - symbol)
                    return 0;
                zero_run = run - 1;
            } else if (first & 128u) {
                uint8_t second;
                if (!fast55_read(reader, &second, 1u) ||
                    second > (FAST55_SCALE >> 7))
                    return 0;
                frequency = (uint16_t)((first & 127u) |
                                       ((unsigned)second << 7));
            } else {
                frequency = first;
            }
            if (frequency > FAST55_SCALE - start) return 0;
        }
        frequencies[symbol] = frequency;
        start += frequency;
    }
    return start == FAST55_SCALE;
}

static QLIC_FORCEINLINE int fast55_rans_get(
    uint32_t *state, const Fast55DecodeEntry *table,
    const uint8_t **cursor, const uint8_t *end, unsigned *symbol) {
    unsigned slot = *state & (FAST55_SCALE - 1u);
    Fast55DecodeEntry entry = table[slot];
    *symbol = entry.symbol;
    *state = (uint32_t)entry.frequency * (*state >> FAST55_SCALE_BITS) +
             slot - entry.start;
    while (*state < FAST55_RANS_L) {
        if (*cursor >= end) return 0;
        *state = (*state << 8) | *(*cursor)++;
    }
    return 1;
}

static QLIC_FORCEINLINE int fast55_rans_get_primary(
    uint32_t *state, const uint8_t *table, const Fast55Symbol *symbols,
    const uint8_t **cursor, const uint8_t *end, unsigned *symbol) {
    unsigned slot = *state & (FAST55_SCALE - 1u);
    *symbol = table[slot];
    Fast55Symbol entry = symbols[*symbol];
    *state = (uint32_t)entry.frequency * (*state >> FAST55_SCALE_BITS) +
             slot - entry.start;
    while (*state < FAST55_RANS_L) {
        if (*cursor >= end) return 0;
        *state = (*state << 8) | *(*cursor)++;
    }
    return 1;
}

static int fast55_decode_plane(const uint8_t *data, size_t size,
                               uint16_t *plane, const uint8_t *map,
                               uint32_t width, uint32_t height, int depth,
                               int tile_log, uint8_t *state_out,
                               const uint8_t *state_in) {
    Fast55Reader reader = {data, data + size};
    uint8_t header[8];
    if (!fast55_read(&reader, header, sizeof(header)))
        return STREAM_E_CORRUPT;
    int regional = !memcmp(header, "R57P", 4);
    int simple_context = !!(header[7] & FAST55_SIMPLE_CONTEXT_FLAG);
    int paired_context = !!(header[7] & FAST55_PAIRED_CONTEXT_FLAG);
    if ((!regional && memcmp(header, "R55P", 4)) || header[4] != depth ||
        !header[5] || header[5] > FAST55_CLUSTERS || header[6] > 2u ||
        (regional && !(header[7] & 4u)) ||
        (regional ? (header[7] & ~63u) : (header[7] & ~51u)))
        return STREAM_E_CORRUPT;
    int cluster_count = header[5];
    int region_count = regional ? 2 : 1;
    int region_split = regional && (header[7] & 8u) ? 1 : 2;
    int paired_final = paired_context && state_in && !state_out;
    int context_count = paired_final ? FAST55_PAIRED_CONTEXTS
                                     : FAST55_BASE_CONTEXTS;
    if (region_count > FAST55_REGIONS) return STREAM_E_CORRUPT;
    int use_runs = header[7] & 1u;
    int range_frequencies = header[7] & 2u;
    uint8_t mapping[FAST55_CONTEXTS_MAX];
    if (!fast55_read_mapping(&reader, header[6], cluster_count,
                             (size_t)context_count, mapping))
        return STREAM_E_CORRUPT;
    for (int context = 0; context < context_count; ++context)
        if (mapping[context] >= cluster_count)
            return STREAM_E_CORRUPT;
    size_t primary_entries =
        (size_t)region_count * (size_t)cluster_count * FAST55_SCALE;
    size_t primary_symbol_entries =
        (size_t)region_count * (size_t)cluster_count * FAST55_PRIMARY;
    int tail_alphabet = (1 << depth) - FAST55_DIRECT;
    size_t primary_bytes = primary_entries * sizeof(uint8_t);
    size_t primary_symbol_bytes =
        primary_symbol_entries * sizeof(Fast55Symbol);
    size_t table_bytes = primary_bytes + primary_symbol_bytes +
                          FAST55_SCALE * sizeof(Fast55DecodeEntry);
    uint8_t *table_storage = malloc(table_bytes);
    uint8_t *tables = table_storage;
    Fast55Symbol *primary_symbols =
        (Fast55Symbol *)(table_storage + primary_bytes);
    Fast55DecodeEntry *tail_table =
        (Fast55DecodeEntry *)(table_storage + primary_bytes +
                              primary_symbol_bytes);
    uint8_t *up_error = calloc(width, 1u);
    if (!table_storage || !up_error) {
        free(up_error); free(table_storage);
        return STREAM_E_ALLOC;
    }
    if (range_frequencies) {
        uint32_t frequency_size;
        if (!fast55_read_u32(&reader, &frequency_size) ||
            frequency_size > (size_t)(reader.end - reader.cursor)) {
            free(up_error); free(table_storage);
            return STREAM_E_CORRUPT;
        }
        Dec frequency_decoder;
        dec_init(&frequency_decoder, reader.cursor, frequency_size,
                 ADAPT_DEFAULT);
        reader.cursor += frequency_size;
        Fast55FrequencyModel frequency_model;
        probabilities_init((Prob *)&frequency_model,
                           sizeof(frequency_model) / sizeof(Prob));
        uint16_t previous[FAST55_PRIMARY];
        uint16_t current[FAST55_PRIMARY];
        const uint16_t *previous_table = NULL;
        for (int region = 0; region < region_count; ++region) {
            for (int cluster = 0; cluster < cluster_count; ++cluster) {
                if (!fast55_decode_frequency_range(
                        &frequency_decoder, &frequency_model, current,
                        previous_table, FAST55_PRIMARY) ||
                    !fast55_fill_primary_decode_table(
                        current, FAST55_PRIMARY,
                        tables + ((size_t)region * (size_t)cluster_count +
                                  (size_t)cluster) * FAST55_SCALE,
                        primary_symbols +
                            ((size_t)region * (size_t)cluster_count +
                             (size_t)cluster) * FAST55_PRIMARY)) {
                    free(up_error); free(table_storage);
                    return STREAM_E_CORRUPT;
                }
                memcpy(previous, current, sizeof(previous));
                previous_table = previous;
            }
        }
        uint16_t tail_frequencies[512];
        if (!fast55_decode_frequency_range(
                &frequency_decoder, &frequency_model, tail_frequencies,
                NULL, tail_alphabet) ||
            !fast55_fill_decode_table(tail_frequencies, tail_alphabet,
                                      tail_table) ||
            frequency_decoder.truncated) {
            free(up_error); free(table_storage);
            return STREAM_E_CORRUPT;
        }
    } else {
        uint16_t frequencies[FAST55_PRIMARY];
        for (int region = 0; region < region_count; ++region)
            for (int cluster = 0; cluster < cluster_count; ++cluster)
                if (!fast55_read_frequency_table(
                        &reader, FAST55_PRIMARY, frequencies) ||
                    !fast55_fill_primary_decode_table(
                        frequencies, FAST55_PRIMARY,
                        tables + ((size_t)region * (size_t)cluster_count +
                                  (size_t)cluster) * FAST55_SCALE,
                        primary_symbols +
                            ((size_t)region * (size_t)cluster_count +
                             (size_t)cluster) * FAST55_PRIMARY)) {
                    free(up_error); free(table_storage);
                    return STREAM_E_CORRUPT;
                }
        uint16_t tail_frequencies[512];
        if (!fast55_read_frequency_table(
                &reader, tail_alphabet, tail_frequencies) ||
            !fast55_fill_decode_table(
                tail_frequencies, tail_alphabet, tail_table)) {
            free(up_error); free(table_storage);
            return STREAM_E_CORRUPT;
        }
    }
    uint32_t run_size;
    if (!fast55_read_u32(&reader, &run_size) ||
        run_size > (size_t)(reader.end - reader.cursor)) {
        free(up_error); free(table_storage);
        return STREAM_E_CORRUPT;
    }
    Fast55Reader runs = {reader.cursor, reader.cursor + run_size};
    reader.cursor += run_size;
    uint32_t ans_size;
    if (!fast55_read_u32(&reader, &ans_size) ||
        ans_size < FAST55_LANES * sizeof(uint32_t) ||
        ans_size > (size_t)(reader.end - reader.cursor)) {
        free(up_error); free(table_storage);
        return STREAM_E_CORRUPT;
    }
    Fast55Reader ans = {reader.cursor, reader.cursor + ans_size};
    reader.cursor += ans_size;
    if (reader.cursor != reader.end) {
        free(up_error); free(table_storage);
        return STREAM_E_CORRUPT;
    }
    uint32_t states[FAST55_LANES];
    for (int lane = 0; lane < FAST55_LANES; ++lane)
        if (!fast55_read_u32(&ans, &states[lane]) ||
            states[lane] < FAST55_RANS_L) {
            free(up_error); free(table_storage);
            return STREAM_E_CORRUPT;
        }
    const uint8_t *ans_cursor = ans.cursor;
    uint32_t w = width;
    int half = 1 << (depth - 1);
    int maximum = (1 << depth) - 1;
    size_t pixels = (size_t)width * height;
    uint32_t tiles_x = (width + (1u << tile_log) - 1u) >> tile_log;
    size_t index = 0;
    size_t event = 0;
    uint32_t run_remaining = 0;
    uint32_t region_row = region_split == 1 ? (height + 3u) / 4u :
                                              (height + 1u) / 2u;
    const uint8_t *input_state = state_in;
    uint8_t *output_state = state_out;
    int pack_state = paired_context && state_out && state_in &&
                     state_out == state_in;
    for (uint32_t y = 0; y < height; ++y) {
        uint16_t *row = plane + (size_t)y * width;
        const uint16_t *up = y ? row - width : row;
        const uint16_t *up2 = y > 1 ? up - width : up;
        const uint8_t *tile_map =
            map + (size_t)(y >> tile_log) * tiles_x;
        int entropy_region =
            region_count == 1 ? 0 : y >= region_row;
        const uint8_t *region_tables =
            tables + (size_t)entropy_region * (size_t)cluster_count *
                          FAST55_SCALE;
        const Fast55Symbol *region_symbols =
            primary_symbols +
            (size_t)entropy_region * (size_t)cluster_count * FAST55_PRIMARY;
        uint8_t left_error = 0;
#define FAST55_DECODE_RANGE(PREDICTION, REFERENCE_PREDICTOR) do { \
        while (x < tile_end) { \
            int Wv, Nv, NWv, NEv; NEIGHBORS(); \
            int WWv = x > 1 ? row[x - 2u] : Wv; \
            int NNv = y > 1 ? up2[x] : Nv; \
            int prediction; \
            unsigned symbol = 0; \
            int channel_context__ = input_state ? *input_state++ : 0; \
            if (run_remaining) { \
                prediction = (PREDICTION); \
                --run_remaining; \
            } else { \
                int cross = channel_context__; \
                if (paired_context && state_in && !state_out) { \
                    int current__ = channel_context__ & 15; \
                    int previous__ = channel_context__ >> 4; \
                    cross = current__ ? current__ : \
                            previous__ ? previous__ + 4 : 0; \
                } \
                int context; \
                if (simple_context) { \
                    prediction = (PREDICTION); \
                    int activity; \
                    int reference = fast55_activity_hint_reference( \
                        Wv, Nv, NWv, NEv, WWv, NNv, &activity); \
                    (void)reference; \
                    int hint = (REFERENCE_PREDICTOR) ? 1 : \
                               (reference > prediction) - \
                               (reference < prediction) + 1; \
                    context = fast55_activity(activity) + \
                              12 * fast55_error_state( \
                                  left_error, up_error[x]) + \
                              predictor_context + \
                              288 * hint + 864 * cross; \
                } else { \
                    int activity; \
                    int reference = fast55_activity_reference( \
                        Wv, Nv, NWv, NEv, WWv, NNv, maximum, &activity); \
                    prediction = (REFERENCE_PREDICTOR) ? reference : \
                                                         (PREDICTION); \
                    int hint = (reference > prediction) - \
                               (reference < prediction) + 1; \
                    context = fast55_activity(activity) + \
                              12 * fast55_error_state( \
                                  left_error, up_error[x]) + \
                              predictor_context + \
                              288 * hint + 864 * cross; \
                } \
                int cluster = mapping[context]; \
                uint32_t *state = \
                    &states[event & (FAST55_LANES - 1u)]; \
                ++event; \
                 if (!fast55_rans_get_primary( \
                         state, region_tables + \
                             (size_t)cluster * FAST55_SCALE, \
                         region_symbols + \
                             (size_t)cluster * FAST55_PRIMARY, \
                         &ans_cursor, ans.end, &symbol)) { \
                    free(up_error); free(table_storage); \
                    return STREAM_E_CORRUPT; \
                } \
                if (symbol == FAST55_RUN) { \
                    uint32_t stored_run; \
                    if (!use_runs || \
                        !fast55_read_uint(&runs, &stored_run) || \
                        stored_run > UINT32_MAX - FAST55_RUN_MIN || \
                        (size_t)stored_run + FAST55_RUN_MIN > \
                            pixels - index) { \
                        free(up_error); free(table_storage); \
                        return STREAM_E_CORRUPT; \
                    } \
                    run_remaining = stored_run + FAST55_RUN_MIN - 1u; \
                    symbol = 0; \
                } else if (symbol == FAST55_ESCAPE) { \
                    unsigned tail; \
                    if (!fast55_rans_get( \
                            state, tail_table, &ans_cursor, ans.end, &tail)) { \
                        free(up_error); free(table_storage); \
                        return STREAM_E_CORRUPT; \
                    } \
                    symbol += tail; \
                } \
                if (symbol > (unsigned)maximum) { \
                    free(up_error); free(table_storage); \
                    return STREAM_E_CORRUPT; \
                } \
            } \
            Fast55Residual residual__ = fast55_residual_lut[symbol]; \
            row[x] = (uint16_t)((prediction + residual__.error) & maximum); \
            if (output_state) { \
                uint8_t next__ = residual__.channel_state; \
                if (pack_state) \
                    next__ = (uint8_t)((channel_context__ << 4) | next__); \
                *output_state++ = next__; \
            } \
            left_error = residual__.error_state; \
            up_error[x] = left_error; \
            ++index; \
            ++x; \
        } \
    } while (0)
        uint32_t x = 0;
        for (uint32_t tile = 0; tile < tiles_x; ++tile) {
            uint32_t tile_end = (tile + 1u) << tile_log;
            if (tile_end > width) tile_end = width;
            int predictor = tile_map[tile];
            int predictor_context = decode_predictor_context[predictor];
            switch (predictor) {
            case 0: FAST55_DECODE_RANGE(
                        predict(0, Wv, Nv, NWv, NEv, maximum), 0); break;
            case 1: FAST55_DECODE_RANGE(paethp(Wv, Nv, NWv), 0); break;
            case 2: FAST55_DECODE_RANGE(Wv, 0); break;
            case 3: FAST55_DECODE_RANGE(Nv, 0); break;
            case 4: FAST55_DECODE_RANGE((Wv + Nv + 1) >> 1, 0); break;
            case 5: FAST55_DECODE_RANGE(
                        clampi(Nv + Wv - NWv, 0, maximum), 0); break;
            case 6: FAST55_DECODE_RANGE(NEv, 0); break;
            case 7: FAST55_DECODE_RANGE((Wv + NEv + 1) >> 1, 0); break;
            case 8: FAST55_DECODE_RANGE((Nv + NEv + 1) >> 1, 0); break;
            case 9: FAST55_DECODE_RANGE(
                        clampi(2 * Wv - WWv, 0, maximum), 0); break;
            case 10: FAST55_DECODE_RANGE(
                         clampi(2 * Nv - NNv, 0, maximum), 0); break;
            case 11: FAST55_DECODE_RANGE(
                         clampi(Wv + (((Nv - NWv) * 3) >> 2), 0, maximum),
                         0); break;
            case 12: FAST55_DECODE_RANGE(
                         clampi(Nv + (((Wv - NWv) * 3) >> 2), 0, maximum),
                         0); break;
            case 13: FAST55_DECODE_RANGE(
                         gapp(Wv, Nv, NWv, NEv, WWv, NNv, maximum), 1);
                     break;
            case 14: FAST55_DECODE_RANGE(
                         clampi((Wv + Nv + NEv + NWv + 2) >> 2,
                                0, maximum), 0); break;
            case 15: FAST55_DECODE_RANGE(
                         clampi((5 * Wv + 2 * Nv - 3 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            case 16: FAST55_DECODE_RANGE(
                         clampi((Wv + 3 * Nv + 2) >> 2, 0, maximum), 0);
                     break;
            case 17: FAST55_DECODE_RANGE(
                         clampi((3 * Wv + Nv + 2) >> 2, 0, maximum), 0);
                     break;
            case 18: FAST55_DECODE_RANGE(
                         clampi((5 * Nv + 2 * Wv - 3 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            case 19: FAST55_DECODE_RANGE(
                         clampi((2 * Wv + Nv - NWv + 1) >> 1,
                                0, maximum), 0); break;
            case 20: FAST55_DECODE_RANGE(
                         clampi((Wv + 2 * Nv - NWv + 1) >> 1,
                                0, maximum), 0); break;
            case 21: FAST55_DECODE_RANGE(
                         clampi(Wv + ((NEv - NWv) >> 1), 0, maximum), 0);
                     break;
            case 22: FAST55_DECODE_RANGE(
                         clampi(Nv + ((NEv - NWv) >> 1), 0, maximum), 0);
                     break;
            case 23: FAST55_DECODE_RANGE(
                         clampi((Wv + Nv + NEv + 1) / 3, 0, maximum), 0);
                     break;
            case 24: FAST55_DECODE_RANGE(
                         clampi((2 * Wv + Nv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            case 25: FAST55_DECODE_RANGE(
                         clampi((Wv + 2 * Nv + NWv + 2) >> 2,
                                0, maximum), 0); break;
            case 26: FAST55_DECODE_RANGE(
                         clampi((3 * Wv + 3 * Nv - 2 * NWv + 2) >> 2,
                                0, maximum), 0); break;
            case 27: FAST55_DECODE_RANGE(
                         clampi((4 * Nv + Wv - 2 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            case 28: FAST55_DECODE_RANGE(
                         clampi((4 * Wv + Nv - 2 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            case 29: FAST55_DECODE_RANGE(
                         clampi((Wv + Nv + NEv - NWv + 1) >> 1,
                                0, maximum), 0); break;
            case 30: FAST55_DECODE_RANGE(
                         clampi((6 * Wv + 2 * Nv - 5 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            default: FAST55_DECODE_RANGE(
                         clampi((2 * Wv + 6 * Nv - 5 * NWv + NEv + 2) >> 2,
                                0, maximum), 0); break;
            }
        }
#undef FAST55_DECODE_RANGE
    }
    int ok = ans_cursor == ans.end && runs.cursor == runs.end &&
             !run_remaining;
    for (int lane = 0; lane < FAST55_LANES; ++lane)
        ok &= states[lane] == FAST55_RANS_L;
    free(up_error);
    free(table_storage);
    return ok ? STREAM_OK : STREAM_E_CORRUPT;
}



static int fast55_stream_decode(const uint8_t *data, size_t size,
                                uint32_t width, uint32_t height,
                                int transform, int tile_log, uint32_t crc,
                                int rgba, uint8_t **out, int *channels) {
    model_ensure();
    Fast55Reader reader = {data, data + size};
    uint8_t magic[4];
    uint32_t map_size;
    if (!fast55_read(&reader, magic, sizeof(magic)) ||
        memcmp(magic, "Q55A", 4) || !fast55_read_u32(&reader, &map_size) ||
        map_size > (size_t)(reader.end - reader.cursor))
        return STREAM_E_CORRUPT;
    uint32_t tiles_x = (width + (1u << tile_log) - 1u) >> tile_log;
    uint32_t tiles_y = (height + (1u << tile_log) - 1u) >> tile_log;
    size_t tiles = (size_t)tiles_x * tiles_y;
    if (tiles > SIZE_MAX / 3u) return STREAM_E_ALLOC;
    uint8_t *maps = malloc(tiles * 3u);
    if (!maps) return STREAM_E_ALLOC;
    Dec map_decoder;
    dec_init(&map_decoder, reader.cursor, map_size, ADAPT_DEFAULT);
    reader.cursor += map_size;
    int error = STREAM_OK;
    for (int plane = 0; plane < 3 && error == STREAM_OK; ++plane)
        error = fast55_decode_map(&map_decoder, maps + (size_t)plane * tiles,
                                  tiles, tiles_x);
    size_t pixels = (size_t)width * height;
    uint16_t *plane_storage = error == STREAM_OK
                                  ? malloc(pixels * 3u * sizeof(uint16_t))
                                  : NULL;
    uint8_t *state = plane_storage ? malloc(pixels) : NULL;
    if (error == STREAM_OK && (!plane_storage || !state))
        error = STREAM_E_ALLOC;
    uint16_t *planes[3] = {
        plane_storage, plane_storage ? plane_storage + pixels : NULL,
        plane_storage ? plane_storage + pixels * 2u : NULL
    };
    int depth[3] = {8, transform ? 9 : 8, transform ? 9 : 8};
    for (int plane = 0; plane < 3 && error == STREAM_OK; ++plane) {
        uint32_t block_size;
        if (!fast55_read_u32(&reader, &block_size) ||
            block_size > (size_t)(reader.end - reader.cursor)) {
            error = STREAM_E_CORRUPT;
            break;
        }
        uint8_t *state_out = plane < 2 ? state : NULL;
        const uint8_t *state_in = plane ? state : NULL;
        error = fast55_decode_plane(
            reader.cursor, block_size, planes[plane],
            maps + (size_t)plane * tiles, width, height, depth[plane],
            tile_log, state_out, state_in);
        reader.cursor += block_size;
    }
    if (error == STREAM_OK && reader.cursor != reader.end)
        error = STREAM_E_CORRUPT;
    size_t stride = rgba ? 4u : 3u;
    uint8_t *pixels_out = error == STREAM_OK ? malloc(pixels * stride) : NULL;
    if (error == STREAM_OK && !pixels_out) error = STREAM_E_ALLOC;
    if (error == STREAM_OK) {
        inv_transform(planes, pixels, (int)stride, transform, pixels_out);
    }
    if (error == STREAM_OK && rgba)
        for (size_t index = 0; index < pixels; ++index)
            pixels_out[index * 4u + 3u] = 255;
    if (error == STREAM_OK) {
        uint32_t decoded_crc = rgba ? stream_crc32_rgbx(pixels_out, pixels) :
                                      stream_crc32(pixels_out, pixels * 3u);
        if (decoded_crc != crc) error = STREAM_E_CORRUPT;
    }
    free(state);
    free(plane_storage);
    free(maps);
    if (error != STREAM_OK) {
        free(pixels_out);
        return error;
    }
    *out = pixels_out;
    *channels = rgba ? 4 : 3;
    return STREAM_OK;
}

static int try_encode_limited(const EncCtx *c, int mode, int t, int tlog,
                              int adapt, size_t limit, uint8_t **out,
                              size_t *outn) {
    /* the limit lets a candidate stop as soon as it cannot beat the current best */
    size_t npix = (size_t)c->w * c->h;
    size_t sch = c->stride;
    size_t palb0 = mode == 1 ? 2 + (size_t)c->pal_n * (size_t)c->ch : 0;
    if (mode == 42) return split37_stream_encode(c, t, tlog, limit, out, outn);
    if (mode == 55) return fast55_stream_encode(c, t, tlog, limit, out, outn);
    if (adapt != ADAPT_DEFAULT && c->ch == 4 && c->const_alpha) adapt = ADAPT_DEFAULT;
    size_t prefix = STREAM_HDR + palb0;
    if (limit != SIZE_MAX && limit <= prefix) return STREAM_E_FORMAT;
    Enc e; enc_init(&e, adapt);
    e.len = prefix;
    e.max = limit;
    if (mode != 1) {
        size_t capacity = npix + npix / 2u;
        if (capacity < ((size_t)1u << 16))
            capacity = (size_t)1u << 16;
        if (capacity > ((size_t)1u << 20))
            capacity = (size_t)1u << 20;
        enc_reserve(&e, capacity + prefix);
    }
    int err = STREAM_OK;
    int planes_cached = c->transform_plane_cache && mode != 1 &&
                        c->ch == 3 && !c->gray;
    uint16_t *cached_planes[3] = {0};
    uint16_t *P = NULL;
    if (planes_cached) {
        err = cached_transform_planes(c, t, cached_planes);
        if (err != STREAM_OK) {
            free(e.buf);
            return err;
        }
        P = cached_planes[0];
    } else {
        P = malloc(npix * sizeof(uint16_t));
        if (!P) {
            free(e.buf);
            return STREAM_E_ALLOC;
        }
    }
    int plane_method = plane_method_for(mode);
    int context_plane =
        plane_method == PLANE_CONTEXT || plane_method == PLANE_SPARSE;
    int local_mode = mode == 45 || (mode >= 52 && mode <= 54);
    int map_kind =
        mode == 45 || ((mode >= 52 && mode <= 54) &&
                       (t != 5 || tlog != 3))
            ? MAP37_REUSE_PENALTY
            : 0;
    if (mode >= 52 && mode <= 54 && t == 35) map_kind = 4;
    if (mode == 1) {
        int depth = nbits((unsigned)(c->pal_n - 1)); if (depth < 1) depth = 1;
        for (size_t i = 0; i < npix; i++) P[i] = c->pal_idx[i];
        err = encode_plane(&e, P, c->w, c->h, depth, tlog, 0);
    } else if (c->ch == 1 || c->gray) {
        for (size_t i = 0; i < npix; i++) P[i] = c->pix[i * sch];
        const uint8_t *map = NULL;
        if (mode == 33 && tlog >= 3 && tlog <= 5 &&
            c->gray_map_cache) {
            if (!c->gray_map_cache->ready)
                err = build_gray_map_cache(c, P, 8);
            if (err == STREAM_OK && c->gray_map_cache->ready)
                map = c->gray_map_cache->map[tlog - 3];
        }
        if (err == STREAM_OK && context_plane)
            err = mode == 54
                      ? cached_weighted_map37(
                            c, 0, tlog, 0, P, 8, map_kind, &map, NULL, NULL)
                      : cached_map37(
                            c, 0, tlog, 0, P, 8, map_kind, &map);
        if (err == STREAM_OK)
            err = encode_mode_plane(c, &e, P, 8, mode, 0, tlog, 0, NULL,
                                    NULL, map, map_kind);
    } else {
        uint16_t *Q = planes_cached ? cached_planes[1] : malloc(npix * sizeof(uint16_t));
        uint16_t *R = planes_cached ? cached_planes[2] : malloc(npix * sizeof(uint16_t));
        if (!Q || !R) {
            if (!planes_cached) { free(Q); free(R); free(P); }
            free(e.buf);
            return STREAM_E_ALLOC;
        }
        uint16_t *pls[3] = {P, Q, R};
        int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
        if (!planes_cached)
            fwd_transform(c->pix, npix, (int)c->stride, t, pls);
        uint8_t *state = local_mode ? malloc(npix) : NULL;
        if (local_mode && !state) {
            if (!planes_cached) { free(Q); free(R); free(P); }
            free(e.buf);
            return STREAM_E_ALLOC;
        }
        for (int p = 0; p < 3 && err == STREAM_OK; p++) {
            int plane_map_kind = map_kind;
            if (c->map37_override) plane_map_kind = c->map37_penalty[p];
            const uint8_t *map = NULL;
            if (context_plane)
                err = mode == 54
                          ? cached_weighted_map37(
                                c, t, tlog, p, pls[p], depth[p],
                                plane_map_kind, &map, NULL, NULL)
                          : cached_map37(
                                c, t, tlog, p, pls[p], depth[p],
                                plane_map_kind, &map);
            if (err != STREAM_OK) break;
            err = encode_mode_plane(
                c, &e, pls[p], depth[p], mode, t, tlog, p,
                p == 0 || (local_mode && p == 1) ? state : NULL,
                p ? state : NULL, map, plane_map_kind);
        }
        free(state);
        if (!planes_cached) { free(Q); free(R); }
    }
    if (mode != 1 && c->ch == 4 && !c->const_alpha && err == STREAM_OK) {
        for (size_t i = 0; i < npix; i++) P[i] = c->pix[i * sch + 3u];
        const uint8_t *map = NULL;
        if (context_plane)
            err = mode == 54
                      ? cached_weighted_map37(
                            c, t, tlog, 3, P, 8, map_kind, &map, NULL, NULL)
                      : cached_map37(
                            c, t, tlog, 3, P, 8, map_kind, &map);
        if (err == STREAM_OK)
            err = encode_mode_plane(c, &e, P, 8, mode, t, tlog, 3, NULL,
                                    NULL, map, map_kind);
    }
    if (!planes_cached) free(P);
    enc_flush(&e);
    if (err == STREAM_OK && e.cut) { free(e.buf); return STREAM_E_FORMAT; }
    if (err == STREAM_OK && e.oom) err = STREAM_E_ALLOC;
    if (err == STREAM_OK && e.len - prefix > 0xFFFFFFFFu) err = STREAM_E_DIM;
    if (err != STREAM_OK) { free(e.buf); return err; }
    int flags = 0;
    if (mode != 1 && c->gray) flags |= 1;
    if (mode != 1 && c->ch == 4 && c->const_alpha) flags |= 2;
    size_t payload_size = e.len - prefix;
    size_t total = e.len;
    uint8_t *f = realloc(e.buf, total);
    if (!f) f = e.buf;
    memcpy(f, "QST1", 4);
    put32(f + 4, c->w);
    put32(f + 8, c->h);
    f[12] = (uint8_t)c->ch;
    f[13] = (uint8_t)flags;
    f[14] = (uint8_t)mode;
    f[15] = (uint8_t)t;
    f[16] = (uint8_t)tlog;
    f[17] = (uint8_t)((flags & 2) ? c->alpha_val : adapt == ADAPT_DEFAULT ? 0 : adapt);
    put32(f + 18, c->crc);
    put32(f + 22, (uint32_t)payload_size);
    put32(f + 26, 0);
    if (mode == 1) {
        f[STREAM_HDR] = (uint8_t)(c->pal_n & 0xFF);
        f[STREAM_HDR + 1] = (uint8_t)(c->pal_n >> 8);
        for (int i = 0; i < c->pal_n; i++)
            memcpy(f + STREAM_HDR + 2 + (size_t)i * (size_t)c->ch,
                   c->pal[i], (size_t)c->ch);
    }
    put32(f + 26, container_crc32(f, total));
    *out = f; *outn = total;
    return STREAM_OK;
}

static int try_improve(const EncCtx *c, int mode, int transform, int tlog,
                       int adapt, uint8_t **best, size_t *best_size) {
    uint8_t *candidate = NULL;
    size_t candidate_size = 0;
    int err = try_encode_limited(c, mode, transform, tlog, adapt, *best_size,
                                 &candidate, &candidate_size);
    if (err == STREAM_OK && candidate_size < *best_size) {
        free(*best);
        *best = candidate;
        *best_size = candidate_size;
        return 1;
    }
    free(candidate);
    return 0;
}

static int try_fast55_candidate(const EncCtx *context, int transform,
                                int tile_log, uint8_t **best,
                                size_t *best_size) {
    size_t margin = *best_size / 20u;
    size_t limit = *best_size <= SIZE_MAX - margin
                       ? *best_size + margin : SIZE_MAX;
    uint8_t *candidate = NULL;
    size_t candidate_size = 0;
    int error = try_encode_limited(context, 55, transform, tile_log,
                                   ADAPT_DEFAULT, limit, &candidate,
                                   &candidate_size);
    if (error == STREAM_OK && candidate_size < *best_size) {
        free(*best);
        *best = candidate;
        *best_size = candidate_size;
        return 1;
    }
    free(candidate);
    return 0;
}

static int try_improve_margin(const EncCtx *c, int mode, int transform,
                              int tlog, int adapt, unsigned minimum_bps,
                              size_t minimum_bytes, uint8_t **best,
                              size_t *best_size) {
    size_t whole = *best_size / 10000u;
    size_t part = *best_size % 10000u;
    size_t minimum =
        whole * minimum_bps + (part * minimum_bps + 9999u) / 10000u;
    if (minimum < minimum_bytes) minimum = minimum_bytes;
    if (minimum >= *best_size) return 0;
    uint8_t *candidate = NULL;
    size_t candidate_size = 0;
    int err = try_encode_limited(c, mode, transform, tlog, adapt,
                                 *best_size - minimum, &candidate,
                                 &candidate_size);
    if (err == STREAM_OK && candidate_size < *best_size &&
        *best_size - candidate_size >= minimum) {
        free(*best);
        *best = candidate;
        *best_size = candidate_size;
        return 1;
    }
    free(candidate);
    return 0;
}

static int weighted_proxy_for(const EncCtx *c, int transform, int tlog,
                              uint64_t *baseline_cost,
                              uint64_t *candidate_cost) {
    *baseline_cost = 0;
    *candidate_cost = 0;
    if (c->ch != 3 || !c->transform_plane_cache || !c->map37_cache)
        return STREAM_E_ARG;
    uint16_t *planes[3];
    int err = cached_transform_planes(c, transform, planes);
    if (err != STREAM_OK) return err;
    int depth[3] = {8, transform ? 9 : 8, transform ? 9 : 8};
    int map_kind =
        transform == 35
            ? 4
            : (transform != 5 || tlog != 3) ? MAP37_REUSE_PENALTY : 0;
    for (int plane = 0; plane < 3; ++plane) {
        int plane_kind =
            c->map37_override ? c->map37_penalty[plane] : map_kind;
        const uint8_t *map = NULL;
        uint64_t base = 0;
        uint64_t weighted = 0;
        err = cached_weighted_map37(
            c, transform, tlog, plane, planes[plane], depth[plane],
            plane_kind, &map, &base, &weighted);
        if (err != STREAM_OK) return err;
        *baseline_cost += base;
        *candidate_cost += weighted;
    }
    return STREAM_OK;
}

static int weighted_map_gate(const EncCtx *c, int transform, int tlog) {
    if (tlog == 3) return 1;
    if (!c->map37_cache) return 0;
    uint64_t explicit_ids = 0;
    uint64_t tiles = 0;
    uint32_t tile_size = 1u << tlog;
    uint32_t ntx = (c->w + tile_size - 1u) >> tlog;
    uint32_t nty = (c->h + tile_size - 1u) >> tlog;
    size_t count = (size_t)ntx * nty;
    int map_penalty =
        transform == 35
            ? 4
            : (transform != 5 || tlog != 3) ? MAP37_REUSE_PENALTY : 0;
    for (int plane = 0; plane < 3; ++plane) {
        int penalty =
            c->map37_override ? c->map37_penalty[plane] : map_penalty;
        const uint8_t *map = NULL;
        for (int i = 0; i < c->map37_cache->count; ++i) {
            const Map37Entry *entry = &c->map37_cache->entries[i];
            if (entry->transform == transform && entry->tlog == tlog &&
                entry->plane == plane) {
                map = penalty ? entry->reuse_map : entry->map;
                break;
            }
        }
        if (!map) return 0;
        for (size_t i = 0; i < count; ++i) {
            int explicit_id = 1;
            uint32_t tx = (uint32_t)(i % ntx);
            if (tx && map[i] == map[i - 1]) explicit_id = 0;
            if (explicit_id && i >= ntx &&
                (!tx || map[i - ntx] != map[i - 1]) &&
                map[i] == map[i - ntx])
                explicit_id = 0;
            explicit_ids += (unsigned)explicit_id;
        }
        tiles += count;
    }
    return transform == 34 || explicit_ids * 100u >= tiles * 25u;
}

typedef struct {
    uint8_t mode, t, tlog, adapt, map_variant;
} TrialSpec;

typedef struct {
    const EncCtx *ctx;
    const TrialSpec *specs;
    int count;
    uint8_t **res;
    size_t *rlen;
    int *rerr;
    size_t limit;
} TrialRun;

typedef struct {
    const EncCtx *ctx;
    uint8_t transform[36];
    int error[36];
    int count;
} TransformPrep;

static void transform_prep_item(void *context, unsigned index) {
    TransformPrep *prep = context;
    uint16_t *planes[3];
    prep->error[index] = cached_transform_planes(
        prep->ctx, prep->transform[index], planes);
}

static int prepare_trial_transforms(const TrialRun *r, unsigned threads) {
    if (!r->ctx->transform_plane_cache || r->ctx->ch != 3 ||
        r->ctx->gray)
        return STREAM_OK;
    /* shared transform planes avoid another full image pass in every trial */
    TransformPrep prep;
    memset(&prep, 0, sizeof(prep));
    uint64_t seen = 0;
    for (int i = 0; i < r->count; ++i) {
        if (r->specs[i].mode == 1) continue;
        unsigned transform = r->specs[i].t;
        if (transform >= 36u) return STREAM_E_FORMAT;
        uint64_t bit = UINT64_C(1) << transform;
        if (seen & bit) continue;
        seen |= bit;
        prep.transform[prep.count++] = (uint8_t)transform;
    }
    prep.ctx = r->ctx;
    if (prep.count > 0)
        qlic_parallel_for((unsigned)prep.count, threads, transform_prep_item,
                          &prep);
    for (int i = 0; i < prep.count; ++i)
        if (prep.error[i] != STREAM_OK) return prep.error[i];
    return STREAM_OK;
}

static void run_one_trial(TrialRun *r, int i) {
    const EncCtx *ctx = r->ctx;
    EncCtx alternate;
    if (r->specs[i].map_variant) {
        alternate = *ctx;
        alternate.map37_cache = NULL;
        alternate.map37_override = 1;
        alternate.map37_penalty[0] = 4;
        alternate.map37_penalty[1] = 2;
        alternate.map37_penalty[2] = 2;
        ctx = &alternate;
    }
    r->rerr[i] = try_encode_limited(
        ctx, r->specs[i].mode, r->specs[i].t, r->specs[i].tlog,
        r->specs[i].adapt ? r->specs[i].adapt : ADAPT_DEFAULT, r->limit,
        &r->res[i], &r->rlen[i]);
}

static void trial_item(void *context, unsigned index) {
    run_one_trial((TrialRun *)context, (int)index);
}

static void run_trials(TrialRun *r, unsigned threads) {
    if (threads < 1) threads = 1;
    if (threads > (unsigned)r->count) threads = (unsigned)r->count;
    if (threads <= 1) {
        size_t best = SIZE_MAX;
        for (int i = 0; i < r->count; i++) {
            r->rerr[i] = try_encode_limited(
                r->ctx, r->specs[i].mode, r->specs[i].t,
                r->specs[i].tlog,
                r->specs[i].adapt ? r->specs[i].adapt : ADAPT_DEFAULT,
                best, &r->res[i],
                &r->rlen[i]);
            if (r->rerr[i] == STREAM_OK && r->rlen[i] < best) best = r->rlen[i];
        }
        return;
    }
    int prep_error = prepare_trial_transforms(r, threads);
    if (prep_error != STREAM_OK) {
        for (int i = 0; i < r->count; ++i) r->rerr[i] = prep_error;
        return;
    }
    qlic_parallel_for((unsigned)r->count, threads, trial_item, r);
}

static int try_improve_trials(const EncCtx *c, const TrialSpec *specs,
                              int count, unsigned threads, uint8_t **best,
                              size_t *best_size) {
    if (count <= 0) return 0;
    if (threads <= 1u || count == 1) {
        int improved = 0;
        for (int i = 0; i < count; ++i)
            improved |= try_improve(
                c, specs[i].mode, specs[i].t, specs[i].tlog,
                specs[i].adapt ? specs[i].adapt : ADAPT_DEFAULT, best,
                best_size);
        return improved;
    }
    uint8_t *res[8] = {0};
    size_t rlen[8] = {0};
    int rerr[8] = {0};
    if (count > 8) count = 8;
    TrialRun run;
    memset(&run, 0, sizeof(run));
    run.ctx = c;
    run.specs = specs;
    run.count = count;
    run.res = res;
    run.rlen = rlen;
    run.rerr = rerr;
    run.limit = *best_size;
    run_trials(&run, threads);
    int improved = 0;
    for (int i = 0; i < count; ++i) {
        if (rerr[i] == STREAM_OK && rlen[i] < *best_size) {
            free(*best);
            *best = res[i];
            *best_size = rlen[i];
            res[i] = NULL;
            improved = 1;
        }
        free(res[i]);
    }
    return improved;
}

static int accept_trial(uint8_t **best, size_t *best_size,
                        uint8_t **candidate, size_t candidate_size,
                        int candidate_error) {
    if (candidate_error != STREAM_OK || candidate_size >= *best_size)
        return 0;
    free(*best);
    *best = *candidate;
    *best_size = candidate_size;
    *candidate = NULL;
    return 1;
}

static int root_tile_log_for(const TrialSpec *specs, const size_t *sizes,
                             const int *errors, int count, int transform,
                             int fallback, size_t npix) {
    int probe3 = -1, probe4 = -1;
    for (int i = 0; i < count; ++i) {
        if (errors[i] != STREAM_OK || specs[i].t != transform ||
            (specs[i].mode != 37 && specs[i].mode != 41))
            continue;
        if (specs[i].tlog == 3 &&
            (probe3 < 0 || sizes[i] < sizes[probe3]))
            probe3 = i;
        if (specs[i].tlog == 4 &&
            (probe4 < 0 || sizes[i] < sizes[probe4]))
            probe4 = i;
    }
    if (probe3 < 0 || probe4 < 0) return fallback;
    return sizes[probe3] + npix / 512u < sizes[probe4] ? 3 : 4;
}

static int find_trial_spec(const TrialSpec *specs, int start, int count,
                           int mode, int transform, int tlog, int adapt,
                           int map_variant) {
    for (int i = start; i < count; ++i)
        if (specs[i].mode == mode && specs[i].t == transform &&
            specs[i].tlog == tlog && specs[i].adapt == adapt &&
            specs[i].map_variant == map_variant)
            return i;
    return -1;
}

static QLIC_NOINLINE int refine_context_trials(
    const EncCtx *c, const TrialSpec *trials, int best,
    const size_t *trial_sizes, const int *trial_errors, int trial_count,
    const int base[][2], int base_count, int search, size_t npix,
    int root_tlog, unsigned threads,
    uint8_t **best_data, size_t *best_size) {
    TrialSpec specs[24];
    int count = 0;
    int first_index = count;
    specs[count++] =
        (TrialSpec){52, trials[best].t, (uint8_t)root_tlog, ADAPT_DEFAULT, 0};
    int fast_index = -1;
    if (npix <= 1000000u) {
        fast_index = count;
        specs[count++] =
            (TrialSpec){trials[best].mode, trials[best].t,
                        trials[best].tlog, ADAPT_FAST, 0};
    }
    int transform_index = count;
    if (search == 1 && base_count >= 2) {
        int transforms[2];
        int transform_count = 0;
        int current = trials[best].t;
        for (int i = 0; i < 2; ++i) {
            if (base[i][0] == current) continue;
            transforms[transform_count++] = base[i][0];
        }
        for (int i = 0; i < transform_count; ++i) {
            int transform = transforms[i];
            int tile_log = root_tile_log_for(
                trials, trial_sizes, trial_errors, trial_count, transform, 4,
                npix);
            specs[count++] =
                (TrialSpec){52, (uint8_t)transform, (uint8_t)tile_log,
                            ADAPT_DEFAULT, 0};
        }
    }
    if (threads <= 1u || count == 1) {
        try_improve(c, specs[first_index].mode, specs[first_index].t,
                    specs[first_index].tlog, ADAPT_DEFAULT, best_data,
                    best_size);
        if (fast_index >= 0 && *best_size > STREAM_HDR &&
            ((*best_data)[14] == 37 || (*best_data)[14] == 41))
            try_improve(c, specs[fast_index].mode, specs[fast_index].t,
                        specs[fast_index].tlog, ADAPT_FAST, best_data,
                        best_size);
        for (int i = transform_index; i < count; ++i)
            try_improve(c, specs[i].mode, specs[i].t, specs[i].tlog,
                        ADAPT_DEFAULT, best_data, best_size);
        return 0;
    }
    int primary_count = count;
    int fused = search == 1 && npix <= 1000000u;
    if (fused) {
        for (int i = 0; i < primary_count; ++i) {
            if (specs[i].mode != 52) continue;
            specs[count++] = (TrialSpec){
                53, specs[i].t, specs[i].tlog, specs[i].adapt, 0};
            if (specs[i].tlog == 3) {
                specs[count++] = (TrialSpec){
                    52, specs[i].t, specs[i].tlog, specs[i].adapt, 1};
                specs[count++] = (TrialSpec){
                    53, specs[i].t, specs[i].tlog, specs[i].adapt, 1};
            }
        }
    }
    uint8_t *results[24] = {0};
    size_t sizes[24] = {0};
    int errors[24] = {0};
    TrialRun run;
    memset(&run, 0, sizeof(run));
    run.ctx = c;
    run.specs = specs;
    run.count = count;
    run.res = results;
    run.rlen = sizes;
    run.rerr = errors;
    run.limit = *best_size;
    run_trials(&run, threads);
    accept_trial(best_data, best_size, &results[first_index],
                 sizes[first_index], errors[first_index]);
    if (fast_index >= 0 && *best_size > STREAM_HDR &&
        ((*best_data)[14] == 37 || (*best_data)[14] == 41))
        accept_trial(best_data, best_size, &results[fast_index],
                     sizes[fast_index], errors[fast_index]);
    for (int i = transform_index; i < primary_count; ++i)
        accept_trial(best_data, best_size, &results[i], sizes[i], errors[i]);
    if (fused && *best_size > STREAM_HDR && (*best_data)[14] == 52) {
        int transform = (*best_data)[15];
        int tlog = (*best_data)[16];
        int adapt = (*best_data)[17];
        if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
            adapt = ADAPT_DEFAULT;
        int refined_map = 0;
        if (tlog == 3) {
            int index = find_trial_spec(specs, primary_count, count, 52,
                                        transform, tlog, adapt, 1);
            if (index >= 0)
                refined_map = accept_trial(
                    best_data, best_size, &results[index], sizes[index],
                    errors[index]);
        }
        if (*best_size > STREAM_HDR && (*best_data)[14] == 52) {
            transform = (*best_data)[15];
            tlog = (*best_data)[16];
            adapt = (*best_data)[17];
            if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
                adapt = ADAPT_DEFAULT;
            int index = find_trial_spec(specs, primary_count, count, 53,
                                        transform, tlog, adapt, refined_map);
            if (index >= 0)
                accept_trial(best_data, best_size, &results[index],
                             sizes[index], errors[index]);
        }
    }
    for (int i = 0; i < count; ++i)
        free(results[i]);
    return fused;
}

static int stream_encode_strided_base(
    const uint8_t *pix, uint32_t w, uint32_t h, int channels,
    size_t pixel_stride, int search, unsigned threads, int sample_bits,
    size_t limit,
    uint8_t **out, size_t *outn) {
    if (out) *out = NULL;
    if (outn) *outn = 0;
    if (!pix || !out || !outn) return STREAM_E_ARG;
    size_t npix = 0;
    if (channels != 1 && channels != 3 && channels != 4) return STREAM_E_ARG;
    if (pixel_stride < (size_t)channels || pixel_stride > (size_t)INT32_MAX)
        return STREAM_E_ARG;
    if (!dims_ok(w, h, channels, &npix, NULL)) return STREAM_E_DIM;
    search = search <= 0 ? 0 : search == 1 ? 1 : 7;

    EncCtx c; memset(&c, 0, sizeof c);
    Map37Cache map_cache; memset(&map_cache, 0, sizeof(map_cache));
    GrayMapCache gray_map_cache;
    memset(&gray_map_cache, 0, sizeof(gray_map_cache));
    TransformPlaneCache transform_plane_cache;
    memset(&transform_plane_cache, 0, sizeof(transform_plane_cache));
    c.pix = pix; c.w = w; c.h = h; c.ch = channels;
    c.stride = pixel_stride;
    c.crc =
        stream_crc32_pixels(pix, npix, (size_t)channels, pixel_stride);
    c.map37_cache = &map_cache;
    c.gray_map_cache = &gray_map_cache;

    if (channels == 3) {
        c.gray = 1;
        for (size_t i = 0; i < npix && c.gray; i++)
            if (pix[i * pixel_stride] != pix[i * pixel_stride + 1u] ||
                pix[i * pixel_stride + 1u] != pix[i * pixel_stride + 2u])
                c.gray = 0;
    }
    if (channels == 4) {
        c.const_alpha = 1; c.alpha_val = pix[3];
        for (size_t i = 0; i < npix && c.const_alpha; i++)
            if (pix[i * pixel_stride + 3u] != c.alpha_val) c.const_alpha = 0;
    }
    int perr = channels >= 3 ? build_palette(&c) : STREAM_OK;
    if (perr != STREAM_OK) return perr;
    int pal_ok = c.pal_n > 0;

    int color = channels >= 3 && !c.gray;
    if (threads > 1u && search == 1 && (channels == 1 || c.gray) &&
        npix <= 1000000u) {
        uint16_t *samples = malloc(npix * sizeof(*samples));
        if (!samples) {
            free(c.pal_idx);
            return STREAM_E_ALLOC;
        }
        for (size_t i = 0; i < npix; ++i)
            samples[i] = pix[i * pixel_stride];
        /* finish the mutable work before parallel trials share these maps */
        perr = build_gray_map_cache(&c, samples, 8);
        free(samples);
        if (perr != STREAM_OK) {
            free_gray_map_cache(&gray_map_cache);
            free(c.pal_idx);
            return perr;
        }
    }
    if (channels == 3 && color &&
        (threads > 1u || (search == 1 && npix <= 1000000u) ||
         (search == 0 && npix <= 4000000u)))
        c.transform_plane_cache = &transform_plane_cache;
    int base[15][2], nb = candidates(search, base);
    int fast_ordinary = nb > 0 ? base[0][0] : 0;
    int fast_alternate = -1;
    int fast_alternate_close = 0;
    int fast_dense = 0;
    if (search <= 0 && color && nb > 0)
        base[0][0] = fast_transform_for(
            &c, &fast_ordinary, &fast_alternate, &fast_alternate_close,
            &fast_dense);
    if (search == 1 && color)
        nb = ranked_transforms_for(&c, base, 2);
    unsigned fast_zero_rate =
        search <= 1 && color && !pal_ok && nb > 0 && npix > 1000000u
            ? zero_run_rate_for(&c, base[0][0])
            : 0u;
    if (fast_zero_rate >= 7500u) {
        uint8_t *fast = NULL;
        size_t fastn = 0;
        int ferr = try_encode_limited(&c, 45, base[0][0], 3, ADAPT_DEFAULT,
                                      SIZE_MAX, &fast, &fastn);
        if (ferr == STREAM_OK && fastn < npix * 3u / 8u) {
            if (mapfree_candidate_for(&c, base[0][0], 3))
                try_improve(&c, 45, base[0][0], 0, ADAPT_DEFAULT, &fast,
                            &fastn);
            if (fast_zero_rate >= 9400u && base[0][0] >= 30)
                try_improve(&c, 56, base[0][0], 3, ADAPT_DEFAULT, &fast,
                            &fastn);
            *out = fast;
            *outn = fastn;
            free_map37_cache(&map_cache);
            free_gray_map_cache(&gray_map_cache);
            free_transform_plane_cache(&transform_plane_cache);
            free(c.pal_idx);
            return STREAM_OK;
        }
        free(fast);
    }
    int small_l5 = npix <= 300000u;
    int event_ok = npix <= 0xffffffu;
    /* dense photos usually end at mode 53, starting there avoids two discarded encodes */
    int direct_context =
        search <= 0 && color && !pal_ok && !sample_bits && fast_dense &&
        npix >= 200000u && npix <= 500000u && base[0][0] != 0 &&
        base[0][0] != 9 && base[0][0] != 10;
    TrialSpec cl[64] = {0};
    int nc = 0;
    char seen[8] = {0}, seen_x[8] = {0}, seen_r[8] = {0};
    int special_color_trials = 0;
    for (int i = 0; i < nb; i++) {
        uint8_t t = (uint8_t)base[i][0], L = (uint8_t)base[i][1];
        if (color && search == 1) {
            cl[nc].mode = 37; cl[nc].t = t; cl[nc].tlog = 4; nc++;
            if (i < 2) {
                cl[nc].mode = 37; cl[nc].t = t; cl[nc].tlog = 3; nc++;
            }
            if (i < 3 && zero_run_candidate_for(&c, t)) {
                cl[nc].mode = 25; cl[nc].t = t; cl[nc].tlog = 4; nc++;
            }
            continue;
        }
        if (color) {
            if (search <= 0 && i == 0 && L == 4) {
                cl[nc].mode =
                    direct_context ? 53
                                   : npix > 1000000u && !pal_ok ? 52 : 37;
                cl[nc].t =
                    npix > 1000000u && !pal_ok
                        ? (uint8_t)fast_ordinary
                        : t;
                cl[nc].tlog =
                    npix > 1000000u && !pal_ok ? 3 : L;
                nc++;
            } else {
                cl[nc].mode = 0; cl[nc].t = t; cl[nc].tlog = L; nc++;
            }
            if (search == 7 && L >= 3 && L <= 4) {
                cl[nc].mode = 26; cl[nc].t = t; cl[nc].tlog = L; nc++;
                cl[nc].mode = 27; cl[nc].t = t; cl[nc].tlog = L; nc++;
                if (L == 4) { cl[nc].mode = 25; cl[nc].t = t; cl[nc].tlog = L; nc++; }
            }
            if (search == 7 && L == 4) { cl[nc].mode = 38; cl[nc].t = t; cl[nc].tlog = 0; nc++; }
            if (search > 0 && event_ok && L == 4 &&
                special_color_trials < 2) {
                special_color_trials++;
                cl[nc].mode = 39; cl[nc].t = t; cl[nc].tlog = 0; nc++;
                cl[nc].mode = 40; cl[nc].t = t; cl[nc].tlog = 1; nc++;
            }
            if (search == 7 && L == 4 && i < 3) { cl[nc].mode = 42; cl[nc].t = t; cl[nc].tlog = L; nc++; }
        } else {
            if (!seen[L]) { seen[L] = 1; cl[nc].mode = 0; cl[nc].t = 0; cl[nc].tlog = L; nc++; }
            if (search == 1 && L == 4 && !seen_x[L]) {
                seen_x[L] = 1;
                cl[nc].mode = 33; cl[nc].t = 0; cl[nc].tlog = L; nc++;
                cl[nc].mode = 33; cl[nc].t = 0; cl[nc].tlog = 3; nc++;
                cl[nc].mode = 25; cl[nc].t = 0; cl[nc].tlog = L; nc++;
                if (small_l5) { cl[nc].mode = 33; cl[nc].t = 0; cl[nc].tlog = 5; nc++; }
                cl[nc].mode = 37; cl[nc].t = 0; cl[nc].tlog = 3; nc++;
                cl[nc].mode = 37; cl[nc].t = 0; cl[nc].tlog = L; nc++;
                cl[nc].mode = 42; cl[nc].t = 0; cl[nc].tlog = L; nc++;
            }
            if (search == 7 && L >= 3 && L <= 4 && !seen_x[L]) {
                seen_x[L] = 1;
                cl[nc].mode = 26; cl[nc].t = 0; cl[nc].tlog = L; nc++;
                cl[nc].mode = 27; cl[nc].t = 0; cl[nc].tlog = L; nc++;
                if (L == 4) { cl[nc].mode = 25; cl[nc].t = 0; cl[nc].tlog = L; nc++; }
                if (L == 4) { cl[nc].mode = 42; cl[nc].t = 0; cl[nc].tlog = L; nc++; }
            }
            if (L == 4 && !seen_r[L] && search > 0) {
                seen_r[L] = 1;
                cl[nc].mode = 38; cl[nc].t = 0; cl[nc].tlog = 0; nc++;
                if (event_ok) { cl[nc].mode = 39; cl[nc].t = 0; cl[nc].tlog = 0; nc++; }
                if (event_ok) { cl[nc].mode = 40; cl[nc].t = 0; cl[nc].tlog = 1; nc++; }
            }
        }
    }
    if (color && search == 1 && !pal_ok && npix <= 300000u) {
        int selected = 0;
        for (int i = 0; i < nb; ++i)
            if (base[i][0] == 8) selected = 1;
        if (!selected && zero_run_candidate_for(&c, 8)) {
            cl[nc].mode = 25;
            cl[nc].t = 8;
            cl[nc].tlog = 4;
            nc++;
        }
    }
    if (pal_ok) {
        char seen2[8] = {0};
        for (int i = 0; i < nb; i++) {
            uint8_t L = (uint8_t)base[i][1];
            if (!seen2[L]) { seen2[L] = 1; cl[nc].mode = 1; cl[nc].t = 0; cl[nc].tlog = L; nc++; }
        }
    }
    if (nc <= 0) {
        free_map37_cache(&map_cache);
        free_gray_map_cache(&gray_map_cache);
        free_transform_plane_cache(&transform_plane_cache);
        free(c.pal_idx);
        return STREAM_E_FORMAT;
    }
    if (c.map37_cache) {
        uint64_t map3 = 0, map4 = 0;
        for (int i = 0; i < nc; ++i) {
            if (cl[i].t > 35) continue;
            uint64_t bit = UINT64_C(1) << cl[i].t;
            if (color && search == 1 && cl[i].mode == 25 &&
                cl[i].tlog == 4)
                c.xzr_map_mask |= bit;
            if (cl[i].mode == 37 || cl[i].mode == 41) {
                if (cl[i].tlog == 3) map3 |= bit;
                else if (cl[i].tlog == 4) map4 |= bit;
            }
            if (color && search <= 0 && npix > 1000000u &&
                cl[i].mode == 52 && cl[i].tlog == 3) {
                map3 |= bit;
                map4 |= bit;
            }
        }
        c.map37_pair_mask = map3 & map4;
    }
    uint8_t *res[64] = {0}; size_t rlen[64] = {0}; int rerr[64];
    TrialRun run;
    memset(&run, 0, sizeof(run));
    run.ctx = &c;
    run.specs = cl;
    run.count = nc;
    run.res = res;
    run.rlen = rlen;
    run.rerr = rerr;
    run.limit = limit;
    /* cache construction is mutable, parallel trials build their maps separately */
    if (threads > 1u) {
        c.map37_cache = NULL;
        if (!gray_map_cache.ready) c.gray_map_cache = NULL;
    }
    run_trials(&run, threads);
    if (threads > 1u) {
        c.map37_cache = &map_cache;
        c.gray_map_cache = &gray_map_cache;
    }

    int best = -1;
    for (int i = 0; i < nc; i++)
        if (rerr[i] == STREAM_OK && (best < 0 || rlen[i] < rlen[best])) best = i;
    if (best >= 0) {
        for (int i = 0; i < nc; i++) {
            if (i != best && rerr[i] == STREAM_OK && cl[i].mode == 42 &&
                rlen[i] == rlen[best]) {
                best = i;
                break;
            }
        }
    }
    if (best >= 0 && npix <= 1000000u && !(c.ch == 4 && c.const_alpha) &&
        !(color && (cl[best].mode == 37 || cl[best].mode == 41 ||
                    direct_context))) {
        TrialSpec adapt_specs[2] = {
            {cl[best].mode, cl[best].t, cl[best].tlog, ADAPT_SLOW, 0},
            {cl[best].mode, cl[best].t, cl[best].tlog, ADAPT_FAST, 0}
        };
        try_improve_trials(&c, adapt_specs, 2, threads, &res[best],
                           &rlen[best]);
    }
    if (best >= 0 && search <= 0 && !color && npix >= 65536u &&
        npix <= 1000000u) {
        if (sample_bits >= 3)
            try_improve(&c, 37, 0, 3, ADAPT_DEFAULT, &res[best],
                        &rlen[best]);
        else if (!sample_bits &&
                 (zero_run_candidate_for(&c, 0) ||
                  gray_unique_count(&c, 240) <= 240))
            try_improve(&c, 37, 0, 3, ADAPT_SLOW, &res[best], &rlen[best]);
    }
    int spatial_dominant = 0;
    if (best >= 0 && search > 0 && color && npix > 1000000u &&
        (cl[best].mode == 37 || cl[best].mode == 41) &&
        zero_run_candidate_for(&c, cl[best].t)) {
        size_t spatial_base = rlen[best];
        TrialSpec spatial_specs[2] = {
            {45, cl[best].t, cl[best].tlog, ADAPT_DEFAULT, 0},
            {45, cl[best].t, 0, ADAPT_DEFAULT, 0}
        };
        int spatial_count = cl[best].tlog ? 2 : 1;
        try_improve_trials(&c, spatial_specs, spatial_count, threads,
                           &res[best], &rlen[best]);
        spatial_dominant = spatial_base - rlen[best] > spatial_base / 9u;
    }
    int root_tlog = best >= 0
                        ? root_tile_log_for(cl, rlen, rerr, nc, cl[best].t,
                                            cl[best].tlog, npix)
                        : 0;
    int fused_context = 0;
    if (best >= 0 && !(search <= 0 && npix > 1000000u) &&
        color && !spatial_dominant &&
        (cl[best].mode == 37 || cl[best].mode == 41)) {
        fused_context = refine_context_trials(
            &c, cl, best, rlen, rerr, nc, base, nb, search, npix,
            root_tlog, threads, &res[best], &rlen[best]);
    }
    if (best >= 0 && search <= 0 && color && npix >= 200000u &&
        npix <= 1000000u && rlen[best] > STREAM_HDR) {
        uint64_t bits = (uint64_t)rlen[best] * 8u;
        int transform = res[best][15];
        if (res[best][14] == 52 && transform == 0 &&
            bits <= (uint64_t)npix * 6u &&
            zero_run_candidate_for(&c, transform))
            try_improve(&c, 25, transform, 4, ADAPT_SLOW, &res[best],
                        &rlen[best]);
        if (npix <= 300000u && bits <= (uint64_t)npix * 12u &&
            zero_run_candidate_for(&c, 8))
            try_improve(&c, 25, 8, 4, ADAPT_SLOW, &res[best], &rlen[best]);
        if (rlen[best] > STREAM_HDR && res[best][14] == 52 &&
            res[best][15] == 0 && bits * 2u <= (uint64_t)npix * 5u)
            try_improve(&c, 37, 0, 3, ADAPT_FAST, &res[best], &rlen[best]);
        int alternate_profile =
            (res[best][14] == 40 && res[best][15] == 7) ||
            (res[best][14] == 52 && res[best][15] == 34);
        if (alternate_profile && fast_alternate >= 0 &&
            fast_alternate_close && bits <= (uint64_t)npix * 4u)
            try_improve(&c, 52, fast_alternate, 4, ADAPT_DEFAULT, &res[best],
                        &rlen[best]);
    }
    int refined_map = 0;
    Map37Cache refined_map_cache;
    memset(&refined_map_cache, 0, sizeof(refined_map_cache));
    if (!fused_context && best >= 0 && color && search == 1 &&
        npix <= 1000000u &&
        rlen[best] > STREAM_HDR && res[best][14] == 52 &&
        res[best][16] == 3) {
        int transform = res[best][15];
        int adapt = res[best][17];
        if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
            adapt = ADAPT_DEFAULT;
        EncCtx alternate = c;
        alternate.map37_cache = &refined_map_cache;
        alternate.map37_override = 1;
        alternate.map37_penalty[0] = 4;
        alternate.map37_penalty[1] = 2;
        alternate.map37_penalty[2] = 2;
        refined_map = try_improve(&alternate, 52, transform, 3, adapt,
                                  &res[best], &rlen[best]);
    }
    int refined_context_candidate = 0;
    if (!fused_context && best >= 0 && color &&
        rlen[best] > STREAM_HDR && res[best][14] == 52) {
        int transform = res[best][15];
        uint64_t bits = (uint64_t)rlen[best] * 8u;
        refined_context_candidate =
            (transform == 5 && npix <= 1000000u) ||
            (npix >= 200000u && npix <= 500000u &&
             transform != 0 && transform != 9 && transform != 10 &&
             bits >= (uint64_t)npix * 6u);
    }
    if (refined_context_candidate) {
        int transform = res[best][15];
        int tile_log = res[best][16];
        int adapt = res[best][17];
        if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
            adapt = ADAPT_DEFAULT;
        EncCtx refined = c;
        if (refined_map) {
            refined.map37_cache = &refined_map_cache;
            refined.map37_override = 1;
            refined.map37_penalty[0] = 4;
            refined.map37_penalty[1] = 2;
            refined.map37_penalty[2] = 2;
        }
        try_improve(&refined, 53, transform, tile_log, adapt, &res[best],
                    &rlen[best]);
        if (rlen[best] > STREAM_HDR && res[best][14] == 53 &&
            res[best][16] == 4) {
            transform = res[best][15];
            uint64_t bits = (uint64_t)rlen[best] * 8u;
            int smaller_tiles =
                transform == 33 ||
                (transform == 32 && bits >= (uint64_t)npix * 10u) ||
                (transform == 34 && npix < 300000u);
            if (smaller_tiles)
                try_improve(&refined, 53, transform, 3, adapt, &res[best],
                            &rlen[best]);
        }
    }
    if (direct_context && best >= 0 && rlen[best] > STREAM_HDR &&
        res[best][14] == 53 && res[best][16] == 4) {
        int transform = res[best][15];
        uint64_t bits = (uint64_t)rlen[best] * 8u;
        if (transform == 33 ||
            (transform == 32 && bits >= (uint64_t)npix * 10u)) {
            int adapt = res[best][17];
            if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
                adapt = ADAPT_DEFAULT;
            try_improve(&c, 53, transform, 3, adapt, &res[best],
                        &rlen[best]);
        }
    }
    if (best >= 0 && c.ch == 3 && color && npix >= 250000u &&
        npix <= 500000u && rlen[best] > STREAM_HDR &&
        res[best][14] == 53 && res[best][15] >= 32 &&
        res[best][15] <= 35) {
        int transform = res[best][15];
        int tile_log = res[best][16];
        int adapt = res[best][17];
        if (adapt != ADAPT_FAST && adapt != ADAPT_SLOW)
            adapt = ADAPT_DEFAULT;
        EncCtx weighted = c;
        if (refined_map) {
            weighted.map37_cache = &refined_map_cache;
            weighted.map37_override = 1;
            weighted.map37_penalty[0] = 4;
            weighted.map37_penalty[1] = 2;
            weighted.map37_penalty[2] = 2;
        }
        uint64_t baseline_cost = 0;
        uint64_t candidate_cost = 0;
        if (weighted_map_gate(&weighted, transform, tile_log) &&
            weighted_proxy_for(&weighted, transform, tile_log,
                               &baseline_cost, &candidate_cost) == STREAM_OK &&
            candidate_cost < baseline_cost) {
            uint64_t whole = baseline_cost / 10000u;
            uint64_t part = baseline_cost % 10000u;
            uint64_t minimum =
                whole * WEIGHTED_PROXY_BPS +
                (part * WEIGHTED_PROXY_BPS + 9999u) / 10000u;
            if (baseline_cost - candidate_cost >= minimum)
                try_improve_margin(
                    &weighted, 54, transform, tile_log, adapt,
                    WEIGHTED_MIN_GAIN_BPS, 256u, &res[best], &rlen[best]);
        }
    }
    if (best >= 0 && search <= 0 && c.ch == 3 && color &&
        npix >= 65536u && rlen[best] > STREAM_HDR && res[best][16]) {
        int transform = res[best][15];
        size_t incumbent_size = rlen[best];
        int sparse_selected = 0;
        unsigned zero_rate = zero_run_rate_for(&c, transform);
        if (zero_rate >= 8000u)
            sparse_selected =
                try_improve(&c, 56, transform, 3, ADAPT_DEFAULT,
                            &res[best], &rlen[best]);
        size_t decisive_gain = incumbent_size / 100u;
        if (zero_rate < 6000u &&
            (!sparse_selected || incumbent_size - rlen[best] < decisive_gain))
            try_fast55_candidate(&c, transform, 4,
                                 &res[best], &rlen[best]);
    }
    free_map37_cache(&refined_map_cache);
    int err = STREAM_OK;
    if (best < 0) err = rerr[0];
    else { *out = res[best]; *outn = rlen[best]; }
    for (int i = 0; i < nc; i++) if (i != best) free(res[i]);
    free_map37_cache(&map_cache);
    free_gray_map_cache(&gray_map_cache);
    free_transform_plane_cache(&transform_plane_cache);
    free(c.pal_idx);
    return err;
}

static uint8_t sample_grid_mask(uint8_t value) {
    uint8_t mask = 0;
    for (int bits = 1; bits < 8; ++bits) {
        unsigned maximum = (1u << bits) - 1u;
        unsigned compact = ((unsigned)value * maximum + 127u) / 255u;
        unsigned restored =
            (compact * 255u + maximum / 2u) / maximum;
        if (restored == value) mask |= (uint8_t)(1u << bits);
    }
    return mask;
}

static int common_sample_grid(const uint8_t *pix, size_t npix, int channels,
                              size_t pixel_stride) {
    if (channels != 1 && channels < 3) return 0;
    uint8_t membership[256];
    for (unsigned i = 0; i < 256u; ++i)
        membership[i] = sample_grid_mask((uint8_t)i);
    uint8_t common = 0xfeu;
    int sample_channels = channels == 1 ? 1 : 3;
    for (size_t i = 0; i < npix && common; ++i) {
        const uint8_t *pixel = pix + i * pixel_stride;
        for (int channel = 0; channel < sample_channels; ++channel)
            common &= membership[pixel[channel]];
    }
    for (int bits = 1; bits < 8; ++bits)
        if (common & (uint8_t)(1u << bits)) return bits;
    return 0;
}

static uint8_t compact_sample(uint8_t value, int bits) {
    unsigned maximum = (1u << bits) - 1u;
    return (uint8_t)(((unsigned)value * maximum + 127u) / 255u);
}

int stream_encode_strided_threads(
    const uint8_t *pix, uint32_t w, uint32_t h, int channels,
    size_t pixel_stride, int search, unsigned threads, uint8_t **out,
    size_t *outn) {
    if (out) *out = NULL;
    if (outn) *outn = 0;
    if (!pix || !out || !outn) return STREAM_E_ARG;
    size_t npix = 0;
    if (pixel_stride < (size_t)channels ||
        pixel_stride > (size_t)INT32_MAX)
        return STREAM_E_ARG;
    if (!dims_ok(w, h, channels, &npix, NULL)) return STREAM_E_DIM;
    int sample_bits =
        common_sample_grid(pix, npix, channels, pixel_stride);
    if (sample_bits) {
        size_t compact_size = npix * (size_t)channels;
        uint8_t *compact = malloc(compact_size);
        if (compact) {
            for (size_t i = 0; i < npix; ++i) {
                const uint8_t *source = pix + i * pixel_stride;
                uint8_t *target = compact + i * (size_t)channels;
                target[0] = compact_sample(source[0], sample_bits);
                if (channels >= 3) {
                    target[1] = compact_sample(source[1], sample_bits);
                    target[2] = compact_sample(source[2], sample_bits);
                }
                if (channels == 4) target[3] = source[3];
            }
            uint8_t *candidate = NULL;
            size_t candidate_size = 0;
            int candidate_err = stream_encode_strided_base(
                compact, w, h, channels, (size_t)channels, search, threads,
                sample_bits, SIZE_MAX, &candidate, &candidate_size);
            free(compact);
            if (candidate_err == STREAM_OK && candidate_size >= STREAM_HDR &&
                candidate[14] != 42) {
                candidate[13] |= (uint8_t)(sample_bits << 2);
                put32(candidate + 18,
                      stream_crc32_pixels(pix, npix, (size_t)channels,
                                          pixel_stride));
                put32(candidate + 26, 0);
                put32(candidate + 26,
                      container_crc32(candidate, candidate_size));
                size_t compact_payload = candidate_size - STREAM_HDR;
                if (candidate[14] != 1 &&
                    compact_payload > npix / 8u + 64u) {
                    *out = candidate;
                    *outn = candidate_size;
                    return STREAM_OK;
                }
                /* palette streams and tiny residuals can still gain after expansion */
                uint8_t *original = NULL;
                size_t original_size = 0;
                int original_err = stream_encode_strided_base(
                    pix, w, h, channels, pixel_stride, search, threads,
                    sample_bits, candidate_size, &original, &original_size);
                if (original_err == STREAM_OK) {
                    if (original_size < candidate_size) {
                        free(candidate);
                        *out = original;
                        *outn = original_size;
                    } else {
                        free(original);
                        *out = candidate;
                        *outn = candidate_size;
                    }
                    return STREAM_OK;
                }
                free(original);
                if (original_err == STREAM_E_FORMAT) {
                    *out = candidate;
                    *outn = candidate_size;
                    return STREAM_OK;
                }
                free(candidate);
                return original_err;
            }
            free(candidate);
        }
    }
    return stream_encode_strided_base(
        pix, w, h, channels, pixel_stride, search, threads, sample_bits,
        SIZE_MAX, out, outn);
}

int stream_encode_threads(const uint8_t *pix, uint32_t w, uint32_t h,
                          int channels, int search, unsigned threads,
                          uint8_t **out, size_t *outn) {
    return stream_encode_strided_threads(
        pix, w, h, channels, (size_t)channels, search, threads, out, outn);
}

static int stream_parse_info(const uint8_t *data, size_t n,
                             uint32_t expected_w, uint32_t expected_h,
                             int expected_ch, StreamInfo *info,
                             size_t *pixel_count, size_t *byte_count,
                             size_t *payload_offset) {
    /* one parser keeps metadata reporting and decoder validation in sync */
    if (!info) return STREAM_E_ARG;
    memset(info, 0, sizeof(*info));
    if (!data) return STREAM_E_ARG;
    if (n < STREAM_HDR || memcmp(data, "QST1", 4)) return STREAM_E_FORMAT;
    uint32_t w = get32(data + 4), h = get32(data + 8);
    int ch = data[12], flags = data[13], mode = data[14], t = data[15], tlog = data[16];
    uint8_t aval = data[17];
    uint32_t crc = get32(data + 18), plen = get32(data + 22);
    if ((ch != 1 && ch != 3 && ch != 4) || (flags & ~31) ||
        !zmode_valid(mode) || t > 35 || tlog > 7)
        return STREAM_E_FORMAT;
    if ((expected_w && w != expected_w) ||
        (expected_h && h != expected_h) ||
        (expected_ch && ch != expected_ch))
        return STREAM_E_CORRUPT;
    size_t npix = 0, nbytes = 0;
    if (!dims_ok(w, h, ch, &npix, &nbytes)) return STREAM_E_DIM;
    size_t sch = (size_t)ch;
    int gray = flags & 1, calpha = (flags >> 1) & 1;
    int sample_bits = (flags >> 2) & 7;
    if ((gray && ch != 3) || (calpha && ch != 4)) return STREAM_E_FORMAT;
    if (sample_bits && mode == 42) return STREAM_E_FORMAT;
    if (mode == 1 && ((flags & 3) || t != 0 || ch < 3))
        return STREAM_E_FORMAT;
    if (mode != 1 && (ch == 1 || gray) && t != 0) return STREAM_E_FORMAT;
    if ((mode == 38 || mode == 39) && tlog != 0) return STREAM_E_FORMAT;
    if (mode == 40 && tlog != 1) return STREAM_E_FORMAT;
    if (mode == 55 && (ch != 3 || gray || calpha || !tlog))
        return STREAM_E_FORMAT;
    if (mode == 56 && !tlog) return STREAM_E_FORMAT;

    size_t off = STREAM_HDR, pal_n = 0;
    int adapt = ADAPT_DEFAULT;
    if (!calpha) {
        if (aval == ADAPT_FAST || aval == ADAPT_SLOW) adapt = aval;
        else if (aval) return STREAM_E_FORMAT;
    }
    if (mode == 1) {
        if (n < STREAM_HDR + 2) return STREAM_E_FORMAT;
        pal_n = data[STREAM_HDR] | ((size_t)data[STREAM_HDR + 1] << 8);
        if (!pal_n || pal_n > 256 || n < STREAM_HDR + 2 + pal_n * sch) return STREAM_E_FORMAT;
        off = STREAM_HDR + 2 + pal_n * sch;
    }
    if ((uint64_t)off + plen != n) return STREAM_E_CORRUPT;

    info->width = w;
    info->height = h;
    info->pixel_checksum = crc;
    info->payload_size = plen;
    info->palette_count = (uint32_t)pal_n;
    info->channels = ch;
    info->flags = flags;
    info->mode = mode;
    info->transform = t;
    info->tile_log = tlog;
    info->control = aval;
    info->adaptation = adapt;
    info->sample_bits = sample_bits;
    if (pixel_count) *pixel_count = npix;
    if (byte_count) *byte_count = nbytes;
    if (payload_offset) *payload_offset = off;
    return STREAM_OK;
}

int stream_get_info(const uint8_t *data, size_t n, StreamInfo *info) {
    return stream_parse_info(data, n, 0, 0, 0, info, NULL, NULL, NULL);
}

static int expand_sample_grid(uint8_t *pix, size_t npix, size_t stride,
                              int sample_channels, int bits) {
    unsigned maximum = (1u << bits) - 1u;
    for (size_t i = 0; i < npix; ++i) {
        uint8_t *pixel = pix + i * stride;
        for (int channel = 0; channel < sample_channels; ++channel) {
            unsigned compact = pixel[channel];
            if (compact > maximum) return STREAM_E_CORRUPT;
            pixel[channel] =
                (uint8_t)((compact * 255u + maximum / 2u) / maximum);
        }
    }
    return STREAM_OK;
}

static int stream_decode_impl(const uint8_t *data, size_t n,
                              int rgba,
                              uint32_t expected_w, uint32_t expected_h,
                              int expected_ch,
                              uint8_t **pixout, uint32_t *pw, uint32_t *ph,
                              int *pch) {
    if (pixout) *pixout = NULL;
    if (pw) *pw = 0;
    if (ph) *ph = 0;
    if (pch) *pch = 0;
    if (!data || !pixout || !pw || !ph || !pch) return STREAM_E_ARG;
    StreamInfo info;
    size_t npix = 0, nbytes = 0, off = 0;
    int result = stream_parse_info(
        data, n, expected_w, expected_h, expected_ch, &info,
        &npix, &nbytes, &off);
    if (result != STREAM_OK) return result;
    uint32_t w = info.width, h = info.height;
    int ch = info.channels, flags = info.flags, mode = info.mode;
    int t = info.transform, tlog = info.tile_log;
    uint8_t aval = (uint8_t)info.control;
    uint32_t crc = info.pixel_checksum, plen = info.payload_size;
    size_t sch = (size_t)ch, pal_n = info.palette_count;
    int gray = flags & 1, calpha = (flags >> 1) & 1;
    int adapt = info.adaptation;
    int sample_bits = info.sample_bits;
    int local_mode = mode >= 44 && mode <= 54;
    uint8_t pal[256][4];
    if (mode == 1)
        for (size_t i = 0; i < pal_n; i++)
            memcpy(pal[i], data + STREAM_HDR + 2 + i * sch, sch);
    if (mode == 42) {
        result =
            split37_stream_decode(data + off, plen, w, h, ch, gray, calpha,
                                  aval, t, tlog, crc, pixout, pw, ph, pch);
        if (result != STREAM_OK || !rgba || ch == 4)
            return result;
        if (npix > SIZE_MAX / 4u) {
            free(*pixout);
            *pixout = NULL;
            return STREAM_E_ALLOC;
        }
        uint8_t *expanded = malloc(npix * 4u);
        if (!expanded) {
            free(*pixout);
            *pixout = NULL;
            return STREAM_E_ALLOC;
        }
        for (size_t i = 0; i < npix; ++i) {
            const uint8_t *s = *pixout + i * sch;
            uint8_t *d = expanded + i * 4u;
            if (ch == 1) {
                d[0] = s[0];
                d[1] = s[0];
                d[2] = s[0];
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
            d[3] = 255;
        }
        free(*pixout);
        *pixout = expanded;
        *pch = 4;
        return STREAM_OK;
    }
    if (mode == 55) {
        result = fast55_stream_decode(data + off, plen, w, h, t, tlog, crc,
                                      rgba, pixout, pch);
        if (result == STREAM_OK) {
            *pw = w;
            *ph = h;
        }
        return result;
    }

    Dec d; dec_init(&d, data + off, plen, adapt);
    size_t output_stride = rgba && ch != 4 ? 4u : sch;
    size_t pixel_capacity = npix * output_stride;
    uint8_t  *pix = malloc(pixel_capacity);
    uint16_t *P   = malloc(npix * sizeof(uint16_t));
    if (!pix || !P) { free(pix); free(P); return STREAM_E_ALLOC; }
    int err = STREAM_OK;

    if (mode == 1) {
        int depth = nbits((unsigned)(pal_n - 1)); if (depth < 1) depth = 1;
        err = decode_plane(&d, P, w, h, depth, tlog, 0);
        for (size_t i = 0; i < npix && err == STREAM_OK; i++) {
            if (P[i] >= pal_n) { err = STREAM_E_CORRUPT; break; }
            memcpy(pix + i * output_stride, pal[P[i]], sch);
            if (rgba && ch != 4) pix[i * output_stride + 3u] = 255;
        }
    } else if (ch == 1 || gray) {
        err = decode_mode_plane(&d, P, w, h, 8, mode, tlog, 0, NULL, NULL);
        if (err == STREAM_OK && rgba) {
            for (size_t i = 0; i < npix; ++i) {
                uint8_t v = (uint8_t)P[i];
                uint8_t *dst = pix + i * output_stride;
                dst[0] = v;
                dst[1] = v;
                dst[2] = v;
                dst[3] = 255;
            }
        } else if (err == STREAM_OK) {
            for (size_t i = 0; i < npix; i++) {
                uint8_t v = (uint8_t)P[i];
                for (int k = 0; k < (ch < 3 ? 1 : 3); k++) pix[i * sch + (size_t)k] = v;
            }
        }
    } else {
        uint16_t *Q = malloc(npix * sizeof(uint16_t));
        uint16_t *R = (uint16_t *)pix;
        if (!Q) { free(pix); free(P); return STREAM_E_ALLOC; }
        uint16_t *pls[3] = {P, Q, R};
        int depth[3] = {8, t ? 9 : 8, t ? 9 : 8};
        uint8_t *state =
            mode == 43 || local_mode ? pix + npix * sizeof(uint16_t) : NULL;
        for (int p = 0; p < 3 && err == STREAM_OK; p++) {
            err = decode_mode_plane(
                &d, pls[p], w, h, depth[p], mode, tlog, p,
                p == 0 || (local_mode && p == 1) ? state : NULL,
                p ? state : NULL);
        }
        if (err == STREAM_OK)
            inv_transform_reverse(pls, npix, (int)output_stride, t, pix,
                                  rgba && ch == 3);
        free(Q);
    }
    if (mode != 1 && ch == 4 && err == STREAM_OK) {
        if (calpha) for (size_t i = 0; i < npix; i++) pix[i*4 + 3] = aval;
        else {
            err = decode_mode_plane(&d, P, w, h, 8, mode, tlog, 3, NULL,
                                    NULL);
            if (err == STREAM_OK) for (size_t i = 0; i < npix; i++) pix[i*4 + 3] = (uint8_t)P[i];
        }
    }
    if (err == STREAM_OK && sample_bits)
        err = expand_sample_grid(
            pix, npix, output_stride, ch == 1 && !rgba ? 1 : 3,
            sample_bits);
    free(P);
    if (err == STREAM_OK && d.truncated) err = STREAM_E_CORRUPT;
    uint32_t decoded_crc = 0;
    if (err == STREAM_OK) {
        if (rgba && ch == 3)
            decoded_crc = stream_crc32_rgbx(pix, npix);
        else if (rgba && ch == 1)
            decoded_crc = stream_crc32_grayx(pix, npix);
        else
            decoded_crc = stream_crc32(pix, nbytes);
        /* this catches predictor or inverse transform errors after entropy decoding */
        if (decoded_crc != crc) err = STREAM_E_CORRUPT;
    }
    if (err != STREAM_OK) { free(pix); return err; }
    *pixout = pix; *pw = w; *ph = h; *pch = rgba ? 4 : ch;
    return STREAM_OK;
}

int stream_decode_trusted_expected(const uint8_t *data, size_t n,
                                   uint32_t expected_w, uint32_t expected_h,
                                   int expected_ch, uint8_t **pixout,
                                   uint32_t *pw, uint32_t *ph, int *pch) {
    return stream_decode_impl(data, n, 0, expected_w, expected_h, expected_ch,
                              pixout, pw, ph, pch);
}

int stream_decode_trusted_expected_rgba(
    const uint8_t *data, size_t n, uint32_t expected_w, uint32_t expected_h,
    int expected_ch, uint8_t **pixout, uint32_t *pw, uint32_t *ph, int *pch) {
    return stream_decode_impl(data, n, 1, expected_w, expected_h, expected_ch,
                              pixout, pw, ph, pch);
}

int stream_decode_trusted_expected_threads(
    const uint8_t *data, size_t n, unsigned threads, uint32_t expected_w,
    uint32_t expected_h, int expected_ch, uint8_t **pixout, uint32_t *pw,
    uint32_t *ph, int *pch) {
    unsigned previous = stream_threads;
    stream_threads = threads ? threads : 1u;
    int result = stream_decode_trusted_expected(
        data, n, expected_w, expected_h, expected_ch, pixout, pw, ph, pch);
    stream_threads = previous;
    return result;
}
