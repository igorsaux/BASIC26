// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

// ==========================================================================
// BASIC26 Example 01 - Comprehensive Feature Tour
// ==========================================================================
//
// This example demonstrates the full lifecycle of a BASIC26 script from the
// host application's perspective. It creates a VM, compiles a script, and
// runs it in a loop that handles YIELD.
//
// The script itself is a minimal tour of the language features:
//
//   PRINT "Hello, world!"    - call a native function with a string argument
//   VAR "x"                  - declare a variable by name (using a STRING arg)
//   x = NULL                 - assign the NULL value to the variable x
//   PRINT x                  - print the value of x (NULL)
//   PRINT $xyz               - print a symbol literal ($xyz)
//   NEW $obj1, 111           - create a host-side object and bind it to a
//   variable PRINT obj1               - print the object stored in variable
//   obj1 DEL $obj1                - destroy the host-side object and reset the
//   variable WAIT 2                   - suspend execution for 2 seconds (async
//   YIELD) PRINT "Done waiting!"    - resumes after the wait WAIT 2 - another
//   2-second pause PRINT "All done!"        - final message
//
// The five native functions demonstrate different aspects of the callback
// system:
//
//   PRINT  - prints any value type (INT, FLOAT, STRING, SYMBOL, NULL, OBJECT,
//   ADDRESS) VAR    - declares a variable initialized to NULL (takes a STRING
//   arg as the name) NEW    - allocates a host-side NativeObject and stores it
//   as an OBJECT value
//            in a variable (takes a SYMBOL for the variable name and an INT for
//            data)
//   DEL    - frees a previously allocated OBJECT and resets the variable to
//   NULL WAIT   - suspends execution for a given number of seconds. Returns
//   YIELD so the
//            host's run loop can sleep and then resume the VM.
//
// The SYMBOL type ($xyz) allows the script to pass an identifier *by name*
// rather than by value. When the script writes `NEW $obj1, 111`, the callback
// receives the symbol "obj1" and uses it as a key to store the new object.
// Without symbols, the host would have no way to know which variable the script
// intends the result to be stored in.
//
// ==========================================================================

#include "common.h"
#include <stdalign.h>

// A simple host-side structure that can be wrapped as a
// BASIC26_VALUE_TYPE_OBJECT.
typedef struct NativeObject {
  size_t data;
} NativeObject;

// The script source code.
static const char *SOURCE = "PRINT \"Hello, world!\"\n"
                            "VAR \"x\"\n"
                            "x = NULL\n"
                            "PRINT x\n"
                            "PRINT $xyz\n"
                            "NEW $obj1, 111\n"
                            "PRINT obj1\n"
                            "DEL $obj1\n"
                            "WAIT 2\n"
                            "PRINT \"Done waiting!\"\n"
                            "WAIT 2\n"
                            "PRINT \"All done!\"";

// Global handles for the VM, State, and Script.
basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script = NULL;

// Tracks the remaining sleep time for the WAIT function across yield/resume
// cycles.
static double pending_wait_seconds = 0.0;

// Declares a variable by name and initializes it to NULL.
//
// Script usage:  VAR "variable_name"
//
// Uses a STRING argument for the variable name. Because StringId and SymbolId
// are interchangeable (they share the same interning table), the string ID can
// be used directly as a symbol ID for variable operations.
static basic26_FunctionResult var_function(const basic26_CallInfo *info,
                                           size_t argc,
                                           const basic26_Value *argv) {
  UNUSED(info);

  if (argc != 1 || argv[0].type != BASIC26_VALUE_TYPE_STRING) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  CHECK(basic26_State_set_var(info->state, argv[0].as.string_id,
                              &(basic26_Value){
                                  .type = BASIC26_VALUE_TYPE_NULL,
                              }) == BASIC26_RESULT_OK);

  return BASIC26_FUNCTION_RESULT_OK;
}

