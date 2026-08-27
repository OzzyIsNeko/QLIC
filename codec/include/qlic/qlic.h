#ifndef QLIC_QLIC_H
#define QLIC_QLIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  define QLIC_CALL __cdecl
#  if defined(QLIC_STATIC)
#    define QLIC_API
#  elif defined(QLIC_API_BUILD)
#    define QLIC_API __declspec(dllexport)
#  else
#    define QLIC_API __declspec(dllimport)
#  endif
#else
#  define QLIC_CALL
#  if defined(__GNUC__) || defined(__clang__)
#    define QLIC_API __attribute__((visibility("default")))
#  else
#    define QLIC_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QLIC_API_VERSION 8u

enum {
  QLIC_OK = 0,
  QLIC_ERROR = -1,
  QLIC_BAD_ARGUMENT = -2,
  QLIC_OUT_OF_MEMORY = -3,
  QLIC_BAD_DATA = -4,
  QLIC_LIMIT_EXCEEDED = -5,
  QLIC_UNSUPPORTED_FORMAT = -6,
  QLIC_CANCELLED = -7
};

enum {
  QLIC_PROFILE_CORE_STILL = 1u << 0,
  QLIC_PROFILE_ANIMATION = 1u << 1,
  QLIC_PROFILE_WIDE_INTEGER = 1u << 2,
  QLIC_PROFILE_HDR = 1u << 3,
  QLIC_PROFILE_LEGACY = 1u << 4
};

enum {
  QLIC_FEATURE_THREADS = 1u << 0,
  QLIC_FEATURE_LIMITS_V2 = 1u << 1,
  QLIC_FEATURE_PORTABLE_LZMS = 1u << 2,
  QLIC_FEATURE_ICC = 1u << 3,
  QLIC_FEATURE_CICP = 1u << 4,
  QLIC_FEATURE_PHOTO_METADATA = 1u << 5,
  QLIC_FEATURE_REGION_DECODE = 1u << 6,
  QLIC_FEATURE_ROW_CALLBACKS = 1u << 7
};

typedef struct qlic_capabilities {
  /* Set struct_size and zero the remaining fields before calling. */
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t decode_profiles;
  uint32_t encode_profiles;
  uint32_t features;
  uint32_t max_channels;
  uint32_t max_bits_per_sample;
  uint32_t reserved[5];
} qlic_capabilities;

typedef struct qlic_encode_options {
  /* Initialize with qlic_encode_options_default. Newer libraries accept
     larger structs; all unknown flags must be zero. */
  uint32_t struct_size;
  uint32_t flags;
  /* zero keeps the one thread default */
  uint32_t threads;
  /* Must be zero. QLIC intentionally exposes one automatic encode policy. */
  uint32_t reserved;
} qlic_encode_options;

typedef struct qlic_decode_limits {
  /* Initialize with qlic_decode_limits_default. Newer libraries accept
     larger structs; all reserved fields must be zero. */
  uint32_t struct_size;
  /* zero keeps the one thread default */
  uint32_t threads;
  uint64_t max_file_bytes;
  uint64_t max_payload_bytes;
  uint64_t max_pixels;
  uint64_t max_animation_bytes;
  uint32_t max_frames;
  uint32_t reserved2;
} qlic_decode_limits;

typedef struct qlic_image {
  uint32_t width;
  uint32_t height;
  /* decoded storage belongs to this image until qlic_image_free */
  uint8_t *rgba;
  size_t rgba_size;
  size_t stride;
} qlic_image;

enum {
  QLIC_PIXELS_GRAY8 = 1,
  QLIC_PIXELS_GRAYA8 = 2,
  QLIC_PIXELS_RGB8 = 3,
  QLIC_PIXELS_RGBA8 = 4
};

typedef struct qlic_pixel_buffer {
  /* Set struct_size, format, pixels, pixels_size, and stride. Width and height
     are written on success. Pixel storage remains owned by the caller. */
  uint32_t struct_size;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  void *pixels;
  size_t pixels_size;
  size_t stride;
  uint64_t reserved[2];
} qlic_pixel_buffer;

typedef struct qlic_pixel_input {
  /* Set every field. Pixel storage remains owned by the caller. */
  uint32_t struct_size;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  const void *pixels;
  size_t pixels_size;
  size_t stride;
  uint64_t reserved[2];
} qlic_pixel_input;

typedef int(QLIC_CALL *qlic_progress_callback)(
    void *user, uint64_t completed, uint64_t total);
typedef int(QLIC_CALL *qlic_cancel_callback)(void *user);
typedef int(QLIC_CALL *qlic_row_callback)(
    void *user, uint32_t row, const uint8_t *rgba, size_t row_bytes);

/* Optional observer for long-running delivery APIs. Callbacks must be
   thread-safe if the caller shares their state. Returning zero from progress,
   or nonzero from cancelled, stops at the next documented checkpoint. */
typedef struct qlic_decode_observer {
  uint32_t struct_size;
  uint32_t reserved0;
  qlic_progress_callback progress;
  qlic_cancel_callback cancelled;
  void *user;
  uint64_t reserved[4];
} qlic_decode_observer;

