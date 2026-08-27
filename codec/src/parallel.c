#include "parallel.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct {
  volatile LONG next;
  unsigned count;
  QlicParallelFn fn;
  void *context;
} QlicParallelRun;

static void qlic_parallel_drain(QlicParallelRun *run) {
  /* tasks are uniform and indexed, one atomic counter avoids a queue */
  for (;;) {
    unsigned index = (unsigned)(InterlockedIncrement(&run->next) - 1);
    if (index >= run->count)
      return;
    run->fn(run->context, index);
  }
}

static void CALLBACK qlic_parallel_callback(PTP_CALLBACK_INSTANCE instance,
                                            void *context, PTP_WORK work) {
  /* Prevent pool-starvation handling for long codec tasks. */
  (void)CallbackMayRunLong(instance);
  (void)work;
  qlic_parallel_drain((QlicParallelRun *)context);
}
#elif !defined(QLIC_WASM)
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
  atomic_uint next;
  unsigned count;
  QlicParallelFn fn;
  void *context;
} QlicParallelRun;

static void qlic_parallel_drain(QlicParallelRun *run) {
  for (;;) {
    unsigned index =
        atomic_fetch_add_explicit(&run->next, 1u, memory_order_relaxed);
    if (index >= run->count)
      return;
    run->fn(run->context, index);
  }
}

static void *qlic_parallel_callback(void *context) {
  qlic_parallel_drain((QlicParallelRun *)context);
  return NULL;
}
#endif

void qlic_parallel_for(unsigned count, unsigned workers, QlicParallelFn fn,
                       void *context) {
  if (!count || !fn)
    return;
  if (workers > count)
    workers = count;
#ifdef _WIN32
  if (workers > 1u) {
    QlicParallelRun run;
    run.next = 0;
    run.count = count;
    run.fn = fn;
    run.context = context;
    PTP_WORK work =
        CreateThreadpoolWork(qlic_parallel_callback, &run, NULL);
    if (work) {
      for (unsigned i = 1; i < workers; ++i)
        SubmitThreadpoolWork(work);
      /* the caller counts as one worker so the requested worker count stays exact */
      qlic_parallel_drain(&run);
      WaitForThreadpoolWorkCallbacks(work, TRUE);
      CloseThreadpoolWork(work);
      return;
    }
  }
#elif !defined(QLIC_WASM)
  if (workers > 1u) {
    QlicParallelRun run;
    atomic_init(&run.next, 0u);
    run.count = count;
    run.fn = fn;
    run.context = context;
    size_t thread_count = (size_t)workers - 1u;
    pthread_t *threads =
        (pthread_t *)malloc(thread_count * sizeof(*threads));
    if (threads) {
      size_t created = 0;
      for (; created < thread_count; ++created) {
        if (pthread_create(&threads[created], NULL, qlic_parallel_callback,
                           &run) != 0)
          break;
      }
      qlic_parallel_drain(&run);
      for (size_t i = 0; i < created; ++i)
        pthread_join(threads[i], NULL);
      free(threads);
      return;
    }
  }
#else
  (void)workers;
#endif
  for (unsigned i = 0; i < count; ++i)
    fn(context, i);
}
