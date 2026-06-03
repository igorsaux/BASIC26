// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

// ==========================================================================
// BASIC26 Example 01 - Comprehensive Feature Tour
// ==========================================================================
//
// This example demonstrates the full lifecycle of a BASIC26 script from the
// host application's perspective. It walks through every major API capability
// and shows how the different parts of the library work together.
//
// WHAT THIS EXAMPLE DOES
// ----------------------
// The host application creates a BASIC26 VM, compiles a short script, and
// runs it. The script itself is a minimal tour of the language features:
//
//   PRINT "Hello, world!"    - call a native function with a string argument
//   VAR "x"                  - declare a variable by name (using a string arg;
//                              compare with the $-prefixed SYMBOL syntax used
//                              by NEW and DEL)
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
// API LIFECYCLE IN THIS EXAMPLE
// -----------------------------
// 1. basic26_Vm_create()        - create the VM
// 2. basic26_State_create()     - create an execution state
// 3. basic26_Script_create()    - create a script object
// 4. basic26_Script_compile()   - compile source code into bytecode
// 5. basic26_Vm_get_string_id()  - obtain symbol IDs for function names
// 6. basic26_Vm_register_function()  - register native callbacks
// 7. basic26_Vm_run() in a loop  - execute the script; loop handles YIELD
// 8. basic26_Script_destroy()   - destroy script
// 9. basic26_State_destroy()    - destroy state
// 10. basic26_Vm_destroy()      - destroy VM
//
// The five native functions registered in this example demonstrate different
// aspects of the callback system:
//
//   PRINT  - prints any value type (INT, FLOAT, STRING, SYMBOL, NULL, OBJECT)
//   VAR    - declares a variable initialized to NULL (takes a STRING arg as
//            the variable name; this uses the STRING type rather than SYMBOL
//            for historical reasons  - compare with the $-prefixed SYMBOL
//            syntax used by NEW and DEL, which is the preferred idiom)
//   NEW    - allocates a host-side NativeObject and stores it as an OBJECT
//            value in a variable (takes a SYMBOL for the variable name and an
//            INT for the initial data)
//   DEL    - frees a previously allocated OBJECT and resets the variable to
//            NULL (takes a SYMBOL for the variable name)
//   WAIT   - suspends execution for a given number of seconds (takes an INT
//            or FLOAT argument). Returns BASIC26_FUNCTION_RESULT_YIELD so the
//            host's run loop can call sleep() and then resume the VM.
//
// The SYMBOL type ($xyz) is particularly important for NEW and DEL: it allows
// the script to pass an identifier *by name* rather than by value. When the
// script writes `NEW $obj1, 111`, the callback receives the symbol "obj1" and
// can then use it as a key to store the new object in the variable table.
// Without symbols, the host would have no way to know *which* variable the
// script intends the result to be stored in.
//
// The WAIT function demonstrates the YIELD mechanism: the native callback
// returns BASIC26_FUNCTION_RESULT_YIELD, which causes basic26_Vm_run() to
// return BASIC26_RESULT_YIELDED. The host then performs the actual sleep,
// and calls basic26_Vm_run() again to resume execution from where it left off.
// This pattern enables asynchronous operations in an event-driven host without
// blocking the entire application.
//
// ==========================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MICROSECONDS(us) Sleep((DWORD)((us) / 1000))
#else
#include <unistd.h>
#define SLEEP_MICROSECONDS(us) usleep((useconds_t)(us))
#endif

#include <basic26.h>

// Helper macro: abort with a message if an expression is false.
// Used throughout this example to verify that API calls succeed.
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
// In a real application this could be a texture handle, a file descriptor, a
// network connection, or any other resource that the script should not directly
// manipulate.
typedef struct NativeObject
{
    size_t data;
} NativeObject;

// The script source code.
//
// Line-by-line explanation:
//   PRINT "Hello, world!"   - string literal argument to PRINT
//   VAR "x"                 - create a variable named "x" initialized to NULL
//                             (uses STRING argument; compare with $-prefixed
//                             SYMBOL syntax used by NEW/DEL below)
//   x = NULL                - explicit NULL assignment (redundant but demonstrates syntax)
//   PRINT x                 - print the current value of x (NULL)
//   PRINT $xyz              - print a symbol literal; $ prefix means "pass the name, not the value"
//   NEW $obj1, 111          - allocate a NativeObject with data=111, store it in variable "obj1"
//   PRINT obj1              - print the object stored in variable obj1
//   DEL $obj1               - free the object and reset variable "obj1" to NULL
//   WAIT 2                  - suspend for 2 seconds (YIELD-based async)
//   PRINT "Done waiting!"   - resumes after the first wait
//   WAIT 2                  - suspend for another 2 seconds
//   PRINT "All done!"       - resumes after the second wait
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
// In a real application these would typically be stored in a context struct
// rather than as globals, but globals keep the example simple.
basic26_Vm *vm = NULL;
basic26_State *state = NULL;
basic26_Script *script = NULL;

