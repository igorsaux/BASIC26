// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

#ifndef BASIC26_H
#define BASIC26_H

/**
 * @file basic26.h
 * @brief BASIC26 Virtual Machine API.
 *
 * @details
 * BASIC26 is a lightweight, embeddable scripting language and virtual machine.
 * It exposes a C API so that host applications can compile and execute simple scripts at runtime.
 * The language is line-oriented, inspired by classic BASIC, and is designed for use cases such as
 * game modding, configuration logic, automation rules, and other scenarios where a small, safe,
 * sandboxed scripting layer is desirable.
 *
 * ## Architecture Overview
 *
 * The library is organized around three core objects:
 *
 * - **Vm** (`basic26_Vm`): The top-level virtual machine instance. It owns the string interning
 *   table and the registry of native function callbacks. A single Vm can manage multiple scripts
 *   and states, but must not be shared across threads without external synchronization.
 *
 * - **Script** (`basic26_Script`): A compiled bytecode container. Source text is parsed into an
 *   internal opcode representation. Scripts are created from and tied to a specific Vm. A script
 *   can be compiled once and executed many times with different states.
 *
 * - **State** (`basic26_State`): The runtime execution state, consisting of a value stack, a
 *   variable store, and an instruction pointer (IP). States are created from a Vm and can be
 *   reused across multiple runs or scripts.
 *
 * ## Language Reference
 *
 * The BASIC26 scripting language is line-based: each line contains a single statement. Blank lines
 * and lines starting with `//` are ignored. The supported constructs are:
 *
 * ### Data Types
 * - `INT`     - 64-bit signed integer (e.g. `42`, `-7`)
 * - `FLOAT`   - 64-bit double-precision floating point (e.g. `3.14`, `NAN`, `INF`, `-INF`)
 * - `STRING`  - interned UTF-8 string literal (e.g. `"hello"`)
 * - `SYMBOL`  - identifier literal prefixed with `$` (e.g. `$foo`). Symbols are used to pass
 *               identifier names to native functions without resolving them as variables.
 * - `ADDRESS` - label address literal prefixed with `@` (e.g. `@my_label`). Addresses represent
 *               the instruction pointer (IP) of a label in the compiled bytecode. They are resolved
 *               at compile time and can be used for computed jumps or passing jump targets to native
 *               functions. Addresses support all comparison operators (==, !=, <, >, <=, >=).
 * - `NULL`    - the null value
 * - `OBJECT`  - opaque host pointer, only created and consumed by native callbacks
 *
 * ### Variables and Assignment
 * Variables are NOT created implicitly by the interpreter. All variables must be explicitly
 * created by the host application via `basic26_State_set_var()` before the script references them.
 * The interpreter does not allocate memory for variables on its own - variable creation is entirely
 * the host's responsibility. A script can assign to an existing variable with any value type:
 * @code
 *   x = 10
 *   y = 3.14
 *   name = "Alice"
 * @endcode
 *
 * Attempting to read or assign to a variable that was never created by the host will result in a
 * `BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE` error at runtime.
 *
 * ### Expressions and Operators
 * Expressions support the following operators, listed from lowest to highest precedence:
 *
 * | Precedence | Operator   | Description             | Operand Types    |
 * |:----------:|:----------:|:------------------------|:-----------------|
 * | 9          | OR         | Boolean OR              | INT, INT         |
 * | 8          | AND        | Boolean AND             | INT, INT         |
 * | 7          | == !=      | Equality / Inequality   | same types       |
 * | 7          | < > <= >=  | Ordering comparison     | INT or FLOAT     |
 * | 6          | \| ^ &     | Bitwise OR / XOR / AND  | INT, INT         |
 * | 5          | << >>      | Bit shift left / right  | INT, INT         |
 * | 4          | + -        | Addition / Subtraction  | INT or FLOAT     |
 * | 3          | * / %      | Multiply / Divide / Rem | INT or FLOAT     |
 * | 1          | NOT        | Boolean NOT (unary)     | INT              |
 * | 1          | ~          | Bitwise NOT (unary)     | INT              |
 * | 1          | -          | Unary minus (negation)  | INT or FLOAT     |
 *
 * Parentheses `()` can be used to override precedence. Comparison and boolean operators produce
 * INT values: 1 for true, 0 for false. Type mismatches (e.g. adding INT to FLOAT) result in a
 * runtime error.
 *
 * ### Control Flow
 * - `IF expr ... ELSE ... ENDIF` - conditional branching. The `ELSE` branch is optional.
 *   Multiple conditions can be chained using `ELSE IF expr` (two keywords) or `ELSEIF expr`
 *   (single keyword). Only a single `ENDIF` is required to close the entire chain. Example:
 *   @code
 *     IF x == 1
 *       y = 10
 *     ELSE IF x == 2
 *       y = 20
 *     ELSEIF x == 3
 *       y = 30
 *     ELSE
 *       y = 40
 *     ENDIF
 *   @endcode
 * - `WHILE expr ... ENDWHILE`    - loop that evaluates the condition before each iteration.
 * - `GOTO @label`                 - unconditional jump to a label defined elsewhere as `label:`.
 *
 * ### Function Calls
 * All functions are native callbacks registered by the host application. A call takes the form:
 * @code
 *   FUNC_NAME arg1, arg2, arg3
 * @endcode
 * Arguments are evaluated left-to-right and pushed onto the stack, then the corresponding native
 * callback is invoked. There is no return value mechanism; native functions communicate results
 * back to the script by setting variables via `basic26_State_set_var()`.
 *
 * ### Labels
 * Labels are defined on their own line followed by a colon:
 * @code
 *   my_label:
 * @endcode
 * They are used as targets for `GOTO` statements and `@address` literals. Forward references are
 * allowed; the compiler resolves them after all lines have been parsed.
 *
 * ## Typical Usage Pattern
 *
 * 1. Create a Vm with `basic26_Vm_create()`.
 * 2. Create a State with `basic26_State_create()`.
 * 3. Create a Script with `basic26_Script_create()`.
 * 4. Register native functions with `basic26_Vm_register_function()`.
 * 5. Compile source code with `basic26_Script_compile()`.
 * 6. Create initial variables with `basic26_State_set_var()`. This step is mandatory --
 *    the interpreter does not create variables implicitly.
 * 7. Execute the script with `basic26_Vm_run()`.
 * 8. Read results from variables with `basic26_State_get_var()`.
 * 9. Destroy objects in reverse order when done.
 *
 * ## Sandbox and Safety
 *
 * The VM provides two categories of execution limits via `basic26_RunLimits`:
 * - `max_ops`: caps the number of bytecode instructions executed (0 = unlimited).
 * - `max_time_ns`: caps wall-clock execution time in nanoseconds (0 = unlimited).
 *
 * When a limit is exceeded, `basic26_Vm_run()` returns `BASIC26_RESULT_OUT_OF_LIMITS`. This
 * allows the host to prevent runaway or malicious scripts from consuming unbounded resources.
 *
 * Additionally, a native function callback can return `BASIC26_FUNCTION_RESULT_YIELD` to
 * suspend execution. The host can later resume by calling `basic26_Vm_run()` again; execution
 * picks up at the next instruction. This is useful for implementing asynchronous operations
 * (e.g. waiting for I/O) in an event-driven host.
 *
 * @warning Thread Safety
 * The BASIC26 API is **NOT thread-safe**. There are no internal synchronization or locking
 * mechanisms. A single `basic26_Vm` instance, along with its associated `basic26_State` and
 * `basic26_Script` objects, must only be accessed and manipulated from a single thread at a
 * time. If you need to use the VM across multiple threads, you must provide your own external
 * synchronization (e.g. mutexes) or create separate, isolated VM instances per thread.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(BASIC26_STATIC)
#define BASIC26_API
#elif defined(BASIC26_DYNAMIC)
#define BASIC26_API __declspec(dllimport)
#else
#define BASIC26_API
#endif
#if defined(_M_IX86) || defined(__i386__)
#define BASIC26_API_CALL __cdecl
#else
#define BASIC26_API_CALL
#endif
#else
#if __GNUC__ >= 4 || defined(__clang__)
#define BASIC26_API __attribute__((visibility("default")))
#else
#define BASIC26_API
#endif
#define BASIC26_API_CALL
#endif

#if __has_feature(nullability)
#define BASIC26_NONNULL _Nonnull
#define BASIC26_NULLABLE _Nullable
#else
#define BASIC26_NONNULL
#define BASIC26_NULLABLE
#endif

    /**
     * @brief Maximum number of values that fit on the execution stack.
     *
     * This constant controls the size of the inline stack array inside every
     * basic26_State.
     *
     * @note Setting this too low will cause stack overflow errors during
     *       script execution. Setting it too high wastes memory per State.
     */
