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

#define QLIC_API_VERSION 1u

enum {
  QLIC_OK = 0,
  QLIC_ERROR = -1,
  QLIC_BAD_ARGUMENT = -2,
  QLIC_OUT_OF_MEMORY = -3,
  QLIC_BAD_DATA = -4,
  QLIC_LIMIT_EXCEEDED = -5
};

typedef struct qlic_encode_options {
  /* initialize this struct with qlic_encode_options_default */
  uint32_t struct_size;
  uint32_t flags;
  /* zero keeps the one thread default */
  uint32_t threads;
  uint32_t reserved;
} qlic_encode_options;

typedef struct qlic_decode_limits {
  /* initialize this struct with qlic_decode_limits_default */
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
  uint32_t delay_ms;
} qlic_frame_input;

typedef struct qlic_animation {
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t loop_count;
  qlic_frame *frames;
} qlic_animation;

typedef struct qlic_info {
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  uint32_t animated;
} qlic_info;

QLIC_API const char *QLIC_CALL qlic_version(void);
QLIC_API const char *QLIC_CALL qlic_status_string(int status);
QLIC_API const char *QLIC_CALL qlic_last_error(void);
QLIC_API uint32_t QLIC_CALL qlic_hardware_thread_count(void);

QLIC_API void QLIC_CALL qlic_free(void *ptr);
QLIC_API void QLIC_CALL qlic_image_free(qlic_image *image);
QLIC_API void QLIC_CALL qlic_animation_free(qlic_animation *animation);
QLIC_API void QLIC_CALL
qlic_decode_limits_default(qlic_decode_limits *limits);
QLIC_API void QLIC_CALL
qlic_encode_options_default(qlic_encode_options *options);

QLIC_API int QLIC_CALL qlic_encode_rgba(
    const uint8_t *rgba, size_t rgba_size, uint32_t width, uint32_t height,
    size_t stride, const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size);

QLIC_API int QLIC_CALL qlic_decode_rgba(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_image *out_image);

QLIC_API int QLIC_CALL qlic_encode_animation(
    const qlic_frame_input *frames, uint32_t frame_count, uint32_t loop_count,
    const qlic_encode_options *options, uint8_t **out_data, size_t *out_size);

QLIC_API int QLIC_CALL qlic_decode_animation(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_animation *out_animation);

QLIC_API int QLIC_CALL qlic_get_info(
    const uint8_t *data, size_t size, const qlic_decode_limits *limits,
    qlic_info *out_info);

#ifdef __cplusplus
}
#endif

#endif
