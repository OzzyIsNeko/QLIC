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
    return exact ? 0 : 1;
}
