#include <qlic/qlic.h>
#include "qlic_core.h"

#include <stdlib.h>
#include <string.h>

const char *QLIC_CALL qlic_version(void) { return QLIC_VERSION; }

const char *QLIC_CALL qlic_status_string(int status) {
  switch (status) {
  case QLIC_OK:
    return "success";
  case QLIC_ERROR:
    return "codec error";
  case QLIC_BAD_ARGUMENT:
    return "bad argument";
  case QLIC_OUT_OF_MEMORY:
    return "out of memory";
  case QLIC_BAD_DATA:
    return "bad data";
  case QLIC_LIMIT_EXCEEDED:
    return "resource limit exceeded";
  default:
    return "unknown status";
  }
}

const char *QLIC_CALL qlic_last_error(void) {
  const char *error = qlic_core_error();
  return error && error[0] ? error : "";
}

static int bad_argument(const char *message) {
  set_err_status(QLIC_CORE_BAD_ARGUMENT, "%s", message);
  return QLIC_BAD_ARGUMENT;
}

uint32_t QLIC_CALL qlic_hardware_thread_count(void) {
  return (uint32_t)qlic_core_hardware_threads();
}

static int result_code(int fallback) {
  switch (qlic_core_status()) {
  case QLIC_CORE_OUT_OF_MEMORY:
    return QLIC_OUT_OF_MEMORY;
  case QLIC_CORE_LIMIT_EXCEEDED:
    return QLIC_LIMIT_EXCEEDED;
  case QLIC_CORE_BAD_ARGUMENT:
    return QLIC_BAD_ARGUMENT;
  case QLIC_CORE_BAD_DATA:
    return QLIC_BAD_DATA;
  default:
    return fallback;
  }
}

static int decode_limits(const qlic_decode_limits *source,
                         QlicDecodeLimits *limits) {
  if (!source) {
    qlic_core_default_limits(limits);
    return 1;
  }
  if (source->struct_size < sizeof(*source) || source->reserved2 ||
      !source->max_file_bytes ||
      !source->max_payload_bytes || !source->max_pixels ||
      !source->max_animation_bytes || !source->max_frames) {
    bad_argument("invalid decode limits");
    return 0;
  }
  limits->max_file_bytes = source->max_file_bytes;
  limits->max_payload_bytes = source->max_payload_bytes;
  limits->max_pixels = source->max_pixels;
  limits->max_animation_bytes = source->max_animation_bytes;
  limits->max_frames = source->max_frames;
  return 1;
}

typedef struct {
  unsigned threads;
} EncodeState;

typedef struct {
  unsigned threads;
} DecodeState;

static int decode_begin(const qlic_decode_limits *limits,
                        DecodeState *state) {
  state->threads = qlic_core_thread_count();
  if (limits && limits->threads &&
      !qlic_core_set_thread_count((unsigned)limits->threads)) {
    bad_argument("invalid decode thread count");
    return 0;
  }
  return 1;
}

static void decode_end(const DecodeState *state) {
  qlic_core_set_thread_count(state->threads);
}

static int encode_begin(const qlic_encode_options *options,
                        EncodeState *state) {
  state->threads = qlic_core_thread_count();
  if (!options)
    return 1;
  if (options->struct_size < sizeof(*options) ||
      options->flags || options->reserved) {
    bad_argument("invalid encode options");
    return 0;
  }
  if (options->threads &&
      !qlic_core_set_thread_count((unsigned)options->threads)) {
    bad_argument("invalid encode thread count");
    return 0;
  }
  return 1;
}

static void encode_end(const EncodeState *state) {
  qlic_core_set_thread_count(state->threads);
}

static int rgba_layout(uint32_t width, uint32_t height, size_t stride,
                       size_t *row, size_t *required) {
  if (!width || !height)
    return 0;
#if SIZE_MAX < UINT64_MAX
  if ((uint64_t)width * 4u > SIZE_MAX)
    return 0;
#endif
  *row = (size_t)width * 4u;
  if (stride < *row ||
      (height > 1u && stride > (SIZE_MAX - *row) / (height - 1u)))
    return 0;
  *required = (size_t)(height - 1u) * stride + *row;
  return 1;
}

static void take_image(qlic_image *destination, Image *source) {
  size_t stride = (size_t)source->width * 4u;
  destination->width = source->width;
  destination->height = source->height;
  destination->rgba = source->rgba;
  destination->stride = stride;
  destination->rgba_size = stride * (size_t)source->height;
  source->rgba = NULL;
}

void QLIC_CALL qlic_free(void *ptr) { free(ptr); }

void QLIC_CALL qlic_image_free(qlic_image *image) {
  if (!image)
    return;
  free(image->rgba);
  memset(image, 0, sizeof(*image));
}

