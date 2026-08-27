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
  case QLIC_UNSUPPORTED_FORMAT:
    return "unsupported format";
  case QLIC_CANCELLED:
    return "cancelled";
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

int QLIC_CALL qlic_get_capabilities(qlic_capabilities *capabilities) {
  clear_err();
  if (!capabilities || capabilities->struct_size < sizeof(*capabilities))
    return bad_argument("invalid capabilities output");
  for (size_t i = 0;
       i < sizeof(capabilities->reserved) / sizeof(capabilities->reserved[0]);
       ++i) {
    if (capabilities->reserved[i])
      return bad_argument("invalid capabilities output");
  }
  qlic_capabilities result;
  memset(&result, 0, sizeof(result));
  result.struct_size = sizeof(result);
  result.api_version = QLIC_API_VERSION;
  result.decode_profiles = QLIC_PROFILE_CORE_STILL |
                           QLIC_PROFILE_ANIMATION |
                           QLIC_PROFILE_WIDE_INTEGER | QLIC_PROFILE_HDR |
                           QLIC_PROFILE_LEGACY;
  result.encode_profiles = result.decode_profiles;
  result.features = QLIC_FEATURE_THREADS | QLIC_FEATURE_LIMITS_V2 |
                     QLIC_FEATURE_PORTABLE_LZMS | QLIC_FEATURE_ICC |
                     QLIC_FEATURE_CICP | QLIC_FEATURE_PHOTO_METADATA |
                     QLIC_FEATURE_REGION_DECODE | QLIC_FEATURE_ROW_CALLBACKS;
  result.max_channels = 4u;
  result.max_bits_per_sample = 24u;
  *capabilities = result;
  return QLIC_OK;
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
  case QLIC_CORE_UNSUPPORTED_FORMAT:
    return QLIC_UNSUPPORTED_FORMAT;
  case QLIC_CORE_CANCELLED:
    return QLIC_CANCELLED;
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
  qlic_core_default_limits(limits);
  limits->max_file_bytes = source->max_file_bytes;
  limits->max_payload_bytes = source->max_payload_bytes;
  limits->max_pixels = source->max_pixels;
  limits->max_animation_bytes = source->max_animation_bytes;
  limits->max_frames = source->max_frames;
  /* Fixed v1 ABI; payload bytes also bound wide output. */
  limits->max_decoded_bytes = source->max_payload_bytes;
  return 1;
}

