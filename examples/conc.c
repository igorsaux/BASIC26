// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

/*
 * conc.c - Multithreaded benchmark for the BASIC26 preemptible VM.
 *
 * NUM_WORKERS threads pull tasks from a shared mutex-protected queue.
 * Each task is a numeric range [lo, hi]. For every number in the range
 * the owning thread runs a compiled BASIC26 script that tests primality
 * via trial division.
 *
 * The script is executed in quanta of MAX_OPS_PER_RUN opcodes. When the
 * limit is hit, basic26_Vm_run() returns BASIC26_RESULT_OUT_OF_LIMITS;
 * the host then re-enters the VM, which resumes from the saved IP and
 * stack. This models a lightweight VM that can be preempted at any
 * instruction boundary without losing progress.
 *
 * Each completed task increments an atomic counter. The program exits
 * once all N_TASKS tasks have finished.
 */

#include <assert.h>
#include <basic26.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_TASKS 50
#define RANGE_MAX 20000000L
#define NUM_WORKERS 8
#define MAX_OPS_PER_RUN 0

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

enum {
  VAR_N = 0,
  VAR_I,
  VAR_FINISHED,
  VAR_R2,
  VAR_R3,
  VAR_RI,
  VAR_RI2,
  VAR_SQ,
  VAR_I2,
  VAR_COUNT
};

static const char *VAR_NAMES[VAR_COUNT] = {"n",  "i",   "FINISHED", "r2", "r3",
                                           "ri", "ri2", "sq",       "i2"};

typedef struct {
  basic26_SymbolId ids[VAR_COUNT];
} VarIds;

typedef struct {
  int id;
  basic26_Vm *vm;
  basic26_State *state;
  basic26_Script *script;
  VarIds vars;
  TaskQueue *queue;
  atomic_int *completed;
  atomic_long *total_primes;
} worker_t;

static const char *SOURCE = "is_prime:\n"
                            "  IF n < 2\n"
                            "    FINISHED 0\n"
                            "    GOTO @end\n"
                            "  ENDIF\n"
                            "  IF n < 4\n"
                            "    FINISHED 1\n"
                            "    GOTO @end\n"
                            "  ENDIF\n"
                            "  r2 = n % 2\n"
                            "  IF r2 == 0\n"
                            "    FINISHED 0\n"
                            "    GOTO @end\n"
                            "  ENDIF\n"
                            "  r3 = n % 3\n"
                            "  IF r3 == 0\n"
                            "    FINISHED 0\n"
                            "    GOTO @end\n"
                            "  ENDIF\n"
                            "  i = 5\n"
                            "  sq = i * i\n"
                            "  WHILE sq <= n\n"
                            "    ri = n % i\n"
                            "    IF ri == 0\n"
                            "      FINISHED 0\n"
                            "      GOTO @end\n"
                            "    ENDIF\n"
                            "    i2 = i + 2\n"
                            "    ri2 = n % i2\n"
                            "    IF ri2 == 0\n"
                            "      FINISHED 0\n"
                            "      GOTO @end\n"
                            "    ENDIF\n"
                            "    i = i + 6\n"
                            "    sq = i * i\n"
                            "  ENDWHILE\n"
                            "  FINISHED 1\n"
                            "end:\n";

static basic26_FunctionResult finished_callback(const basic26_CallInfo *info,
                                                size_t argc,
                                                const basic26_Value *argv) {
  if (argc != 1 || argv[0].type != BASIC26_VALUE_TYPE_INT)
    return BASIC26_FUNCTION_RESULT_ERROR;

  basic26_SymbolId var = *(const basic26_SymbolId *)info->userdata;
  basic26_State_set_var(info->state, var, &argv[0]);
  return BASIC26_FUNCTION_RESULT_OK;
}

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

static VarIds create_vars(basic26_Vm *vm, basic26_State *state) {
  VarIds vars;
  basic26_Value zero = {.type = BASIC26_VALUE_TYPE_INT, .as.int_val = 0};

  for (size_t i = 0; i < VAR_COUNT; i++) {
    basic26_Result ret =
        basic26_Vm_get_string_id(vm, (const uint8_t *)VAR_NAMES[i],
                                 strlen(VAR_NAMES[i]), true, &vars.ids[i]);
    assert(ret == BASIC26_RESULT_OK);

    ret = basic26_State_set_var(state, vars.ids[i], &zero);
    assert(ret == BASIC26_RESULT_OK);
  }
  return vars;
}

