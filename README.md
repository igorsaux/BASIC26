# BASIC26

A lightweight, embeddable scripting language and virtual machine.

BASIC26 exposes a C API so that host applications can compile and execute any scripts at runtime. The builtin language is line-oriented, inspired by classic BASIC, and is designed for use cases such as game modding, configuration logic, automation rules, and any scenario where a small, safe, sandboxed scripting layer is desirable.

## Features

- **Mechanisms, Not Policies** - the VM exposes only raw primitives without built-in data structures, high-level semantics or implicit behaviour.
- **Embeddable C API** - compile scripts and execute them from any C-compatible host application.
- **Sandboxed execution** - configurable limits on opcode count and execution time prevent runaway scripts.
- **Yield/resume** - native callbacks can suspend execution (`BASIC26_FUNCTION_RESULT_YIELD`) and the host can resume later, enabling async patterns in event-driven applications.
- **No external dependencies** - the library is self-contained and links only against libc for the examples.
- **Bare-metal ready** — fully deterministic with zero OS assumptions or system API calls. Porting to any environment is trivial; even the default allocator and time source are optional and overridable.

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

#### Results

MacBook Air M3:

```
Run  1: 1155 ns/iter
Run  2: 1167 ns/iter
Run  3: 1191 ns/iter
Run  4: 1188 ns/iter
Run  5: 1183 ns/iter
Run  6: 1181 ns/iter
Run  7: 1180 ns/iter
Run  8: 1188 ns/iter
Run  9: 1182 ns/iter
Run 10: 1188 ns/iter

--- Results (ns/iter) ---
Mean:     1180 ns
Median:   1183 ns
Min:      1155 ns
Max:      1191 ns
Std Dev:  10.5 ns (0.9%)
P95:      1191 ns
P99:      1191 ns

--- Detailed per-iteration analysis (last run) ---
Mean:     1171 ns
Median:   1167 ns
Min:      1041 ns
Max:      13417 ns
Std Dev:  133.6 ns (11.4%)
P95:      1209 ns
P99:      1375 ns
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

### Example 03 - Native Call Overhead Benchmark (`examples/03.c`)

Measures the cost of a single native call (script bytecode -> C callback -> back) by comparing a pure-bytecode inner loop (`temp = a + b`) against a native-call variant (`NADD a, b, $temp`), and additionally measures a no-op native call (`NOOP a`) to isolate the call-frame overhead from the callback body. Reports per-iteration stats (mean / median / min / max / std dev / p95 / p99), derived per-call overhead, and native-call throughput in calls/sec. Links against the static library.

#### Results

MacBook Air M3:

```
--- Per-variant results (loop body x 100) ---
  bytecode   Run  1: 2334 ns/iter
  bytecode   Run  2: 2355 ns/iter
  bytecode   Run  3: 2349 ns/iter
  bytecode   Run  4: 2350 ns/iter
  bytecode   Run  5: 2394 ns/iter
  bytecode   Run  6: 2377 ns/iter
  bytecode   Run  7: 2381 ns/iter
  bytecode   Run  8: 2402 ns/iter
  bytecode   Run  9: 2353 ns/iter
  bytecode   Run 10: 2339 ns/iter
--- bytecode (ns/iter) ---
Mean:     2363 ns
Median:   2355 ns
Min:      2334 ns
Max:      2402 ns
Std Dev:  22.2 ns (0.9%)
P95:      2402 ns
P99:      2402 ns

  native     Run  1: 3014 ns/iter
  native     Run  2: 2974 ns/iter
  native     Run  3: 2976 ns/iter
  native     Run  4: 2984 ns/iter
  native     Run  5: 2982 ns/iter
  native     Run  6: 2982 ns/iter
  native     Run  7: 2971 ns/iter
  native     Run  8: 2981 ns/iter
  native     Run  9: 2994 ns/iter
  native     Run 10: 3016 ns/iter
--- native (ns/iter) ---
Mean:     2987 ns
Median:   2982 ns
Min:      2971 ns
Max:      3016 ns
Std Dev:  15.1 ns (0.5%)
P95:      3016 ns
P99:      3016 ns

  noop       Run  1: 2379 ns/iter
  noop       Run  2: 2375 ns/iter
  noop       Run  3: 2366 ns/iter
  noop       Run  4: 2371 ns/iter
  noop       Run  5: 2362 ns/iter
  noop       Run  6: 2377 ns/iter
  noop       Run  7: 2370 ns/iter
  noop       Run  8: 2371 ns/iter
  noop       Run  9: 2365 ns/iter
  noop       Run 10: 2367 ns/iter
