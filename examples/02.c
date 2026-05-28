// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
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

#define ITERATIONS 100000

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
    clock_gettime(CLOCK_MONOTONIC, &ts);

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
                                 .error_out = &run_err,
                             }) == BASIC26_RESULT_OK);

    const int64_t result = get_int_var(state, symbol_b);

    printf("fib(%d) = %lld (expected %lld) %s\n",
           FIB_N, (long long)result, (long long)expected,
           result == expected ? "OK" : "MISMATCH");

    if (result != expected)
    {
        cleanup();
        return 1;
    }

    const long long bench_start = now_ns();

    for (int i = 0; i < ITERATIONS; i++)
    {
        reset_state(state);

        CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                     .state = state,
                                     .script = script,
                                     .limits = &(basic26_RunLimits){
                                         .max_ops = 0,
                                         .max_time_ns = 0,
                                     },
                                     .userdata = NULL,
                                     .error_out = &run_err,
                                 }) == BASIC26_RESULT_OK);
    }

    const long long bench_end = now_ns();
    const int64_t bench_result = get_int_var(state, symbol_b);

    printf("Post-benchmark: fib(%d) = %lld (expected %lld) %s\n",
           FIB_N, (long long)bench_result, (long long)expected,
           bench_result == expected ? "OK" : "MISMATCH");

    if (bench_result != expected)
    {
        cleanup();
        return 1;
    }

    const long long total_ns = bench_end - bench_start;
    const double total_ms = total_ns / 1e6;
    const double per_iter_ns = (double)total_ns / ITERATIONS;

    printf("\n--- Benchmark ---\n");
    printf("fib(%d) x %d iterations\n", FIB_N, ITERATIONS);
    printf("Total:   %.2f ms\n", total_ms);
    printf("Average: %.0f ns/iter\n", per_iter_ns);

    cleanup();
    return 0;
}
