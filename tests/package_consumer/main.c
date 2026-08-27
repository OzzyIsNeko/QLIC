#include <qlic/qlic.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    const uint8_t source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t *encoded = 0;
    size_t encoded_size = 0;
    qlic_image decoded = {0};
    int result = qlic_encode_rgba(source, sizeof(source), 2, 1, 8, NULL,
                                  &encoded, &encoded_size);
    if (result == QLIC_OK)
        result = qlic_decode_rgba(encoded, encoded_size, NULL, &decoded);
    int exact = result == QLIC_OK && decoded.width == 2 &&
                decoded.height == 1 &&
                decoded.stride == 8 && decoded.rgba_size == sizeof(source) &&
                memcmp(decoded.rgba, source, sizeof(source)) == 0;
    qlic_image_free(&decoded);
    qlic_free(encoded);

    const uint16_t wide_source[12] = {
        0, 1, 4095, 17, 2048, 3001, 4094, 123, 777, 1024, 2047, 3333};
    encoded = NULL;
    encoded_size = 0;
    qlic_wide_image wide = {0};
    result = qlic_encode_wide(
        wide_source, sizeof(wide_source), 2, 2, 2u * 3u * sizeof(uint16_t),
        3, 12, NULL, &encoded, &encoded_size);
    if (result == QLIC_OK)
        result = qlic_decode_wide(encoded, encoded_size, NULL, &wide);
    int wide_exact = result == QLIC_OK && wide.width == 2 &&
                     wide.height == 2 && wide.channels == 3 &&
                     wide.bits_per_sample == 12 &&
                     wide.pixels_size == sizeof(wide_source) &&
                     memcmp(wide.pixels, wide_source, sizeof(wide_source)) == 0;
    qlic_wide_image_free(&wide);
    qlic_free(encoded);
    return exact && wide_exact ? 0 : 1;
}
