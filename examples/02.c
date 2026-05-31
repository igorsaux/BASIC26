// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <sched.h>
#endif

#include <basic26.h>

#define CHECK(expr)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(expr))                                                            \
        {                                                                       \
            fprintf(stderr, "FATAL: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define FIB_N 40

#define WARMUP_ITERATIONS 1000
#define BENCH_ITERATIONS 10000
#define RUNS 10

basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script = NULL;

basic26_SymbolId symbol_n;
basic26_SymbolId symbol_a;
basic26_SymbolId symbol_b;
basic26_SymbolId symbol_i;
basic26_SymbolId symbol_temp;

const char *SOURCE =
    "i = 0\n"
    "WHILE i < n\n"
    "  temp = a + b\n"
    "  a = b\n"
    "  b = temp\n"
    "  i = i + 1\n"
    "ENDWHILE\n";

static void set_int_var(basic26_State *st, basic26_SymbolId symbol_id, int64_t val)
{
    CHECK(basic26_State_set_var(st, symbol_id,
                                &(basic26_Value){
                                    .type = BASIC26_VALUE_TYPE_INT,
                                    .as.int_val = val,
                                }) == BASIC26_RESULT_OK);
}

static int64_t get_int_var(basic26_State *st, basic26_SymbolId symbol_id)
{
    basic26_Value out;
    CHECK(basic26_State_get_var(st, symbol_id, &out) == BASIC26_RESULT_OK);
    CHECK(out.type == BASIC26_VALUE_TYPE_INT);

    return out.as.int_val;
}

static void reset_state(basic26_State *st)
{
    basic26_State_clear(state, &(basic26_ClearStateOptions){
                                   .clear_stack = true,
                               });

    set_int_var(st, symbol_n, FIB_N);
    set_int_var(st, symbol_a, 0);
    set_int_var(st, symbol_b, 1);
    set_int_var(st, symbol_i, 0);
    set_int_var(st, symbol_temp, 0);

    basic26_State_set_ip(st, 0);
}

#ifdef _WIN32
static long long now_ns(void)
{
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    double seconds = (double)counter.QuadPart / (double)freq.QuadPart;
    return (long long)(seconds * 1000000000.0);
}
#else
static long long now_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

static int64_t fib_reference(int n)
{
    if (n <= 0)
        return 0;

    int64_t a = 0, b = 1;

    for (int i = 0; i < n; i++)
    {
        int64_t temp = a + b;
        a = b;
        b = temp;
    }

    return b;
}

void cache_symbols()
{
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"n", strlen("n"), true, &symbol_n) == BASIC26_RESULT_OK);
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"a", strlen("a"), true, &symbol_a) == BASIC26_RESULT_OK);
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"b", strlen("b"), true, &symbol_b) == BASIC26_RESULT_OK);
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"i", strlen("i"), true, &symbol_i) == BASIC26_RESULT_OK);
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"temp", strlen("temp"), true, &symbol_temp) == BASIC26_RESULT_OK);
}

void print_dump()
{
    uint8_t *str = NULL;
    size_t str_len = 0;

    CHECK(
        basic26_Script_dump(script, vm, &str, &str_len) == BASIC26_RESULT_OK);

    char buf[1024];

    memcpy(buf, str, str_len);
    buf[str_len] = 0;

    printf("SCRIPT DUMP:\n%s\n", buf);

    basic26_Script_dump_free(vm, str, str_len);
}

void cleanup(void)
{
    if (vm == NULL)
        return;

    basic26_Script_destroy(script, vm);
    script = NULL;

    basic26_State_destroy(state, vm);
    state = NULL;

    basic26_Vm_destroy(vm);
    vm = NULL;
}

int compare_doubles(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;

    if (da < db)
        return -1;

    if (da > db)
        return 1;

    return 0;
}

typedef struct
{
    double mean;
    double median;
    double min;
    double max;
    double std_dev;
    double p95;
    double p99;
} Stats;

Stats compute_stats(double *values, size_t n)
{
    Stats s;

    qsort(values, n, sizeof(double), compare_doubles);

    s.min = values[0];
    s.max = values[n - 1];
    s.median = values[n / 2];
    s.p95 = values[(size_t)(n * 0.95)];
    s.p99 = values[(size_t)(n * 0.99)];

    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        sum += values[i];
    }

    s.mean = sum / n;

    double variance = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        double diff = values[i] - s.mean;
        variance += diff * diff;
    }

    s.std_dev = sqrt(variance / n);

    return s;
}

double run_benchmark(int iterations)
{
    basic26_RuntimeErrorInfo run_err;

    const long long start = now_ns();

    for (int i = 0; i < iterations; i++)
    {
        reset_state(state);

        CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                     .state = state,
                                     .script = script,
                                     .limits = &(basic26_RunLimits){
                                         .max_ops = 0,
                                         .max_time_ns = 0,
                                         .time_check_interval = 0,
                                     },
                                     .userdata = NULL,
                                 }, &run_err) == BASIC26_RESULT_OK);
    }

    const long long end = now_ns();

    return (double)(end - start) / iterations;
}

