# BASIC26

A lightweight, embeddable scripting language and virtual machine.

BASIC26 exposes a C API so that host applications can compile and execute simple scripts at runtime. The language is line-oriented, inspired by classic BASIC, and is designed for use cases such as game modding, configuration logic, automation rules, and any scenario where a small, safe, sandboxed scripting layer is desirable.

## Features

- **Mechanisms, Not Policies** - the VM exposes only raw primitives without built-in data structures, high-level semantics or implicit behaviour.
- **Embeddable C API** - compile scripts and execute them from any C-compatible host application.
- **Sandboxed execution** - configurable limits on opcode count and wall-clock time prevent runaway scripts.
- **Yield/resume** - native callbacks can suspend execution (`BASIC26_FUNCTION_RESULT_YIELD`) and the host can resume later, enabling async patterns in event-driven applications.
- **Custom allocators** - plug in your own memory allocation callbacks to integrate with custom memory pools or track allocations.
- **No external dependencies** - the library is self-contained and links only against libc for the examples.
- **Cross-platform** - builds on Linux, macOS, Windows, and freestanding targets (with a failing allocator fallback).

## Language Reference

BASIC26 is a line-based language: each line contains a single statement. Blank lines and lines starting with `//` are ignored.

### Data Types

| Type   | Description                                        | Example              |
|--------|----------------------------------------------------|----------------------|
| INT    | 64-bit signed integer                              | `42`, `-7`, `0`      |
| FLOAT  | 64-bit double-precision floating point             | `3.14`, `NAN`, `INF` |
| STRING | Interned UTF-8 string literal                      | `"hello"`            |
| SYMBOL | Identifier literal (prefixed with `$`)             | `$foo`               |
| NULL   | The null value                                     | `NULL`               |
| OBJECT | Opaque host pointer, created/consumed by callbacks | (not constructable from script) |

### Variables and Assignment

Variables are **NOT** created implicitly by the interpreter. All variables must be explicitly created by the host application via `basic26_State_set_var()` before the script references them. The interpreter does not allocate memory for variables on its own - variable creation is entirely the host's responsibility. Once created, a script can assign any type to the variable:

```
x = 10
y = 3.14
name = "Alice"
x = NULL
```

Attempting to read or assign to a variable that was never created by the host will result in a `BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE` error at runtime.

### Operators

Listed from lowest to highest precedence:

| Precedence | Operator | Description             | Operand Types |
|:----------:|:--------:|:------------------------|:--------------|
| 9          | OR       | Boolean OR              | INT, INT      |
| 8          | AND      | Boolean AND             | INT, INT      |
| 7          | == !=    | Equality / Inequality   | same types    |
| 7          | < > <= >=| Ordering comparison     | INT or FLOAT  |
| 6          | \| ^ &   | Bitwise OR / XOR / AND  | INT, INT      |
| 5          | << >>    | Bit shift left / right  | INT, INT      |
| 4          | + -      | Addition / Subtraction  | INT or FLOAT  |
| 3          | * / %    | Multiply / Divide / Rem | INT or FLOAT  |
| 1          | NOT      | Boolean NOT (unary)     | INT           |
| 1          | ~        | Bitwise NOT (unary)     | INT           |

Parentheses `()` can be used to override precedence. Comparison and boolean operators produce INT values: `1` for true, `0` for false. Type mismatches (e.g. adding INT to FLOAT) cause a runtime error.

### Control Flow

```
IF expr
  ...
ELSE
  ...
ENDIF
```

```
WHILE expr
  ...
ENDWHILE
```

```
GOTO label_name
```

Labels are defined on their own line with a colon:

```
my_label:
```

The `ELSE` branch is optional. Forward label references are allowed; the compiler resolves them after all lines have been parsed.

### Function Calls

All functions are native callbacks registered by the host application. A call takes the form:

```
FUNC_NAME arg1, arg2, arg3
```

Arguments are comma-separated and evaluated left-to-right. There is no return-value mechanism; native functions communicate results back to the script by setting variables via `basic26_State_set_var()`.

### Symbols

Symbol literals (prefixed with `$`) are used to pass identifier *names* to native functions without resolving them as variables. For example, `NEW $obj1, 42` passes the symbol `obj1` so the callback knows which variable to store the result in. Without symbols, the host would have no way to determine the intended target variable.

