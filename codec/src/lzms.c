#include "lzms.h"

#ifdef QLIC_WASM
void *malloc(size_t size);
void free(void *pointer);
void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
#else
#  include <stdlib.h>
#  include <string.h>
#endif

/*
 * This is a C port of Deployment Theory's clean room LZMS decoder
 * See third_party/lzms/LICENSE
 */

enum {
  LZMS_REPS = 3,
  LZMS_MAIN_PROBS = 16,
  LZMS_MATCH_PROBS = 32,
  LZMS_LZ_PROBS = 64,
  LZMS_REP_PROBS = 64,
  LZMS_PROB_BITS = 6,
  LZMS_PROB_DENOMINATOR = 1 << LZMS_PROB_BITS,
  LZMS_INITIAL_PROB = 48,
  LZMS_LITERAL_SYMBOLS = 256,
  LZMS_LENGTH_SYMBOLS = 54,
  LZMS_DELTA_POWER_SYMBOLS = 8,
  LZMS_MAX_OFFSET_SYMBOLS = 799,
  LZMS_MAX_CODEWORD_BITS = 15,
  LZMS_TABLE_SIZE = 1 << LZMS_MAX_CODEWORD_BITS,
  LZMS_SYMBOL_BITS = 10,
  LZMS_SYMBOL_MASK = (1 << LZMS_SYMBOL_BITS) - 1,
  LZMS_X86_WINDOW = 65535,
  LZMS_X86_MAX_TRANSLATION = 1023
};

#define LZMS_INITIAL_RECENT UINT64_C(0x0000000055555555)
#define LZMS_FREQ_MASK (~(uint32_t)LZMS_SYMBOL_MASK)

typedef struct {
  uint32_t zeros;
  uint64_t recent;
} LzmsProb;

typedef struct {
  uint32_t range;
  uint32_t code;
  const uint8_t *source;
  size_t next;
  size_t size;
} LzmsRange;

typedef struct {
  uint64_t bits;
  unsigned available;
  const uint8_t *source;
  size_t next;
} LzmsBits;

typedef struct {
  unsigned symbols;
  unsigned rebuild_frequency;
  unsigned until_rebuild;
  uint32_t *frequencies;
  uint32_t *scratch;
  uint8_t *lengths;
  uint16_t *table;
} LzmsHuffman;

typedef struct {
  LzmsHuffman literal;
  LzmsHuffman lz_offset;
  LzmsHuffman length;
  LzmsHuffman delta_offset;
  LzmsHuffman delta_power;
  LzmsProb main[LZMS_MAIN_PROBS];
  LzmsProb match[LZMS_MATCH_PROBS];
  LzmsProb lz[LZMS_LZ_PROBS];
  LzmsProb delta[LZMS_LZ_PROBS];
  LzmsProb lz_rep[LZMS_REPS - 1][LZMS_REP_PROBS];
  LzmsProb delta_rep[LZMS_REPS - 1][LZMS_REP_PROBS];
  uint8_t offset_extra[LZMS_MAX_OFFSET_SYMBOLS];
  uint32_t offset_base[LZMS_MAX_OFFSET_SYMBOLS + 1];
  uint32_t length_base[LZMS_LENGTH_SYMBOLS + 1];
} LzmsDecoder;

static const uint8_t lzms_length_extra[LZMS_LENGTH_SYMBOLS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 6, 7, 8, 9, 10, 16, 30};