// Tracks the remaining sleep time for the WAIT function across yield/resume
// cycles. When the WAIT callback fires, this is set to the requested number of
// seconds. The host run loop then sleeps and calls Vm_run() again.
static double pending_wait_seconds = 0.0;

// --------------------------------------------------------------------------
// VAR function callback
// --------------------------------------------------------------------------
// Declares a variable by name and initializes it to NULL.
//
// Script usage:  VAR "variable_name"
//
// This example uses a STRING argument (not a SYMBOL) for the variable name.
// This works because StringId and SymbolId are fully interchangeable  - they
// share the same interning table. The preferred idiom in modern BASIC26 code
// is to use a SYMBOL argument (e.g. VAR $x) for clarity, but this example
// retains the STRING form to show both approaches for comparison.
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

    // Use the string content as the variable name (symbol ID). Because
    // StringId and SymbolId are fully interchangeable (they share the same
    // interning table), the string ID can be used directly as a symbol ID
    // for variable operations.
    CHECK(basic26_State_set_var(info->state, arg.as.string_id, &(basic26_Value){
                                                                   .type = BASIC26_VALUE_TYPE_NULL,
                                                               }) == BASIC26_RESULT_OK);

    return BASIC26_FUNCTION_RESULT_OK;
}

// --------------------------------------------------------------------------
// PRINT function callback
// --------------------------------------------------------------------------
// Prints a single value of any type to stdout.
//
// Script usage: PRINT value
//
// This callback demonstrates how to handle every value type that the VM
// supports. For STRING and SYMBOL values, the raw bytes are retrieved from
// the VM's string table via basic26_Vm_get_string(). For OBJECT values, the
// callback dereferences the opaque pointer - this is safe because only this
// host application creates objects, so it knows the concrete type.
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

    switch (arg.type)
    {
    case BASIC26_VALUE_TYPE_INT:
        printf("SCRIPT: %d\n", arg.as.int_val);
        break;
    case BASIC26_VALUE_TYPE_FLOAT:
        printf("SCRIPT: %f\n", arg.as.float_val);
        break;
    case BASIC26_VALUE_TYPE_NULL:
        printf("SCRIPT: NULL\n");
        break;
    case BASIC26_VALUE_TYPE_STRING:
        // Retrieve the actual bytes of the interned string from the VM.
        CHECK(basic26_Vm_get_string(vm, arg.as.string_id, &str, &str_len) == BASIC26_RESULT_OK);

        memcpy(buf, str, str_len);
        buf[str_len] = 0;

        printf("SCRIPT: \"%s\"\n", buf);

        break;
    case BASIC26_VALUE_TYPE_SYMBOL:
        // Symbols are also stored in the string table, so the same retrieval
        // function works. We prefix the output with $ to distinguish them from
        // regular strings.
        CHECK(basic26_Vm_get_string(vm, arg.as.symbol_id, &str, &str_len) == BASIC26_RESULT_OK);

        memcpy(buf, str, str_len);
        buf[str_len] = 0;

        printf("SCRIPT: $%s\n", buf);

        break;
    case BASIC26_VALUE_TYPE_OBJECT:
        // OBJECT values hold an opaque pointer set by the host. Here we cast
        // it back to our known NativeObject type. In a real application you
        // might want to store a type tag alongside the pointer for safety.
        const NativeObject *obj = arg.as.object_ptr;

        printf("SCRIPT: (@NativeObject){ .data = %d }\n", obj->data);

        break;
    }

    return BASIC26_FUNCTION_RESULT_OK;
}

// --------------------------------------------------------------------------
// NEW function callback
// --------------------------------------------------------------------------
// Allocates a host-side NativeObject and stores it in a script variable.
//
// Script usage: NEW $variable_name, initial_data
//
// The first argument must be a SYMBOL (e.g. $obj1) specifying which variable
// the new object should be stored in. The second argument must be an INT
// providing the initial value for the object's data field.
//
// This pattern - using a SYMBOL to tell the host *where* to store a result -
// is a common idiom in BASIC26. Since native functions have no return value,
// they need a way to communicate results back to the script, and storing them
// in a named variable is the standard approach.
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
    NativeObject *obj = malloc(sizeof(NativeObject));
    obj->data = argv[1].as.int_val;

    // Store the object pointer as an OBJECT-typed value in the variable named
    // by the symbol argument. The symbol_id doubles as the variable key because
    // strings and symbols share the interning table (StringId and SymbolId are
    // interchangeable).
    CHECK(
        basic26_State_set_var(state, argv[0].as.symbol_id, &(basic26_Value){
                                                               .type = BASIC26_VALUE_TYPE_OBJECT,
                                                               .as = {
                                                                   .object_ptr = obj,
                                                               },
                                                           }) == BASIC26_RESULT_OK);

    return BASIC26_FUNCTION_RESULT_OK;
}

