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
//   NEW $obj1, 111           - create a host-side object and bind it to a variable
//   PRINT obj1               - print the object stored in variable obj1
//   DEL $obj1                - destroy the host-side object and reset the variable
//   WAIT 2                   - suspend execution for 2 seconds (async YIELD)
//   PRINT "Done waiting!"    - resumes after the wait
//   WAIT 2                   - another 2-second pause
//   PRINT "All done!"        - final message
//
// The five native functions demonstrate different aspects of the callback system:
//
//   PRINT  - prints any value type (INT, FLOAT, STRING, SYMBOL, NULL, OBJECT, ADDRESS)
//   VAR    - declares a variable initialized to NULL (takes a STRING arg as the name)
//   NEW    - allocates a host-side NativeObject and stores it as an OBJECT value
//            in a variable (takes a SYMBOL for the variable name and an INT for data)
//   DEL    - frees a previously allocated OBJECT and resets the variable to NULL
//   WAIT   - suspends execution for a given number of seconds. Returns YIELD so the
//            host's run loop can sleep and then resume the VM.
//
// The SYMBOL type ($xyz) allows the script to pass an identifier *by name* rather
// than by value. When the script writes `NEW $obj1, 111`, the callback receives
// the symbol "obj1" and uses it as a key to store the new object. Without symbols,
// the host would have no way to know which variable the script intends the result
// to be stored in.
//
// ==========================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MICROSECONDS(us) Sleep((DWORD)((us) / 1000))
#else
#include <unistd.h>
#define SLEEP_MICROSECONDS(us) usleep((useconds_t)(us))
#endif

#include <basic26.h>