typedef struct qlic_region {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} qlic_region;

typedef struct qlic_frame {
  qlic_image image;
  uint32_t delay_ms;
} qlic_frame;

typedef struct qlic_frame_input {
  const uint8_t *rgba;
  size_t rgba_size;
  size_t stride;
  uint32_t width;
  uint32_t height;
  /* A zero delay is normalized to 100 ms. */
  uint32_t delay_ms;
} qlic_frame_input;

typedef struct qlic_animation {
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  /* Zero means repeat indefinitely. */
  uint32_t loop_count;
  qlic_frame *frames;
} qlic_animation;

typedef struct qlic_info {
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t animated;
} qlic_info;

typedef struct qlic_wide_image {
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t bits_per_sample;
  /* 9..16 bits use native-endian uint16_t samples; 17..24 use uint32_t.
     Samples are interleaved and storage belongs to this image until freed. */
  void *pixels;
  size_t pixels_size;
  size_t stride;
} qlic_wide_image;

typedef struct qlic_info_ex {
  /* Initialize struct_size and zero every other field before calling.
     The current ABI accepts exactly the fields through reserved. */
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t animated;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t reserved;
} qlic_info_ex;

enum {
  QLIC_SAMPLE_UINT = 1
};

enum {
  QLIC_ALPHA_NONE = 0,
  QLIC_ALPHA_STRAIGHT = 1,
  QLIC_ALPHA_PREMULTIPLIED = 2
};

enum {
  QLIC_COLOR_UNSPECIFIED = 0,
  QLIC_COLOR_ICC = 1,
  QLIC_COLOR_CICP = 2,
  QLIC_COLOR_ICC_PREFERRED = 3,
  QLIC_COLOR_CICP_PREFERRED = 4
};

enum {
  QLIC_CICP_PRIMARIES_BT2020 = 9,
  QLIC_CICP_TRANSFER_PQ = 16,
  QLIC_CICP_TRANSFER_HLG = 18,
  QLIC_CICP_MATRIX_RGB = 0
};

typedef struct qlic_cicp {
  uint16_t color_primaries;
  uint16_t transfer_characteristics;
  uint16_t matrix_coefficients;
  uint8_t full_range;
  uint8_t reserved;
} qlic_cicp;

typedef struct qlic_mastering_display {
  uint16_t primary_x[3];
  uint16_t primary_y[3];
  uint16_t white_x;
  uint16_t white_y;
  uint32_t max_luminance;
  uint32_t min_luminance;
} qlic_mastering_display;

typedef struct qlic_content_light {
  uint16_t max_cll;
  uint16_t max_fall;
} qlic_content_light;

/* An opaque ancillary QSW2 metadata block. The four-byte tag identifies the
   payload without changing it. Common photographic tags are EXIF, XMP_,
   IPTC, and JUMB (JUMBF/C2PA). Core QSW2 tags are reserved by the codec.
   Decode-owned data is released by qlic_hdr_image_free. */
typedef struct qlic_metadata_block {
  uint8_t tag[4];
  uint32_t reserved;
  uint8_t *data;
  size_t size;
} qlic_metadata_block;

typedef struct qlic_decode_limits_v2 {
  uint32_t struct_size;
  uint32_t threads;
  uint64_t max_file_bytes;
  uint64_t max_payload_bytes;
  uint64_t max_pixels;
  uint64_t max_animation_bytes;
  uint64_t max_decoded_bytes;
  uint64_t max_metadata_bytes;
  uint32_t max_frames;
  uint32_t max_chunks;
  uint64_t reserved[2];
} qlic_decode_limits_v2;

/* Self-describing integer image. Version 1 stores unsigned 8..24-bit Gray,
   RGB, or RGBA samples. The codec preserves samples and metadata exactly and
   never performs implicit color conversion, alpha conversion, or tone mapping.
   Decode-owned pixels, ICC, and metadata storage is released by
   qlic_hdr_image_free. Metadata blocks are emitted and returned in order. */
typedef struct qlic_hdr_image {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t sample_type;
  uint32_t alpha_mode;
  uint32_t color_authority;
  uint32_t has_cicp;
  uint32_t has_mastering_display;
  uint32_t has_content_light;
  uint32_t reserved;
  void *pixels;
  size_t pixels_size;
  size_t stride;
  uint8_t *icc;
  size_t icc_size;
  qlic_cicp cicp;
  qlic_mastering_display mastering_display;
  qlic_content_light content_light;
  qlic_metadata_block *metadata;
  uint32_t metadata_count;
  uint32_t metadata_reserved;
} qlic_hdr_image;

/* Binary size of qlic_hdr_image in API version 6. A version-7 decoder accepts
   this size for metadata-free streams and rejects streams whose ancillary
   metadata could not be returned. */
#define QLIC_HDR_IMAGE_V1_SIZE offsetof(qlic_hdr_image, metadata)