void QLIC_CALL qlic_animation_free(qlic_animation *animation) {
  if (!animation)
    return;
  if (animation->frames) {
    for (uint32_t i = 0; i < animation->frame_count; ++i)
      qlic_image_free(&animation->frames[i].image);
  }
  free(animation->frames);
  memset(animation, 0, sizeof(*animation));
}

void QLIC_CALL qlic_decode_limits_default(qlic_decode_limits *limits) {
  if (!limits)
    return;
  QlicDecodeLimits core;
  qlic_core_default_limits(&core);
  memset(limits, 0, sizeof(*limits));
  limits->struct_size = sizeof(*limits);
  limits->max_file_bytes = core.max_file_bytes;
  limits->max_payload_bytes = core.max_payload_bytes;
  limits->max_pixels = core.max_pixels;
  limits->max_animation_bytes = core.max_animation_bytes;
  limits->max_frames = core.max_frames;
}

void QLIC_CALL qlic_encode_options_default(qlic_encode_options *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
}

static int encode_packed(const uint8_t *rgba, uint32_t width,
                         uint32_t height, uint8_t **out_data,
                         size_t *out_size) {
  Image image = {width, height, (uint8_t *)rgba};
  Buf file = {0};
  if (!enc_mem(&image, &file, NULL)) {
    buf_free(&file);
    return result_code(QLIC_ERROR);
  }
  *out_data = file.data;
  *out_size = file.size;
  return QLIC_OK;
}

int QLIC_CALL qlic_encode_rgba(
    const uint8_t *rgba, size_t rgba_size, uint32_t width, uint32_t height,
    size_t stride, const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size) {
  clear_err();
  if (out_data)
    *out_data = NULL;
  if (out_size)
    *out_size = 0;
  size_t row = 0;
  size_t required = 0;
  if (!rgba || !out_data || !out_size ||
      !rgba_layout(width, height, stride, &row, &required) ||
      rgba_size < required)
    return bad_argument("invalid RGBA image");

  EncodeState state;
  if (!encode_begin(options, &state))
    return QLIC_BAD_ARGUMENT;

  int result = QLIC_OK;
  if (stride == row) {
    result = encode_packed(rgba, width, height, out_data, out_size);
  } else {
    if (height > SIZE_MAX / row) {
      result = bad_argument("RGBA image storage is too large");
    } else {
      size_t packed_size = row * (size_t)height;
      uint8_t *packed = (uint8_t *)malloc(packed_size);
      if (!packed) {
        set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
        result = QLIC_OUT_OF_MEMORY;
      } else {
        for (uint32_t y = 0; y < height; ++y) {
          memcpy(packed + (size_t)y * row,
                 rgba + (size_t)y * stride, row);
        }
        result =
            encode_packed(packed, width, height, out_data, out_size);
        free(packed);
      }
    }
  }
  encode_end(&state);
  return result;
}

