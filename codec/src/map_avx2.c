#include "map_avx2.h"

#include <immintrin.h>
#include <stddef.h>

static __forceinline __m256i load8(const uint16_t *p) {
  return _mm256_cvtepu16_epi32(
      _mm_loadu_si128((const __m128i *)(const void *)p));
}

static __forceinline __m256i clampv(__m256i v, __m256i zero, __m256i maxv) {
  return _mm256_min_epi32(_mm256_max_epi32(v, zero), maxv);
}

static __forceinline uint32_t hsum8(__m256i v) {
  __m128i sum =
      _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  return (uint32_t)_mm_cvtsi128_si32(sum);
}

static __forceinline __m256i gather_cost(const uint32_t *table, __m256i value,
                                         __m256i prediction, __m256i offset) {
  __m256i index = _mm256_add_epi32(_mm256_sub_epi32(value, prediction), offset);
  return _mm256_i32gather_epi32((const int *)(const void *)table, index, 4);
}

static __forceinline __m256i gradient(__m256i w, __m256i n, __m256i nw) {
  __m256i mx = _mm256_max_epi32(w, n);
  __m256i mn = _mm256_min_epi32(w, n);
  __m256i middle = _mm256_sub_epi32(_mm256_add_epi32(w, n), nw);
  __m256i ge =
      _mm256_or_si256(_mm256_cmpgt_epi32(nw, mx), _mm256_cmpeq_epi32(nw, mx));
  __m256i le =
      _mm256_or_si256(_mm256_cmpgt_epi32(mn, nw), _mm256_cmpeq_epi32(nw, mn));
  __m256i result = _mm256_blendv_epi8(middle, mx, le);
  return _mm256_blendv_epi8(result, mn, ge);
}

static __forceinline __m256i paeth(__m256i w, __m256i n, __m256i nw) {
  __m256i p = _mm256_sub_epi32(_mm256_add_epi32(w, n), nw);
  __m256i a = _mm256_abs_epi32(_mm256_sub_epi32(p, w));
  __m256i b = _mm256_abs_epi32(_mm256_sub_epi32(p, n));
  __m256i c = _mm256_abs_epi32(_mm256_sub_epi32(p, nw));
  __m256i a_le_b =
      _mm256_xor_si256(_mm256_cmpgt_epi32(a, b), _mm256_set1_epi32(-1));
  __m256i a_le_c =
      _mm256_xor_si256(_mm256_cmpgt_epi32(a, c), _mm256_set1_epi32(-1));
  __m256i b_le_c =
      _mm256_xor_si256(_mm256_cmpgt_epi32(b, c), _mm256_set1_epi32(-1));
  __m256i choose_w = _mm256_and_si256(a_le_b, a_le_c);
  __m256i result = _mm256_blendv_epi8(nw, n, b_le_c);
  return _mm256_blendv_epi8(result, w, choose_w);
}