static const uint16_t lzms_offset_extra_counts[31] = {
    8, 0, 9, 7, 10, 15, 15, 20, 20, 30, 33, 40, 42, 45, 60, 73,
    80, 85, 95, 105, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static uint16_t lzms_read16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t lzms_read32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t lzms_read64(const uint8_t *p) {
  return (uint64_t)lzms_read32(p) | (uint64_t)lzms_read32(p + 4) << 32;
}

static void lzms_sort(uint32_t *values, size_t count) {
  if (count < 2)
    return;
  for (size_t root = count / 2; root > 0;) {
    --root;
    size_t parent = root;
    for (;;) {
      size_t child = parent * 2u + 1u;
      if (child >= count)
        break;
      if (child + 1u < count && values[child] < values[child + 1u])
        ++child;
      if (values[parent] >= values[child])
        break;
      uint32_t swap = values[parent];
      values[parent] = values[child];
      values[child] = swap;
      parent = child;
    }
  }
  for (size_t end = count; end > 1;) {
    --end;
    uint32_t swap = values[0];
    values[0] = values[end];
    values[end] = swap;
    size_t parent = 0;
    for (;;) {
      size_t child = parent * 2u + 1u;
      if (child >= end)
        break;
      if (child + 1u < end && values[child] < values[child + 1u])
        ++child;
      if (values[parent] >= values[child])
        break;
      swap = values[parent];
      values[parent] = values[child];
      values[child] = swap;
      parent = child;
    }
  }
}

static void lzms_build_tree(uint32_t *tree, size_t symbols) {
  size_t last = symbols - 1u;
  size_t leaf = 0;
  size_t queued = 0;
  size_t end = 0;
  for (;;) {
    uint32_t frequency;
    if (leaf + 1u <= last &&
        (queued == end ||
         (tree[leaf + 1u] & LZMS_FREQ_MASK) <=
             (tree[queued] & LZMS_FREQ_MASK))) {
      frequency = (tree[leaf] & LZMS_FREQ_MASK) +
                  (tree[leaf + 1u] & LZMS_FREQ_MASK);
      leaf += 2u;
    } else if (queued + 2u <= end &&
               (leaf > last ||
                (tree[queued + 1u] & LZMS_FREQ_MASK) <
                    (tree[leaf] & LZMS_FREQ_MASK))) {
      frequency = (tree[queued] & LZMS_FREQ_MASK) +
                  (tree[queued + 1u] & LZMS_FREQ_MASK);
      tree[queued] =
          (uint32_t)(end << LZMS_SYMBOL_BITS) |
          (tree[queued] & LZMS_SYMBOL_MASK);
      tree[queued + 1u] =
          (uint32_t)(end << LZMS_SYMBOL_BITS) |
          (tree[queued + 1u] & LZMS_SYMBOL_MASK);
      queued += 2u;
    } else {
      frequency = (tree[leaf] & LZMS_FREQ_MASK) +
                  (tree[queued] & LZMS_FREQ_MASK);
      tree[queued] =
          (uint32_t)(end << LZMS_SYMBOL_BITS) |
          (tree[queued] & LZMS_SYMBOL_MASK);
      ++leaf;
      ++queued;
    }
    tree[end] = frequency | (tree[end] & LZMS_SYMBOL_MASK);
    ++end;
    if (end >= last)
      return;
  }
}

static int lzms_make_lengths(LzmsHuffman *huffman) {
  size_t used = 0;
  for (size_t symbol = 0; symbol < huffman->symbols; ++symbol) {
    if (!huffman->frequencies[symbol]) {
      huffman->lengths[symbol] = 0;
      continue;
    }
    huffman->scratch[used++] =
        (uint32_t)symbol |
        (huffman->frequencies[symbol] << LZMS_SYMBOL_BITS);
  }
  lzms_sort(huffman->scratch, used);
  if (!used)
    return 0;
  if (used == 1u) {
    size_t symbol = huffman->scratch[0] & LZMS_SYMBOL_MASK;
    size_t other = symbol ? symbol : 1u;
    if (other >= huffman->symbols)
      return 0;
    huffman->lengths[symbol] = 1;
    huffman->lengths[other] = 1;
    return 1;
  }
  lzms_build_tree(huffman->scratch, used);
  int counts[LZMS_MAX_CODEWORD_BITS + 2] = {0};
  counts[1] = 2;
  huffman->scratch[used - 2u] &= LZMS_SYMBOL_MASK;
  for (size_t node = used - 2u; node > 0;) {
    --node;
    size_t parent = huffman->scratch[node] >> LZMS_SYMBOL_BITS;
    unsigned depth =
        (huffman->scratch[parent] >> LZMS_SYMBOL_BITS) + 1u;
    huffman->scratch[node] =
        (huffman->scratch[node] & LZMS_SYMBOL_MASK) |
        (uint32_t)(depth << LZMS_SYMBOL_BITS);
    unsigned length = depth;
    if (length >= LZMS_MAX_CODEWORD_BITS) {
      length = LZMS_MAX_CODEWORD_BITS;
      while (length && !counts[length])
        --length;
      if (!length)
        return 0;
    }
    --counts[length];
    counts[length + 1u] += 2;
  }
  size_t index = 0;
  for (unsigned length = LZMS_MAX_CODEWORD_BITS; length > 0; --length) {
    for (int count = counts[length]; count > 0; --count) {
      if (index >= used)
        return 0;
      size_t symbol = huffman->scratch[index++] & LZMS_SYMBOL_MASK;
      huffman->lengths[symbol] = (uint8_t)length;
    }
  }
  return index == used;
}

static int lzms_build_table(LzmsHuffman *huffman) {
  size_t counts[LZMS_MAX_CODEWORD_BITS + 1] = {0};
  for (size_t symbol = 0; symbol < huffman->symbols; ++symbol) {
    unsigned length = huffman->lengths[symbol];
    if (length > LZMS_MAX_CODEWORD_BITS)
      return 0;
    ++counts[length];
  }
  size_t positions[LZMS_MAX_CODEWORD_BITS + 2] = {0};
  for (unsigned length = 1; length <= LZMS_MAX_CODEWORD_BITS; ++length)
    positions[length + 1u] =
        positions[length] +
        (counts[length] << (LZMS_MAX_CODEWORD_BITS - length));
  for (size_t symbol = 0; symbol < huffman->symbols; ++symbol) {
    unsigned length = huffman->lengths[symbol];
    if (!length)
      continue;
    size_t start = positions[length];
    size_t end = start + ((size_t)1u <<
                          (LZMS_MAX_CODEWORD_BITS - length));
    if (end > LZMS_TABLE_SIZE)
      return 0;
    for (size_t index = start; index < end; ++index)
      huffman->table[index] = (uint16_t)symbol;
    positions[length] = end;
  }
  return positions[LZMS_MAX_CODEWORD_BITS + 1u] == LZMS_TABLE_SIZE;
}

static int lzms_huffman_rebuild(LzmsHuffman *huffman) {
  if (!lzms_make_lengths(huffman) || !lzms_build_table(huffman))
    return 0;
  huffman->until_rebuild = huffman->rebuild_frequency;
  for (size_t symbol = 0; symbol < huffman->symbols; ++symbol)
    huffman->frequencies[symbol] =
        (huffman->frequencies[symbol] >> 1) + 1u;
  return 1;
}

static int lzms_huffman_init(LzmsHuffman *huffman, unsigned symbols,
                             unsigned rebuild_frequency) {
  memset(huffman, 0, sizeof(*huffman));
  if (!symbols)
    return 1;
  huffman->symbols = symbols;
  huffman->rebuild_frequency = rebuild_frequency;
  huffman->frequencies =
      (uint32_t *)malloc((size_t)symbols * sizeof(uint32_t));
  huffman->scratch =
      (uint32_t *)malloc((size_t)symbols * sizeof(uint32_t));
  huffman->lengths = (uint8_t *)malloc(symbols);
  huffman->table =
      (uint16_t *)malloc((size_t)LZMS_TABLE_SIZE * sizeof(uint16_t));
  if (!huffman->frequencies || !huffman->scratch || !huffman->lengths ||
      !huffman->table)
    return 0;
  memset(huffman->lengths, 0, symbols);
  for (size_t symbol = 0; symbol < symbols; ++symbol)
    huffman->frequencies[symbol] = 1;
  return lzms_huffman_rebuild(huffman);
}

static void lzms_huffman_free(LzmsHuffman *huffman) {
  free(huffman->frequencies);
  free(huffman->scratch);
  free(huffman->lengths);
  free(huffman->table);
  memset(huffman, 0, sizeof(*huffman));
}

static void lzms_probs_init(LzmsProb *probabilities, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    probabilities[index].zeros = LZMS_INITIAL_PROB;
    probabilities[index].recent = LZMS_INITIAL_RECENT;
  }
}

