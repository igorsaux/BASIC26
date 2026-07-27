// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

// ==========================================================================
// BASIC26 Example 03 - Native Call Overhead Benchmark
// ==========================================================================
//
// This benchmark isolates the cost of a BASIC26 *native call* - i.e. the
// transition from interpreter bytecode into a C callback registered via
// basic26_Vm_register_function() and back.
//
// We run two scripts that perform the same amount of arithmetic work:
//
//   (a) "bytecode" - the addition `temp = a + b` is performed by the
//       interpreter's own ADD opcode.
//
//   (b) "native"   - the addition is delegated to a C callback `NADD`
//       invoked from the script as `NADD a, b, $temp`. The callback
//       reads two INTs and a SYMBOL from the stack, computes the sum,
//       and writes it back into the named variable.
//
// Everything else (loop counter, WHILE test, three variable assignments
// per iteration) is identical between the two scripts, so the delta in
// per-iteration time is almost exactly the cost of one native call.
//
// We also report the absolute native-call throughput (calls/sec) and the
// per-call overhead (ns), computed both from the delta and from a pure
// "noop" native function (`NOOP`) that takes one INT and does nothing -
// that one isolates the call frame overhead from any work the callback
// actually does.
//
// This example links against the static library.
//
// ==========================================================================

#include "common.h"

#define LOOP_N 100
#define WARMUP_ITERATIONS 20000
#define BENCH_ITERATIONS 100000
#define RUNS 10
#define T_CRITICAL 2.262

basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script_bytecode = NULL;
basic26_Script *script_native = NULL;
basic26_Script *script_noop = NULL;

basic26_SymbolId symbol_a;
basic26_SymbolId symbol_b;
basic26_SymbolId symbol_temp;
basic26_SymbolId symbol_i;
basic26_SymbolId symbol_n;

static int64_t g_native_call_count = 0;

// --------------------------------------------------------------------------
// Scripts
// --------------------------------------------------------------------------

// (a) Pure-bytecode baseline: the addition is an interpreter opcode.
static const char *SOURCE_BYTECODE = "i = 0\n"
                                     "WHILE i < n\n"
                                     "  temp = a + b\n"
                                     "  i = i + 1\n"
                                     "ENDWHILE\n";

// (b) Native-call variant: the addition is performed by NADD.
static const char *SOURCE_NATIVE = "i = 0\n"
                                   "WHILE i < n\n"
                                   "  NADD a, b, $temp\n"
                                   "  i = i + 1\n"
                                   "ENDWHILE\n";

// (c) Bare native-call overhead: NOOP takes one INT and returns immediately.
//     The loop body does the same amount of *interpreter* work as (a) minus
//     the addition, so the delta between (a) and (c) is roughly the cost of
//     one native call frame with zero callback body.
static const char *SOURCE_NOOP = "i = 0\n"
                                 "WHILE i < n\n"
                                 "  NOOP a\n"
                                 "  i = i + 1\n"
                                 "ENDWHILE\n";

// --------------------------------------------------------------------------
// Native callbacks
// --------------------------------------------------------------------------

// NADD a, b, $out -> out = a + b
static basic26_FunctionResult nadd_function(const basic26_CallInfo *info,
                                            size_t argc,
                                            const basic26_Value *argv) {
  g_native_call_count++;

  if (argc != 3 || argv[0].type != BASIC26_VALUE_TYPE_INT ||
      argv[1].type != BASIC26_VALUE_TYPE_INT ||
      argv[2].type != BASIC26_VALUE_TYPE_SYMBOL) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  const int64_t sum = argv[0].as.int_val + argv[1].as.int_val;

  CHECK(basic26_State_set_var(info->state, argv[2].as.symbol_id,
                              &(basic26_Value){
                                  .type = BASIC26_VALUE_TYPE_INT,
                                  .as.int_val = sum,
                              }) == BASIC26_RESULT_OK);

  return BASIC26_FUNCTION_RESULT_OK;
}

// NOOP a -> does nothing, just counts the call
static basic26_FunctionResult noop_function(const basic26_CallInfo *info,
                                            size_t argc,
                                            const basic26_Value *argv) {
  UNUSED(info);
  UNUSED(argc);
  UNUSED(argv);

  g_native_call_count++;

  if (argc != 1 || argv[0].type != BASIC26_VALUE_TYPE_INT) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  return BASIC26_FUNCTION_RESULT_OK;
}

