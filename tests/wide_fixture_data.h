#ifndef QLIC_WIDE_FIXTURE_DATA_H
#define QLIC_WIDE_FIXTURE_DATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t bits_per_sample;
  const void *pixels;
  size_t sample_count;
} WideFixture;

typedef struct {
  const char *name;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t bits_per_sample;
  const void *pixels;
  size_t sample_count;
  uint32_t alpha_mode;
  uint32_t color_authority;
  uint32_t has_cicp;
  uint32_t has_mastering_display;
  uint32_t has_content_light;
  const uint8_t *icc;
  size_t icc_size;
  qlic_cicp cicp;
  qlic_mastering_display mastering_display;
  qlic_content_light content_light;
} HdrFixture;

static const uint16_t wide_u16_10_boundary[] = {0u,   1u,    511u,
                                                512u, 1022u, 1023u};

static const uint16_t wide_u16_16_rgba[] = {
    0u,     1u, 65534u, 65535u, 0x1234u, 0x5678u, 0x9abcu, 0xdef0u,
    65535u, 0u, 32768u, 1u,     42u,     4242u,   60000u,  32767u};

static const uint32_t wide_u32_17_boundary[] = {0u,     1u,      65535u,
                                                65536u, 131070u, 131071u};

static const uint32_t wide_u32_24_rgb[] = {
    0u,        1u,        0xffffffu, 0x123456u, 0xabcdefu, 0x800000u,
    0xfffffeu, 0x010203u, 0xfedcbau, 0x00ff00u, 0xff0000u, 0x0000ffu};

static const uint16_t hdr_u16_12_pq_rgba[] = {
    0u,  1u,   4095u, 4095u, 4095u,  2048u,  1024u,  3000u,
    17u, 255u, 256u,  1u,    0x123u, 0x456u, 0x789u, 0xabcu};

static const uint16_t hdr_u16_10_rgb[] = {
    0u, 1u, 1023u, 1023u, 512u, 256u,
    17u, 511u, 1000u, 64u, 900u, 333u};

static const uint16_t described_u16_8_srgb_rgb[] = {
    0u, 1u, 255u, 17u, 127u, 254u, 255u, 128u, 2u, 33u, 66u, 99u};

static const uint8_t hdr_pq_icc[] = {0x00u, 0x00u, 0x00u, 0x10u, 'a', 'c',
                                     's',   'p',   'Q',   'L',   'I', 'C',
                                     0x20u, 0x26u, 0x08u, 0x14u};

static const WideFixture wide_fixtures[] = {
    {"wide-u16-10-boundary.qlic", 3u, 2u, 1u, 10u, wide_u16_10_boundary,
     sizeof(wide_u16_10_boundary) / sizeof(wide_u16_10_boundary[0])},
    {"wide-u16-16-rgba.qlic", 2u, 2u, 4u, 16u, wide_u16_16_rgba,
     sizeof(wide_u16_16_rgba) / sizeof(wide_u16_16_rgba[0])},
    {"wide-u32-17-boundary.qlic", 3u, 2u, 1u, 17u, wide_u32_17_boundary,
     sizeof(wide_u32_17_boundary) / sizeof(wide_u32_17_boundary[0])},
    {"wide-u32-24-rgb.qlic", 2u, 2u, 3u, 24u, wide_u32_24_rgb,
     sizeof(wide_u32_24_rgb) / sizeof(wide_u32_24_rgb[0])}};

static const HdrFixture hdr_fixtures[] = {
    {
        "hdr-u16-10-pq-rgb.qlic",
        2u,
        2u,
        3u,
        10u,
        hdr_u16_10_rgb,
        sizeof(hdr_u16_10_rgb) / sizeof(hdr_u16_10_rgb[0]),
        QLIC_ALPHA_NONE,
        QLIC_COLOR_CICP,
        1u,
        1u,
        1u,
        NULL,
        0u,
        {QLIC_CICP_PRIMARIES_BT2020, QLIC_CICP_TRANSFER_PQ,
         QLIC_CICP_MATRIX_RGB, 1u, 0u},
        {{35400u, 8500u, 6550u},
         {14600u, 39850u, 2300u},
         15635u,
         16450u,
         10000000u,
         50u},
        {1000u, 400u},
    },
    {
        "hdr-u16-10-hlg-rgb.qlic",
        2u,
        2u,
        3u,
        10u,
        hdr_u16_10_rgb,
        sizeof(hdr_u16_10_rgb) / sizeof(hdr_u16_10_rgb[0]),
        QLIC_ALPHA_NONE,
        QLIC_COLOR_CICP,
        1u,
        0u,
        0u,
        NULL,
        0u,
        {QLIC_CICP_PRIMARIES_BT2020, QLIC_CICP_TRANSFER_HLG,
         QLIC_CICP_MATRIX_RGB, 1u, 0u},
        {{0u, 0u, 0u}, {0u, 0u, 0u}, 0u, 0u, 0u, 0u},
        {0u, 0u},
    },
    {
        "hdr-u16-12-pq-rgba.qlic",
        2u,
        2u,
        4u,
        12u,
        hdr_u16_12_pq_rgba,
        sizeof(hdr_u16_12_pq_rgba) / sizeof(hdr_u16_12_pq_rgba[0]),
        QLIC_ALPHA_STRAIGHT,
        QLIC_COLOR_ICC_PREFERRED,
        1u,
        1u,
        1u,
        hdr_pq_icc,
        sizeof(hdr_pq_icc),
        {QLIC_CICP_PRIMARIES_BT2020, QLIC_CICP_TRANSFER_PQ,
         QLIC_CICP_MATRIX_RGB, 1u, 0u},
        {{35400u, 8500u, 6550u},
         {14600u, 39850u, 2300u},
         15635u,
         16450u,
         10000000u,
         50u},
        {1000u, 400u},
    },
    {
        "described-u16-8-srgb-rgb.qlic",
        2u,
        2u,
        3u,
        8u,
        described_u16_8_srgb_rgb,
        sizeof(described_u16_8_srgb_rgb) /
            sizeof(described_u16_8_srgb_rgb[0]),
        QLIC_ALPHA_NONE,
        QLIC_COLOR_CICP,
        1u,
        0u,
        0u,
        NULL,
        0u,
        {1u, 13u, 0u, 1u, 0u},
        {{0u, 0u, 0u}, {0u, 0u, 0u}, 0u, 0u, 0u, 0u},
        {0u, 0u},
    }};

#endif