### StringId and SymbolId

`basic26_StringId` and `basic26_SymbolId` are **fully interchangeable**. They share the same interning table, so a given character sequence always produces the same numeric ID regardless of which type alias is used. The distinction exists solely for semantic clarity: use `StringId` when working with string data and `SymbolId` when working with identifier names. You can safely pass a `StringId` where a `SymbolId` is expected and vice versa.

### Comments

Lines starting with `//` are ignored:

```
// This is a comment
x = 10 // inline comments are also supported
```

## API Overview

The library is organized around three opaque handles:

- **`basic26_Vm`** - the virtual machine instance. Owns the string interning table and the registry of native function callbacks.
- **`basic26_Script`** - a compiled bytecode container. Created from a Vm, compiled from source text, and executed with a State.
- **`basic26_State`** - the runtime execution state. Holds the value stack, variable store, and instruction pointer.

### _clear Functions and Memory Reuse

The `_clear` functions (`basic26_Vm_clear`, `basic26_State_clear`, `basic26_Script_clear`) do **NOT** free memory. They only reset internal data structures (hash maps, lists, etc.) to an empty state while retaining their previously allocated capacity. This design enables maximum memory reuse and avoids unnecessary allocation/deallocation cycles and fragmentation. Any cleared object can be reused without having to recreate it from scratch.

**Warning**: clearing interned strings or script labels can break the interpreter. If a compiled script or a running state still references string IDs or label names that have been cleared, subsequent operations will produce `BASIC26_RESULT_RUNTIME_ERROR` results (e.g. undefined variable, undefined function). The interpreter will not crash, but the script will not run correctly. Only clear strings or labels when you are certain no active script or state depends on them.

### Typical Lifecycle

```c
// 1. Create the VM
basic26_Vm *vm;
basic26_Vm_create(
    &(basic26_CreateVmOptions){ .alloc = NULL }, &vm);

// 2. Create a State
basic26_State *state;
basic26_State_create(
    &(basic26_CreateStateOptions){ .vm = vm }, &state);

// 3. Create a Script
basic26_Script *script;
basic26_Script_create(vm, &script);

// 4. Compile source code
basic26_Script_compile(script,
    &(basic26_CompileOptions){
        .vm = vm,
        .source = (const uint8_t *)src,
        .source_len = strlen(src),
        .limits = &(basic26_ScriptLimits){ 0 },
    }, NULL);

// 5. Register native functions
basic26_SymbolId print_id;
basic26_Vm_get_string_id(vm, (const uint8_t *)"PRINT", 5, true, &print_id);
basic26_Vm_register_function(vm,
    &(basic26_RegisterFunctionOptions){
        .name = print_id,
        .callback = my_print_callback,
    });

// 6. Create variables (mandatory - the interpreter does not create them)
basic26_SymbolId x_id;
basic26_Vm_get_string_id(vm, (const uint8_t *)"x", 1, true, &x_id);
basic26_State_set_var(state, x_id, &(basic26_Value){
    .type = BASIC26_VALUE_TYPE_INT,
    .as.int_val = 0,
});

// 7. Execute
basic26_Vm_run(vm,
    &(basic26_RunOptions){
        .state = state,
        .script = script,
        .limits = &(basic26_RunLimits){ 0 },
        .userdata = NULL,
        .error_out = NULL,
    });

// 8. Clean up
basic26_Script_destroy(script, vm);
basic26_State_destroy(state, vm);
basic26_Vm_destroy(vm);
```

See `src/basic26.h` for the complete API reference with detailed documentation on every type and function.

## Building