// Prints a single value of any type to stdout.
//
// Script usage: PRINT value
//
// For STRING and SYMBOL values, the raw bytes are retrieved from the VM's
// string table via basic26_Vm_get_string(). For OBJECT values, the opaque
// pointer is cast back to NativeObject.
static basic26_FunctionResult print_function(const basic26_CallInfo *info,
                                             size_t argc,
                                             const basic26_Value *argv) {
  UNUSED(info);

  if (argc != 1) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  char buf[1024];
  const uint8_t *str = NULL;
  size_t str_len = 0;

  const basic26_Value arg = argv[0];
  const NativeObject *obj = NULL;

  switch (arg.type) {
  case BASIC26_VALUE_TYPE_INT:
    printf("SCRIPT: %lld\n", (long long)arg.as.int_val);

    break;
  case BASIC26_VALUE_TYPE_FLOAT:
    printf("SCRIPT: %f\n", arg.as.float_val);

    break;
  case BASIC26_VALUE_TYPE_NULL:
    printf("SCRIPT: NULL\n");

    break;
  case BASIC26_VALUE_TYPE_STRING:
    CHECK(basic26_Vm_get_string(vm, arg.as.string_id, &str, &str_len) ==
          BASIC26_RESULT_OK);

    memcpy(buf, str, str_len);
    buf[str_len] = 0;

    printf("SCRIPT: \"%s\"\n", buf);

    break;
  case BASIC26_VALUE_TYPE_SYMBOL:
    CHECK(basic26_Vm_get_string(vm, arg.as.symbol_id, &str, &str_len) ==
          BASIC26_RESULT_OK);

    memcpy(buf, str, str_len);
    buf[str_len] = 0;

    printf("SCRIPT: $%s\n", buf);

    break;
  case BASIC26_VALUE_TYPE_ADDRESS:
    printf("SCRIPT: @%lu\n", (unsigned long)arg.as.address_val);

    break;
  case BASIC26_VALUE_TYPE_OBJECT:
    obj = arg.as.object_ptr;
    printf("SCRIPT: (@NativeObject){ .data = %lu }\n",
           (unsigned long)obj->data);

    break;
  default:
    CHECK(false);

    break;
  }

  return BASIC26_FUNCTION_RESULT_OK;
}

// Allocates a host-side NativeObject and stores it in a script variable.
//
// Script usage: NEW $variable_name, initial_data
//
// The first argument must be a SYMBOL specifying which variable the new object
// should be stored in. The second argument must be an INT providing the initial
// value for the object's data field. Using a SYMBOL to tell the host *where*
// to store a result is a common idiom since native functions have no return
// value.
static basic26_FunctionResult new_function(const basic26_CallInfo *info,
                                           size_t argc,
                                           const basic26_Value *argv) {
  UNUSED(info);

  if (argc != 2 || argv[0].type != BASIC26_VALUE_TYPE_SYMBOL ||
      argv[1].type != BASIC26_VALUE_TYPE_INT) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  // Allocate the host-side object.
  NativeObject *obj =
      basic26_Vm_alloc(vm, sizeof(NativeObject), alignof(NativeObject));
  CHECK(obj != NULL);

  obj->data = (size_t)argv[1].as.int_val;

  CHECK(basic26_State_set_var(state, argv[0].as.symbol_id,
                              &(basic26_Value){
                                  .type = BASIC26_VALUE_TYPE_OBJECT,
                                  .as = {.object_ptr = obj},
                              }) == BASIC26_RESULT_OK);

  return BASIC26_FUNCTION_RESULT_OK;
}

// Frees a previously allocated NativeObject and resets the variable to NULL.
//
// Script usage: DEL $variable_name
//
// The argument must be a SYMBOL naming the variable that holds the object to
// delete.
static basic26_FunctionResult del_function(const basic26_CallInfo *info,
                                           size_t argc,
                                           const basic26_Value *argv) {
  UNUSED(info);

  if (argc != 1 || argv[0].type != BASIC26_VALUE_TYPE_SYMBOL) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  basic26_Value target_var;
  if (basic26_State_get_var(state, argv[0].as.symbol_id, &target_var) !=
          BASIC26_RESULT_OK ||
      target_var.type != BASIC26_VALUE_TYPE_OBJECT) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  // Free the host-side memory.
  basic26_Vm_free(vm, target_var.as.object_ptr, sizeof(NativeObject),
                  alignof(NativeObject));

  CHECK(basic26_State_set_var(state, argv[0].as.symbol_id,
                              &(basic26_Value){
                                  .type = BASIC26_VALUE_TYPE_NULL,
                              }) == BASIC26_RESULT_OK);

  return BASIC26_FUNCTION_RESULT_OK;
}