static __forceinline __m256i gappv(__m256i w, __m256i n, __m256i nw, __m256i ne,
                                   __m256i ww, __m256i nn, __m256i zero,
                                   __m256i maxv) {
  __m256i edge = _mm256_abs_epi32(_mm256_sub_epi32(ne, n));
  __m256i dh = _mm256_add_epi32(
      _mm256_add_epi32(_mm256_abs_epi32(_mm256_sub_epi32(w, ww)),
                       _mm256_abs_epi32(_mm256_sub_epi32(n, nw))),
      edge);
  __m256i dv = _mm256_add_epi32(
      _mm256_add_epi32(_mm256_abs_epi32(_mm256_sub_epi32(w, nw)),
                       _mm256_abs_epi32(_mm256_sub_epi32(n, nn))),
      edge);
  __m256i d = _mm256_sub_epi32(dv, dh);
  __m256i p = _mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(w, n), 1),
                               _mm256_srai_epi32(_mm256_sub_epi32(ne, nw), 2));
  __m256i three_p = _mm256_add_epi32(_mm256_slli_epi32(p, 1), p);
  __m256i toward_w = _mm256_srai_epi32(_mm256_add_epi32(p, w), 1);
  __m256i slight_w = _mm256_srai_epi32(_mm256_add_epi32(three_p, w), 2);
  __m256i toward_n = _mm256_srai_epi32(_mm256_add_epi32(p, n), 1);
  __m256i slight_n = _mm256_srai_epi32(_mm256_add_epi32(three_p, n), 2);
  __m256i gt32 = _mm256_cmpgt_epi32(d, _mm256_set1_epi32(32));
  __m256i gt8 = _mm256_cmpgt_epi32(d, _mm256_set1_epi32(8));
  __m256i lt32 = _mm256_cmpgt_epi32(_mm256_set1_epi32(-32), d);
  __m256i lt8 = _mm256_cmpgt_epi32(_mm256_set1_epi32(-8), d);
  __m256i result = p;
  result = _mm256_blendv_epi8(result, slight_w, gt8);
  result = _mm256_blendv_epi8(result, toward_w, gt32);
  result = _mm256_blendv_epi8(result, slight_n, lt8);
  result = _mm256_blendv_epi8(result, toward_n, lt32);
  result = _mm256_blendv_epi8(result, w,
                              _mm256_cmpgt_epi32(d, _mm256_set1_epi32(80)));
  result = _mm256_blendv_epi8(result, n,
                              _mm256_cmpgt_epi32(_mm256_set1_epi32(-80), d));
  return clampv(result, zero, maxv);
}

static __forceinline __m256i div3_nonnegative(__m256i v) {
  return _mm256_srli_epi32(_mm256_mullo_epi32(v, _mm256_set1_epi32(21846)), 16);
}