static uint32_t lzms_probability(const LzmsProb *probability) {
  uint32_t value = probability->zeros;
  value += (value - 1u) >> 31;
  value -= value >> LZMS_PROB_BITS;
  return value;
}

static int lzms_range_bit(LzmsRange *range, uint32_t *state,
                          uint32_t states, LzmsProb *probabilities) {
  LzmsProb *probability = &probabilities[*state];
  *state = (*state << 1) & (states - 1u);
  uint32_t chance = lzms_probability(probability);
  if (!(range->range & UINT32_C(0xffff0000))) {
    range->range <<= 16;
    range->code <<= 16;
    if (range->next + 2u <= range->size) {
      range->code |= lzms_read16(range->source + range->next);
      range->next += 2u;
    }
  }
  uint32_t boundary = (range->range >> LZMS_PROB_BITS) * chance;
  uint32_t bit;
  if (range->code < boundary) {
    range->range = boundary;
    bit = 0;
  } else {
    range->range -= boundary;
    range->code -= boundary;
    *state |= 1u;
    bit = 1;
  }
  int32_t delta =
      (int32_t)(probability->recent >> (LZMS_PROB_DENOMINATOR - 1)) -
      (int32_t)bit;
  probability->zeros =
      (uint32_t)((int32_t)probability->zeros + delta);
  probability->recent = (probability->recent << 1) | bit;
  return (int)bit;
}