// Suspends script execution for a given number of seconds.
//
// Script usage: WAIT seconds
//
// The argument can be an INT or FLOAT. Returns BASIC26_FUNCTION_RESULT_YIELD
// so the host's run loop can perform the actual sleep and then resume the VM.
static basic26_FunctionResult wait_function(const basic26_CallInfo *info,
                                            size_t argc,
                                            const basic26_Value *argv) {
  UNUSED(info);

  if (argc != 1) {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  const basic26_Value arg = argv[0];

  if (arg.type == BASIC26_VALUE_TYPE_INT) {
    pending_wait_seconds = (double)arg.as.int_val;
  } else if (arg.type == BASIC26_VALUE_TYPE_FLOAT) {
    pending_wait_seconds = arg.as.float_val;
  } else {
    return BASIC26_FUNCTION_RESULT_ERROR;
  }

  // Return YIELD so the host's run loop can perform the actual sleep
  // and then resume the VM.
  return BASIC26_FUNCTION_RESULT_YIELD;
}

// Destroys all BASIC26 objects in reverse order.
static void cleanup(void) {
  if (vm == NULL) {
    return;
  }

  basic26_Script_destroy(script);
  script = NULL;

  basic26_State_destroy(state);
  state = NULL;

  basic26_Vm_destroy(vm);
  vm = NULL;
}

int main(int argc, const char **argv) {
  UNUSED(argc);
  UNUSED(argv);

  // Step 1: Create the VM, State, and Script.
  CHECK(basic26_Vm_create(NULL, &vm) == BASIC26_RESULT_OK);
  CHECK(basic26_State_create(vm, &state) == BASIC26_RESULT_OK);
  CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);

  // Step 2: Compile the source code.
  basic26_compile(vm, script, SOURCE);

  // Print the compiled bytecode for inspection.
  basic26_print_dump(vm, script);

  // Step 3: Register the native function callbacks.
  basic26_register(vm, "PRINT", print_function);
  basic26_register(vm, "VAR", var_function);
  basic26_register(vm, "NEW", new_function);
  basic26_register(vm, "DEL", del_function);
  basic26_register(vm, "WAIT", wait_function);

  // Step 4: Run the script in a loop that handles YIELD.
  // When a native callback returns YIELD, Vm_run() returns RESULT_YIELDED.
  // The host can then perform pending work and call Vm_run() again to resume.
  basic26_RuntimeErrorInfo runtime_err_info;

  while (true) {
    basic26_Result result = basic26_Vm_run(vm,
                                           &(basic26_RunOptions){
                                               .state = state,
                                               .script = script,
                                               .limits =
                                                   &(basic26_RunLimits){
                                                       .max_ops = 0,
                                                       .max_time_ns = 0,
                                                   },
                                               .userdata = NULL,
                                           },
                                           &runtime_err_info);

    if (result == BASIC26_RESULT_OK) {
      break;
    } else if (result == BASIC26_RESULT_YIELDED) {
      // A native callback yielded (the WAIT function in this example).
      // Perform the pending sleep, then loop back to resume execution.
      if (pending_wait_seconds > 0.0) {
        printf("HOST: sleeping for %.1f seconds...\n", pending_wait_seconds);
        basic26_sleep_us((long long)(pending_wait_seconds * 1000000.0));
        pending_wait_seconds = 0.0;
      }
    } else if (result == BASIC26_RESULT_RUNTIME_ERROR) {
      fprintf(stderr, "RUNTIME ERROR at IP=%zu: %s (%d)\n", runtime_err_info.ip,
              basic26_runtime_error_to_string(runtime_err_info.code),
              runtime_err_info.code);
      cleanup();

      return 1;
    } else {
      fprintf(stderr, "UNEXPECTED RESULT: %d\n", (int)result);
      cleanup();

      return 1;
    }
  }

  // Step 5: Clean up all resources.
  cleanup();

  return 0;
}
