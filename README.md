# BASIC26

A lightweight, embeddable scripting language and virtual machine.

BASIC26 exposes a C API so that host applications can compile and execute simple scripts at runtime. The language is line-oriented, inspired by classic BASIC, and is designed for use cases such as game modding, configuration logic, automation rules, and any scenario where a small, safe, sandboxed scripting layer is desirable.

## Features

- **Mechanisms, Not Policies** - the VM exposes only raw primitives without built-in data structures, high-level semantics or implicit behaviour.
- **Embeddable C API** - compile scripts and execute them from any C-compatible host application.
- **Sandboxed execution** - configurable limits on opcode count and wall-clock time prevent runaway scripts.
- **Yield/resume** - native callbacks can suspend execution (`BASIC26_FUNCTION_RESULT_YIELD`) and the host can resume later, enabling async patterns in event-driven applications.
- **No external dependencies** - the library is self-contained and links only against libc for the examples.
- **Bare-metal ready** — fully deterministic with zero OS assumptions or system API calls. Porting to any environment is trivial, even the default allocator is optional and overridable.

## Language Reference

See [basic26.h](src/basic26.h) for more information.

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