static void lzms_bits_ensure(LzmsBits *bits, unsigned count) {
  if (bits->available >= count)
    return;
  unsigned space = 64u - bits->available;
  if (bits->next >= 2u) {
    bits->next -= 2u;
    bits->bits |=
        (uint64_t)lzms_read16(bits->source + bits->next) << (space - 16u);
  }
  if (bits->next >= 2u) {
    bits->next -= 2u;
    bits->bits |=
        (uint64_t)lzms_read16(bits->source + bits->next) << (space - 32u);
  }
  bits->available += 32u;
}

static uint32_t lzms_bits_peek(const LzmsBits *bits, unsigned count) {
  return (uint32_t)((bits->bits >> 1) >> (63u - count));
}

static uint32_t lzms_bits_read(LzmsBits *bits, unsigned count) {
  if (!count)
    return 0;
  lzms_bits_ensure(bits, count);
  uint32_t value = lzms_bits_peek(bits, count);
  bits->bits <<= count;
  bits->available -= count;
  return value;
}

static int lzms_huffman_decode(LzmsHuffman *huffman, LzmsBits *bits) {
  if (!huffman->symbols)
    return -1;
  lzms_bits_ensure(bits, LZMS_MAX_CODEWORD_BITS);
  uint32_t index = lzms_bits_peek(bits, LZMS_MAX_CODEWORD_BITS);
  unsigned symbol = huffman->table[index];
  if (symbol >= huffman->symbols || !huffman->lengths[symbol])
    return -1;
  unsigned length = huffman->lengths[symbol];
  bits->bits <<= length;
  bits->available -= length;
  ++huffman->frequencies[symbol];
  if (!--huffman->until_rebuild &&
      !lzms_huffman_rebuild(huffman))
    return -1;
  return (int)symbol;
}

static unsigned lzms_make_slots(LzmsDecoder *decoder,
                                size_t uncompressed_size) {
  uint32_t base = 1;
  size_t slot = 0;
  unsigned needed = 0;
  uint32_t target =
      uncompressed_size > 1u ? (uint32_t)(uncompressed_size - 1u) : 0;
  for (unsigned bits = 0; bits <= 30; ++bits) {
    for (unsigned count = 0;
         count < lzms_offset_extra_counts[bits]; ++count) {
      if (slot >= LZMS_MAX_OFFSET_SYMBOLS)
        return 0;
      decoder->offset_extra[slot] = (uint8_t)bits;
      decoder->offset_base[slot] = base;
      uint32_t width = UINT32_C(1) << bits;
      if (!needed && target >= base && target - base < width)
        needed = (unsigned)slot + 1u;
      base += width;
      ++slot;
    }
  }
  decoder->offset_base[slot] = base;
  base = 1;
  for (size_t index = 0; index < LZMS_LENGTH_SYMBOLS; ++index) {
    decoder->length_base[index] = base;
    base += UINT32_C(1) << lzms_length_extra[index];
  }
  decoder->length_base[LZMS_LENGTH_SYMBOLS] = base;
  return uncompressed_size < 2u ? 0u : needed;
}