int QLIC_CALL qlic_decode_rgba(
    const uint8_t *data, size_t size, const qlic_decode_limits *source_limits,
    qlic_image *out_image) {
  clear_err();
  if (!out_image)
    return bad_argument("missing output image");
  memset(out_image, 0, sizeof(*out_image));
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  DecodeState state;
  if (!decode_begin(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  Image image = {0};
  if (!dec_qlic_limited(data, size, &image, NULL, &limits)) {
    image_free(&image);
    int result = result_code(QLIC_BAD_DATA);
    decode_end(&state);
    return result;
  }
  decode_end(&state);
  take_image(out_image, &image);
  return QLIC_OK;
}

static int encode_animation_packed(const qlic_frame *frames,
                                   uint32_t frame_count,
                                   uint32_t loop_count,
                                   uint8_t **out_data,
                                   size_t *out_size) {
  Anim animation = {0};
  animation.frames =
      (AnimFrame *)calloc(frame_count, sizeof(*animation.frames));
  if (!animation.frames) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return QLIC_OUT_OF_MEMORY;
  }
  animation.width = frames[0].image.width;
  animation.height = frames[0].image.height;
  animation.count = frame_count;
  animation.loop_count = loop_count;
  for (uint32_t i = 0; i < frame_count; ++i) {
    animation.frames[i].image.width = frames[i].image.width;
    animation.frames[i].image.height = frames[i].image.height;
    animation.frames[i].image.rgba = frames[i].image.rgba;
    animation.frames[i].delay_ms =
        frames[i].delay_ms ? frames[i].delay_ms : 100u;
  }
  Buf file = {0};
  int ok = enc_anim_mem(&animation, &file, NULL);
  free(animation.frames);
  if (!ok) {
    buf_free(&file);
    return result_code(QLIC_ERROR);
  }
  *out_data = file.data;
  *out_size = file.size;
  return QLIC_OK;
}

int QLIC_CALL qlic_encode_animation(
    const qlic_frame_input *frames, uint32_t frame_count, uint32_t loop_count,
    const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size) {
  clear_err();
  if (out_data)
    *out_data = NULL;
  if (out_size)
    *out_size = 0;
  if (!frames || !frame_count || frame_count > 100000u || !out_data ||
      !out_size)
    return bad_argument("invalid animation input");

  uint32_t width = frames[0].width;
  uint32_t height = frames[0].height;
  size_t row = 0;
  size_t required = 0;
  if (!frames[0].rgba ||
      !rgba_layout(width, height, frames[0].stride, &row, &required) ||
      frames[0].rgba_size < required || height > SIZE_MAX / row)
    return bad_argument("invalid animation frame");

  size_t frame_size = row * (size_t)height;
  size_t strided_count = 0;
  for (uint32_t i = 0; i < frame_count; ++i) {
    size_t frame_row = 0;
    size_t frame_required = 0;
    if (!frames[i].rgba || frames[i].width != width ||
        frames[i].height != height ||
        !rgba_layout(frames[i].width, frames[i].height, frames[i].stride,
                     &frame_row, &frame_required) ||
        frame_row != row || frames[i].rgba_size < frame_required)
      return bad_argument("invalid animation frame");
    if (frames[i].stride != row)
      ++strided_count;
  }
  if (strided_count && frame_size > SIZE_MAX / strided_count)
    return bad_argument("animation frame storage is too large");

  EncodeState state;
  if (!encode_begin(options, &state))
    return QLIC_BAD_ARGUMENT;

  qlic_frame *adapted =
      (qlic_frame *)calloc(frame_count, sizeof(*adapted));
  uint8_t *packed =
      strided_count ? (uint8_t *)malloc(frame_size * strided_count) : NULL;
  int result = QLIC_OK;
  if (!adapted || (strided_count && !packed)) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    result = QLIC_OUT_OF_MEMORY;
  } else {
    size_t packed_position = 0;
    for (uint32_t i = 0; i < frame_count; ++i) {
      adapted[i].image.width = width;
      adapted[i].image.height = height;
      adapted[i].image.stride = row;
      adapted[i].image.rgba_size = frame_size;
      adapted[i].delay_ms = frames[i].delay_ms;
      if (frames[i].stride == row) {
        adapted[i].image.rgba = (uint8_t *)frames[i].rgba;
        continue;
      }
      adapted[i].image.rgba = packed + packed_position;
      for (uint32_t y = 0; y < height; ++y) {
        memcpy(adapted[i].image.rgba + (size_t)y * row,
               frames[i].rgba + (size_t)y * frames[i].stride, row);
      }
      packed_position += frame_size;
    }
    result = encode_animation_packed(adapted, frame_count, loop_count,
                                     out_data, out_size);
  }
  free(adapted);
  free(packed);
  encode_end(&state);
  return result;
}

int QLIC_CALL qlic_decode_animation(
    const uint8_t *data, size_t size, const qlic_decode_limits *source_limits,
    qlic_animation *out_animation) {
  clear_err();
  if (!out_animation)
    return bad_argument("missing output animation");
  memset(out_animation, 0, sizeof(*out_animation));
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  DecodeState state;
  if (!decode_begin(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  Anim animation = {0};
  if (!dec_any_qlic_limited(data, size, &animation, NULL, &limits)) {
    anim_free(&animation);
    int result = result_code(QLIC_BAD_DATA);
    decode_end(&state);
    return result;
  }
  decode_end(&state);
  qlic_frame *frames =
      (qlic_frame *)calloc(animation.count, sizeof(*frames));
  if (!frames) {
    anim_free(&animation);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return QLIC_OUT_OF_MEMORY;
  }
  for (uint32_t i = 0; i < animation.count; ++i) {
    take_image(&frames[i].image, &animation.frames[i].image);
    frames[i].delay_ms = animation.frames[i].delay_ms;
  }
  out_animation->width = animation.width;
  out_animation->height = animation.height;
  out_animation->frame_count = animation.count;
  out_animation->loop_count = animation.loop_count;
  out_animation->frames = frames;
  anim_free(&animation);
  return QLIC_OK;
}

int QLIC_CALL qlic_get_info(
    const uint8_t *data, size_t size, const qlic_decode_limits *source_limits,
    qlic_info *out_info) {
  clear_err();
  if (!out_info)
    return bad_argument("missing output info");
  memset(out_info, 0, sizeof(*out_info));
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  QlicHeader header = {0};
  if (!rd_head_limited(data, size, &header, &limits))
    return result_code(QLIC_BAD_DATA);
  out_info->width = header.width;
  out_info->height = header.height;
  out_info->animated = header.mode == MODE_ANIM ? 1u : 0u;
  out_info->frame_count =
      header.mode == MODE_ANIM ? header.palette_count : 1u;
  return QLIC_OK;
}