BASIC26 uses the Zig build system. You need [Zig 0.16.0](https://ziglang.org/download/) or later.

```bash
# Build the static and dynamic libraries
zig build

# Run the test suite
zig build test

# Build and install the examples
zig build install
```

The build produces:

- `libbasic26-static.a` - static library
- `libbasic26.so` (or `.dylib` / `.dll`) - shared library
- `basic26.h` - installed header file
- `example_01` - compiled example executable (linked against the shared library)
- `example_02` - compiled example executable (linked against the static library)

### Linking from a C project

1. Include `src/basic26.h` in your source files.
2. Link against the static library (`-lbasic26-static`) or the dynamic library (`-lbasic26`).
3. On Windows, define `BASIC26_STATIC` when linking against the static library, or `BASIC26_DYNAMIC` when linking against the dynamic library. These macros control symbol visibility via `__declspec(dllimport)` and must be set consistently with your chosen linkage to avoid linker errors. On non-Windows platforms, these macros have no effect and can be omitted.

### Using in a Zig project

Add BASIC26 as a dependency in your `build.zig.zon` and reference it in `build.zig`:

```zig
const basic26 = b.dependency("basic26", .{
    .target = target,
    .optimize = optimize,
});

const mod = b.createModule(.{
    .root_source_file = b.path("src/root.zig"),
    .target = target,
    .optimize = optimize,
    // ...your other options...
    .imports = &.{
        // ...your other imports...
        .{ .name = "basic26", .module = basic26.module("basic26") },
    },
});

// Static linking
mod.linkLibrary(basic26.artifact("basic26-static"));

// OR dynamic linking
// mod.linkLibrary(basic26.artifact("basic26"));
```

- `basic26.module("basic26")` - exposes the Zig module with translated C API types and declarations, which you can import in your Zig source with `@import("basic26")`.
- `basic26.artifact("basic26-static")` - the static library artifact, for static linking.
- `basic26.artifact("basic26")` - the shared library artifact, for dynamic linking.

When using dynamic linking, you typically also want to install the shared library so it is available at runtime:

```zig
b.installArtifact(basic26.artifact("basic26"));
```

## Examples

### Example 01 - Feature Tour (`examples/01.c`)

Demonstrates the full API lifecycle: creating a VM, compiling a script, registering native functions (PRINT, VAR, NEW, DEL, WAIT), running the script with a yield-aware loop, and cleaning up. The script itself exercises strings, symbols, NULL, variables, object pointers, all value types, and the YIELD mechanism for asynchronous operations (WAIT suspends for 2 seconds, then resumes). This example links against the shared library.

### Example 02 - Fibonacci Benchmark (`examples/02.c`)

Computes Fibonacci numbers using a WHILE loop. Shows how to set variables from C before execution, read them back after, reset state for repeated runs, and measure performance with execution timing. Demonstrates the WHILE/ENDWHILE control flow construct and the State reset/clear API. This example links against the static library.

## Benchmarks

MacBook Air M3:

```
Run  1: 1872 ns/iter
Run  2: 1546 ns/iter
Run  3: 1398 ns/iter
Run  4: 1286 ns/iter
Run  5: 1235 ns/iter
Run  6: 1205 ns/iter
Run  7: 1192 ns/iter
Run  8: 1236 ns/iter
Run  9: 1231 ns/iter
Run 10: 1239 ns/iter

--- Results (ns/iter) ---
Mean:     1344 ns
Median:   1239 ns
Min:      1192 ns
Max:      1872 ns
Std Dev:  203.9 ns (15.2%)
P95:      1872 ns
P99:      1872 ns

--- Detailed per-iteration analysis (last run) ---
Mean:     1215 ns
Median:   1208 ns
Min:      1083 ns
Max:      8750 ns
Std Dev:  155.6 ns (12.8%)
P95:      1291 ns
P99:      1459 ns
```

Intel Core i5-11600K:

```
Run  1: 1837 ns/iter
Run  2: 1738 ns/iter
Run  3: 1739 ns/iter
Run  4: 1709 ns/iter
Run  5: 1722 ns/iter
Run  6: 1724 ns/iter
Run  7: 1752 ns/iter
Run  8: 1735 ns/iter
Run  9: 1730 ns/iter
Run 10: 1747 ns/iter

--- Results (ns/iter) ---
Mean:     1743 ns
Median:   1738 ns
Min:      1709 ns
Max:      1837 ns
Std Dev:  33.5 ns (1.9%)
P95:      1837 ns
P99:      1837 ns

--- Detailed per-iteration analysis (last run) ---
Mean:     1681 ns
Median:   1700 ns
Min:      1600 ns
Max:      14300 ns
Std Dev:  206.5 ns (12.3%)
P95:      1800 ns
P99:      1800 ns
```