static int lzms_decoder_init(LzmsDecoder *decoder, size_t output_size) {
  memset(decoder, 0, sizeof(*decoder));
  unsigned offsets = lzms_make_slots(decoder, output_size);
  if (output_size >= 2u && !offsets)
    return 0;
  if (!lzms_huffman_init(&decoder->literal, LZMS_LITERAL_SYMBOLS, 1024) ||
      !lzms_huffman_init(&decoder->lz_offset, offsets, 1024) ||
      !lzms_huffman_init(&decoder->length, LZMS_LENGTH_SYMBOLS, 512) ||
      !lzms_huffman_init(&decoder->delta_offset, offsets, 1024) ||
      !lzms_huffman_init(&decoder->delta_power,
                         LZMS_DELTA_POWER_SYMBOLS, 512))
    return 0;
  lzms_probs_init(decoder->main, LZMS_MAIN_PROBS);
  lzms_probs_init(decoder->match, LZMS_MATCH_PROBS);
  lzms_probs_init(decoder->lz, LZMS_LZ_PROBS);
  lzms_probs_init(decoder->delta, LZMS_LZ_PROBS);
  for (size_t index = 0; index < LZMS_REPS - 1; ++index) {
    lzms_probs_init(decoder->lz_rep[index], LZMS_REP_PROBS);
    lzms_probs_init(decoder->delta_rep[index], LZMS_REP_PROBS);
  }
  return 1;
}

static void lzms_decoder_free(LzmsDecoder *decoder) {
  lzms_huffman_free(&decoder->literal);
  lzms_huffman_free(&decoder->lz_offset);
  lzms_huffman_free(&decoder->length);
  lzms_huffman_free(&decoder->delta_offset);
  lzms_huffman_free(&decoder->delta_power);
}

static uint32_t lzms_decode_offset(LzmsHuffman *huffman,
                                   const LzmsDecoder *decoder,
                                   LzmsBits *bits) {
  int slot = lzms_huffman_decode(huffman, bits);
  if (slot < 0)
    return 0;
  return decoder->offset_base[slot] +
         lzms_bits_read(bits, decoder->offset_extra[slot]);
}

static uint32_t lzms_decode_length(LzmsDecoder *decoder, LzmsBits *bits) {
  int slot = lzms_huffman_decode(&decoder->length, bits);
  if (slot < 0)
    return 0;
  return decoder->length_base[slot] +
         lzms_bits_read(bits, lzms_length_extra[slot]);
}

static int lzms_opcode(uint8_t value) {
  return value == 0x48 || value == 0x4c || value == 0xe8 ||
         value == 0xe9 || value == 0xf0 || value == 0xff;
}

static size_t lzms_next_opcode(const uint8_t *data, size_t position) {
  while (!lzms_opcode(data[position]))
    ++position;
  return position;
}

static size_t lzms_translate(uint8_t *data, size_t position,
                             int *last_x86, int *last_targets) {
  int max_offset = LZMS_X86_MAX_TRANSLATION;
  size_t opcode_bytes = 0;
  if (data[position] >= 0xf0) {
    if (data[position] & 0x0f) {
      if (data[position + 1u] != 0x15)
        return position + 1u;
      opcode_bytes = 2;
    } else {
      if (data[position + 1u] != 0x83 ||
          data[position + 2u] != 0x05)
        return position + 1u;
      opcode_bytes = 3;
    }
  } else if (data[position] <= 0x4c) {
    if ((data[position + 2u] & 0x07) != 0x05 ||
        (data[position + 1u] != 0x8d &&
         (data[position + 1u] != 0x8b ||
          (data[position] & 0x04) ||
          (data[position + 2u] & 0xf0))))
      return position + 1u;
    opcode_bytes = 3;
  } else {
    if (data[position] & 1u)
      return position + 4u;
    opcode_bytes = 1;
    max_offset >>= 1;
  }
  size_t instruction = position;
  position += opcode_bytes;
  if ((int)instruction - *last_x86 <= max_offset) {
    uint32_t value = lzms_read32(data + position);
    value -= (uint32_t)instruction;
    data[position] = (uint8_t)value;
    data[position + 1u] = (uint8_t)(value >> 8);
    data[position + 2u] = (uint8_t)(value >> 16);
    data[position + 3u] = (uint8_t)(value >> 24);
  }
  uint16_t target =
      (uint16_t)((uint32_t)instruction + lzms_read16(data + position));
  instruction += opcode_bytes + 3u;
  if ((int)instruction - last_targets[target] <= LZMS_X86_WINDOW)
    *last_x86 = (int)instruction;
  last_targets[target] = (int)instruction;
  return position + 4u;
}