// --------------------------------------------------------------------------
// Setup / teardown
// --------------------------------------------------------------------------

static void cache_symbols(void) {
  symbol_a = basic26_intern(vm, "a");
  symbol_b = basic26_intern(vm, "b");
  symbol_temp = basic26_intern(vm, "temp");
  symbol_i = basic26_intern(vm, "i");
  symbol_n = basic26_intern(vm, "n");
}

static void reset_state(basic26_State *st) {
  basic26_State_clear(st, &(basic26_ClearStateOptions){
                              .clear_stack = true,
                          });

  basic26_set_int_var(st, symbol_n, LOOP_N);
  basic26_set_int_var(st, symbol_a, 0);
  basic26_set_int_var(st, symbol_b, 1);
  basic26_set_int_var(st, symbol_i, 0);

  // For the native and noop variants the script never assigns to `temp`
  // itself (the callback does), so we have to pre-create it here - the
  // interpreter refuses to write to an undefined variable.
  basic26_set_int_var(st, symbol_temp, 0);

  basic26_State_set_ip(st, 0);
}

static void cleanup(void) {
  if (vm == NULL)
    return;

  basic26_Script_destroy(script_bytecode);
  basic26_Script_destroy(script_native);
  basic26_Script_destroy(script_noop);
  script_bytecode = NULL;
  script_native = NULL;
  script_noop = NULL;

  basic26_State_destroy(state);
  state = NULL;

  basic26_Vm_destroy(vm);
  vm = NULL;
}

// --------------------------------------------------------------------------
// Benchmark
// --------------------------------------------------------------------------

// Returns mean ns/iter over `iterations` runs of the given script.
static double bench_script(basic26_Script *script, int iterations) {
  const long long start = basic26_now_ns();

  for (int i = 0; i < iterations; i++) {
    reset_state(state);
    basic26_run_sync(vm, state, script);
  }

  const long long end = basic26_now_ns();

  return (double)(end - start) / (double)iterations;
}

// Runs RUNS independent measurements, computes and prints statistics.
// Returns the stats struct so the caller can reuse mean / CI.
static basic26_Stats report_variant(const char *label, basic26_Script *script) {
  double *runs = malloc((size_t)RUNS * sizeof(double));
  CHECK(runs != NULL);

  for (int run = 0; run < RUNS; run++) {
    runs[run] = bench_script(script, BENCH_ITERATIONS);
    printf("  %-10s Run %2d: %.0f ns/iter\n", label, run + 1, runs[run]);
  }

  basic26_Stats s = basic26_compute_stats(runs, RUNS, T_CRITICAL);
  basic26_print_stats(label, &s);

  free(runs);
  return s;
}

