// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

#ifndef BASIC26_EXAMPLES_COMMON_H
#define BASIC26_EXAMPLES_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include <basic26.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // --------------------------------------------------------------------------
    // Macros
    // --------------------------------------------------------------------------

#define CHECK(expr)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(expr))                                                            \
        {                                                                       \
            fprintf(stderr, "FATAL: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define UNUSED(x) (void)(x)

    // --------------------------------------------------------------------------
    // Time / sleep
    // --------------------------------------------------------------------------

    static inline long long basic26_now_ns(void)
    {
#ifdef _WIN32
        static LARGE_INTEGER freq = {0};
        if (freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&freq);
        }
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        double seconds = (double)counter.QuadPart / (double)freq.QuadPart;
        return (long long)(seconds * 1000000000.0);
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
    }

    // Cross-platform sleep in microseconds.
    static inline void basic26_sleep_us(long long us)
    {
#ifdef _WIN32
        Sleep((DWORD)(us / 1000));
#else
    usleep((useconds_t)us);
#endif
    }

    // --------------------------------------------------------------------------
    // BASIC26 helpers
    // --------------------------------------------------------------------------

    static inline const char *basic26_runtime_error_to_string(basic26_RuntimeError code)
    {
        switch (code)
        {
        case BASIC26_RUNTIME_ERROR_UNKNOWN:
            return "UNKNOWN";
        case BASIC26_RUNTIME_ERROR_DIVISION_BY_ZERO:
            return "DIVISION_BY_ZERO";
        case BASIC26_RUNTIME_ERROR_TYPE_MISMATCH:
            return "TYPE_MISMATCH";
        case BASIC26_RUNTIME_ERROR_STACK_UNDERFLOW:
            return "STACK_UNDERFLOW";
        case BASIC26_RUNTIME_ERROR_STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case BASIC26_RUNTIME_ERROR_UNKNOWN_OPCODE:
            return "UNKNOWN_OPCODE";
        case BASIC26_RUNTIME_ERROR_UNDEFINED_FUNCTION:
            return "UNDEFINED_FUNCTION";
        case BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE:
            return "UNDEFINED_VARIABLE";
        case BASIC26_RUNTIME_ERROR_INVALID_BIT_SHIFT:
            return "INVALID_BIT_SHIFT";
        case BASIC26_RUNTIME_ERROR_FUNCTION:
            return "FUNCTION_ERROR";
        default:
            return "(invalid)";
        }
    }

    // Print a compiled script's bytecode dump to stdout. The dump string is
    // allocated by the VM and freed via basic26_Vm_free() before returning.
    static inline void basic26_print_dump(basic26_Vm *vm, basic26_Script *script)
    {
        uint8_t *str = NULL;
        size_t str_len = 0;

        CHECK(basic26_Script_dump(script, &str, &str_len) == BASIC26_RESULT_OK);

        char *buf = (char *)malloc(str_len + 1);
        CHECK(buf != NULL);

        memcpy(buf, str, str_len);
        buf[str_len] = 0;

        printf("SCRIPT DUMP:\n%s\n", buf);

        free(buf);
        basic26_Vm_free(vm, str, str_len, 1);
    }

    static inline void basic26_set_int_var(basic26_State *st, basic26_SymbolId sym, int64_t val)
    {
        CHECK(basic26_State_set_var(st, sym, &(basic26_Value){
                                                 .type = BASIC26_VALUE_TYPE_INT,
                                                 .as.int_val = val,
                                             }) == BASIC26_RESULT_OK);
    }

    static inline int64_t basic26_get_int_var(basic26_State *st, basic26_SymbolId sym)
    {
        basic26_Value out;
        CHECK(basic26_State_get_var(st, sym, &out) == BASIC26_RESULT_OK);
        CHECK(out.type == BASIC26_VALUE_TYPE_INT);

        return out.as.int_val;
    }

    static inline basic26_SymbolId basic26_intern(basic26_Vm *vm, const char *name)
    {
        basic26_SymbolId id;
        CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)name, strlen(name), true, &id) == BASIC26_RESULT_OK);

        return id;
    }

    static inline void basic26_register(basic26_Vm *vm, const char *name, basic26_FunctionCallback cb)
    {
        basic26_SymbolId id = basic26_intern(vm, name);
        CHECK(basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                                   .name = id,
                                                   .callback = cb,
                                               }) == BASIC26_RESULT_OK);
    }

    static inline void basic26_compile(basic26_Vm *vm, basic26_Script *script, const char *source)
    {
        UNUSED(vm);

        basic26_CompileErrorInfo err;
        CHECK(basic26_Script_compile(script, &(basic26_CompileOptions){
                                                 .source = (const uint8_t *)source,
                                                 .source_len = strlen(source),
                                                 .limits = &(basic26_ScriptLimits){0},
                                             },
                                     &err) == BASIC26_RESULT_OK);
    }

    static inline void basic26_run_sync(basic26_Vm *vm, basic26_State *state, basic26_Script *script)
    {
        basic26_RuntimeErrorInfo err;
        CHECK(basic26_Vm_run(vm, &(basic26_RunOptions){
                                     .state = state,
                                     .script = script,
                                     .limits = &(basic26_RunLimits){0},
                                     .userdata = NULL,
                                 },
                             &err) == BASIC26_RESULT_OK);
    }

    // --------------------------------------------------------------------------
    // Statistics
    // --------------------------------------------------------------------------

    // Aggregated stats for a sample of per-iteration timings (in nanoseconds).
    typedef struct
    {
        double mean;
        double median;
        double min;
        double max;
        double std_dev;
        double p95;
        double p99;
    } basic26_Stats;

    static inline int basic26_compare_doubles(const void *a, const void *b)
    {
        double da = *(const double *)a;
        double db = *(const double *)b;
        if (da < db)
            return -1;
        if (da > db)
            return 1;
        return 0;
    }

    // Compute mean / median / min / max / std_dev / p95 / p99 over a sample.
    // Sorts `values` in place.
    static inline basic26_Stats basic26_compute_stats(double *values, size_t n)
    {
        basic26_Stats s;

        qsort(values, n, sizeof(double), basic26_compare_doubles);

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
     
        s.mean = sum / (double)n;

        double variance = 0.0;

        for (size_t i = 0; i < n; i++)
        {
            double diff = values[i] - s.mean;
            variance += diff * diff;
        }
     
        s.std_dev = sqrt(variance / (double)n);

        return s;
    }

    static inline void basic26_print_stats(const char *label, const basic26_Stats *s)
    {
        printf("--- %s (ns/iter) ---\n", label);
        printf("Mean:     %.0f ns\n", s->mean);
        printf("Median:   %.0f ns\n", s->median);
        printf("Min:      %.0f ns\n", s->min);
        printf("Max:      %.0f ns\n", s->max);
        printf("Std Dev:  %.1f ns (%.1f%%)\n", s->std_dev, (s->std_dev / s->mean) * 100.0);
        printf("P95:      %.0f ns\n", s->p95);
        printf("P99:      %.0f ns\n", s->p99);
    }

#ifdef __cplusplus
}
#endif

#endif // BASIC26_EXAMPLES_COMMON_H