// Helper macro: abort with a message if an expression is false.
#define CHECK(expr)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(expr))                                                            \
        {                                                                       \
            fprintf(stderr, "FATAL: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

// A simple host-side structure that can be wrapped as a BASIC26_VALUE_TYPE_OBJECT.
typedef struct NativeObject
{
    size_t data;
} NativeObject;

// The script source code.
const char *SOURCE =
    "PRINT \"Hello, world!\"\n"
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

// Tracks the remaining sleep time for the WAIT function across yield/resume cycles.
static double pending_wait_seconds = 0.0;

// Declares a variable by name and initializes it to NULL.
//
// Script usage:  VAR "variable_name"
//
// Uses a STRING argument for the variable name. Because StringId and SymbolId
// are interchangeable (they share the same interning table), the string ID can
// be used directly as a symbol ID for variable operations.
basic26_FunctionResult var_function(const basic26_CallInfo *info, size_t argc, const basic26_Value *argv)
{
    if (argc != 1)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    const basic26_Value arg = argv[0];

    if (arg.type != BASIC26_VALUE_TYPE_STRING)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    CHECK(basic26_State_set_var(info->state, arg.as.string_id, &(basic26_Value){
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
basic26_FunctionResult print_function(const basic26_CallInfo *info, size_t argc, const basic26_Value *argv)
{
    if (argc != 1)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    char buf[1024];
    const uint8_t *str = NULL;
    size_t str_len = 0;

    const basic26_Value arg = argv[0];
    const NativeObject *obj = NULL;

    switch (arg.type)
    {
    case BASIC26_VALUE_TYPE_INT:
        printf("SCRIPT: %lld\n", arg.as.int_val);
        break;
    case BASIC26_VALUE_TYPE_FLOAT:
        printf("SCRIPT: %f\n", arg.as.float_val);
        break;
    case BASIC26_VALUE_TYPE_NULL:
        printf("SCRIPT: NULL\n");
        break;
    case BASIC26_VALUE_TYPE_STRING:
        CHECK(basic26_Vm_get_string(vm, arg.as.string_id, &str, &str_len) == BASIC26_RESULT_OK);

        memcpy(buf, str, str_len);
        buf[str_len] = 0;

        printf("SCRIPT: \"%s\"\n", buf);

        break;
    case BASIC26_VALUE_TYPE_SYMBOL:
        CHECK(basic26_Vm_get_string(vm, arg.as.symbol_id, &str, &str_len) == BASIC26_RESULT_OK);

        memcpy(buf, str, str_len);
        buf[str_len] = 0;

        printf("SCRIPT: $%s\n", buf);

        break;
    case BASIC26_VALUE_TYPE_ADDRESS:
        printf("SCRIPT: @%lu\n", arg.as.address_val);

        break;
    case BASIC26_VALUE_TYPE_OBJECT:
        obj = arg.as.object_ptr;
        printf("SCRIPT: (@NativeObject){ .data = %lu }\n", obj->data);

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
// to store a result is a common idiom since native functions have no return value.
basic26_FunctionResult new_function(const basic26_CallInfo *info, size_t argc, const basic26_Value *argv)
{
    if (argc != 2)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    if (argv[0].type != BASIC26_VALUE_TYPE_SYMBOL)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    if (argv[1].type != BASIC26_VALUE_TYPE_INT)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    // Allocate the host-side object.
    NativeObject *obj = basic26_Vm_alloc(vm, sizeof(NativeObject), alignof(NativeObject));
    CHECK(obj != NULL);

    obj->data = argv[1].as.int_val;

    CHECK(
        basic26_State_set_var(state, argv[0].as.symbol_id, &(basic26_Value){
                                                               .type = BASIC26_VALUE_TYPE_OBJECT,
                                                               .as = {
                                                                   .object_ptr = obj,
                                                               },
                                                           }) == BASIC26_RESULT_OK);

    return BASIC26_FUNCTION_RESULT_OK;
}

// Frees a previously allocated NativeObject and resets the variable to NULL.
//
// Script usage: DEL $variable_name
//
// The argument must be a SYMBOL naming the variable that holds the object to
// delete.
basic26_FunctionResult del_function(const basic26_CallInfo *info, size_t argc, const basic26_Value *argv)
{
    if (argc != 1)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    if (argv[0].type != BASIC26_VALUE_TYPE_SYMBOL)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    basic26_Value target_var;

    if (basic26_State_get_var(state, argv[0].as.symbol_id, &target_var) != BASIC26_RESULT_OK)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    if (target_var.type != BASIC26_VALUE_TYPE_OBJECT)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    // Free the host-side memory.
    basic26_Vm_free(vm, target_var.as.object_ptr, sizeof(NativeObject), alignof(NativeObject));

    // Reset the variable to NULL so the script cannot access the freed pointer.
    CHECK(
        basic26_State_set_var(state, argv[0].as.symbol_id, &(basic26_Value){
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
basic26_FunctionResult wait_function(const basic26_CallInfo *info, size_t argc, const basic26_Value *argv)
{
    if (argc != 1)
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    const basic26_Value arg = argv[0];

    if (arg.type == BASIC26_VALUE_TYPE_INT)
    {
        pending_wait_seconds = (double)arg.as.int_val;
    }
    else if (arg.type == BASIC26_VALUE_TYPE_FLOAT)
    {
        pending_wait_seconds = arg.as.float_val;
    }
    else
    {
        return BASIC26_FUNCTION_RESULT_ERROR;
    }

    // Return YIELD so the host's run loop can perform the actual sleep
    // and then resume the VM.
    return BASIC26_FUNCTION_RESULT_YIELD;
}

// Uses basic26_Script_dump() to print the compiled bytecode in a human-readable
// format. The dump string is allocated by the VM and must be freed using
// basic26_Vm_free().
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

    basic26_Vm_free(vm, str, str_len, 1);
}

const char *runtime_error_to_string(basic26_RuntimeError code)
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

// Destroys all BASIC26 objects in reverse order.
void cleanup()
{
    if (vm == NULL)
    {
        return;
    }

    basic26_Script_destroy(script, vm);
    script = NULL;

    basic26_State_destroy(state, vm);
    state = NULL;

    basic26_Vm_destroy(vm);
    vm = NULL;
}

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    // Step 1: Create the VM.
    CHECK(basic26_Vm_create(&(basic26_CreateVmOptions){.alloc = NULL}, &vm) == BASIC26_RESULT_OK);

    // Step 2: Create an execution State.
    CHECK(basic26_State_create(&(basic26_CreateStateOptions){.vm = vm}, &state) == BASIC26_RESULT_OK);

    // Step 3: Create a Script object.
    CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);

    // Step 4: Compile the source code into bytecode.
    basic26_CompileErrorInfo compile_err_info;

    CHECK(
        basic26_Script_compile(script, &(basic26_CompileOptions){
                                           .vm = vm,
                                           .source = (const uint8_t *)SOURCE,
                                           .source_len = strlen(SOURCE),
                                           .limits = &(basic26_ScriptLimits){
                                               .max_opcodes = 0,
                                               .max_strings = 0,
                                           },
                                       },
                               &compile_err_info) == BASIC26_RESULT_OK);

    // Print the compiled bytecode for inspection.
    print_dump();

    // Step 5: Obtain symbol IDs for the native function names.
    basic26_SymbolId print_symbol_id;
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"PRINT", strlen("PRINT"), true, &print_symbol_id) == BASIC26_RESULT_OK);

    basic26_SymbolId var_symbol_id;
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"VAR", strlen("VAR"), true, &var_symbol_id) == BASIC26_RESULT_OK);

    basic26_SymbolId new_symbol_id;
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"NEW", strlen("NEW"), true, &new_symbol_id) == BASIC26_RESULT_OK);

    basic26_SymbolId del_symbol_id;
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"DEL", strlen("DEL"), true, &del_symbol_id) == BASIC26_RESULT_OK);

    basic26_SymbolId wait_symbol_id;
    CHECK(basic26_Vm_get_string_id(vm, (const uint8_t *)"WAIT", strlen("WAIT"), true, &wait_symbol_id) == BASIC26_RESULT_OK);

    // Step 6: Register the native function callbacks.
    CHECK(
        basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                             .name = print_symbol_id,
                                             .callback = print_function,
                                         }) == BASIC26_RESULT_OK);

    CHECK(
        basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                             .name = var_symbol_id,
                                             .callback = var_function,
                                         }) == BASIC26_RESULT_OK);

    CHECK(
        basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                             .name = new_symbol_id,
                                             .callback = new_function,
                                         }) == BASIC26_RESULT_OK);

    CHECK(
        basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                             .name = del_symbol_id,
                                             .callback = del_function,
                                         }) == BASIC26_RESULT_OK);

    CHECK(
        basic26_Vm_register_function(vm, &(basic26_RegisterFunctionOptions){
                                             .name = wait_symbol_id,
                                             .callback = wait_function,
                                         }) == BASIC26_RESULT_OK);

    // Step 7: Run the script in a loop that handles YIELD.
    // When a native callback returns YIELD, Vm_run() returns RESULT_YIELDED.
    // The host can then perform pending work and call Vm_run() again to resume.
    basic26_RuntimeErrorInfo runtime_err_info;

    while (true)
    {
        basic26_Result result = basic26_Vm_run(vm, &(basic26_RunOptions){
                                                       .state = state,
                                                       .script = script,
                                                       .limits = &(basic26_RunLimits){
                                                           .max_ops = 0,
                                                           .max_time_ns = 0,
                                                       },
                                                       .userdata = NULL,
                                                   },
                                               &runtime_err_info);

        if (result == BASIC26_RESULT_OK)
        {
            break;
        }
        else if (result == BASIC26_RESULT_YIELDED)
        {
            // A native callback yielded (the WAIT function in this example).
            // Perform the pending sleep, then loop back to resume execution.
            if (pending_wait_seconds > 0.0)
            {
                printf("HOST: sleeping for %.1f seconds...\n", pending_wait_seconds);
                SLEEP_MICROSECONDS(pending_wait_seconds * 1000000.0);
                pending_wait_seconds = 0.0;
            }
        }
        else if (result == BASIC26_RESULT_RUNTIME_ERROR)
        {
            fprintf(stderr,
                    "RUNTIME ERROR at IP=%zu: %s (%d)\n",
                    runtime_err_info.ip,
                    runtime_error_to_string(runtime_err_info.code),
                    runtime_err_info.code);
            cleanup();

            return 1;
        }
        else
        {
            fprintf(stderr, "UNEXPECTED RESULT: %d\n", (int)result);
            cleanup();

            return 1;
        }
    }

    // Step 8: Clean up all resources.
    cleanup();

    return 0;
}