static int lzms_x86_filter(uint8_t *data, size_t size) {
  if (size <= 17u)
    return 1;
  int *last_targets = (int *)malloc(65536u * sizeof(int));
  if (!last_targets)
    return 0;
  for (size_t index = 0; index < 65536u; ++index)
    last_targets[index] = -LZMS_X86_WINDOW - 1;
  int last_x86 = -LZMS_X86_MAX_TRANSLATION - 1;
  size_t sentinel = size - 8u;
  uint8_t saved = data[sentinel];
  data[sentinel] = 0xe8;
  size_t tail = size - 16u;
  size_t position = 1;
  for (;;) {
    position = lzms_next_opcode(data, position);
    if (position >= tail)
      break;
    position =
        lzms_translate(data, position, &last_x86, last_targets);
  }
  data[sentinel] = saved;
  free(last_targets);
  return 1;
}

static int lzms_raw_decompress(const uint8_t *source, size_t source_size,
                               uint8_t *destination,
                               size_t destination_size) {
  if (!source || !destination || (source_size & 1u) || source_size < 4u)
    return 0;
  LzmsDecoder decoder;
  if (!lzms_decoder_init(&decoder, destination_size)) {
    lzms_decoder_free(&decoder);
    return 0;
  }
  LzmsRange range = {
      UINT32_MAX,
      (uint32_t)lzms_read16(source) << 16 | lzms_read16(source + 2),
      source,
      4,
      source_size};
  LzmsBits bits = {0, 0, source, source_size};
  uint32_t recent_lz[LZMS_REPS + 1] = {1, 2, 3, 4};
  uint64_t recent_delta[LZMS_REPS + 1] = {1, 2, 3, 4};
  uint32_t main_state = 0;
  uint32_t match_state = 0;
  uint32_t lz_state = 0;
  uint32_t delta_state = 0;
  uint32_t lz_rep_states[LZMS_REPS - 1] = {0};
  uint32_t delta_rep_states[LZMS_REPS - 1] = {0};
  int previous = 0;
  size_t position = 0;
  int ok = 1;
  while (ok && position < destination_size) {
    if (!lzms_range_bit(&range, &main_state, LZMS_MAIN_PROBS,
                        decoder.main)) {
      int symbol = lzms_huffman_decode(&decoder.literal, &bits);
      if (symbol < 0) {
        ok = 0;
        break;
      }
      destination[position++] = (uint8_t)symbol;
      previous = 0;
      continue;
    }
    if (!lzms_range_bit(&range, &match_state, LZMS_MATCH_PROBS,
                        decoder.match)) {
      uint32_t offset;
      if (!lzms_range_bit(&range, &lz_state, LZMS_LZ_PROBS,
                          decoder.lz)) {
        offset = lzms_decode_offset(&decoder.lz_offset, &decoder, &bits);
        recent_lz[3] = recent_lz[2];
        recent_lz[2] = recent_lz[1];
        recent_lz[1] = recent_lz[0];
      } else {
        size_t adjustment = (size_t)(previous & 1);
        if (!lzms_range_bit(&range, &lz_rep_states[0], LZMS_REP_PROBS,
                            decoder.lz_rep[0])) {
          offset = recent_lz[adjustment];
          recent_lz[adjustment] = recent_lz[0];
        } else if (!lzms_range_bit(&range, &lz_rep_states[1],
                                   LZMS_REP_PROBS,
                                   decoder.lz_rep[1])) {
          offset = recent_lz[1u + adjustment];
          recent_lz[1u + adjustment] = recent_lz[1];
          recent_lz[1] = recent_lz[0];
        } else {
          offset = recent_lz[2u + adjustment];
          recent_lz[2u + adjustment] = recent_lz[2];
          recent_lz[2] = recent_lz[1];
          recent_lz[1] = recent_lz[0];
        }
      }
      recent_lz[0] = offset;
      previous = 1;
      uint32_t length = lzms_decode_length(&decoder, &bits);
      if (!offset || !length || offset > position ||
          length > destination_size - position) {
        ok = 0;
        break;
      }
      size_t match = position - offset;
      for (uint32_t index = 0; index < length; ++index)
        destination[position++] = destination[match++];
      continue;
    }
    uint64_t pair;
    if (!lzms_range_bit(&range, &delta_state, LZMS_LZ_PROBS,
                        decoder.delta)) {
      int power = lzms_huffman_decode(&decoder.delta_power, &bits);
      uint32_t offset =
          lzms_decode_offset(&decoder.delta_offset, &decoder, &bits);
      if (power < 0 || !offset) {
        ok = 0;
        break;
      }
      pair = (uint64_t)(unsigned)power << 32 | offset;
      recent_delta[3] = recent_delta[2];
      recent_delta[2] = recent_delta[1];
      recent_delta[1] = recent_delta[0];
    } else {
      size_t adjustment = (size_t)(previous >> 1);
      if (!lzms_range_bit(&range, &delta_rep_states[0],
                          LZMS_REP_PROBS, decoder.delta_rep[0])) {
        pair = recent_delta[adjustment];
        recent_delta[adjustment] = recent_delta[0];
      } else if (!lzms_range_bit(&range, &delta_rep_states[1],
                                 LZMS_REP_PROBS,
                                 decoder.delta_rep[1])) {
        pair = recent_delta[1u + adjustment];
        recent_delta[1u + adjustment] = recent_delta[1];
        recent_delta[1] = recent_delta[0];
      } else {
        pair = recent_delta[2u + adjustment];
        recent_delta[2u + adjustment] = recent_delta[2];
        recent_delta[2] = recent_delta[1];
        recent_delta[1] = recent_delta[0];
      }
    }
    recent_delta[0] = pair;
    previous = 2;
    uint32_t length = lzms_decode_length(&decoder, &bits);
    uint32_t power = (uint32_t)(pair >> 32);
    uint32_t raw_offset = (uint32_t)pair;
    uint32_t span = UINT32_C(1) << power;
    uint32_t offset = raw_offset << power;
    if (!length || !offset || offset >> power != raw_offset ||
        offset + span < offset || offset + span > position ||
        length > destination_size - position) {
      ok = 0;
      break;
    }
    size_t match = position - offset;
    for (uint32_t index = 0; index < length; ++index) {
      destination[position] =
          (uint8_t)(destination[match] +
                    destination[position - span] -
                    destination[match - span]);
      ++position;
      ++match;
    }
  }
  if (ok)
    ok = lzms_x86_filter(destination, destination_size);
  lzms_decoder_free(&decoder);
  return ok;
}

