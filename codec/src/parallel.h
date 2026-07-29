#ifndef QLIC_PARALLEL_H
#define QLIC_PARALLEL_H

typedef void (*QlicParallelFn)(void *context, unsigned index);

void qlic_parallel_for(unsigned count, unsigned workers, QlicParallelFn fn,
                       void *context);

#endif