__declspec(noinline) void
qlic_map37_cost_tile_avx2(const uint16_t *plane, uint32_t stride, uint32_t x,
                          uint32_t y, int maxv, const uint32_t *diff_cost,
                          const uint32_t *diff_xzr, uint64_t base_cost[32],
                          uint64_t *xzr_cost, uint32_t tile_size) {
  /* tiles stop at 16 by 16 so 32 bit lane sums stay bounded */
  __m256i base[32];
  __m256i xzr[16];
  const __m256i zero = _mm256_setzero_si256();
  const __m256i vmax = _mm256_set1_epi32(maxv);
  const __m256i offset = vmax;
  for (int i = 0; i < 32; ++i)
    base[i] = zero;
  if (xzr_cost)
    for (int i = 0; i < 16; ++i)
      xzr[i] = zero;

  for (uint32_t ry = 0; ry < tile_size; ++ry) {
    for (uint32_t rx = 0; rx < tile_size; rx += 8u) {
      const uint16_t *row = plane + (size_t)(y + ry) * stride + x + rx;
      const uint16_t *up = row - stride;
      const uint16_t *up2 = up - stride;
      __m256i value = load8(row);
      __m256i w = load8(row - 1);
      __m256i n = load8(up);
      __m256i nw = load8(up - 1);
      __m256i ne = load8(up + 1);
      __m256i ww = load8(row - 2);
      __m256i nn = load8(up2);
      __m256i p[32];

      p[0] = gradient(w, n, nw);
      p[1] = paeth(w, n, nw);
      p[2] = w;
      p[3] = n;
      p[4] = _mm256_srai_epi32(
          _mm256_add_epi32(_mm256_add_epi32(w, n), _mm256_set1_epi32(1)), 1);
      p[5] = clampv(_mm256_sub_epi32(_mm256_add_epi32(n, w), nw), zero, vmax);
      p[6] = ne;
      p[7] = _mm256_srai_epi32(
          _mm256_add_epi32(_mm256_add_epi32(w, ne), _mm256_set1_epi32(1)), 1);
      p[8] = _mm256_srai_epi32(
          _mm256_add_epi32(_mm256_add_epi32(n, ne), _mm256_set1_epi32(1)), 1);
      p[9] = clampv(_mm256_sub_epi32(_mm256_slli_epi32(w, 1), ww), zero, vmax);
      p[10] = clampv(_mm256_sub_epi32(_mm256_slli_epi32(n, 1), nn), zero, vmax);
      p[11] = clampv(
          _mm256_add_epi32(
              w, _mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(n, nw),
                                                      _mm256_set1_epi32(3)),
                                   2)),
          zero, vmax);
      p[12] = clampv(
          _mm256_add_epi32(
              n, _mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(w, nw),
                                                      _mm256_set1_epi32(3)),
                                   2)),
          zero, vmax);
      p[13] = gappv(w, n, nw, ne, ww, nn, zero, vmax);
      p[14] = clampv(
          _mm256_srai_epi32(
              _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(w, n),
                                                _mm256_add_epi32(ne, nw)),
                               _mm256_set1_epi32(2)),
              2),
          zero, vmax);
      p[15] = clampv(
          _mm256_srai_epi32(
              _mm256_add_epi32(
                  _mm256_add_epi32(
                      _mm256_add_epi32(
                          _mm256_mullo_epi32(w, _mm256_set1_epi32(5)),
                          _mm256_slli_epi32(n, 1)),
                      _mm256_sub_epi32(
                          ne, _mm256_mullo_epi32(nw, _mm256_set1_epi32(3)))),
                  _mm256_set1_epi32(2)),
              2),
          zero, vmax);
      p[16] = clampv(_mm256_srai_epi32(
                         _mm256_add_epi32(
                             _mm256_add_epi32(w, _mm256_mullo_epi32(
                                                     n, _mm256_set1_epi32(3))),
                             _mm256_set1_epi32(2)),
                         2),
                     zero, vmax);
      p[17] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_mullo_epi32(w, _mm256_set1_epi32(3)), n),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[18] = clampv(
          _mm256_srai_epi32(
              _mm256_add_epi32(
                  _mm256_add_epi32(
                      _mm256_add_epi32(
                          _mm256_mullo_epi32(n, _mm256_set1_epi32(5)),
                          _mm256_slli_epi32(w, 1)),
                      _mm256_sub_epi32(
                          ne, _mm256_mullo_epi32(nw, _mm256_set1_epi32(3)))),
                  _mm256_set1_epi32(2)),
              2),
          zero, vmax);
      p[19] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_sub_epi32(
                             _mm256_add_epi32(_mm256_slli_epi32(w, 1), n), nw),
                         _mm256_set1_epi32(1)),
                     1),
                 zero, vmax);
      p[20] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_sub_epi32(
                             _mm256_add_epi32(w, _mm256_slli_epi32(n, 1)), nw),
                         _mm256_set1_epi32(1)),
                     1),
                 zero, vmax);
      __m256i ne_nw_half = _mm256_srai_epi32(_mm256_sub_epi32(ne, nw), 1);
      p[21] = clampv(_mm256_add_epi32(w, ne_nw_half), zero, vmax);
      p[22] = clampv(_mm256_add_epi32(n, ne_nw_half), zero, vmax);
      p[23] = clampv(div3_nonnegative(_mm256_add_epi32(
                         _mm256_add_epi32(_mm256_add_epi32(w, n), ne),
                         _mm256_set1_epi32(1))),
                     zero, vmax);
      p[24] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(_mm256_add_epi32(_mm256_slli_epi32(w, 1),
                                                       _mm256_add_epi32(n, ne)),
                                      _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[25] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_add_epi32(w, _mm256_slli_epi32(n, 1)), nw),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[26] = clampv(
          _mm256_srai_epi32(
              _mm256_add_epi32(
                  _mm256_sub_epi32(_mm256_mullo_epi32(_mm256_add_epi32(w, n),
                                                      _mm256_set1_epi32(3)),
                                   _mm256_slli_epi32(nw, 1)),
                  _mm256_set1_epi32(2)),
              2),
          zero, vmax);
      p[27] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_sub_epi32(
                                 _mm256_add_epi32(_mm256_slli_epi32(n, 2), w),
                                 _mm256_slli_epi32(nw, 1)),
                             ne),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[28] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_sub_epi32(
                                 _mm256_add_epi32(_mm256_slli_epi32(w, 2), n),
                                 _mm256_slli_epi32(nw, 1)),
                             ne),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[29] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_sub_epi32(
                             _mm256_add_epi32(_mm256_add_epi32(w, n), ne), nw),
                         _mm256_set1_epi32(1)),
                     1),
                 zero, vmax);
      p[30] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_sub_epi32(
                                 _mm256_add_epi32(_mm256_mullo_epi32(
                                                      w, _mm256_set1_epi32(6)),
                                                  _mm256_slli_epi32(n, 1)),
                                 _mm256_mullo_epi32(nw, _mm256_set1_epi32(5))),
                             ne),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);
      p[31] =
          clampv(_mm256_srai_epi32(
                     _mm256_add_epi32(
                         _mm256_add_epi32(
                             _mm256_sub_epi32(
                                 _mm256_add_epi32(_mm256_slli_epi32(w, 1),
                                                  _mm256_mullo_epi32(
                                                      n, _mm256_set1_epi32(6))),
                                 _mm256_mullo_epi32(nw, _mm256_set1_epi32(5))),
                             ne),
                         _mm256_set1_epi32(2)),
                     2),
                 zero, vmax);

      static const int8_t xmap[32] = {
          0,  -1, 1,  2,  3,  4,  5,  6,  -1, 8,  9, -1, -1, -1, -1, -1,
          -1, 15, -1, -1, -1, -1, -1, -1, -1, -1, 7, -1, -1, -1, -1, -1};
      for (int i = 0; i < 32; ++i) {
        base[i] = _mm256_add_epi32(base[i],
                                   gather_cost(diff_cost, value, p[i], offset));
        if (xzr_cost && xmap[i] >= 0)
          xzr[xmap[i]] = _mm256_add_epi32(
              xzr[xmap[i]], gather_cost(diff_xzr, value, p[i], offset));
      }
      if (xzr_cost) {
        __m256i xp;
        xp = clampv(_mm256_srai_epi32(
                        _mm256_add_epi32(
                            _mm256_add_epi32(
                                _mm256_sub_epi32(_mm256_slli_epi32(w, 1), ww),
                                _mm256_sub_epi32(_mm256_slli_epi32(n, 1), nn)),
                            _mm256_set1_epi32(1)),
                        1),
                    zero, vmax);
        xzr[10] =
            _mm256_add_epi32(xzr[10], gather_cost(diff_xzr, value, xp, offset));
        xp = clampv(
            _mm256_add_epi32(w, _mm256_srai_epi32(_mm256_sub_epi32(n, nw), 1)),
            zero, vmax);
        xzr[11] =
            _mm256_add_epi32(xzr[11], gather_cost(diff_xzr, value, xp, offset));
        xp = clampv(
            _mm256_add_epi32(n, _mm256_srai_epi32(_mm256_sub_epi32(w, nw), 1)),
            zero, vmax);
        xzr[12] =
            _mm256_add_epi32(xzr[12], gather_cost(diff_xzr, value, xp, offset));
        xp = clampv(
            _mm256_srai_epi32(
                _mm256_add_epi32(
                    _mm256_add_epi32(
                        _mm256_sub_epi32(
                            _mm256_add_epi32(_mm256_slli_epi32(w, 2),
                                             _mm256_slli_epi32(n, 1)),
                            _mm256_mullo_epi32(nw, _mm256_set1_epi32(3))),
                        ne),
                    _mm256_set1_epi32(2)),
                2),
            zero, vmax);
        xzr[13] =
            _mm256_add_epi32(xzr[13], gather_cost(diff_xzr, value, xp, offset));
        xp = clampv(
            _mm256_srai_epi32(
                _mm256_add_epi32(
                    _mm256_add_epi32(
                        w, _mm256_add_epi32(_mm256_slli_epi32(n, 1), ne)),
                    _mm256_set1_epi32(2)),
                2),
            zero, vmax);
        xzr[14] =
            _mm256_add_epi32(xzr[14], gather_cost(diff_xzr, value, xp, offset));
      }
    }
  }

  for (int i = 0; i < 32; ++i)
    base_cost[i] += hsum8(base[i]);
  if (xzr_cost)
    for (int i = 0; i < 16; ++i)
      xzr_cost[i] += hsum8(xzr[i]);
  _mm256_zeroupper();
}