#ifndef BASIC26_STACK_CAPACITY
#define BASIC26_STACK_CAPACITY 256
#endif

    /**
     * @brief 64-bit signed integer type used by the VM.
     *
     * All integer literals in BASIC26 scripts and all integer-valued variables
     * are represented using this type. On the C host side it maps to `int64_t`.
     */
    typedef int64_t basic26_IntType;

    /**
     * @brief 64-bit double-precision floating point type used by the VM.
     *
     * Float literals (e.g. `3.14`, `NAN`, `INF`) and float-valued variables
     * use this type. It maps to C `double`.
     */
    typedef double basic26_FloatType;

    /**
     * @brief Opaque handle identifying an interned string within a Vm.
     *
     * String IDs are assigned by the Vm's string interning table. The same
     * string content always maps to the same ID within a Vm instance. Use
     * `basic26_Vm_get_string()` to retrieve the actual bytes from an ID, and
     * `basic26_Vm_get_string_id()` to obtain an ID from a string.
     *
     * @note StringId and SymbolId are **fully interchangeable**. They share the
     * same interning table, so a given character sequence always produces the
     * same numeric ID regardless of which type alias is used. The distinction
     * exists solely for semantic clarity: use StringId when working with string
     * data and SymbolId when working with identifier names. You can safely pass
     * a StringId where a SymbolId is expected and vice versa.
     */
    typedef size_t basic26_StringId;

    /**
     * @brief Opaque handle identifying a symbol (identifier) within a Vm.
     *
     * Symbol IDs are used for variable names and function names. They share the
     * same interning table as StringIds, so a given name always has the same
     * numeric ID within a Vm. This allows fast equality checks by comparing IDs
     * instead of string contents.
     *
     * @note SymbolId and StringId are **fully interchangeable**. See the note on
     * `basic26_StringId` for details.
     */
    typedef size_t basic26_SymbolId;

    /**
     * @brief Result codes returned by most BASIC26 API functions.
     *
     * API calls return one of these codes to indicate the outcome. Check for
     * `BASIC26_RESULT_OK` before using any output parameters. When a run or
     * compile fails, more specific error details can be obtained from the
     * `basic26_RuntimeErrorInfo` or `basic26_CompileErrorInfo` output structs.
     */
    typedef enum basic26_Result
    {
        BASIC26_RESULT_OK = 0,            /**< Operation successful. */
        BASIC26_RESULT_YIELDED = 1,       /**< Execution suspended due to YIELD. */
        BASIC26_RESULT_OUT_OF_MEMORY = 2, /**< Memory allocation failed. */
        BASIC26_RESULT_COMPILE_ERROR = 3, /**< Script compilation failed. */
        BASIC26_RESULT_OUT_OF_LIMITS = 4, /**< Execution exceeded limits (ops/time). */
        BASIC26_RESULT_RUNTIME_ERROR = 5, /**< Runtime error during execution. */
        BASIC26_RESULT_NOT_FOUND = 6,     /**< Requested item (variable, string, label) not found. */
        BASIC26_RESULT_FORCE_32BIT = 0x7FFFFFFF,
    } basic26_Result;

    /**
     * @brief Result codes returned by user-defined native function callbacks.
     *
     * When a native function callback finishes, it returns one of these values
     * to tell the VM how to proceed:
     * - OK:     continue executing the next instruction.
     * - ERROR:  abort the current run with a runtime error.
     * - YIELD:  suspend execution; the host can resume later by calling
     *           `basic26_Vm_run()` again.
     */
    typedef enum basic26_FunctionResult
    {
        BASIC26_FUNCTION_RESULT_OK = 0,    /**< Continue execution normally. */
        BASIC26_FUNCTION_RESULT_ERROR = 1, /**< Abort execution with runtime error. */
        BASIC26_FUNCTION_RESULT_YIELD = 2, /**< Suspend execution (async yield). */
        BASIC26_FUNCTION_RESULT_FORCE_32BIT = 0x7FFFFFFF,
    } basic26_FunctionResult;

    /**
     * @brief Supported value types in the VM.
     *
     * Every `basic26_Value` has a `type` field set to one of these enumerators.
     * The corresponding union member in `basic26_Value.as` should be accessed
     * based on the type. Mixing types (e.g. arithmetic on INT and FLOAT) causes
     * a runtime TypeMismatch error.
     */
    typedef enum basic26_ValueType
    {
        BASIC26_VALUE_TYPE_NULL = 0,    /**< Null value. */
        BASIC26_VALUE_TYPE_INT = 1,     /**< 64-bit signed integer. */
        BASIC26_VALUE_TYPE_FLOAT = 2,   /**< 64-bit floating-point number. */
        BASIC26_VALUE_TYPE_STRING = 3,  /**< String (stored as an ID). */
        BASIC26_VALUE_TYPE_SYMBOL = 4,  /**< Symbol (stored as an ID). */
        BASIC26_VALUE_TYPE_OBJECT = 5,  /**< Object (stored as a pointer). */
        BASIC26_VALUE_TYPE_ADDRESS = 6, /**< Label address (stored as an IP). */
        BASIC26_VALUE_TYPE_FORCE_32BIT = 0x7FFFFFFF,
    } basic26_ValueType;

    /**
     * @brief Represents a dynamically-typed value in the VM.
     *
     * A Value is a tagged union: the `type` field determines which member of
     * the `as` union is valid. Values flow through the execution stack during
     * script evaluation and are passed to/from native function callbacks.
     *
     * When constructing a Value from C, set `type` and the appropriate `as`
     * member. For example:
     * @code
     *   basic26_Value v;
     *   v.type = BASIC26_VALUE_TYPE_INT;
     *   v.as.int_val = 42;
     * @endcode
     */
    typedef struct basic26_Value
    {
        basic26_ValueType type; /**< Type of the value. */
        union
        {
            basic26_IntType int_val;           /**< Integer value (if type is INT). */
            basic26_FloatType float_val;       /**< Float value (if type is FLOAT). */
            basic26_StringId string_id;        /**< String ID (if type is STRING). */
            basic26_SymbolId symbol_id;        /**< Symbol ID (if type is SYMBOL). */
            void *BASIC26_NULLABLE object_ptr; /**< Object pointer (if type is OBJECT). */
            size_t address_val;                /**< Address value / label IP (if type is ADDRESS). */
        } as;                                  /**< Union holding the actual value. */
    } basic26_Value;

    /**
     * @brief Opaque handle to a Virtual Machine instance.
     *
     * Created by `basic26_Vm_create()`, destroyed by `basic26_Vm_destroy()`.
     * See the Architecture Overview in the file-level documentation for details.
     */
    typedef struct basic26_Vm basic26_Vm;

    /**
     * @brief Opaque handle to an execution State.
     *
     * Created by `basic26_State_create()`, destroyed by `basic26_State_destroy()`.
     * A State holds the stack, variables, and instruction pointer for a single
     * execution context.
     */
    typedef struct basic26_State basic26_State;

    /**
     * @brief Opaque handle to a compiled Script.
     *
     * Created by `basic26_Script_create()`, destroyed by `basic26_Script_destroy()`.
     * A Script holds the compiled bytecode, label table, and string references.
     */
    typedef struct basic26_Script basic26_Script;

    /**
     * @brief Context information passed to native function callbacks.
     *
     * When the VM calls a native function, it fills in this struct so the
     * callback can access the VM, State, Script, and any userdata that was
     * passed to `basic26_Vm_run()`. This enables the callback to read/write
     * variables, look up strings, or interact with the host application.
     */
    typedef struct basic26_CallInfo
    {
        basic26_Vm *BASIC26_NONNULL vm;               /**< [in] The VM instance. */
        basic26_State *BASIC26_NONNULL state;         /**< [in] The current execution state. */
        const basic26_Script *BASIC26_NONNULL script; /**< [in] The script being executed. */
        void *BASIC26_NULLABLE userdata;              /**< [in] User-provided context. */
        basic26_SymbolId function_name;               /**< [in] Symbol ID of the called function. */
    } basic26_CallInfo;

    /**
     * @brief Callback signature for native functions registered with the VM.
     *
     * When a script calls a registered function name, the VM pops the argument
     * count and the arguments themselves from the stack and invokes the
     * corresponding callback. The callback receives a `basic26_CallInfo` pointer
     * for context, the number of arguments, and an array of `basic26_Value`
     * items.
     *
     * The callback can inspect arguments, set variables via
     * `basic26_State_set_var()`, read variables via `basic26_State_get_var()`,
     * or perform any host-side operation. It must return one of the
     * `basic26_FunctionResult` codes.
     *
     * @note There is no built-in "return value" mechanism. To communicate a
     *       result back to the script, the callback should set a variable that
     *       the script can then read.
     *
     * @param [in] info    Call context (VM, State, Script, userdata).
     * @param [in] argc    Number of arguments passed by the script.
     * @param [in] argv    Array of argument values. May be NULL if argc is 0.
     * @return Execution status: OK to continue, ERROR to abort, YIELD to suspend.
     */
    typedef basic26_FunctionResult(BASIC26_API_CALL *basic26_function_callback)(const basic26_CallInfo *BASIC26_NONNULL info, size_t argc, const basic26_Value *BASIC26_NULLABLE argv);

    /**
     * @brief Custom memory allocation callback.
     *
     * If provided during VM creation, all internal allocations will go through
     * this function instead of the default allocator. This allows the host to
     * use a custom memory pool, track allocations, or enforce memory budgets.
     *
     * @param [in] userdata  User-provided context pointer.
     * @param [in] len       Number of bytes to allocate.
     * @param [in] alignment Required alignment in bytes.
     * @return Pointer to allocated memory, or NULL on failure.
     */
    typedef void *BASIC26_NULLABLE(BASIC26_API_CALL *basic26_alloc)(void *BASIC26_NULLABLE userdata, size_t len, size_t alignment);

    /**
     * @brief Custom memory deallocation callback.
     *
     * Paired with `basic26_alloc`. Called to release memory previously allocated
     * by the corresponding allocation callback.
     *
     * @param [in] userdata  User-provided context pointer.
     * @param [in] ptr       Pointer to memory to free. May be NULL.
     * @param [in] len       Size of the allocation in bytes (as passed to alloc).
     * @param [in] alignment Alignment of the allocation in bytes.
     */
    typedef void(BASIC26_API_CALL *basic26_free)(void *BASIC26_NULLABLE userdata, void *BASIC26_NULLABLE ptr, size_t len, size_t alignment);

    /**
     * @brief Pair of custom memory allocation callbacks for the VM.
     *
     * Pass a pointer to this struct in `basic26_CreateVmOptions.alloc` to
     * override the default allocator. Both `alloc` and `free` must be non-NULL
     * if the struct is provided.
     */
    typedef struct basic26_AllocCallbacks
    {
        void *BASIC26_NULLABLE userdata;     /**< [in] User context for allocator. */
        basic26_alloc BASIC26_NONNULL alloc; /**< [in] Allocation function. */
        basic26_free BASIC26_NONNULL free;   /**< [in] Free function. */
    } basic26_AllocCallbacks;

    /**
     * @brief Options for creating a VM instance.
     *
     * Initialize with `basic26_CreateVmOptions_zeroed()` to get default values,
     * then override fields as needed.
     */
    typedef struct basic26_CreateVmOptions
    {
        const basic26_AllocCallbacks *BASIC26_NULLABLE alloc; /**< [in] Custom allocator. NULL for default (or debug) allocator. */
    } basic26_CreateVmOptions;

    /**
     * @brief Returns a zeroed CreateVmOptions struct with default values.
     */
    BASIC26_API basic26_CreateVmOptions BASIC26_API_CALL basic26_CreateVmOptions_zeroed(void);

    /**
     * @brief Creates a new Virtual Machine instance.
     *
     * @param [in]  options  Creation options.
     * @param [out] out      Receives the VM handle.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Vm_create(const basic26_CreateVmOptions *BASIC26_NONNULL options, basic26_Vm * BASIC26_NULLABLE * BASIC26_NONNULL out);

    /**
     * @brief Destroys a Virtual Machine instance.
     *
     * @param [in] vm  The VM to destroy. May be NULL (no-op).
     */
    BASIC26_API void BASIC26_API_CALL basic26_Vm_destroy(basic26_Vm *BASIC26_NULLABLE vm);

    /**
     * @brief Options for clearing VM resources.
     *
     * @note The `_clear` functions do NOT free memory. They only reset the internal
     * data structures (hash maps, lists, etc.) to an empty state while retaining
     * their previously allocated capacity. This design enables maximum memory reuse
     * and avoids unnecessary allocation/deallocation cycles and fragmentation. Any
     * cleared object can be reused without having to recreate it from scratch.
     *
     * @warning Clearing interned strings (`clear_strings = true`) or script labels
     * can break the interpreter. If a compiled script or a running state still
     * references string IDs or label names that have been cleared, subsequent
     * operations will likely produce `BASIC26_RESULT_RUNTIME_ERROR` results (e.g.
     * undefined variable, undefined function). The interpreter will not crash, but
     * the script will not run correctly. Only clear strings when you are certain no
     * active script or state depends on them.
     */
    typedef struct basic26_ClearVmOptions
    {
        bool clear_strings;   /**< [in] If true, clears all interned strings. */
        bool clear_functions; /**< [in] If true, unregisters all native functions. */
    } basic26_ClearVmOptions;

    /**
     * @brief Returns a zeroed ClearVmOptions struct with default values.
     */
    BASIC26_API basic26_ClearVmOptions BASIC26_API_CALL basic26_ClearVmOptions_zeroed(void);

    /**
     * @brief Clears VM resources based on provided options.
     *
     * @param [in] vm      The VM instance.
     * @param [in] options Clearing options.
     */
    BASIC26_API void BASIC26_API_CALL basic26_Vm_clear(basic26_Vm *BASIC26_NONNULL vm, const basic26_ClearVmOptions *BASIC26_NONNULL options);

    /**
     * @brief Retrieves a string by its ID.
     *
     * @param [in]  vm        The VM instance.
     * @param [in]  string_id The ID of the string.
     * @param [out] out       Receives a pointer to the string data.
     * @param [out] out_len   Receives the length of the string in bytes.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if string_id is invalid.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Vm_get_string(const basic26_Vm *BASIC26_NONNULL vm, basic26_StringId string_id, const uint8_t *BASIC26_NONNULL *BASIC26_NONNULL out, size_t *BASIC26_NONNULL out_len);

    /**
     * @brief Retrieves or creates a string ID for a given string.
     *
     * @param [in]  vm         The VM instance.
     * @param [in]  string     Pointer to the string data.
     * @param [in]  string_len Length of the string in bytes.
     * @param [in]  create     If true, creates the string if it doesn't exist.
     * @param [out] out        Receives the string ID.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if not found and create is false,
     *         BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Vm_get_string_id(basic26_Vm *BASIC26_NONNULL vm, const uint8_t *BASIC26_NONNULL string, size_t string_len, bool create, basic26_StringId *BASIC26_NONNULL out);

    /**
     * @brief Options for registering a native function.
     */
    typedef struct basic26_RegisterFunctionOptions
    {
        basic26_SymbolId name;                              /**< [in] The symbol ID representing the function name. */
        basic26_function_callback BASIC26_NONNULL callback; /**< [in] Callback function. */
    } basic26_RegisterFunctionOptions;

    /**
     * @brief Returns a zeroed RegisterFunctionOptions struct with default values.
     */
    BASIC26_API basic26_RegisterFunctionOptions BASIC26_API_CALL basic26_RegisterFunctionOptions_zeroed(void);

    /**
     * @brief Registers a native function in the VM.
     *
     * @param [in] vm      The VM instance.
     * @param [in] options Registration options.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Vm_register_function(basic26_Vm *BASIC26_NONNULL vm, const basic26_RegisterFunctionOptions *BASIC26_NONNULL options);

    /**
     * @brief Unregisters a previously registered native function.
     *
     * @param [in] vm        The VM instance.
     * @param [in] symbol_id The symbol ID of the function to unregister.
     */
    BASIC26_API void BASIC26_API_CALL basic26_Vm_unregister_function(basic26_Vm *BASIC26_NONNULL vm, basic26_SymbolId symbol_id);

    /**
     * @brief Execution limits for a script run.
     *
     * `max_ops` and `max_time_ns` default to 0 (unlimited). Set one or both to
     * enforce a cap on resource consumption during `basic26_Vm_run()`. If any
     * limit is exceeded, the run returns `BASIC26_RESULT_OUT_OF_LIMITS`.
     *
     * When `max_time_ns` is set, the VM checks elapsed wall-clock time every
     * `time_check_interval` opcodes rather than on every single opcode, reducing
     * the overhead of frequent system clock queries. Time is always checked
     * immediately after a CALL opcode completes, regardless of the interval, so
     * that long-running native callbacks cannot bypass the time limit.
     */
    typedef struct basic26_RunLimits
    {
        size_t max_ops;             /**< [in] Maximum opcodes to execute. 0 = unlimited. */
        uint64_t max_time_ns;       /**< [in] Maximum execution time in nanoseconds. 0 = unlimited. */
        size_t time_check_interval; /**< [in] Opcodes between elapsed-time checks. 0 = every opcode. */
    } basic26_RunLimits;

    /**
     * @brief Returns a zeroed RunLimits struct with default values.
     */
    BASIC26_API basic26_RunLimits BASIC26_API_CALL basic26_RunLimits_zeroed(void);

    /**
     * @brief Represents specific runtime errors caught during script execution.
     *
     * These errors are returned via `basic26_RuntimeErrorInfo.code` when
     * `basic26_Vm_run()` returns `BASIC26_RESULT_RUNTIME_ERROR`.
     */
    typedef enum basic26_RuntimeError
    {
        BASIC26_RUNTIME_ERROR_UNKNOWN = 0,            /**< Unknown or unclassified runtime error. */
        BASIC26_RUNTIME_ERROR_DIVISION_BY_ZERO = 1,   /**< Division or modulo by zero. */
        BASIC26_RUNTIME_ERROR_TYPE_MISMATCH = 2,      /**< Operand type mismatch. */
        BASIC26_RUNTIME_ERROR_STACK_UNDERFLOW = 3,    /**< Stack underflow (not enough elements). */
        BASIC26_RUNTIME_ERROR_STACK_OVERFLOW = 4,     /**< Stack overflow. */
        BASIC26_RUNTIME_ERROR_UNKNOWN_OPCODE = 5,     /**< Unknown opcode encountered. */
        BASIC26_RUNTIME_ERROR_UNDEFINED_FUNCTION = 6, /**< Call to an undefined function. */
        BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE = 7, /**< Access to an undefined variable. */
        BASIC26_RUNTIME_ERROR_INVALID_BIT_SHIFT = 8,  /**< Invalid bit shift (negative or >= 64). */
        BASIC26_RUNTIME_ERROR_FUNCTION = 9,           /**< An error originated from a function. */
        BASIC26_RUNTIME_ERROR_FORCE_32BIT = 0x7FFFFFFF,
    } basic26_RuntimeError;

    /**
     * @brief Detailed information about a runtime error.
     */
    typedef struct basic26_RuntimeErrorInfo
    {
        size_t ip;                 /**< [out] The Instruction Pointer where the error occurred. */
        basic26_RuntimeError code; /**< [out] Detailed runtime error code. */
    } basic26_RuntimeErrorInfo;

    /**
     * @brief Returns a zeroed RuntimeErrorInfo struct with default values.
     */
    BASIC26_API basic26_RuntimeErrorInfo BASIC26_API_CALL basic26_RuntimeErrorInfo_zeroed(void);

    /**
     * @brief Options for executing a script.
     */
    typedef struct basic26_RunOptions
    {
        basic26_State *BASIC26_NONNULL state;            /**< [in] Execution state. */
        const basic26_Script *BASIC26_NONNULL script;    /**< [in] Script to run. */
        const basic26_RunLimits *BASIC26_NONNULL limits; /**< [in] Execution limits. */
        void *BASIC26_NULLABLE userdata;                 /**< [in] User context passed to callbacks. */
    } basic26_RunOptions;

    /**
     * @brief Returns a zeroed RunOptions struct with default values.
     */
    BASIC26_API basic26_RunOptions BASIC26_API_CALL basic26_RunOptions_zeroed(void);

    /**
     * @brief Executes a script.
     *
     * @param [in] vm          The VM instance.
     * @param [in] options     Execution options.
     * @param [out] error_out  Receives runtime error info.
     * @return BASIC26_RESULT_OK on completion, BASIC26_RESULT_YIELDED if suspended,
     *         BASIC26_RESULT_OUT_OF_LIMITS if limits exceeded, or BASIC26_RESULT_RUNTIME_ERROR on error.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Vm_run(basic26_Vm *BASIC26_NONNULL vm, const basic26_RunOptions *BASIC26_NONNULL options, basic26_RuntimeErrorInfo *BASIC26_NONNULL error_out);

    /**
     * @brief Options for creating an execution state.
     */
    typedef struct basic26_CreateStateOptions
    {
        basic26_Vm *BASIC26_NONNULL vm; /**< [in] The VM instance. */
    } basic26_CreateStateOptions;

    /**
     * @brief Returns a zeroed CreateStateOptions struct with default values.
     */
    BASIC26_API basic26_CreateStateOptions BASIC26_API_CALL basic26_CreateStateOptions_zeroed(void);

    /**
     * @brief Creates a new execution state.
     *
     * @param [in]  options  Creation options.
     * @param [out] out      Receives the state handle.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_State_create(const basic26_CreateStateOptions *BASIC26_NONNULL options, basic26_State * BASIC26_NULLABLE * BASIC26_NONNULL out);

    /**
     * @brief Destroys an execution state.
     *
     * @param [in] state  The state to destroy. May be NULL (no-op).
     * @param [in] vm     The VM instance.
     */
    BASIC26_API void BASIC26_API_CALL basic26_State_destroy(basic26_State *BASIC26_NULLABLE state, basic26_Vm *BASIC26_NONNULL vm);

    /**
     * @brief Options for clearing execution state.
     *
     * @note The `_clear` functions do NOT free memory. They only reset the internal
     * data structures while retaining their allocated capacity. See the note on
     * `basic26_ClearVmOptions` for the rationale.
     */
    typedef struct basic26_ClearStateOptions
    {
        bool clear_stack; /**< [in] If true, clears the execution stack. */
        bool clear_vars;  /**< [in] If true, clears all defined variables. */
    } basic26_ClearStateOptions;

    /**
     * @brief Returns a zeroed ClearStateOptions struct with default values.
     */
    BASIC26_API basic26_ClearStateOptions BASIC26_API_CALL basic26_ClearStateOptions_zeroed(void);

    /**
     * @brief Clears execution state resources based on provided options.
     *
     * @param [in] state   The state instance.
     * @param [in] options Clearing options.
     */
    BASIC26_API void BASIC26_API_CALL basic26_State_clear(basic26_State *BASIC26_NONNULL state, const basic26_ClearStateOptions *BASIC26_NONNULL options);

    /**
     * @brief Gets the current instruction pointer (IP).
     *
     * @param [in]  state The state instance.
     * @param [out] out   Receives the current IP.
     */
    BASIC26_API void BASIC26_API_CALL basic26_State_get_ip(const basic26_State *BASIC26_NONNULL state, size_t *BASIC26_NONNULL out);

    /**
     * @brief Sets the instruction pointer (IP). Used for GOTO/GOSUB.
     *
     * @param [in] state  The state instance.
     * @param [in] ip     The new IP value.
     */
    BASIC26_API void BASIC26_API_CALL basic26_State_set_ip(basic26_State *BASIC26_NONNULL state, size_t ip);

    /**
     * @brief Returns the stack capacity.
     *
     * This is the value of BASIC26_STACK_CAPACITY that the library was
     * compiled with. It is useful for applications that link against the
     * dynamic library and cannot inspect the compile-time constant directly.
     *
     * @return The stack capacity (number of slots).
     */
    BASIC26_API size_t BASIC26_API_CALL basic26_State_get_stack_capacity();

    /**
     * @brief Reads a variable value.
     *
     * @param [in]  state     The state instance.
     * @param [in]  symbol_id The symbol ID of the variable.
     * @param [out] out       Receives the value.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if variable is not defined.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_State_get_var(const basic26_State *BASIC26_NONNULL state, basic26_SymbolId symbol_id, basic26_Value *BASIC26_NONNULL out);

    /**
     * @brief Writes a variable value. Creates the variable if it doesn't exist yet.
     *
     * This is the primary mechanism for creating variables. The interpreter does not
     * create variables implicitly on assignment - the host must create them via this
     * function before the script attempts to read or write them.
     *
     * @param [in] state     The state instance.
     * @param [in] symbol_id The symbol ID of the variable.
     * @param [in] value     The value to write.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_State_set_var(basic26_State *BASIC26_NONNULL state, basic26_SymbolId symbol_id, const basic26_Value *BASIC26_NONNULL value);

    /**
     * @brief Unsets a variable.
     *
     * This function makes the variable undefined like it does not exist.
     *
     * @param [in] state The state instance.
     * @param [in] symbol_id The symbol ID of the variable.
     */
    BASIC26_API void BASIC26_API_CALL basic26_State_unset_var(basic26_State *BASIC26_NONNULL state, basic26_SymbolId symbol_id);

    /**
     * @brief Iterates over the defined variables in a state.
     *
     * Returns the variables defined in the state one by one, in ascending
     * order of symbol id.
     *
     * To start a new iteration, pass NULL for `prev`. The function then writes
     * the id of the first defined variable to `it`. To advance the iterator,
     * pass `&it` (a pointer to the id returned by the previous call) on
     * subsequent calls.
     *
     * Typical usage:
     * @code
     *   const basic26_SymbolId *prev = NULL;
     *   basic26_SymbolId it = 0;
     *   while (basic26_State_var_next(state, prev, &it)) {
     *       prev = &it;
     *       // use 'it' here
     *   }
     * @endcode
     *
     * @param [in]  state  The state instance.
     * @param [in]  prev   Pointer to the id returned by the previous call, or
     *                     NULL to start a new iteration from the beginning.
     * @param [out] it     Receives the id of the next defined variable.
     * @return true if a defined variable was written to `it`, false if the
     *         iteration has no more variables to yield.
     */
    BASIC26_API bool BASIC26_API_CALL basic26_State_var_next(const basic26_State *BASIC26_NONNULL state, const basic26_SymbolId *BASIC26_NULLABLE prev, basic26_SymbolId *BASIC26_NONNULL it);

    /**
     * @brief Creates a new, empty compiled script instance.
     *
     * @param [in]  vm  The VM instance.
     * @param [out] out Receives the script handle.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_create(basic26_Vm *BASIC26_NONNULL vm, basic26_Script * BASIC26_NULLABLE * BASIC26_NONNULL out);

    /**
     * @brief Destroys a compiled script.
     *
     * @param [in] script  The script to destroy. May be NULL (no-op).
     * @param [in] vm      The VM instance.
     */
    BASIC26_API void BASIC26_API_CALL basic26_Script_destroy(basic26_Script *BASIC26_NULLABLE script, basic26_Vm *BASIC26_NONNULL vm);

    /**
     * @brief Options for clearing script resources.
     *
     * @note The `_clear` functions do NOT free memory. They only reset the internal
     * data structures while retaining their allocated capacity. See the note on
     * `basic26_ClearVmOptions` for the rationale.
     *
     * @warning Clearing labels (`clear_labels = true`) while a state still references
     * those labels (e.g. via a pending GOTO) can cause runtime errors when the script
     * is executed again.
     */
    typedef struct basic26_ClearScriptOptions
    {
        bool clear_ops;    /**< [in] If true, clears the compiled opcodes. */
        bool clear_labels; /**< [in] If true, clears all defined labels. */
    } basic26_ClearScriptOptions;

    /**
     * @brief Returns a zeroed ClearScriptOptions struct with default values.
     */
    BASIC26_API basic26_ClearScriptOptions BASIC26_API_CALL basic26_ClearScriptOptions_zeroed(void);

    /**
     * @brief Clears script resources based on provided options.
     *
     * @param [in] script  The script instance.
     * @param [in] options Clearing options.
     */
    BASIC26_API void BASIC26_API_CALL basic26_Script_clear(basic26_Script *BASIC26_NONNULL script, const basic26_ClearScriptOptions *BASIC26_NONNULL options);

    /**
     * @brief Compilation limits for a script.
     *
     * Both fields default to 0 (unlimited). Set one or both to enforce a cap
     * during `basic26_Script_compile()`. Exceeding a limit causes the compile
     * to fail with an appropriate error code.
     */
    typedef struct basic26_ScriptLimits
    {
        size_t max_opcodes; /**< [in] Maximum opcodes per script. 0 = unlimited. */
        size_t max_strings; /**< [in] Maximum unique strings. 0 = unlimited. */
    } basic26_ScriptLimits;

    /**
     * @brief Returns a zeroed ScriptLimits struct with default values.
     */
    BASIC26_API basic26_ScriptLimits BASIC26_API_CALL basic26_ScriptLimits_zeroed(void);

    /**
     * @brief Represents specific errors caught during script compilation.
     */
    typedef enum basic26_CompileError
    {
        BASIC26_COMPILE_ERROR_UNDEFINED = 0,          /**< Undefined or unclassified compile error. */
        BASIC26_COMPILE_ERROR_EXPECTED_OP = 1,        /**< Expected an operator. */
        BASIC26_COMPILE_ERROR_UNKNOWN_OP = 2,         /**< Unknown operator encountered. */
        BASIC26_COMPILE_ERROR_BAD_OP_ARGS = 3,        /**< Invalid operator arguments. */
        BASIC26_COMPILE_ERROR_BAD_STRING_LITERAL = 4, /**< Malformed string literal. */
        BASIC26_COMPILE_ERROR_BAD_SYMBOL_LITERAL = 5, /**< Malformed symbol/identifier literal. */
        BASIC26_COMPILE_ERROR_BAD_NUMBER_LITERAL = 6, /**< Malformed number literal. */
        BASIC26_COMPILE_ERROR_TOO_BIG_INT = 7,        /**< Integer literal too large. */
        BASIC26_COMPILE_ERROR_SYNTAX = 8,             /**< General syntax error. */
        BASIC26_COMPILE_ERROR_UNKNOWN_LABEL = 9,      /**< Reference to undefined label. */
        BASIC26_COMPILE_ERROR_FORCE_32BIT = 0x7FFFFFFF,
    } basic26_CompileError;

    /**
     * @brief Detailed information about a compilation error.
     */
    typedef struct basic26_CompileErrorInfo
    {
        size_t pos;                /**< [out] Byte offset in source where the error occurred. */
        basic26_CompileError code; /**< [out] Compilation error code. */
    } basic26_CompileErrorInfo;

    /**
     * @brief Returns a zeroed CompileErrorInfo struct with default values.
     */
    BASIC26_API basic26_CompileErrorInfo BASIC26_API_CALL basic26_CompileErrorInfo_zeroed(void);

    /**
     * @brief Opaque handle to debug information for a compiled script.
     *
     * Created by `basic26_DebugInfo_create()`, destroyed by `basic26_DebugInfo_destroy()`.
     * A DebugInfo instance holds the source mapping (instruction pointer to source byte offset).
     * It is populated during compilation if passed via `basic26_CompileOptions.debug_info`.
     */
    typedef struct basic26_DebugInfo basic26_DebugInfo;

    /**
     * @brief Options for compiling a script.
     */
    typedef struct basic26_CompileOptions
    {
        basic26_Vm *BASIC26_NONNULL vm;                     /**< [in] The VM instance. */
        const uint8_t *BASIC26_NONNULL source;              /**< [in] Source code string. */
        size_t source_len;                                  /**< [in] Length of source code in bytes. */
        const basic26_ScriptLimits *BASIC26_NONNULL limits; /**< [in] Compilation limits. */
        basic26_DebugInfo *BASIC26_NULLABLE debug_info;     /**< [in] Debug info instance to populate with source positions. If NULL, source positions are not recorded. */
    } basic26_CompileOptions;

    /**
     * @brief Returns a zeroed CompileOptions struct with default values.
     */
    BASIC26_API basic26_CompileOptions BASIC26_API_CALL basic26_CompileOptions_zeroed(void);

    /**
     * @brief Compiles source code into a script.
     *
     * @param [in]  script     The script instance to compile into.
     * @param [in]  options    Compilation options.
     * @param [out] error_out  Receives error info on failure.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails,
     *         BASIC26_RESULT_COMPILE_ERROR on syntax or semantic errors.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_compile(basic26_Script *BASIC26_NONNULL script, const basic26_CompileOptions *BASIC26_NONNULL options, basic26_CompileErrorInfo *BASIC26_NONNULL error_out);

    /**
     * @brief Bytecode opcodes used by the VM.
     *
     * Each instruction in a compiled script has a code field set to one of
     * these values. The opcode determines which union member of
     * `basic26_Immediate` is valid in the accompanying `imm` field.
     *
     * Opcodes that carry an immediate value use the `imm` field as follows:
     *
     * | Opcode                | `imm` member  | Description                        |
     * |:----------------------|:-------------|:------------------------------------|
     * | PUSH_INT              | as_int       | Push an integer literal.            |
     * | PUSH_FLOAT            | as_float     | Push a float literal.               |
     * | PUSH_STRING           | as_string    | Push an interned string by ID.      |
     * | PUSH_SYMBOL           | as_symbol    | Push a symbol by ID.                |
     * | PUSH_ADDRESS          | as_address   | Push a label address (IP).          |
     * | PUSH_NULL             | (unused)     | Push null.                          |
     * | LOAD                  | as_symbol    | Load a variable by symbol ID.       |
     * | STORE                 | as_symbol    | Store to a variable by symbol ID.   |
     * | ADD..REM, NEG         | (unused)     | Arithmetic / unary ops.             |
     * | EQ..GTE               | (unused)     | Comparison ops.                     |
     * | BOOL_AND..BOOL_NOT    | (unused)     | Boolean logic ops.                  |
     * | BIT_AND..BIT_NOT      | (unused)     | Bitwise ops.                        |
     * | SHL, SHR              | (unused)     | Bit shift ops.                      |
     * | JUMP                  | as_address   | Unconditional jump to IP.           |
     * | JUMP_IF_FALSE         | as_address   | Jump to IP if top-of-stack is falsy.|
     * | CALL                  | as_symbol    | Call native function by symbol ID.  |
     * | POP                   | (unused)     | Discard top-of-stack.               |
     */
    typedef enum basic26_Opcode
    {
        BASIC26_OPCODE_UNDEFINED = 0,
        BASIC26_OPCODE_PUSH_INT = 1,
        BASIC26_OPCODE_PUSH_FLOAT = 2,
        BASIC26_OPCODE_PUSH_STRING = 3,
        BASIC26_OPCODE_PUSH_SYMBOL = 4,
        BASIC26_OPCODE_PUSH_ADDRESS = 5,
        BASIC26_OPCODE_PUSH_NULL = 6,
        BASIC26_OPCODE_LOAD = 7,
        BASIC26_OPCODE_STORE = 8,
        BASIC26_OPCODE_ADD = 9,
        BASIC26_OPCODE_SUB = 10,
        BASIC26_OPCODE_MUL = 11,
        BASIC26_OPCODE_DIV = 12,
        BASIC26_OPCODE_REM = 13,
        BASIC26_OPCODE_NEG = 14,
        BASIC26_OPCODE_EQ = 15,
        BASIC26_OPCODE_NEQ = 16,
        BASIC26_OPCODE_LT = 17,
        BASIC26_OPCODE_GT = 18,
        BASIC26_OPCODE_LTE = 19,
        BASIC26_OPCODE_GTE = 20,
        BASIC26_OPCODE_BOOL_AND = 21,
        BASIC26_OPCODE_BOOL_OR = 22,
        BASIC26_OPCODE_BOOL_NOT = 23,
        BASIC26_OPCODE_BIT_AND = 24,
        BASIC26_OPCODE_BIT_OR = 25,
        BASIC26_OPCODE_BIT_XOR = 26,
        BASIC26_OPCODE_BIT_NOT = 27,
        BASIC26_OPCODE_SHL = 28,
        BASIC26_OPCODE_SHR = 29,
        BASIC26_OPCODE_JUMP = 30,
        BASIC26_OPCODE_JUMP_IF_FALSE = 31,
        BASIC26_OPCODE_CALL = 32,
        BASIC26_OPCODE_POP = 33,
    } basic26_Opcode;

    /**
     * @brief Immediate value union for a bytecode instruction.
     *
     * Which member is valid depends on the `code` field of the containing
     * `basic26_Op`. See `basic26_Opcode` documentation for the mapping.
     */
    typedef union basic26_Immediate
    {
        basic26_IntType as_int;
        basic26_FloatType as_float;
        basic26_StringId as_string;
        basic26_SymbolId as_symbol;
        size_t as_address;
    } basic26_Immediate;

    /**
     * @brief A single bytecode instruction.
     *
     * A script is a sequence of these structures. The `code` field selects
     * the opcode and determines which member of `imm` is meaningful.
     */
    typedef struct basic26_Op
    {
        uint8_t code;
        basic26_Immediate imm;
    } basic26_Op;

    /**
     * @brief Reads a single bytecode instruction from a compiled script.
     *
     * @param [in]  script  The script instance.
     * @param [in]  pos     Zero-based index of the instruction to read.
     * @param [out] out     Receives the instruction.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if pos is out of range.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_get_op(const basic26_Script *BASIC26_NONNULL script, size_t pos, basic26_Op *BASIC26_NONNULL out);

    /**
     * @brief Overwrites a single bytecode instruction in a compiled script.
     *
     * This allows the host to patch instructions after compilation, for
     * example to modify jump targets or immediate values.
     *
     * @param [in] script  The script instance.
     * @param [in] pos     Zero-based index of the instruction to overwrite.
     * @param [in] op      The new instruction value.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if pos is out of range.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_set_op(basic26_Script *BASIC26_NONNULL script, size_t pos, basic26_Op op);

    /**
     * @brief Appends a bytecode instruction to the end of a compiled script.
     *
     * This allows the host to extend a script with additional instructions.
     *
     * @param [in] script  The script instance.
     * @param [in] vm      The VM instance (used for memory allocation).
     * @param [in] op      The instruction to append.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_push_op(basic26_Script *BASIC26_NONNULL script, basic26_Vm *BASIC26_NONNULL vm, basic26_Op op);

    /**
     * @brief Inserts a bytecode instruction at a specific position in a compiled script.
     *
     * Shifts existing instructions at or after `pos` to the right by one.
     *
     * @warning Inserting instructions changes the zero-based indices of all
     *          subsequent instructions. Any immediate address values (e.g., in JUMP
     *          or JUMP_IF_FALSE opcodes) or labels that refer to shifted indices
     *          will become invalid and must be updated manually.
     *
     * @param [in] script  The script instance.
     * @param [in] vm      The VM instance (used for memory allocation).
     * @param [in] pos     Zero-based index where the instruction will be inserted.
     *                     Can be equal to the current instruction count to append.
     * @param [in] op      The instruction to insert.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if pos is out of range,
     *         BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_insert_op(basic26_Script *BASIC26_NONNULL script, basic26_Vm *BASIC26_NONNULL vm, size_t pos, basic26_Op op);

    /**
     * @brief Removes the last bytecode instruction from a compiled script.
     *
     * @param [in]  script  The script instance.
     * @param [out] out     Receives the removed instruction. May be NULL if the
     *                      removed value is not needed.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if the script has no instructions.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_pop_op(basic26_Script *BASIC26_NONNULL script, basic26_Op *BASIC26_NULLABLE out);

    /**
     * @brief Removes a bytecode instruction at a specific position in a compiled script.
     *
     * Shifts all instructions after `pos` to the left by one, preserving their order.
     *
     * @warning Removing an instruction changes the zero-based indices of all
     *          subsequent instructions. Any immediate address values (e.g., in JUMP
     *          or JUMP_IF_FALSE opcodes) or labels that refer to shifted indices
     *          will become invalid and must be updated manually.
     *
     * @param [in]  script  The script instance.
     * @param [in]  pos     Zero-based index of the instruction to remove.
     * @param [out] op      Receives the removed instruction. May be NULL if the
     *                      removed value is not needed.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if pos is out of range
     *         or the script has no instructions.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_remove_op(basic26_Script *BASIC26_NONNULL script, size_t pos, basic26_Op *BASIC26_NULLABLE op);

    /**
     * @brief Returns the number of bytecode instructions in a compiled script.
     *
     * @param [in] script The script instance.
     * @return The number of instructions.
     */
    BASIC26_API size_t BASIC26_API_CALL basic26_Script_count_ops(const basic26_Script *BASIC26_NONNULL script);

    /**
     * @brief Gets the Instruction Pointer (IP) of a label by its name.
     *
     * @param [in]  script   The script instance.
     * @param [in]  name     Pointer to the label name string.
     * @param [in]  name_len Length of the label name in bytes.
     * @param [out] out      Receives the label IP.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if label doesn't exist.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_get_label(const basic26_Script *BASIC26_NONNULL script, const uint8_t *BASIC26_NONNULL name, size_t name_len, size_t *BASIC26_NONNULL out);

    /**
     * @brief Sets the instruction pointer (IP) for a label.
     *
     * If a label with the same name already exists, its IP is updated. Otherwise,
     * a new label is created.
     *
     * @param [in] script    The script instance.
     * @param [in] vm        The VM instance (used for memory allocation).
     * @param [in] name      Pointer to the label name string.
     * @param [in] name_len  Length of the label name in bytes.
     * @param [in] ip        The instruction pointer associated with the label.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_set_label(basic26_Script *BASIC26_NONNULL script, basic26_Vm *BASIC26_NONNULL vm, const uint8_t *BASIC26_NONNULL name, size_t name_len, size_t ip);

    /**
     * @brief Removes a label by its name.
     *
     * @param [in] script    The script instance.
     * @param [in] name      Pointer to the label name string.
     * @param [in] name_len  Length of the label name in bytes.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if the label does not exist.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_remove_label(basic26_Script *BASIC26_NONNULL script, const uint8_t *BASIC26_NONNULL name, size_t name_len);

    /**
     * @brief Returns the number of labels defined in the script.
     *
     * @param [in] script The script instance.
     * @return The number of labels.
     */
    BASIC26_API size_t BASIC26_API_CALL basic26_Script_count_labels(const basic26_Script *BASIC26_NONNULL script);

    /**
     * @brief Dumps the script bytecode to a human-readable string.
     *
     * @param [in]  script   The script instance.
     * @param [in]  vm       The VM instance.
     * @param [out] out      Receives a pointer to the allocated string.
     * @param [out] out_len  Receives the length of the string in bytes.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_Script_dump(const basic26_Script *BASIC26_NONNULL script, basic26_Vm *BASIC26_NONNULL vm, uint8_t *BASIC26_NONNULL *BASIC26_NONNULL out, size_t *BASIC26_NONNULL out_len);

    /**
     * @brief Frees the memory allocated by basic26_Script_dump.
     *
     * @param [in] vm       The VM instance.
     * @param [in] dump     Pointer to the string to free. May be NULL (no-op).
     * @param [in] dump_len Length of the string to free.
     */
    BASIC26_API void BASIC26_API_CALL basic26_Script_dump_free(basic26_Vm *BASIC26_NONNULL vm, uint8_t *BASIC26_NULLABLE dump, size_t dump_len);

    /**
     * @brief Creates a new debug info instance.
     *
     * @param [in]  vm   The VM instance.
     * @param [out] out  Receives the debug info handle.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_create(basic26_Vm *BASIC26_NONNULL vm, basic26_DebugInfo * BASIC26_NULLABLE * BASIC26_NONNULL out);

    /**
     * @brief Destroys a debug info instance.
     *
     * @param [in] debug_info The debug info instance to destroy. May be NULL (no-op).
     * @param [in] vm         The VM instance.
     */
    BASIC26_API void BASIC26_API_CALL basic26_DebugInfo_destroy(basic26_DebugInfo *BASIC26_NONNULL debug_info, basic26_Vm *BASIC26_NONNULL vm);

    /**
     * @brief Clears debug info resources.
     *
     * Resets the internal data structures while retaining their allocated capacity.
     *
     * @param [in] debug_info The debug info instance.
     */
    BASIC26_API void BASIC26_API_CALL basic26_DebugInfo_clear(basic26_DebugInfo *BASIC26_NONNULL debug_info);

    /**
     * @brief Gets the source code position for a given instruction pointer.
     *
     * @param [in]  debug_info The debug info instance.
     * @param [in]  ip         Instruction pointer (zero-based index of the opcode).
     * @param [out] out        Receives the byte offset in the source code.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if the instruction pointer is out of range.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_get_source_pos(const basic26_DebugInfo *BASIC26_NONNULL debug_info, size_t ip, size_t *BASIC26_NONNULL out);

    /**
     * @brief Sets the source code position for a given instruction pointer.
     *
     * @param [in] debug_info The debug info instance.
     * @param [in] ip         Instruction pointer (zero-based index of the opcode).
     * @param [in] pos        Byte offset in the source code.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if the instruction pointer is out of range.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_set_source_pos(basic26_DebugInfo *BASIC26_NONNULL debug_info, size_t ip, size_t pos);

    /**
     * @brief Appends a source code position to the end of the source map.
     *
     * @param [in] debug_info The script instance.
     * @param [in] vm         The VM instance (used for memory allocation).
     * @param [in] pos        Byte offset in the source code.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_push_source_pos(basic26_DebugInfo *BASIC26_NONNULL debug_info, basic26_Vm *BASIC26_NONNULL vm, size_t pos);

    /**
     * @brief Inserts a source code position at a specific index.
     *
     * @param [in] debug_info The debug info instance.
     * @param [in] vm         The VM instance (used for memory allocation).
     * @param [in] ip         Zero-based index where the position will be inserted.
     * @param [in] pos        Byte offset in the source code.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if ip is out of range,
     *         BASIC26_RESULT_OUT_OF_MEMORY if allocation fails.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_insert_source_pos(basic26_DebugInfo *BASIC26_NONNULL debug_info, basic26_Vm *BASIC26_NONNULL vm, size_t ip, size_t pos);

    /**
     * @brief Removes the last source code position.
     *
     * @param [in]  debug_info The debug info instance.
     * @param [out] out        Receives the removed position. May be NULL.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if the source map is empty.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_pop_source_pos(basic26_DebugInfo *BASIC26_NONNULL debug_info, size_t *BASIC26_NULLABLE out);

    /**
     * @brief Removes a source code position at a specific index.
     *
     * @param [in]  debug_info The debug info instance.
     * @param [in]  ip         Zero-based index of the position to remove.
     * @param [out] out        Receives the removed position. May be NULL.
     * @return BASIC26_RESULT_OK on success, BASIC26_RESULT_NOT_FOUND if ip is out of range or the source map is empty.
     */
    BASIC26_API basic26_Result BASIC26_API_CALL basic26_DebugInfo_remove_source_pos(basic26_DebugInfo *BASIC26_NONNULL debug_info, size_t ip, size_t *BASIC26_NULLABLE out);

    /**
     * @brief Returns the number of source code positions in the script's source map.
     *
     * @param [in] debug_info The debug info instance.
     * @return The number of source positions.
     */
    BASIC26_API size_t BASIC26_API_CALL basic26_DebugInfo_count_source_pos(const basic26_DebugInfo *BASIC26_NONNULL debug_info);

#ifdef __cplusplus
}
#endif

#endif // BASIC26_H
