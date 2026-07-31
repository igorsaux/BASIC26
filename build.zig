// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

const std = @import("std");

const Translator = @import("translate_c").Translator;

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const stack_size = b.option(
        usize,
        "stack_size",
        "This constant controls the size of the inline stack array inside every basic26_State",
    );
    const examples = b.option(bool, "examples", "Build examples") orelse (target.result.os.tag != .freestanding);

    const translate_c = b.dependency("translate_c", .{});
    const t: Translator = .init(translate_c, .{
        .c_source_file = b.path("src/basic26.h"),
        .target = target,
        .optimize = optimize,
        .default_init = true,
    });

    if (stack_size != null) {
        t.defineCMacro("BASIC26_STACK_CAPACITY", b.fmt("{}", .{stack_size.?}));
    }

    b.modules.put(b.allocator, "basic26", t.mod) catch @panic("OOM");

    const basic26_mod = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{
            .{ .name = "capi", .module = t.mod },
        },
    });

    const basic26_static_lib = b.addLibrary(.{
        .name = "basic26-static",
        .root_module = basic26_mod,
        .linkage = .static,
    });

    b.installArtifact(basic26_static_lib);

    var basic26_dynamic_lib: *std.Build.Step.Compile = undefined;

    if (target.result.os.tag != .freestanding) {
        basic26_dynamic_lib = b.addLibrary(.{
            .name = "basic26",
            .root_module = basic26_mod,
            .linkage = .dynamic,
        });

        b.installArtifact(basic26_dynamic_lib);
    }

    const install_header_file = b.addInstallHeaderFile(b.path("src/basic26.h"), "basic26.h");
    b.getInstallStep().dependOn(&install_header_file.step);

    const mod_tests = b.addTest(.{
        .root_module = basic26_mod,
    });

    const run_mod_tests = b.addRunArtifact(mod_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_mod_tests.step);

    if (examples) {
        const example_01 = b.addExecutable(.{
            .name = "example_01",
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });

        example_01.root_module.addCSourceFile(.{
            .file = b.path("examples/01.c"),
            .language = .c,
        });
        example_01.root_module.addCMacro("BASIC26_DYNAMIC", "1");
        example_01.root_module.addIncludePath(b.path("src/"));
        example_01.root_module.linkLibrary(basic26_dynamic_lib);

        b.installArtifact(example_01);

        const example_02 = b.addExecutable(.{
            .name = "example_02",
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });

        example_02.root_module.addCSourceFile(.{
            .file = b.path("examples/02.c"),
            .language = .c,
        });
        example_02.root_module.addCMacro("BASIC26_STATIC", "1");
        example_02.root_module.addIncludePath(b.path("src/"));
        example_02.root_module.linkLibrary(basic26_static_lib);

        b.installArtifact(example_02);

        const example_03 = b.addExecutable(.{
            .name = "example_03",
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });

        example_03.root_module.addCSourceFile(.{
            .file = b.path("examples/03.c"),
            .language = .c,
        });
        example_03.root_module.addCMacro("BASIC26_STATIC", "1");
        example_03.root_module.addIncludePath(b.path("src/"));
        example_03.root_module.linkLibrary(basic26_static_lib);

        b.installArtifact(example_03);

        // pthread
        if (target.result.os.tag != .windows) {
            const example_conc = b.addExecutable(.{
                .name = "example_conc",
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                }),
            });

            example_conc.root_module.addCSourceFile(.{
                .file = b.path("examples/conc.c"),
                .language = .c,
            });
            example_conc.root_module.addCMacro("BASIC26_STATIC", "1");
            example_conc.root_module.addIncludePath(b.path("src/"));
            example_conc.root_module.linkLibrary(basic26_static_lib);
            example_conc.root_module.linkSystemLibrary("pthread", .{});

            b.installArtifact(example_conc);
        }

        const example_startup = b.addExecutable(.{
            .name = "example_startup",
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });

        example_startup.root_module.addCSourceFile(.{
            .file = b.path("examples/startup.c"),
            .language = .c,
        });
        example_startup.root_module.addCMacro("BASIC26_STATIC", "1");
        example_startup.root_module.addIncludePath(b.path("src/"));
        example_startup.root_module.linkLibrary(basic26_static_lib);

        b.installArtifact(example_startup);

        if (target.result.os.tag == .macos) {
            const wasm_target = b.resolveTargetQuery(.{
                .cpu_arch = .wasm32,
                .os_tag = .freestanding,
            });

            const is_prime = b.addExecutable(.{
                .name = "is_prime",
                .root_module = b.createModule(.{
                    .root_source_file = b.path("examples/is_prime.zig"),
                    .target = wasm_target,
                    .optimize = .ReleaseFast,
                    .strip = true,
                }),
            });

            is_prime.rdynamic = true;
            is_prime.entry = .disabled;

            const xxd = b.addSystemCommand(&.{ "xxd", "-i", "-n", "is_prime_wasm" });
            xxd.step.dependOn(&is_prime.step);
            xxd.addArtifactArg(is_prime);

            const is_prime_c = xxd.addOutputFileArg("is_prime.c");

            const wasmtime = b.dependency("wasmtime_macos", .{});

            const example_conc_wasmtime = b.addExecutable(.{
                .name = "example_conc_wasmtime",
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                }),
            });

            example_conc_wasmtime.step.dependOn(&xxd.step);

            example_conc_wasmtime.root_module.addCSourceFile(.{
                .file = b.path("examples/conc_wasmtime.c"),
                .language = .c,
            });
            example_conc_wasmtime.root_module.addCSourceFile(.{
                .file = is_prime_c,
                .language = .c,
            });
            example_conc_wasmtime.root_module.addCMacro("BASIC26_STATIC", "1");
            example_conc_wasmtime.root_module.addIncludePath(b.path("src/"));
            example_conc_wasmtime.root_module.addIncludePath(wasmtime.path("include"));
            example_conc_wasmtime.root_module.addLibraryPath(wasmtime.path("lib"));
            example_conc_wasmtime.root_module.linkLibrary(basic26_static_lib);
            example_conc_wasmtime.root_module.linkSystemLibrary("wasmtime", .{ .preferred_link_mode = .static });

            b.installArtifact(example_conc_wasmtime);
        }
    }
}
