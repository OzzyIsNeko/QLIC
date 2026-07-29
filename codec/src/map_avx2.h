#ifndef QLIC_MAP_AVX2_H
#define QLIC_MAP_AVX2_H

#include <stdint.h>

void qlic_map37_cost_tile_avx2(const uint16_t *plane, uint32_t stride,
                               uint32_t x, uint32_t y, int maxv,
                               const uint32_t *diff_cost,
                               const uint32_t *diff_xzr, uint64_t base_cost[32],
                               uint64_t *xzr_cost, uint32_t tile_size);

#endif