typedef struct qlic_info_v2 {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t animated;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t sample_type;
  uint32_t alpha_mode;
  uint32_t color_authority;
  uint32_t has_icc;
  uint32_t has_cicp;
  uint32_t has_mastering_display;
  uint32_t has_content_light;
  uint32_t metadata_count;
  uint32_t reserved;
} qlic_info_v2;

QLIC_API const char *QLIC_CALL qlic_version(void);
QLIC_API const char *QLIC_CALL qlic_status_string(int status);
QLIC_API const char *QLIC_CALL qlic_last_error(void);
QLIC_API uint32_t QLIC_CALL qlic_hardware_thread_count(void);
QLIC_API int QLIC_CALL qlic_get_capabilities(qlic_capabilities *capabilities);

QLIC_API void QLIC_CALL qlic_free(void *ptr);
QLIC_API void QLIC_CALL qlic_image_free(qlic_image *image);
QLIC_API void QLIC_CALL qlic_wide_image_free(qlic_wide_image *image);
QLIC_API void QLIC_CALL qlic_hdr_image_free(qlic_hdr_image *image);
QLIC_API void QLIC_CALL qlic_animation_free(qlic_animation *animation);
QLIC_API void QLIC_CALL
qlic_decode_limits_default(qlic_decode_limits *limits);
QLIC_API void QLIC_CALL
qlic_decode_limits_v2_default(qlic_decode_limits_v2 *limits);
QLIC_API void QLIC_CALL
qlic_encode_options_default(qlic_encode_options *options);

QLIC_API int QLIC_CALL qlic_encode_rgba(
    const uint8_t *rgba, size_t rgba_size, uint32_t width, uint32_t height,
    size_t stride, const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size);

QLIC_API int QLIC_CALL qlic_encode_pixels(
    const qlic_pixel_input *input, const qlic_encode_options *options,
    uint8_t **out_data, size_t *out_size);

QLIC_API int QLIC_CALL qlic_decode_rgba(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_image *out_image);

/* Decode an 8-bit still image into caller-owned storage. RGB8 is accepted
   only when every decoded alpha value is 255. Gray8 additionally requires
   R == G == B; GrayA8 permits alpha but has the same gray requirement.
   Output storage is unspecified on failure. */
QLIC_API int QLIC_CALL qlic_decode_pixels(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits,
    qlic_pixel_buffer *out_pixels);

/* Decode an exact rectangular RGBA8 region into caller-owned storage.
   Native QST1 prediction is causal, so this reduces destination memory and
   copy bandwidth but still validates and entropy-decodes the complete image.
   Width and height remain zero on failure or cancellation. */
QLIC_API int QLIC_CALL qlic_decode_region_rgba(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits,
    const qlic_region *region, const qlic_decode_observer *observer,
    qlic_pixel_buffer *out_pixels);

/* Decode, validate, then deliver exact packed RGBA8 rows in raster order.
   Row storage is valid only for the duration of the callback. Returning zero
   from the row callback cancels delivery. No rows are delivered until the
   complete file and decoded-pixel checksum have passed. */
QLIC_API int QLIC_CALL qlic_decode_rows_rgba(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits,
    const qlic_decode_observer *observer, qlic_row_callback callback,
    void *callback_user);

/* Losslessly encode integer samples with one shared precision per channel.
   Wide images support 1, 3, or 4 channels and 9..24 bits per sample. */
QLIC_API int QLIC_CALL qlic_encode_wide(
    const void *pixels, size_t pixels_size, uint32_t width, uint32_t height,
    size_t stride, uint32_t channels, uint32_t bits_per_sample,
    const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size);

/* Decode a mode-19 wide image without reducing its precision. */
QLIC_API int QLIC_CALL qlic_decode_wide(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_wide_image *out_image);

QLIC_API int QLIC_CALL qlic_encode_hdr(
    const qlic_hdr_image *image, const qlic_encode_options *options,
    uint8_t **out_data, size_t *out_size);

QLIC_API int QLIC_CALL qlic_decode_hdr(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits,
    qlic_hdr_image *out_image);

QLIC_API int QLIC_CALL qlic_encode_animation(
    const qlic_frame_input *frames, uint32_t frame_count, uint32_t loop_count,
    const qlic_encode_options *options, uint8_t **out_data, size_t *out_size);

QLIC_API int QLIC_CALL qlic_decode_animation(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_animation *out_animation);

QLIC_API int QLIC_CALL qlic_get_info(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_info *out_info);

QLIC_API int QLIC_CALL qlic_get_info_ex(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_info_ex *out_info);

QLIC_API int QLIC_CALL qlic_get_info_v2(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits,
    qlic_info_v2 *out_info);

/* Fully decode and verify a still image, animation, wide image, or HDR image,
   then release the decoded storage. This is stronger than the header-only
   qlic_get_info_v2 check and is intended for ingestion and integrity gates. */
QLIC_API int QLIC_CALL qlic_validate(
    const uint8_t *data, size_t size, const qlic_decode_limits_v2 *limits);

#ifdef __cplusplus
}
#endif

#endif