--- noop (ns/iter) ---
Mean:     2371 ns
Median:   2371 ns
Min:      2362 ns
Max:      2379 ns
Std Dev:  5.1 ns (0.2%)
P95:      2379 ns
P99:      2379 ns

--- native (per-iter, last run) (ns/iter) ---
Mean:     2974 ns
Median:   2959 ns
Min:      2708 ns
Max:      44542 ns
Std Dev:  232.8 ns (7.8%)
P95:      3042 ns
P99:      3292 ns

--- Derived native-call metrics ---
bytecode baseline : 2373 ns/script-run  (23.73 ns/loop-iter)
native (NADD)     : 2981 ns/script-run  (29.81 ns/loop-iter)
noop (NOOP)       : 2370 ns/script-run  (23.70 ns/loop-iter)

Marginal cost per native call (native - bytecode) / N : +6.1 ns
Absolute cost per empty native call (noop / N)        : 23.7 ns
Empty native call throughput                          : 42189160 calls/sec
NADD native call throughput (incl. callback body)     : 33549781 calls/sec
```

Intel Core i5-11600K:

```
--- Per-variant results (loop body x 100) ---
  bytecode   Run  1: 3585 ns/iter
  bytecode   Run  2: 3606 ns/iter
  bytecode   Run  3: 3619 ns/iter
  bytecode   Run  4: 3624 ns/iter
  bytecode   Run  5: 3624 ns/iter
  bytecode   Run  6: 3624 ns/iter
  bytecode   Run  7: 3602 ns/iter
  bytecode   Run  8: 3603 ns/iter
  bytecode   Run  9: 3615 ns/iter
  bytecode   Run 10: 3609 ns/iter
--- bytecode (ns/iter) ---
Mean:     3611 ns
Median:   3615 ns
Min:      3585 ns
Max:      3624 ns
Std Dev:  11.9 ns (0.3%)
P95:      3624 ns
P99:      3624 ns

  native     Run  1: 3842 ns/iter
  native     Run  2: 3816 ns/iter
  native     Run  3: 3839 ns/iter
  native     Run  4: 3786 ns/iter
  native     Run  5: 3787 ns/iter
  native     Run  6: 3814 ns/iter
  native     Run  7: 3801 ns/iter
  native     Run  8: 3774 ns/iter
  native     Run  9: 3785 ns/iter
  native     Run 10: 3827 ns/iter
--- native (ns/iter) ---
Mean:     3807 ns
Median:   3814 ns
Min:      3774 ns
Max:      3842 ns
Std Dev:  22.7 ns (0.6%)
P95:      3842 ns
P99:      3842 ns

  noop       Run  1: 2930 ns/iter
  noop       Run  2: 2939 ns/iter
  noop       Run  3: 2958 ns/iter
  noop       Run  4: 2912 ns/iter
  noop       Run  5: 2928 ns/iter
  noop       Run  6: 2946 ns/iter
  noop       Run  7: 2942 ns/iter
  noop       Run  8: 2940 ns/iter
  noop       Run  9: 2936 ns/iter
  noop       Run 10: 2947 ns/iter
--- noop (ns/iter) ---
Mean:     2938 ns
Median:   2940 ns
Min:      2912 ns
Max:      2958 ns
Std Dev:  11.6 ns (0.4%)
P95:      2958 ns
P99:      2958 ns

--- native (per-iter, last run) (ns/iter) ---
Mean:     3715 ns
Median:   3600 ns
Min:      3500 ns
Max:      88900 ns
Std Dev:  580.5 ns (15.6%)
P95:      3900 ns
P99:      5500 ns

--- Derived native-call metrics ---
bytecode baseline : 3612 ns/script-run  (36.12 ns/loop-iter)
native (NADD)     : 3738 ns/script-run  (37.38 ns/loop-iter)
noop (NOOP)       : 2965 ns/script-run  (29.65 ns/loop-iter)

Marginal cost per native call (native - bytecode) / N : +1.3 ns
Absolute cost per empty native call (noop / N)        : 29.7 ns
Empty native call throughput                          : 33725482 calls/sec
NADD native call throughput (incl. callback body)     : 26749039 calls/sec
```
