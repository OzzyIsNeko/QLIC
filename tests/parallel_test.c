#include "parallel.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <stdatomic.h>
#endif

enum {
  ITEM_COUNT = 257,
  OUTER_COUNT = 8,
  INNER_COUNT = 129
};

#ifdef _WIN32
typedef volatile LONG TestCounter;
#define TEST_INCREMENT(pointer) InterlockedIncrement(pointer)
#define TEST_VALUE(value) (value)
#else
typedef atomic_long TestCounter;
#define TEST_INCREMENT(pointer) atomic_fetch_add_explicit((pointer), 1, memory_order_relaxed)
#define TEST_VALUE(value) atomic_load_explicit(&(value), memory_order_relaxed)
#endif

typedef struct {
  TestCounter hits[ITEM_COUNT];
} FlatRun;

typedef struct {
  TestCounter hits[OUTER_COUNT][INNER_COUNT];
} NestedRun;

typedef struct {
  TestCounter *hits;
} InnerRun;

static void flat_item(void *context, unsigned index) {
  FlatRun *run = (FlatRun *)context;
  TEST_INCREMENT(&run->hits[index]);
}

static void inner_item(void *context, unsigned index) {
  InnerRun *run = (InnerRun *)context;
  TEST_INCREMENT(&run->hits[index]);
}

static void outer_item(void *context, unsigned index) {
  NestedRun *run = (NestedRun *)context;
  InnerRun inner;
  inner.hits = run->hits[index];
  qlic_parallel_for(INNER_COUNT, 128u, inner_item, &inner);
}

int main(void) {
  FlatRun flat = {0};
  NestedRun nested = {0};
  qlic_parallel_for(ITEM_COUNT, 128u, flat_item, &flat);
  for (unsigned i = 0; i < ITEM_COUNT; ++i)
    if (TEST_VALUE(flat.hits[i]) != 1)
      return 1;
  qlic_parallel_for(OUTER_COUNT, 8u, outer_item, &nested);
  for (unsigned y = 0; y < OUTER_COUNT; ++y)
    for (unsigned x = 0; x < INNER_COUNT; ++x)
      if (TEST_VALUE(nested.hits[y][x]) != 1)
        return 2;
  return 0;
}
