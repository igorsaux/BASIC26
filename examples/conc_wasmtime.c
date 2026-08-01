// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

/*
 * conc_wasmtime.c - Multithreaded benchmark for wasmtime (JIT).
 *
 * Mirrors conc.c: NUM_WORKERS threads pull tasks from a shared
 * mutex-protected queue. Each task is a range [lo, hi]; every number
 * is tested for primality by calling into a compiled WASM module
 * (is_prime.zig -> is_prime.wasm).
 *
 * Each thread owns a wasmtime_store_t (and thus its own JIT code cache
 * instance). The wasmtime_module_t (compiled code) is shared read-only.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wasm.h>
#include <wasmtime.h>

extern unsigned char is_prime_wasm[];
extern unsigned int is_prime_wasm_len;

#define N_TASKS 50
#define RANGE_MAX 20000000L
#define NUM_WORKERS 8

typedef struct {
  long lo, hi;
  int task_id;
} Task;

typedef struct {
  Task tasks[N_TASKS];
  int count;
  int next;
  pthread_mutex_t mutex;
} TaskQueue;

typedef struct {
  int id;
  wasmtime_store_t *store;
  wasmtime_instance_t instance;
  wasmtime_func_t is_prime;
  TaskQueue *queue;
  atomic_int *completed;
  atomic_long *total_primes;
} worker_t;

static void queue_init(TaskQueue *q) {
  q->count = 0;
  q->next = 0;

  pthread_mutex_init(&q->mutex, NULL);
}

static void queue_push(TaskQueue *q, long lo, long hi, int task_id) {
  assert(q->count < N_TASKS);

  q->tasks[q->count] = (Task){.lo = lo, .hi = hi, .task_id = task_id};
  q->count++;
}

static bool queue_pop(TaskQueue *q, Task *out) {
  pthread_mutex_lock(&q->mutex);

  if (q->next >= q->count) {
    pthread_mutex_unlock(&q->mutex);

    return false;
  }

  *out = q->tasks[q->next++];
  pthread_mutex_unlock(&q->mutex);

  return true;
}

static void *worker_thread(void *arg) {
  worker_t *w = (worker_t *)arg;
  wasmtime_context_t *ctx = wasmtime_store_context(w->store);
  Task task;

  while (queue_pop(w->queue, &task)) {
    long primes = 0;

    for (long n = task.lo; n <= task.hi; n++) {
      wasmtime_val_t args[1];
      args[0].kind = WASMTIME_I64;
      args[0].of.i64 = (int64_t)n;

      wasmtime_val_t results[1];
      memset(results, 0, sizeof(results));

      wasm_trap_t *trap = NULL;
      wasmtime_error_t *err =
          wasmtime_func_call(ctx, &w->is_prime, args, 1, results, 1, &trap);

      if (err != NULL) {
        wasm_name_t msg = {};
        wasmtime_error_message(err, &msg);
        fprintf(stderr, "[thread %2d] error n=%ld: %s\n", w->id, n,
                msg.size ? msg.data : "(unknown)");
        wasmtime_error_delete(err);

        continue;
      }

      if (trap != NULL) {
        wasm_message_t msg;
        wasm_trap_message(trap, &msg);
        fprintf(stderr, "[thread %2d] trap n=%ld: %.*s\n", w->id, n,
                (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasm_trap_delete(trap);

        continue;
      }

      if (results[0].of.i64 == 1) {
        primes++;
      }
    }

    atomic_fetch_add(w->total_primes, primes);
    int done = atomic_fetch_add(w->completed, 1) + 1;

    // printf("[thread %2d] task %2d done primes[%ld..%ld] = %ld  "
    //        "(%d/%d)\n",
    //        w->id, task.task_id, task.lo, task.hi, primes, done, N_TASKS);
  }

  return NULL;
}

int main(void) {
  const uint8_t *wasm_bytes = is_prime_wasm;
  size_t wasm_len = is_prime_wasm_len;

  wasm_config_t *config = wasm_config_new();
  wasmtime_config_consume_fuel_set(config, false);
  // wasmtime_config_target_set(config, "pulley64");
  wasm_engine_t *engine = wasm_engine_new_with_config(config);
  assert(engine != NULL);

  wasmtime_module_t *module = NULL;
  wasmtime_error_t *err =
      wasmtime_module_new(engine, wasm_bytes, wasm_len, &module);

  if (err != NULL) {
    wasm_name_t msg = {};
    wasmtime_error_message(err, &msg);
    fprintf(stderr, "compile error: %s\n", msg.size ? msg.data : "(unknown)");

    return 1;
  }

  TaskQueue queue;
  queue_init(&queue);

  long chunk = (RANGE_MAX - 2) / N_TASKS;
  for (int i = 0; i < N_TASKS; i++) {
    long lo = 2 + i * chunk;
    long hi = (i == N_TASKS - 1) ? RANGE_MAX : 2 + (i + 1) * chunk - 1;

    queue_push(&queue, lo, hi, i);
  }

  atomic_int completed = 0;
  atomic_long total_primes = 0;
  worker_t workers[NUM_WORKERS];

  for (int i = 0; i < NUM_WORKERS; i++) {
    worker_t *w = &workers[i];
    w->id = i;
    w->queue = &queue;
    w->completed = &completed;
    w->total_primes = &total_primes;

    w->store = wasmtime_store_new(engine, NULL, NULL);
    assert(w->store != NULL);

    wasmtime_context_t *ctx = wasmtime_store_context(w->store);

    /* No imports - the module is freestanding. */
    wasm_trap_t *trap = NULL;
    err = wasmtime_instance_new(ctx, module, NULL, 0, &w->instance, &trap);
    if (err != NULL || trap != NULL) {
      wasm_name_t msg = {};
      wasmtime_error_message(err, &msg);
      fprintf(stderr, "[thread %2d] instance creation failed: %s\n", i,
              msg.size ? msg.data : "(unknown)");
      wasmtime_error_delete(err);

      return 1;
    }

    /* Resolve the exported function. */
    wasmtime_extern_t ext;
    bool found =
        wasmtime_instance_export_get(ctx, &w->instance, "is_prime", 8, &ext);
    assert(found && ext.kind == WASMTIME_EXTERN_FUNC);

    w->is_prime = ext.of.func;
  }

  pthread_t threads[NUM_WORKERS];
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (int i = 0; i < NUM_WORKERS; i++) {
    int rc = pthread_create(&threads[i], NULL, worker_thread, &workers[i]);
    assert(rc == 0);
  }

  for (int i = 0; i < NUM_WORKERS; i++) {
    pthread_join(threads[i], NULL);
  }

  printf("\n--- Results ---\n");
  printf("pi(%ld) = %ld\n", RANGE_MAX, (long)atomic_load(&total_primes));
  printf("workers = %d, tasks = %d\n", NUM_WORKERS, N_TASKS);

  /* ---- Cleanup ---------------------------------------------------- */
  pthread_mutex_destroy(&queue.mutex);
  for (int i = 0; i < NUM_WORKERS; i++) {
    wasmtime_store_delete(workers[i].store);
  }

  wasmtime_module_delete(module);
  wasm_engine_delete(engine);

  return 0;
}