int main(void) {
  CHECK(basic26_Vm_create(NULL, &vm) == BASIC26_RESULT_OK);
  CHECK(basic26_State_create(vm, &state) == BASIC26_RESULT_OK);
  CHECK(basic26_Script_create(vm, &script_bytecode) == BASIC26_RESULT_OK);
  CHECK(basic26_Script_create(vm, &script_native) == BASIC26_RESULT_OK);
  CHECK(basic26_Script_create(vm, &script_noop) == BASIC26_RESULT_OK);

  basic26_compile(vm, script_bytecode, SOURCE_BYTECODE);
  basic26_compile(vm, script_native, SOURCE_NATIVE);
  basic26_compile(vm, script_noop, SOURCE_NOOP);

  basic26_register(vm, "NADD", nadd_function);
  basic26_register(vm, "NOOP", noop_function);

  cache_symbols();

  printf("=== BASIC26 native-call overhead benchmark ===\n");
  printf("Loop iterations per script run : %d\n", LOOP_N);
  printf("Iterations per benchmark run   : %d\n", BENCH_ITERATIONS);
  printf("Runs per variant               : %d\n", RUNS);
  printf("Warmup iterations              : %d\n\n", WARMUP_ITERATIONS);

  // ---- Correctness check ---------------------------------------------
  reset_state(state);
  basic26_run_sync(vm, state, script_bytecode);
  const int64_t r_bc = basic26_get_int_var(state, symbol_temp);

  reset_state(state);
  g_native_call_count = 0;
  basic26_run_sync(vm, state, script_native);
  const int64_t r_nat = basic26_get_int_var(state, symbol_temp);
  const int64_t nat_calls = g_native_call_count;

  reset_state(state);
  g_native_call_count = 0;
  basic26_run_sync(vm, state, script_noop);
  const int64_t noop_calls = g_native_call_count;

  printf("Correctness:\n");
  printf("  bytecode: temp = %lld\n", (long long)r_bc);
  printf("  native  : temp = %lld  (calls this run: %lld)\n", (long long)r_nat,
         (long long)nat_calls);
  printf("  noop    :             (calls this run: %lld)\n",
         (long long)noop_calls);
  CHECK(r_bc == r_nat);
  CHECK(nat_calls == LOOP_N);
  CHECK(noop_calls == LOOP_N);
  printf("\n");

  // ---- Warmup --------------------------------------------------------
  printf("Warming up (%d iterations per variant)...\n", WARMUP_ITERATIONS);

  for (int i = 0; i < WARMUP_ITERATIONS; i++) {
    reset_state(state);
    basic26_run_sync(vm, state, script_bytecode);
    reset_state(state);
    basic26_run_sync(vm, state, script_native);
    reset_state(state);
    basic26_run_sync(vm, state, script_noop);
  }

  printf("Warmup complete.\n\n");

  // ---- Per-variant results -------------------------------------------
  printf("--- Per-variant results (loop body x %d) ---\n", LOOP_N);
  const basic26_Stats s_bc = report_variant("bytecode", script_bytecode);
  printf("\n");
  const basic26_Stats s_nat = report_variant("native", script_native);
  printf("\n");
  const basic26_Stats s_nop = report_variant("noop", script_noop);
  printf("\n");

  // ---- Derived: native call overhead ---------------------------------
  //
  // The loop body of each variant executes LOOP_N native calls per
  // script run. So:
  //
  //   ns_per_native_call (delta) =
  //       (ns_per_iter_native - ns_per_iter_bytecode) / LOOP_N
  //
  // This is the *marginal* cost of replacing the bytecode op
  // `temp = a + b` with the native call `NADD a, b, $temp`. It includes
  // argument marshalling, callback dispatch, and the C-side add +
  // set_var, minus the bytecode op it replaces - so it is the most
  // apples-to-apples "what does going native cost me" number.
  //
  // The "noop" variant tells us the absolute throughput of an empty
  // native call (one arg, empty body). Its per-call cost is reported
  // as an upper bound on call-frame throughput.

  const double bc_mean = s_bc.median;
  const double nat_mean = s_nat.median;
  const double nop_mean = s_nop.median;

  const double overhead_vs_bytecode = (nat_mean - bc_mean) / (double)LOOP_N;
  const double noop_per_call = nop_mean / (double)LOOP_N;
  const double noop_throughput = 1e9 / noop_per_call;
  const double nadd_throughput = 1e9 / (nat_mean / (double)LOOP_N);

  printf("--- Derived native-call metrics ---\n");
  printf("bytecode baseline : %.0f ns/script-run  (%.2f ns/loop-iter)\n",
         bc_mean, bc_mean / (double)LOOP_N);
  printf("native (NADD)     : %.0f ns/script-run  (%.2f ns/loop-iter)\n",
         nat_mean, nat_mean / (double)LOOP_N);
  printf("noop (NOOP)       : %.0f ns/script-run  (%.2f ns/loop-iter)\n",
         nop_mean, nop_mean / (double)LOOP_N);
  printf("\n");
  printf("Marginal cost per native call (native - bytecode) / N : %+.1f ns\n",
         overhead_vs_bytecode);
  printf("Absolute cost per empty native call (noop / N)        : %.1f ns\n",
         noop_per_call);
  printf("Empty native call throughput                          : %.0f "
         "calls/sec\n",
         noop_throughput);
  printf("NADD native call throughput (incl. callback body)     : %.0f "
         "calls/sec\n",
         nadd_throughput);

  cleanup();

  return 0;
}
