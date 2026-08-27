#include <qlic/qlic.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  qlic_decode_limits_v2 limits;
  qlic_decode_limits_v2_default(&limits);
  limits.max_file_bytes = 16u * 1024u * 1024u;
  limits.max_payload_bytes = 16u * 1024u * 1024u;
  limits.max_pixels = 4u * 1024u * 1024u;
  limits.max_animation_bytes = 32u * 1024u * 1024u;
  limits.max_decoded_bytes = 64u * 1024u * 1024u;
  limits.max_metadata_bytes = 2u * 1024u * 1024u;
  limits.max_frames = 64u;
  limits.max_chunks = 64u;

  qlic_info_v2 info;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  (void)qlic_get_info_v2(data, size, &limits, &info);
  (void)qlic_validate(data, size, &limits);

  if (size > 12u && data[12] == 20u) {
    qlic_hdr_image image;
    memset(&image, 0, sizeof(image));
    image.struct_size = sizeof(image);
    (void)qlic_decode_hdr(data, size, &limits, &image);
    qlic_hdr_image_free(&image);
  } else if (size > 12u && data[12] == 19u) {
    qlic_decode_limits legacy;
    qlic_decode_limits_default(&legacy);
    legacy.max_file_bytes = limits.max_file_bytes;
    legacy.max_payload_bytes = limits.max_payload_bytes;
    legacy.max_pixels = limits.max_pixels;
    legacy.max_animation_bytes = limits.max_animation_bytes;
    legacy.max_frames = limits.max_frames;
    qlic_wide_image image;
    memset(&image, 0, sizeof(image));
    (void)qlic_decode_wide(data, size, &legacy, &image);
    qlic_wide_image_free(&image);
  } else {
    qlic_animation animation;
    memset(&animation, 0, sizeof(animation));
    qlic_decode_limits legacy;
    qlic_decode_limits_default(&legacy);
    legacy.max_file_bytes = limits.max_file_bytes;
    legacy.max_payload_bytes = limits.max_payload_bytes;
    legacy.max_pixels = limits.max_pixels;
    legacy.max_animation_bytes = limits.max_animation_bytes;
    legacy.max_frames = limits.max_frames;
    (void)qlic_decode_animation(data, size, &legacy, &animation);
    qlic_animation_free(&animation);
  }
  return 0;
}