int qlic_lzms_decompress(const uint8_t *source, size_t source_size,
                         uint8_t *destination, size_t destination_size) {
  static const uint8_t signature[6] = {0x0a, 0x51, 0xe5,
                                       0xc0, 0x18, 0x00};
  if (!source || !destination || source_size < 28u ||
      memcmp(source, signature, sizeof(signature)) != 0 ||
      lzms_read64(source + 8) != destination_size)
    return 0;
  uint64_t block_size64 = lzms_read64(source + 16);
  if (!block_size64 || block_size64 > SIZE_MAX)
    return 0;
  size_t block_size = (size_t)block_size64;
  size_t input = 24;
  size_t output = 0;
  while (output < destination_size) {
    if (source_size - input < 4u)
      return 0;
    size_t compressed = lzms_read32(source + input);
    input += 4u;
    size_t remaining = destination_size - output;
    size_t decoded = remaining < block_size ? remaining : block_size;
    if (compressed > decoded || compressed > source_size - input)
      return 0;
    int ok;
    if (compressed == decoded) {
      memcpy(destination + output, source + input, decoded);
      ok = 1;
    } else {
      ok = lzms_raw_decompress(source + input, compressed,
                               destination + output, decoded);
    }
    if (!ok)
      return 0;
    input += compressed;
    output += decoded;
  }
  return input == source_size;
}
