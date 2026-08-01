// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

/*
 * startup_wasmtime.c - Startup/initialization benchmark for wasmtime.
 *
 * Equivalent to running an empty file in CPython: measures engine
 * creation, module compilation, store creation, and instance
 * instantiation. No imports, no exports, no function calls.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <wasm.h>
#include <wasmtime.h>

/* Minimal valid WASM module: magic + version, no sections. */
static const uint8_t empty_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, /* magic: \0asm */
    0x01, 0x00, 0x00, 0x00  /* version: 1   */
};

int main(void) {
  wasm_config_t *config = wasm_config_new();
  wasmtime_config_consume_fuel_set(config, false);
  wasm_engine_t *engine = wasm_engine_new_with_config(config);
  assert(engine != NULL);

  wasmtime_module_t *module = NULL;
  wasmtime_error_t *err =
      wasmtime_module_new(engine, empty_wasm, sizeof(empty_wasm), &module);
  if (err != NULL) {
    wasm_name_t msg = {};
    wasmtime_error_message(err, &msg);
    fprintf(stderr, "compile error: %s\n", msg.size ? msg.data : "(unknown)");
    wasmtime_error_delete(err);
    return 1;
  }

  wasmtime_store_t *store = wasmtime_store_new(engine, NULL, NULL);
  assert(store != NULL);
  wasmtime_context_t *ctx = wasmtime_store_context(store);

  wasm_trap_t *trap = NULL;
  wasmtime_instance_t instance;
  err = wasmtime_instance_new(ctx, module, NULL, 0, &instance, &trap);
  if (err != NULL || trap != NULL) {
    wasm_name_t msg = {};
    if (err != NULL) {
      wasmtime_error_message(err, &msg);
      fprintf(stderr, "instantiation failed: %s\n",
              msg.size ? msg.data : "(unknown)");
      wasmtime_error_delete(err);
    } else {
      wasm_trap_message(trap, &msg);
      fprintf(stderr, "instantiation trap: %.*s\n", (int)msg.size, msg.data);
      wasm_trap_delete(trap);
    }
    return 1;
  }

  wasmtime_store_delete(store);
  wasmtime_module_delete(module);
  wasm_engine_delete(engine);

  return 0;
}