int main(void)
{
    const int64_t expected = fib_reference(FIB_N);

    CHECK(basic26_Vm_create(&(basic26_CreateVmOptions){.alloc = NULL}, &vm) == BASIC26_RESULT_OK);
    CHECK(basic26_State_create(&(basic26_CreateStateOptions){.vm = vm}, &state) == BASIC26_RESULT_OK);
    CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);

    basic26_CompileErrorInfo compile_err;
    CHECK(basic26_Script_compile(script, &(basic26_CompileOptions){
                                             .vm = vm,
                                             .source = (const uint8_t *)SOURCE,
                                             .source_len = strlen(SOURCE),
                                             .limits = &(basic26_ScriptLimits){0},
                                         },
                                 &compile_err) == BASIC26_RESULT_OK);

    print_dump();
    cache_symbols();

    set_int_var(state, symbol_n, FIB_N);
    set_int_var(state, symbol_a, 0);
    set_int_var(state, symbol_b, 1);
    set_int_var(state, symbol_i, 0);
    set_int_var(state, symbol_temp, 0);

    basic26_RuntimeErrorInfo run_err;
    CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                 .state = state,
                                 .script = script,
                                 .limits = &(basic26_RunLimits){0},
                                 .userdata = NULL,
                             }, &run_err) == BASIC26_RESULT_OK);

    const int64_t result = get_int_var(state, symbol_b);

    printf("fib(%d) = %lld (expected %lld) %s\n",
           FIB_N, (long long)result, (long long)expected,
           result == expected ? "OK" : "MISMATCH");

    if (result != expected)
    {
        cleanup();

        return 1;
    }

    printf("\n--- Benchmark ---\n");
    printf("fib(%d) x %d iterations per run, %d runs\n", FIB_N, BENCH_ITERATIONS, RUNS);
    printf("Warmup: %d iterations\n\n", WARMUP_ITERATIONS);

    printf("Warming up...\n");
    for (int i = 0; i < WARMUP_ITERATIONS; i++)
    {
        reset_state(state);
        CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                     .state = state,
                                     .script = script,
                                     .limits = &(basic26_RunLimits){0},
                                     .userdata = NULL,
                                 }, &run_err) == BASIC26_RESULT_OK);
    }

    printf("Warmup complete.\n\n");

    double *run_results = malloc(RUNS * sizeof(double));

    for (int run = 0; run < RUNS; run++)
    {
        run_results[run] = run_benchmark(BENCH_ITERATIONS);
        printf("Run %2d: %.0f ns/iter\n", run + 1, run_results[run]);
    }

    Stats overall = compute_stats(run_results, RUNS);

    printf("\n--- Results (ns/iter) ---\n");
    printf("Mean:     %.0f ns\n", overall.mean);
    printf("Median:   %.0f ns\n", overall.median);
    printf("Min:      %.0f ns\n", overall.min);
    printf("Max:      %.0f ns\n", overall.max);
    printf("Std Dev:  %.1f ns (%.1f%%)\n", overall.std_dev, (overall.std_dev / overall.mean) * 100);
    printf("P95:      %.0f ns\n", overall.p95);
    printf("P99:      %.0f ns\n", overall.p99);

    double *per_iter_times = malloc(BENCH_ITERATIONS * sizeof(double));
    printf("\n--- Detailed per-iteration analysis (last run) ---\n");

    basic26_RuntimeErrorInfo last_run_err;
    for (int i = 0; i < BENCH_ITERATIONS; i++)
    {
        reset_state(state);

        const long long iter_start = now_ns();
        CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                     .state = state,
                                     .script = script,
                                     .limits = &(basic26_RunLimits){0},
                                     .userdata = NULL,
                                 }, &last_run_err) == BASIC26_RESULT_OK);
        const long long iter_end = now_ns();

        per_iter_times[i] = (double)(iter_end - iter_start);
    }

    Stats per_iter = compute_stats(per_iter_times, BENCH_ITERATIONS);
    printf("Mean:     %.0f ns\n", per_iter.mean);
    printf("Median:   %.0f ns\n", per_iter.median);
    printf("Min:      %.0f ns\n", per_iter.min);
    printf("Max:      %.0f ns\n", per_iter.max);
    printf("Std Dev:  %.1f ns (%.1f%%)\n", per_iter.std_dev, (per_iter.std_dev / per_iter.mean) * 100);
    printf("P95:      %.0f ns\n", per_iter.p95);
    printf("P99:      %.0f ns\n", per_iter.p99);

    const int64_t bench_result = get_int_var(state, symbol_b);
    printf("\nPost-benchmark: fib(%d) = %lld (expected %lld) %s\n",
           FIB_N, (long long)bench_result, (long long)expected,
           bench_result == expected ? "OK" : "MISMATCH");

    if (bench_result != expected)
    {
        free(run_results);
        free(per_iter_times);
        cleanup();

        return 1;
    }

    free(run_results);
    free(per_iter_times);
    cleanup();

    return 0;
}
