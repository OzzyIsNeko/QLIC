# C SDK

Include the public header.

```c
#include <qlic/qlic.h>
```

With CMake, link the static library or DLL import target.

```cmake
find_package(qlic 0.5 CONFIG REQUIRED)
target_link_libraries(app PRIVATE qlic::qlic_static)
```

Use `qlic::qlic` for the DLL. The static package uses the static MSVC runtime.

## Encode

```c
qlic_encode_options options;
qlic_encode_options_default(&options);
options.threads = 1;

uint8_t *encoded = NULL;
size_t encoded_size = 0;
int status = qlic_encode_rgba(
    rgba, rgba_size, width, height, stride, &options,
    &encoded, &encoded_size);
if (status == QLIC_OK) {
  qlic_free(encoded);
}
```

The byte count has to cover the final row, and the stride has to cover one RGBA row. A thread count of zero means one thread. Values above the available hardware count are clamped. Files from this release use the same decoder on every supported platform.

## Decode

```c
qlic_image image = {0};
int status = qlic_decode_rgba(data, size, NULL, &image);
if (status == QLIC_OK) {
  qlic_image_free(&image);
}
```

Pass null to use the default limits. To change them:

```c
qlic_decode_limits limits;
qlic_decode_limits_default(&limits);
limits.threads = qlic_hardware_thread_count();
limits.max_pixels = 16000000;

qlic_image image = {0};
int status = qlic_decode_rgba(data, size, &limits, &image);
```

The defaults are one thread, 512 MiB for input and payload data, 67,108,864 pixels, 512 MiB for decoded animation pixels, and 100,000 frames.

## Animation

`qlic_encode_animation` takes an array of `qlic_frame_input`. Every frame has its own byte count, stride, dimensions, and delay.

`qlic_decode_animation` decodes still or animated QLIC data into `qlic_animation`. Use `qlic_animation_free` when finished.

`qlic_get_info` reads dimensions, frame count, and animation state without decoding pixel data.

## Errors and ownership

Functions return `QLIC_OK` or a negative status. `qlic_status_string` describes the status. `qlic_last_error` gives details from the latest call on the current thread.

Memory returned by an encode is released with `qlic_free`. Decoded still images use `qlic_image_free`. Decoded animations use `qlic_animation_free`.

Input memory stays with the caller and has to remain valid until the call returns. Separate threads can call the SDK at the same time.