// --------------------------------------------------------------------------
// DEL function callback
// --------------------------------------------------------------------------
// Frees a previously allocated NativeObject and resets the variable to NULL.
//
// Script usage: DEL $variable_name
//
// The argument must be a SYMBOL naming the variable that holds the object to
// delete. This callback looks up the variable, verifies that it holds an
// OBJECT, frees the underlying memory, and then sets the variable to NULL to
// prevent use-after-free.
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

    // Retrieve the current value of the variable to verify it is an OBJECT.
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
    free(target_var.as.object_ptr);

    // Reset the variable to NULL so the script cannot access the freed pointer.
    CHECK(
        basic26_State_set_var(state, argv[0].as.symbol_id, &(basic26_Value){
                                                               .type = BASIC26_VALUE_TYPE_NULL,
                                                           }) == BASIC26_RESULT_OK);

    return BASIC26_FUNCTION_RESULT_OK;
}

// --------------------------------------------------------------------------
// WAIT function callback
// --------------------------------------------------------------------------
// Suspends script execution for a given number of seconds.
//
// Script usage: WAIT seconds
//
// The argument can be an INT (whole seconds) or a FLOAT (fractional seconds).
// This callback returns BASIC26_FUNCTION_RESULT_YIELD, which causes
// basic26_Vm_run() to return BASIC26_RESULT_YIELDED. The host's run loop then
// performs the actual sleep and calls basic26_Vm_run() again to resume.
//
// This demonstrates the YIELD mechanism: native callbacks can suspend execution
// at any point, and the host decides when to resume. In a real event-driven
// application, the host would typically return to its event loop instead of
// blocking on sleep(), and resume the VM when the awaited condition is met
// (e.g. a timer fires, I/O completes, a network response arrives).
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

// --------------------------------------------------------------------------
// print_dump helper
// --------------------------------------------------------------------------
// Uses basic26_Script_dump() to print the compiled bytecode in a human-readable
// format. This is useful for debugging and understanding how the compiler
// translates source lines into opcodes.
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

    // The dump string is allocated by the VM and must be freed using the
    // dedicated basic26_Script_dump_free() function (not regular free()).
    basic26_Script_dump_free(vm, str, str_len);
}

// --------------------------------------------------------------------------
// Helper: convert basic26_RuntimeError to a human-readable string.
// --------------------------------------------------------------------------
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

// --------------------------------------------------------------------------
// cleanup helper
// --------------------------------------------------------------------------
// Destroys all BASIC26 objects in the correct order: Script first, then State,
// then VM. The Script and State depend on the VM, so the VM must be destroyed
// last. Passing NULL handles is safe (no-op) for State and Script destroy.
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

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
// Orchestrates the full BASIC26 lifecycle: create -> compile -> register ->
// run (with yield loop) -> destroy.
int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    // Step 1: Create the VM.
    // Passing NULL for alloc uses the default system allocator (or the debug
    // allocator in debug builds).
    CHECK(basic26_Vm_create(&(basic26_CreateVmOptions){.alloc = NULL}, &vm) == BASIC26_RESULT_OK);

    // Step 2: Create an execution State tied to the VM.
    CHECK(basic26_State_create(&(basic26_CreateStateOptions){.vm = vm}, &state) == BASIC26_RESULT_OK);

    // Step 3: Create a Script object tied to the VM.
    CHECK(basic26_Script_create(vm, &script) == BASIC26_RESULT_OK);

    // Step 4: Compile the source code into bytecode.
    // The limits are set to 0 (unlimited) for this example. In production you
    // may want to set max_opcodes and max_strings to prevent malicious or
    // buggy scripts from consuming too many resources at compile time.
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
    // Function names are looked up in the VM's string interning table. The
    // `create = true` flag means the string is added if it does not already
    // exist. The returned ID can then be used as the `name` field when
    // registering a callback.
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
    // Each callback is associated with a symbol ID (the function name). When
    // the script calls e.g. `PRINT "hello"`, the VM looks up the symbol ID
    // for "PRINT" and invokes the corresponding callback.
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
    //
    // When a native callback returns BASIC26_FUNCTION_RESULT_YIELD, the VM
    // returns BASIC26_RESULT_YIELDED from basic26_Vm_run(). The host can then
    // perform any pending work (in this case, sleeping for the requested
    // duration) and call basic26_Vm_run() again to resume execution from
    // the instruction following the YIELD.
    //
    // The loop continues until the script finishes normally (OK) or a runtime
    // error occurs. If a runtime error occurs, we print the error details
    // (instruction pointer and error code) and exit.
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
                                                   }, &runtime_err_info);

        if (result == BASIC26_RESULT_OK)
        {
            // Script completed successfully.
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
            // A runtime error occurred. Print the details and exit.
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
            // Some other unexpected result (out of memory, out of limits, etc.).
            fprintf(stderr, "UNEXPECTED RESULT: %d\n", (int)result);
            cleanup();
            return 1;
        }
    }

    // Step 8: Clean up all resources.
    cleanup();

    return 0;
}
