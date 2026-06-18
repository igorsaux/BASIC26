// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

// ==========================================================================
// BASIC26 Example 02 - Fibonacci Benchmark (pure bytecode)
// ==========================================================================
//
// Computes Fibonacci numbers using a WHILE loop entirely in BASIC26
// bytecode. Demonstrates:
//   - setting variables from C before execution and reading them back after,
//   - resetting state for repeated runs (basic26_State_clear + set_ip),
//   - measuring performance with a warmup phase, multiple runs, and
//     per-iteration statistics.
//
// The script itself is the inner loop:
//
//   i = 0
//   WHILE i < n
//     temp = a + b
//     a = b
//     b = temp
//     i = i + 1
//   ENDWHILE
//
// The result is read back from the variable `b`. With n = 40 and
// (a, b) initialised to (0, 1), the result is fib(41) = 165580141.
//
// This example links against the static library.
//
// ==========================================================================

#include "common.h"

#define FIB_N              40
#define WARMUP_ITERATIONS  50000
#define BENCH_ITERATIONS   100000
#define RUNS               10

basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script = NULL;

basic26_SymbolId symbol_n;
basic26_SymbolId symbol_a;
basic26_SymbolId symbol_b;
basic26_SymbolId symbol_i;
basic26_SymbolId symbol_temp;

static const char *SOURCE =
    "i = 0\n"
    "WHILE i < n\n"
    "  temp = a + b\n"
    "  a = b\n"
    "  b = temp\n"
    "  i = i + 1\n"
    "ENDWHILE\n";

static void reset_state(basic26_State *st)
{
    basic26_State_clear(state, &(basic26_ClearStateOptions){
                                   .clear_stack = true,
                               });

    basic26_set_int_var(st, symbol_n,    FIB_N);
    basic26_set_int_var(st, symbol_a,    0);
    basic26_set_int_var(st, symbol_b,    1);
    basic26_set_int_var(st, symbol_i,    0);
    basic26_set_int_var(st, symbol_temp, 0);

    basic26_State_set_ip(st, 0);
}

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

static void cache_symbols(void)
{
    symbol_n    = basic26_intern(vm, "n");
    symbol_a    = basic26_intern(vm, "a");
    symbol_b    = basic26_intern(vm, "b");
    symbol_i    = basic26_intern(vm, "i");
    symbol_temp = basic26_intern(vm, "temp");
}

static void cleanup(void)
{
    if (vm == NULL)
        return;

    basic26_Script_destroy(script);
    script = NULL;

    basic26_State_destroy(state);
    state = NULL;

    basic26_Vm_destroy(vm);
    vm = NULL;
}

static double run_benchmark(int iterations)
{
    const long long start = basic26_now_ns();

    for (int i = 0; i < iterations; i++)
    {
        reset_state(state);
        basic26_run_sync(vm, state, script);
    }

    const long long end = basic26_now_ns();

    return (double)(end - start) / (double)iterations;
}

int main(void)
{
    const int64_t expected = fib_reference(FIB_N);

    CHECK(basic26_Vm_create(NULL, &vm) == BASIC26_RESULT_OK);
    CHECK(basic26_State_create(vm, &state) == BASIC26_RESULT_OK);
    CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);

    basic26_compile(vm, script, SOURCE);
    basic26_print_dump(vm, script);
    cache_symbols();

    basic26_set_int_var(state, symbol_n,    FIB_N);
    basic26_set_int_var(state, symbol_a,    0);
    basic26_set_int_var(state, symbol_b,    1);
    basic26_set_int_var(state, symbol_i,    0);
    basic26_set_int_var(state, symbol_temp, 0);

    basic26_run_sync(vm, state, script);

    const int64_t result = basic26_get_int_var(state, symbol_b);

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
        basic26_run_sync(vm, state, script);
    }

    printf("Warmup complete.\n\n");

    double *run_results = malloc(RUNS * sizeof(double));
    CHECK(run_results != NULL);

    for (int run = 0; run < RUNS; run++)
    {
        run_results[run] = run_benchmark(BENCH_ITERATIONS);
        printf("Run %2d: %.0f ns/iter\n", run + 1, run_results[run]);
    }

    basic26_Stats overall = basic26_compute_stats(run_results, RUNS);
    basic26_print_stats("Results", &overall);

    double *per_iter_times = malloc(BENCH_ITERATIONS * sizeof(double));
    CHECK(per_iter_times != NULL);

    printf("\n--- Detailed per-iteration analysis (last run) ---\n");

    for (int i = 0; i < BENCH_ITERATIONS; i++)
    {
        reset_state(state);

        const long long iter_start = basic26_now_ns();
        basic26_run_sync(vm, state, script);
        const long long iter_end = basic26_now_ns();

        per_iter_times[i] = (double)(iter_end - iter_start);
    }

    basic26_Stats per_iter = basic26_compute_stats(per_iter_times, BENCH_ITERATIONS);
    basic26_print_stats("Per-iter", &per_iter);

    const int64_t bench_result = basic26_get_int_var(state, symbol_b);
    printf("\nPost-benchmark: fib(%d) = %lld (expected %lld) %s\n",
           FIB_N, (long long)bench_result, (long long)expected,
           bench_result == expected ? "OK" : "MISMATCH");

    free(run_results);
    free(per_iter_times);

    if (bench_result != expected)
    {
        cleanup();

        return 1;
    }

    cleanup();

    return 0;
}