static void *worker_thread(void *arg) {
  worker_t *w = (worker_t *)arg;
  Task task;

  basic26_RunLimits limits = basic26_RunLimits_zeroed();
  limits.max_ops = MAX_OPS_PER_RUN;

  basic26_RunOptions run_opts = basic26_RunOptions_zeroed();
  run_opts.state = w->state;
  run_opts.script = w->script;
  run_opts.limits = &limits;
  run_opts.userdata = &w->vars.ids[VAR_FINISHED];

  basic26_RuntimeErrorInfo err_info;

  size_t start_ip;
  basic26_Result ret = basic26_Script_get_label(
      w->script, (const uint8_t *)"is_prime", strlen("is_prime"), &start_ip);
  assert(ret == BASIC26_RESULT_OK);

  basic26_ClearStateOptions clear = basic26_ClearStateOptions_zeroed();
  clear.clear_stack = true;

  while (queue_pop(w->queue, &task)) {
    long primes = 0;

    for (long n = task.lo; n <= task.hi; n++) {
      basic26_Value val = {.type = BASIC26_VALUE_TYPE_INT, .as.int_val = n};
      basic26_State_set_var(w->state, w->vars.ids[VAR_N], &val);
      basic26_State_set_ip(w->state, start_ip);

      do {
        ret = basic26_Vm_run(w->vm, &run_opts, &err_info);
      } while (ret == BASIC26_RESULT_OUT_OF_LIMITS);

      if (ret == BASIC26_RESULT_OK || ret == BASIC26_RESULT_YIELDED) {
        basic26_Value res;
        basic26_State_get_var(w->state, w->vars.ids[VAR_FINISHED], &res);
        if (res.type == BASIC26_VALUE_TYPE_INT && res.as.int_val == 1)
          primes++;
      } else {
        // fprintf(stderr, "[thread %2d] runtime error: ip=%zu code=%d n=%ld\n",
        //         w->id, err_info.ip, err_info.code, n);
      }

      basic26_State_clear(w->state, &clear);
    }

    atomic_fetch_add(w->total_primes, primes);
    int done = atomic_fetch_add(w->completed, 1) + 1;

    // printf("[thread %2d] task %2d done  primes[%ld..%ld] = %ld  "
    //        "(%d/%d)\n",
    //        w->id, task.task_id, task.lo, task.hi, primes, done, N_TASKS);
  }

  return NULL;
}

int main(void) {
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

    basic26_Result ret;

    ret = basic26_Vm_create(NULL, &w->vm);
    assert(ret == BASIC26_RESULT_OK);

    ret = basic26_State_create(w->vm, &w->state);
    assert(ret == BASIC26_RESULT_OK);

    ret = basic26_Script_create(w->vm, &w->script);
    assert(ret == BASIC26_RESULT_OK);

    w->vars = create_vars(w->vm, w->state);

    basic26_RegisterFunctionOptions reg =
        basic26_RegisterFunctionOptions_zeroed();
    reg.name = w->vars.ids[VAR_FINISHED];
    reg.callback = finished_callback;
    ret = basic26_Vm_register_function(w->vm, &reg);
    assert(ret == BASIC26_RESULT_OK);

    basic26_ScriptLimits slimits = basic26_ScriptLimits_zeroed();
    basic26_CompileOptions copts = basic26_CompileOptions_zeroed();
    copts.source = (const uint8_t *)SOURCE;
    copts.source_len = strlen(SOURCE);
    copts.limits = &slimits;

    basic26_CompileErrorInfo cerr;
    ret = basic26_Script_compile(w->script, &copts, &cerr);
    assert(ret == BASIC26_RESULT_OK);

    // if (i == 0) {
    //   uint8_t *dump = NULL;
    //   size_t dump_len = 0;
    //   ret = basic26_Script_dump(w->script, &dump, &dump_len);
    //   assert(ret == BASIC26_RESULT_OK);
    //   printf("SCRIPT DUMP:\n%.*s\n", (int)dump_len, (const char *)dump);
    //   basic26_Vm_free(w->vm, dump, dump_len, 1);
    // }
  }

  pthread_t threads[NUM_WORKERS];
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (int i = 0; i < NUM_WORKERS; i++) {
    int err = pthread_create(&threads[i], NULL, worker_thread, &workers[i]);
    assert(err == 0);
  }

  for (int i = 0; i < NUM_WORKERS; i++)
    pthread_join(threads[i], NULL);

  printf("\n--- Results ---\n");
  printf("pi(%ld) = %ld\n", RANGE_MAX, (long)atomic_load(&total_primes));
  printf("workers = %d, tasks = %d, quantum = %d ops\n", NUM_WORKERS, N_TASKS,
         MAX_OPS_PER_RUN);

  pthread_mutex_destroy(&queue.mutex);
  for (int i = 0; i < NUM_WORKERS; i++) {
    basic26_Script_destroy(workers[i].script);
    basic26_State_destroy(workers[i].state);
    basic26_Vm_destroy(workers[i].vm);
  }

  return 0;
}