static int decode_limits_v2(const qlic_decode_limits_v2 *source,
                            QlicDecodeLimits *limits) {
  if (!source) {
    qlic_core_default_limits(limits);
    return 1;
  }
  if (source->struct_size < sizeof(*source) || source->reserved[0] ||
      source->reserved[1] || !source->max_file_bytes ||
      !source->max_payload_bytes || !source->max_pixels ||
      !source->max_animation_bytes || !source->max_decoded_bytes ||
      !source->max_metadata_bytes || !source->max_frames ||
      !source->max_chunks) {
    bad_argument("invalid v2 decode limits");
    return 0;
  }
  limits->max_file_bytes = source->max_file_bytes;
  limits->max_payload_bytes = source->max_payload_bytes;
  limits->max_pixels = source->max_pixels;
  limits->max_animation_bytes = source->max_animation_bytes;
  limits->max_decoded_bytes = source->max_decoded_bytes;
  limits->max_metadata_bytes = source->max_metadata_bytes;
  limits->max_frames = source->max_frames;
  limits->max_chunks = source->max_chunks;
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

static int decode_begin_v2(const qlic_decode_limits_v2 *limits,
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

static int observer_valid(const qlic_decode_observer *observer) {
  if (!observer)
    return 1;
  if (observer->struct_size < sizeof(*observer) || observer->reserved0)
    return 0;
  for (size_t i = 0;
       i < sizeof(observer->reserved) / sizeof(observer->reserved[0]); ++i)
    if (observer->reserved[i])
      return 0;
  return 1;
}

static int observer_checkpoint(const qlic_decode_observer *observer,
                               uint64_t completed, uint64_t total) {
  if (!observer)
    return 1;
  if (observer->cancelled && observer->cancelled(observer->user)) {
    set_err_status(QLIC_CORE_CANCELLED, "decode cancelled");
    return 0;
  }
  if (observer->progress &&
      !observer->progress(observer->user, completed, total)) {
    set_err_status(QLIC_CORE_CANCELLED, "decode cancelled by progress callback");
    return 0;
  }
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

void QLIC_CALL qlic_wide_image_free(qlic_wide_image *image) {
  if (!image)
    return;
  free(image->pixels);
  memset(image, 0, sizeof(*image));
}

void QLIC_CALL qlic_hdr_image_free(qlic_hdr_image *image) {
  if (!image)
    return;
  uint32_t struct_size = image->struct_size;
  if (struct_size >= sizeof(*image) && image->metadata) {
    for (uint32_t index = 0; index < image->metadata_count; ++index)
      free(image->metadata[index].data);
    free(image->metadata);
  }
  free(image->pixels);
  free(image->icc);
  size_t clear_size =
      struct_size >= sizeof(*image)
          ? sizeof(*image)
          : struct_size >= QLIC_HDR_IMAGE_V1_SIZE
                ? (size_t)struct_size
                : (size_t)QLIC_HDR_IMAGE_V1_SIZE;
  memset(image, 0, clear_size);
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

void QLIC_CALL qlic_decode_limits_v2_default(qlic_decode_limits_v2 *limits) {
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
  limits->max_decoded_bytes = core.max_decoded_bytes;
  limits->max_metadata_bytes = core.max_metadata_bytes;
  limits->max_frames = core.max_frames;
  limits->max_chunks = core.max_chunks;
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

int QLIC_CALL qlic_encode_pixels(const qlic_pixel_input *input,
                                 const qlic_encode_options *options,
                                 uint8_t **out_data, size_t *out_size) {
  clear_err();
  if (out_data)
    *out_data = NULL;
  if (out_size)
    *out_size = 0;
  if (!input || input->struct_size < sizeof(*input) || input->reserved[0] ||
      input->reserved[1] || !input->pixels || !out_data || !out_size ||
      !input->width || !input->height)
    return bad_argument("invalid pixel input");

  size_t pixel_size = 0;
  if (input->format == QLIC_PIXELS_GRAY8)
    pixel_size = 1u;
  else if (input->format == QLIC_PIXELS_GRAYA8)
    pixel_size = 2u;
  else if (input->format == QLIC_PIXELS_RGB8)
    pixel_size = 3u;
  else if (input->format == QLIC_PIXELS_RGBA8)
    pixel_size = 4u;
  else
    return bad_argument("invalid pixel format");

  if (input->width > SIZE_MAX / pixel_size)
    return bad_argument("pixel input is too large");
  size_t row = (size_t)input->width * pixel_size;
  if (input->stride < row ||
      (input->height > 1u &&
       input->stride > (SIZE_MAX - row) / (input->height - 1u)))
    return bad_argument("invalid pixel input layout");
  size_t required = (size_t)(input->height - 1u) * input->stride + row;
  if (input->pixels_size < required)
    return bad_argument("pixel input is too small");
  if (input->format == QLIC_PIXELS_RGBA8) {
    return qlic_encode_rgba((const uint8_t *)input->pixels,
                            input->pixels_size, input->width, input->height,
                            input->stride, options, out_data, out_size);
  }

  if (input->height > SIZE_MAX / input->width)
    return bad_argument("pixel input is too large");
  size_t pixels = (size_t)input->width * input->height;
  if (pixels > SIZE_MAX / 4u)
    return bad_argument("pixel input is too large");
  size_t rgba_size = pixels * 4u;
  uint8_t *rgba = (uint8_t *)malloc(rgba_size);
  if (!rgba) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return QLIC_OUT_OF_MEMORY;
  }
  const uint8_t *source = (const uint8_t *)input->pixels;
  for (uint32_t y = 0; y < input->height; ++y) {
    const uint8_t *in = source + (size_t)y * input->stride;
    uint8_t *out = rgba + (size_t)y * input->width * 4u;
    for (uint32_t x = 0; x < input->width; ++x) {
      if (input->format == QLIC_PIXELS_GRAY8 ||
          input->format == QLIC_PIXELS_GRAYA8) {
        uint8_t gray = in[(size_t)x * pixel_size];
        out[(size_t)x * 4u] = gray;
        out[(size_t)x * 4u + 1u] = gray;
        out[(size_t)x * 4u + 2u] = gray;
        out[(size_t)x * 4u + 3u] = input->format == QLIC_PIXELS_GRAYA8
                                       ? in[(size_t)x * 2u + 1u]
                                       : 255u;
      } else {
        out[(size_t)x * 4u] = in[(size_t)x * 3u];
        out[(size_t)x * 4u + 1u] = in[(size_t)x * 3u + 1u];
        out[(size_t)x * 4u + 2u] = in[(size_t)x * 3u + 2u];
        out[(size_t)x * 4u + 3u] = 255u;
      }
    }
  }
  int result = qlic_encode_rgba(rgba, rgba_size, input->width, input->height,
                                (size_t)input->width * 4u, options, out_data,
                                out_size);
  free(rgba);
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

int QLIC_CALL qlic_decode_pixels(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits,
    qlic_pixel_buffer *out_pixels) {
  clear_err();
  if (!out_pixels)
    return bad_argument("missing pixel destination");
  out_pixels->width = 0;
  out_pixels->height = 0;
  if (out_pixels->struct_size < sizeof(*out_pixels) ||
      out_pixels->reserved[0] || out_pixels->reserved[1] ||
      !out_pixels->pixels || !data || !size)
    return bad_argument("invalid pixel destination");
  size_t pixel_size = 0;
  if (out_pixels->format == QLIC_PIXELS_GRAY8)
    pixel_size = 1u;
  else if (out_pixels->format == QLIC_PIXELS_GRAYA8)
    pixel_size = 2u;
  else if (out_pixels->format == QLIC_PIXELS_RGB8)
    pixel_size = 3u;
  else if (out_pixels->format == QLIC_PIXELS_RGBA8)
    pixel_size = 4u;
  else
    return bad_argument("invalid pixel format");

  QlicDecodeLimits limits;
  if (!decode_limits_v2(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  QlicHeader header = {0};
  if (!rd_head_limited(data, size, &header, &limits))
    return result_code(QLIC_BAD_DATA);
  if (header.mode == MODE_ANIM) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "animation requires qlic_decode_animation");
    return QLIC_UNSUPPORTED_FORMAT;
  }
  if (header.mode == MODE_NATIVE_WIDE || header.mode == MODE_HDR_WIDE) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "wide samples require the wide or HDR decoder");
    return QLIC_UNSUPPORTED_FORMAT;
  }
  size_t row = 0;
  size_t required = 0;
  if (header.width > SIZE_MAX / pixel_size) {
    return bad_argument("pixel destination is too large");
  }
  row = (size_t)header.width * pixel_size;
  if (out_pixels->stride < row ||
      (header.height > 1u &&
       out_pixels->stride > (SIZE_MAX - row) / (header.height - 1u)))
    return bad_argument("invalid pixel destination layout");
  required = (size_t)(header.height - 1u) * out_pixels->stride + row;
  if (out_pixels->pixels_size < required)
    return bad_argument("pixel destination is too small");

  DecodeState state;
  if (!decode_begin_v2(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  int result = QLIC_OK;
  if (out_pixels->format == QLIC_PIXELS_RGBA8) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (!dec_qlic_rgba_into_limited(
            data, size, (uint8_t *)out_pixels->pixels,
            out_pixels->pixels_size, out_pixels->stride, &width, &height,
            &limits)) {
      result = result_code(QLIC_BAD_DATA);
    } else {
      out_pixels->width = width;
      out_pixels->height = height;
    }
  } else {
    Image image = {0};
    if (!dec_qlic_limited(data, size, &image, NULL, &limits)) {
      result = result_code(QLIC_BAD_DATA);
    } else {
      int representable = 1;
      for (size_t i = 0, count = (size_t)image.width * image.height;
           i < count; ++i) {
        const uint8_t *pixel = image.rgba + i * 4u;
        int gray = pixel[0] == pixel[1] && pixel[0] == pixel[2];
        if ((out_pixels->format != QLIC_PIXELS_GRAYA8 && pixel[3] != 255u) ||
            ((out_pixels->format == QLIC_PIXELS_GRAY8 ||
              out_pixels->format == QLIC_PIXELS_GRAYA8) &&
             !gray)) {
          representable = 0;
          break;
        }
      }
      if (!representable) {
        const char *reason = "image cannot be represented as exact RGB8";
        if (out_pixels->format == QLIC_PIXELS_GRAY8)
          reason = "image cannot be represented as exact Gray8";
        else if (out_pixels->format == QLIC_PIXELS_GRAYA8)
          reason = "image cannot be represented as exact GrayA8";
        set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT, reason);
        result = QLIC_UNSUPPORTED_FORMAT;
      } else {
        uint8_t *destination = (uint8_t *)out_pixels->pixels;
        for (uint32_t y = 0; y < image.height; ++y) {
          uint8_t *output = destination + (size_t)y * out_pixels->stride;
          const uint8_t *input =
              image.rgba + (size_t)y * image.width * 4u;
          for (uint32_t x = 0; x < image.width; ++x) {
            if (out_pixels->format == QLIC_PIXELS_GRAY8) {
              output[x] = input[(size_t)x * 4u];
            } else if (out_pixels->format == QLIC_PIXELS_GRAYA8) {
              output[(size_t)x * 2u] = input[(size_t)x * 4u];
              output[(size_t)x * 2u + 1u] = input[(size_t)x * 4u + 3u];
            } else {
              output[(size_t)x * 3u] = input[(size_t)x * 4u];
              output[(size_t)x * 3u + 1u] = input[(size_t)x * 4u + 1u];
              output[(size_t)x * 3u + 2u] = input[(size_t)x * 4u + 2u];
            }
          }
        }
        out_pixels->width = image.width;
        out_pixels->height = image.height;
      }
    }
    image_free(&image);
  }
  decode_end(&state);
  return result;
}

static int decode_delivery_image(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits,
    const qlic_decode_observer *observer, uint64_t total, Image *image) {
  if (!observer_valid(observer))
    return bad_argument("invalid decode observer");
  if (!observer_checkpoint(observer, 0u, total))
    return QLIC_CANCELLED;
  QlicDecodeLimits limits;
  if (!decode_limits_v2(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  DecodeState state;
  if (!decode_begin_v2(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  int ok = dec_qlic_limited(data, size, image, NULL, &limits);
  decode_end(&state);
  if (!ok) {
    image_free(image);
    return result_code(QLIC_BAD_DATA);
  }
  if (!observer_checkpoint(observer, 1u, total)) {
    image_free(image);
    return QLIC_CANCELLED;
  }
  return QLIC_OK;
}

int QLIC_CALL qlic_decode_region_rgba(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits, const qlic_region *region,
    const qlic_decode_observer *observer, qlic_pixel_buffer *out_pixels) {
  clear_err();
  if (out_pixels) {
    out_pixels->width = 0;
    out_pixels->height = 0;
  }
  if (!data || !size || !region || !region->width || !region->height ||
      !out_pixels || out_pixels->struct_size < sizeof(*out_pixels) ||
      out_pixels->reserved[0] || out_pixels->reserved[1] ||
      out_pixels->format != QLIC_PIXELS_RGBA8 || !out_pixels->pixels)
    return bad_argument("invalid region decode request");
  uint64_t total = (uint64_t)region->height + 1u;
  Image image = {0};
  int result = decode_delivery_image(data, size, source_limits, observer,
                                     total, &image);
  if (result != QLIC_OK)
    return result;
  uint64_t right = (uint64_t)region->x + region->width;
  uint64_t bottom = (uint64_t)region->y + region->height;
  size_t row = 0;
  size_t required = 0;
  if (right > image.width || bottom > image.height ||
      !rgba_layout(region->width, region->height, out_pixels->stride, &row,
                   &required) ||
      out_pixels->pixels_size < required) {
    image_free(&image);
    return bad_argument("region or destination is out of bounds");
  }
  uint8_t *destination = (uint8_t *)out_pixels->pixels;
  size_t source_stride = (size_t)image.width * 4u;
  for (uint32_t y = 0; y < region->height; ++y) {
    if (!observer_checkpoint(observer, (uint64_t)y + 2u, total)) {
      image_free(&image);
      return QLIC_CANCELLED;
    }
    const uint8_t *source =
        image.rgba + ((size_t)region->y + y) * source_stride +
        (size_t)region->x * 4u;
    memcpy(destination + (size_t)y * out_pixels->stride, source, row);
  }
  image_free(&image);
  out_pixels->width = region->width;
  out_pixels->height = region->height;
  return QLIC_OK;
}

int QLIC_CALL qlic_decode_rows_rgba(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits,
    const qlic_decode_observer *observer, qlic_row_callback callback,
    void *callback_user) {
  clear_err();
  if (!data || !size || !callback)
    return bad_argument("invalid row decode request");
  qlic_info_v2 info;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  int result = qlic_get_info_v2(data, size, source_limits, &info);
  if (result != QLIC_OK)
    return result;
  if (info.animated || info.sample_type != QLIC_SAMPLE_UINT ||
      info.bits_per_sample != 8u || info.channels > 4u) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "row delivery requires an 8-bit still image");
    return QLIC_UNSUPPORTED_FORMAT;
  }
  uint64_t total = (uint64_t)info.height + 1u;
  Image image = {0};
  result = decode_delivery_image(data, size, source_limits, observer, total,
                                 &image);
  if (result != QLIC_OK)
    return result;
  size_t row = (size_t)image.width * 4u;
  for (uint32_t y = 0; y < image.height; ++y) {
    if (!callback(callback_user, y, image.rgba + (size_t)y * row, row)) {
      image_free(&image);
      set_err_status(QLIC_CORE_CANCELLED, "row delivery cancelled");
      return QLIC_CANCELLED;
    }
    if (!observer_checkpoint(observer, (uint64_t)y + 2u, total)) {
      image_free(&image);
      return QLIC_CANCELLED;
    }
  }
  image_free(&image);
  return QLIC_OK;
}

int QLIC_CALL qlic_encode_wide(
    const void *pixels, size_t pixels_size, uint32_t width, uint32_t height,
    size_t stride, uint32_t channels, uint32_t bits_per_sample,
    const qlic_encode_options *options, uint8_t **out_data,
    size_t *out_size) {
  clear_err();
  if (out_data)
    *out_data = NULL;
  if (out_size)
    *out_size = 0;
  if (!pixels || !out_data || !out_size)
    return bad_argument("invalid wide encoder output");
  EncodeState state;
  if (!encode_begin(options, &state))
    return QLIC_BAD_ARGUMENT;
  Buf file = {0};
  int ok = enc_wide_mem(pixels, pixels_size, width, height, stride, channels,
                        bits_per_sample, &file);
  encode_end(&state);
  if (!ok) {
    buf_free(&file);
    return result_code(QLIC_ERROR);
  }
  *out_data = file.data;
  *out_size = file.size;
  return QLIC_OK;
}

int QLIC_CALL qlic_decode_wide(
    const uint8_t *data, size_t size, const qlic_decode_limits *source_limits,
    qlic_wide_image *out_image) {
  clear_err();
  if (!out_image)
    return bad_argument("missing wide output image");
  memset(out_image, 0, sizeof(*out_image));
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  DecodeState state;
  if (!decode_begin(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  WideImage image = {0};
  if (!dec_wide_qlic_limited(data, size, &image, NULL, &limits)) {
    wide_image_free(&image);
    int result = result_code(QLIC_BAD_DATA);
    decode_end(&state);
    return result;
  }
  decode_end(&state);
  out_image->width = image.width;
  out_image->height = image.height;
  out_image->channels = image.channels;
  out_image->bits_per_sample = image.bits_per_sample;
  out_image->pixels = image.pixels;
  out_image->pixels_size = image.pixels_size;
  out_image->stride = image.stride;
  image.pixels = NULL;
  return QLIC_OK;
}

int QLIC_CALL qlic_encode_hdr(const qlic_hdr_image *image,
                              const qlic_encode_options *options,
                              uint8_t **out_data, size_t *out_size) {
  clear_err();
  if (out_data)
    *out_data = NULL;
  if (out_size)
    *out_size = 0;
  if (!image ||
      (image->struct_size != QLIC_HDR_IMAGE_V1_SIZE &&
       image->struct_size < sizeof(*image)) ||
      image->reserved ||
      !image->pixels || !out_data || !out_size ||
      image->has_cicp > 1u || image->has_mastering_display > 1u ||
      image->has_content_light > 1u || image->cicp.reserved ||
      (image->struct_size >= sizeof(*image) && image->metadata_reserved))
    return bad_argument("invalid HDR image");
  EncodeState state;
  if (!encode_begin(options, &state))
    return QLIC_BAD_ARGUMENT;
  HdrImage source;
  memset(&source, 0, sizeof(source));
  source.wide.width = image->width;
  source.wide.height = image->height;
  source.wide.channels = image->channels;
  source.wide.bits_per_sample = image->bits_per_sample;
  source.wide.pixels = image->pixels;
  source.wide.pixels_size = image->pixels_size;
  source.wide.stride = image->stride;
  source.sample_type = image->sample_type;
  source.alpha_mode = image->alpha_mode;
  source.color_authority = image->color_authority;
  source.icc = image->icc;
  source.icc_size = image->icc_size;
  source.has_cicp = image->has_cicp;
  source.color_primaries = image->cicp.color_primaries;
  source.transfer_characteristics = image->cicp.transfer_characteristics;
  source.matrix_coefficients = image->cicp.matrix_coefficients;
  source.full_range = image->cicp.full_range;
  source.has_mastering_display = image->has_mastering_display;
  memcpy(source.primary_x, image->mastering_display.primary_x,
         sizeof(source.primary_x));
  memcpy(source.primary_y, image->mastering_display.primary_y,
         sizeof(source.primary_y));
  source.white_x = image->mastering_display.white_x;
  source.white_y = image->mastering_display.white_y;
  source.max_luminance = image->mastering_display.max_luminance;
  source.min_luminance = image->mastering_display.min_luminance;
  source.has_content_light = image->has_content_light;
  source.max_cll = image->content_light.max_cll;
  source.max_fall = image->content_light.max_fall;
  if (image->struct_size >= sizeof(*image)) {
    source.metadata = (HdrMetadataBlock *)image->metadata;
    source.metadata_count = image->metadata_count;
  }
  Buf file = {0};
  int ok = enc_hdr_mem(&source, &file);
  encode_end(&state);
  if (!ok) {
    buf_free(&file);
    return result_code(QLIC_ERROR);
  }
  *out_data = file.data;
  *out_size = file.size;
  return QLIC_OK;
}

int QLIC_CALL qlic_decode_hdr(const uint8_t *data, size_t size,
                              const qlic_decode_limits_v2 *source_limits,
                              qlic_hdr_image *out_image) {
  clear_err();
  if (!out_image)
    return bad_argument("missing HDR output image");
  uint32_t struct_size = out_image->struct_size;
  uint32_t reserved = out_image->reserved;
  if ((struct_size != QLIC_HDR_IMAGE_V1_SIZE &&
       struct_size < sizeof(*out_image)) ||
      reserved ||
      (struct_size >= sizeof(*out_image) && out_image->metadata_reserved))
    return bad_argument("invalid HDR output image");
  size_t clear_size = struct_size < sizeof(*out_image) ? (size_t)struct_size
                                                       : sizeof(*out_image);
  memset(out_image, 0, clear_size);
  out_image->struct_size = (uint32_t)clear_size;
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits_v2(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  DecodeState state;
  if (!decode_begin_v2(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  HdrImage decoded = {0};
  if (!dec_hdr_qlic_limited(data, size, &decoded, NULL, &limits)) {
    hdr_image_free(&decoded);
    int result = result_code(QLIC_BAD_DATA);
    decode_end(&state);
    return result;
  }
  decode_end(&state);
  if (decoded.metadata_count && struct_size < sizeof(*out_image)) {
    hdr_image_free(&decoded);
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "HDR output struct cannot return ancillary metadata");
    return QLIC_UNSUPPORTED_FORMAT;
  }
  out_image->width = decoded.wide.width;
  out_image->height = decoded.wide.height;
  out_image->channels = decoded.wide.channels;
  out_image->bits_per_sample = decoded.wide.bits_per_sample;
  out_image->sample_type = decoded.sample_type;
  out_image->alpha_mode = decoded.alpha_mode;
  out_image->color_authority = decoded.color_authority;
  out_image->pixels = decoded.wide.pixels;
  out_image->pixels_size = decoded.wide.pixels_size;
  out_image->stride = decoded.wide.stride;
  decoded.wide.pixels = NULL;
  out_image->icc = decoded.icc;
  out_image->icc_size = decoded.icc_size;
  decoded.icc = NULL;
  out_image->has_cicp = decoded.has_cicp;
  out_image->cicp.color_primaries = decoded.color_primaries;
  out_image->cicp.transfer_characteristics = decoded.transfer_characteristics;
  out_image->cicp.matrix_coefficients = decoded.matrix_coefficients;
  out_image->cicp.full_range = decoded.full_range;
  out_image->has_mastering_display = decoded.has_mastering_display;
  memcpy(out_image->mastering_display.primary_x, decoded.primary_x,
         sizeof(decoded.primary_x));
  memcpy(out_image->mastering_display.primary_y, decoded.primary_y,
         sizeof(decoded.primary_y));
  out_image->mastering_display.white_x = decoded.white_x;
  out_image->mastering_display.white_y = decoded.white_y;
  out_image->mastering_display.max_luminance = decoded.max_luminance;
  out_image->mastering_display.min_luminance = decoded.min_luminance;
  out_image->has_content_light = decoded.has_content_light;
  out_image->content_light.max_cll = decoded.max_cll;
  out_image->content_light.max_fall = decoded.max_fall;
  if (struct_size >= sizeof(*out_image)) {
    out_image->metadata = (qlic_metadata_block *)decoded.metadata;
    out_image->metadata_count = decoded.metadata_count;
    decoded.metadata = NULL;
    decoded.metadata_count = 0;
  }
  hdr_image_free(&decoded);
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

int QLIC_CALL qlic_get_info_ex(
    const uint8_t *data, size_t size, const qlic_decode_limits *source_limits,
    qlic_info_ex *out_info) {
  clear_err();
  if (!out_info)
    return bad_argument("missing extended output info");
  uint32_t struct_size = out_info->struct_size;
  if (struct_size < sizeof(*out_info) || out_info->reserved)
    return bad_argument("invalid extended output info");
  memset(out_info, 0, sizeof(*out_info));
  out_info->struct_size = sizeof(*out_info);
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
  if (header.mode == MODE_NATIVE_WIDE || header.mode == MODE_HDR_WIDE) {
    out_info->channels = header.palette_count;
    out_info->bits_per_sample = (uint32_t)header.index_bits;
  } else {
    out_info->channels = 4u;
    out_info->bits_per_sample = 8u;
  }
  return QLIC_OK;
}

int QLIC_CALL qlic_get_info_v2(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits, qlic_info_v2 *out_info) {
  clear_err();
  if (!out_info)
    return bad_argument("missing v2 output info");
  uint32_t struct_size = out_info->struct_size;
  if (struct_size < sizeof(*out_info) || out_info->reserved)
    return bad_argument("invalid v2 output info");
  memset(out_info, 0, sizeof(*out_info));
  out_info->struct_size = sizeof(*out_info);
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits_v2(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  QlicHeader header = {0};
  if (!rd_head_limited(data, size, &header, &limits))
    return result_code(QLIC_BAD_DATA);
  out_info->width = header.width;
  out_info->height = header.height;
  out_info->animated = header.mode == MODE_ANIM ? 1u : 0u;
  out_info->frame_count =
      header.mode == MODE_ANIM ? header.palette_count : 1u;
  if (header.mode == MODE_HDR_WIDE) {
    HdrInfo hdr = {0};
    if (!hdr_qlic_info_limited(data, size, &hdr, &header, &limits))
      return result_code(QLIC_BAD_DATA);
    out_info->channels = hdr.channels;
    out_info->bits_per_sample = hdr.bits_per_sample;
    out_info->sample_type = hdr.sample_type;
    out_info->alpha_mode = hdr.alpha_mode;
    out_info->color_authority = hdr.color_authority;
    out_info->has_icc = hdr.has_icc;
    out_info->has_cicp = hdr.has_cicp;
    out_info->has_mastering_display = hdr.has_mastering_display;
    out_info->has_content_light = hdr.has_content_light;
    out_info->metadata_count = hdr.metadata_count;
  } else if (header.mode == MODE_NATIVE_WIDE) {
    out_info->channels = header.palette_count;
    out_info->bits_per_sample = (uint32_t)header.index_bits;
    out_info->sample_type = QLIC_SAMPLE_UINT;
  } else {
    out_info->channels = 4u;
    out_info->bits_per_sample = 8u;
    out_info->sample_type = QLIC_SAMPLE_UINT;
  }
  return QLIC_OK;
}

int QLIC_CALL qlic_validate(
    const uint8_t *data, size_t size,
    const qlic_decode_limits_v2 *source_limits) {
  clear_err();
  if (!data || !size)
    return bad_argument("invalid QLIC input");
  QlicDecodeLimits limits;
  if (!decode_limits_v2(source_limits, &limits))
    return QLIC_BAD_ARGUMENT;
  QlicHeader header = {0};
  if (!rd_head_limited(data, size, &header, &limits))
    return result_code(QLIC_BAD_DATA);

  DecodeState state;
  if (!decode_begin_v2(source_limits, &state))
    return QLIC_BAD_ARGUMENT;
  int ok = 0;
  if (header.mode == MODE_ANIM) {
    Anim animation = {0};
    ok = dec_any_qlic_limited(data, size, &animation, NULL, &limits);
    anim_free(&animation);
  } else if (header.mode == MODE_NATIVE_WIDE) {
    WideImage image = {0};
    ok = dec_wide_qlic_limited(data, size, &image, NULL, &limits);
    wide_image_free(&image);
  } else if (header.mode == MODE_HDR_WIDE) {
    HdrImage image = {0};
    ok = dec_hdr_qlic_limited(data, size, &image, NULL, &limits);
    hdr_image_free(&image);
  } else {
    Image image = {0};
    ok = dec_qlic_limited(data, size, &image, NULL, &limits);
    image_free(&image);
  }
  decode_end(&state);
  return ok ? QLIC_OK : result_code(QLIC_BAD_DATA);
}
