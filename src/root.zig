// Copyright (C) 2026 Igor Spichkin
// SPDX-License-Identifier: MPL-2.0

const std = @import("std");
const builtin = @import("builtin");

const c = @import("capi");

const Value = struct {
    pub inline fn fromInt(value: c.basic26_IntType) c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_INT,
            .as = .{
                .int_val = value,
            },
        };
    }

    pub inline fn fromFloat(value: c.basic26_FloatType) c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_FLOAT,
            .as = .{
                .float_val = value,
            },
        };
    }

    pub inline fn fromString(value: c.basic26_StringId) c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_STRING,
            .as = .{
                .string_id = value,
            },
        };
    }

    pub inline fn fromSymbol(value: c.basic26_SymbolId) c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_SYMBOL,
            .as = .{
                .symbol_id = value,
            },
        };
    }

    pub inline fn fromNull() c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_NULL,
        };
    }

    pub inline fn fromAddress(value: usize) c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_ADDRESS,
            .as = .{
                .address_val = value,
            },
        };
    }

    pub inline fn fromUndefined() c.basic26_Value {
        return .{
            .type = c.BASIC26_VALUE_TYPE_FORCE_32BIT,
        };
    }
};

const UserAllocator = struct {
    userdata: ?*anyopaque,
    alloc_callback: c.basic26_alloc,
    free_callback: c.basic26_free,

    pub inline fn allocator(this: *UserAllocator) std.mem.Allocator {
        return .{
            .ptr = this,
            .vtable = &.{
                .alloc = alloc,
                .resize = resize,
                .remap = remap,
                .free = free,
            },
        };
    }

    fn alloc(ctx: *anyopaque, len: usize, alignment: std.mem.Alignment, ret_addr: usize) ?[*]u8 {
        _ = ret_addr;

        const this: *const UserAllocator = @ptrCast(@alignCast(ctx));
        const ptr = this.alloc_callback.?(this.userdata, len, alignment.toByteUnits());

        return @ptrCast(@alignCast(ptr));
    }

    fn resize(ctx: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ret_addr: usize) bool {
        _ = ctx;
        _ = memory;
        _ = alignment;
        _ = new_len;
        _ = ret_addr;

        return false;
    }

    fn remap(ctx: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ret_addr: usize) ?[*]u8 {
        _ = ctx;
        _ = memory;
        _ = alignment;
        _ = new_len;
        _ = ret_addr;

        return null;
    }

    fn free(ctx: *anyopaque, memory: []u8, alignment: std.mem.Alignment, ret_addr: usize) void {
        _ = ret_addr;

        const this: *const UserAllocator = @ptrCast(@alignCast(ctx));
        this.free_callback.?(this.userdata, memory.ptr, memory.len, alignment.toByteUnits());
    }
};

const Allocator = union(enum) {
    default,
    debug: std.heap.DebugAllocator(.{}),
    failing,
    user: UserAllocator,

    pub inline fn allocator(this: *Allocator) std.mem.Allocator {
        if (builtin.is_test) {
            return std.testing.allocator;
        }

        return switch (this.*) {
            .default => std.heap.smp_allocator,
            .debug => this.debug.allocator(),
            .failing => std.mem.Allocator.failing,
            .user => this.user.allocator(),
        };
    }

    pub inline fn fromDefault() Allocator {
        return .default;
    }

    pub inline fn fromDebug() Allocator {
        return .{ .debug = .init };
    }

    pub inline fn fromFailing() Allocator {
        return .failing;
    }

    pub inline fn fromUser(userdata: ?*anyopaque, alloc_callback: c.basic26_alloc, free_callback: c.basic26_free) Allocator {
        return .{ .user = .{ .userdata = userdata, .alloc_callback = alloc_callback, .free_callback = free_callback } };
    }
};

const ExecuteError = error{
    StackUnderflow,
    StackOverflow,
    TypeMismatch,
    DivisionByZero,
    UndefinedFunction,
    OutOfMemory,
    UndefinedVariable,
    InvalidBitShift,
    FunctionError,
};

const ExecuteResult = union(enum) {
    ok: void,
    yield: void,
    err: ExecuteError,
};

const Strings = struct {
    data: std.ArrayList([]u8) = .empty,
    str_map: std.AutoHashMapUnmanaged(usize, []const u8) = .empty,
    id_map: std.StringArrayHashMapUnmanaged(usize) = .empty,

    pub inline fn init() Strings {
        return .{};
    }

    pub inline fn deinit(this: *Strings, allocator: std.mem.Allocator) void {
        for (this.data.items) |str| {
            allocator.free(str);
        }

        this.data.deinit(allocator);
        this.str_map.deinit(allocator);
        this.id_map.deinit(allocator);
    }

    pub inline fn clear(this: *Strings) void {
        this.data.clearRetainingCapacity();
        this.str_map.clearRetainingCapacity();
        this.id_map.clearRetainingCapacity();
    }

    pub inline fn count(this: *const Strings) usize {
        return this.data.items.len;
    }

    pub inline fn get(this: *const Strings, string: []const u8) ?usize {
        return this.id_map.get(string);
    }

    inline fn put(this: *Strings, allocator: std.mem.Allocator, string: []u8) error{OutOfMemory}!usize {
        const id = this.data.items.len;

        try this.data.append(allocator, string);
        try this.id_map.put(allocator, string, id);
        try this.str_map.put(allocator, id, string);

        return id;
    }

    pub inline fn getOrPutOwned(this: *Strings, allocator: std.mem.Allocator, string: []u8) error{OutOfMemory}!usize {
        if (this.id_map.get(string)) |id| {
            allocator.free(string);
            return id;
        }

        return try this.put(allocator, string);
    }

    pub inline fn getOrPut(this: *Strings, allocator: std.mem.Allocator, string: []const u8) error{OutOfMemory}!usize {
        if (this.id_map.get(string)) |id| {
            return id;
        }

        const dupe = try allocator.dupe(u8, string);
        errdefer allocator.free(dupe);

        return try this.put(allocator, dupe);
    }

    pub inline fn getByid(this: *const Strings, id: c.basic26_StringId) ?[]const u8 {
        return this.str_map.get(id);
    }
};

const Vm = struct {
    allocator: Allocator,
    io: *std.Io.Threaded = .global_single_threaded,
    strings: Strings = .init(),
    function_callbacks: std.AutoHashMapUnmanaged(c.basic26_SymbolId, c.basic26_function_callback) = .empty,

    pub inline fn init(allocator: Allocator) Vm {
        return .{ .allocator = allocator };
    }

    pub inline fn deinit(this: *Vm) void {
        const alloc = this.allocator.allocator();

        this.strings.deinit(alloc);
        this.function_callbacks.deinit(alloc);
    }

    pub inline fn execute(this: *Vm, state: *State, script: *const Script, userdata: ?*anyopaque, op: c.basic26_Op) ExecuteResult {
        switch (op.code) {
            c.BASIC26_OPCODE_PUSH_INT => {
                state.push(Value.fromInt(op.imm.as_int)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_PUSH_FLOAT => {
                state.push(Value.fromFloat(op.imm.as_float)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_PUSH_STRING => {
                state.push(Value.fromString(op.imm.as_string)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_PUSH_SYMBOL => {
                state.push(Value.fromSymbol(op.imm.as_symbol)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_PUSH_ADDRESS => {
                state.push(Value.fromAddress(op.imm.as_symbol)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_PUSH_NULL => {
                state.push(Value.fromNull()) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_LOAD => {
                const val = state.getVar(op.imm.as_symbol) orelse {
                    return .{ .err = ExecuteError.UndefinedVariable };
                };

                state.push(val) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_STORE => {
                const val = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const exists = state.setVar(this.allocator.allocator(), op.imm.as_symbol, val, false) catch unreachable;

                if (!exists) {
                    return .{ .err = ExecuteError.UndefinedVariable };
                }
            },
            c.BASIC26_OPCODE_ADD => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    state.push(Value.fromInt(lhs.as.int_val +% rhs.as.int_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(lhs.as.float_val + rhs.as.float_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_SUB => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    state.push(Value.fromInt(lhs.as.int_val -% rhs.as.int_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(lhs.as.float_val - rhs.as.float_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_MUL => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    state.push(Value.fromInt(lhs.as.int_val *% rhs.as.int_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(lhs.as.float_val * rhs.as.float_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_DIV => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    if (rhs.as.int_val == 0) {
                        return .{ .err = ExecuteError.DivisionByZero };
                    }

                    if (lhs.as.int_val == std.math.minInt(c.basic26_IntType) and rhs.as.int_val == -1) {
                        state.push(Value.fromInt(std.math.minInt(c.basic26_IntType))) catch {
                            return .{ .err = ExecuteError.StackOverflow };
                        };
                    } else {
                        state.push(Value.fromInt(@divTrunc(lhs.as.int_val, rhs.as.int_val))) catch {
                            return .{ .err = ExecuteError.StackOverflow };
                        };
                    }
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(lhs.as.float_val / rhs.as.float_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_REM => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    if (rhs.as.int_val == 0) {
                        return .{ .err = ExecuteError.DivisionByZero };
                    }

                    if (lhs.as.int_val == std.math.minInt(c.basic26_IntType) and rhs.as.int_val == -1) {
                        state.push(Value.fromInt(std.math.minInt(c.basic26_IntType))) catch {
                            return .{ .err = ExecuteError.StackOverflow };
                        };
                    } else {
                        state.push(Value.fromInt(@rem(lhs.as.int_val, rhs.as.int_val))) catch {
                            return .{ .err = ExecuteError.StackOverflow };
                        };
                    }
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(@rem(lhs.as.float_val, rhs.as.float_val))) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_NEG => {
                const val = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (val.type == c.BASIC26_VALUE_TYPE_INT) {
                    state.push(Value.fromInt(-%val.as.int_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else if (val.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    state.push(Value.fromFloat(-val.as.float_val)) catch {
                        return .{ .err = ExecuteError.StackOverflow };
                    };
                } else {
                    return .{ .err = ExecuteError.TypeMismatch };
                }
            },
            c.BASIC26_OPCODE_EQ, c.BASIC26_OPCODE_NEQ, c.BASIC26_OPCODE_LT, c.BASIC26_OPCODE_LTE, c.BASIC26_OPCODE_GT, c.BASIC26_OPCODE_GTE => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                var res = false;

                if (lhs.type == c.BASIC26_VALUE_TYPE_INT and rhs.type == c.BASIC26_VALUE_TYPE_INT) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.int_val == rhs.as.int_val,
                        c.BASIC26_OPCODE_NEQ => lhs.as.int_val != rhs.as.int_val,
                        c.BASIC26_OPCODE_LT => lhs.as.int_val < rhs.as.int_val,
                        c.BASIC26_OPCODE_LTE => lhs.as.int_val <= rhs.as.int_val,
                        c.BASIC26_OPCODE_GT => lhs.as.int_val > rhs.as.int_val,
                        c.BASIC26_OPCODE_GTE => lhs.as.int_val >= rhs.as.int_val,
                        else => unreachable,
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_FLOAT and rhs.type == c.BASIC26_VALUE_TYPE_FLOAT) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.float_val == rhs.as.float_val,
                        c.BASIC26_OPCODE_NEQ => lhs.as.float_val != rhs.as.float_val,
                        c.BASIC26_OPCODE_LT => lhs.as.float_val < rhs.as.float_val,
                        c.BASIC26_OPCODE_LTE => lhs.as.float_val <= rhs.as.float_val,
                        c.BASIC26_OPCODE_GT => lhs.as.float_val > rhs.as.float_val,
                        c.BASIC26_OPCODE_GTE => lhs.as.float_val >= rhs.as.float_val,
                        else => unreachable,
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_STRING and rhs.type == c.BASIC26_VALUE_TYPE_STRING) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.string_id == rhs.as.string_id,
                        c.BASIC26_OPCODE_NEQ => lhs.as.string_id != rhs.as.string_id,
                        else => return .{ .err = ExecuteError.TypeMismatch },
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_NULL and rhs.type == c.BASIC26_VALUE_TYPE_NULL) {
                    res = (op.code == c.BASIC26_OPCODE_EQ or op.code == c.BASIC26_OPCODE_LTE or op.code == c.BASIC26_OPCODE_GTE);
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_SYMBOL and rhs.type == c.BASIC26_VALUE_TYPE_SYMBOL) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.symbol_id == rhs.as.symbol_id,
                        c.BASIC26_OPCODE_NEQ => lhs.as.symbol_id != rhs.as.symbol_id,
                        else => return .{ .err = ExecuteError.TypeMismatch },
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_OBJECT and rhs.type == c.BASIC26_VALUE_TYPE_OBJECT) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.object_ptr == rhs.as.object_ptr,
                        c.BASIC26_OPCODE_NEQ => lhs.as.object_ptr != rhs.as.object_ptr,
                        else => return .{ .err = ExecuteError.TypeMismatch },
                    };
                } else if (lhs.type == c.BASIC26_VALUE_TYPE_ADDRESS and rhs.type == c.BASIC26_VALUE_TYPE_ADDRESS) {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => lhs.as.address_val == rhs.as.address_val,
                        c.BASIC26_OPCODE_NEQ => lhs.as.address_val != rhs.as.address_val,
                        c.BASIC26_OPCODE_LT => lhs.as.address_val < rhs.as.address_val,
                        c.BASIC26_OPCODE_LTE => lhs.as.address_val <= rhs.as.address_val,
                        c.BASIC26_OPCODE_GT => lhs.as.address_val > rhs.as.address_val,
                        c.BASIC26_OPCODE_GTE => lhs.as.address_val >= rhs.as.address_val,
                        else => unreachable,
                    };
                } else {
                    res = switch (op.code) {
                        c.BASIC26_OPCODE_EQ => false,
                        c.BASIC26_OPCODE_NEQ => true,
                        else => return .{ .err = ExecuteError.TypeMismatch },
                    };
                }

                state.push(Value.fromInt(if (res) 1 else 0)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BOOL_AND => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const rhs_true = if (rhs.type == c.BASIC26_VALUE_TYPE_INT)
                    rhs.as.int_val != 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                const lhs_true = if (lhs.type == c.BASIC26_VALUE_TYPE_INT)
                    lhs.as.int_val != 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                state.push(Value.fromInt(if (rhs_true and lhs_true) 1 else 0)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BOOL_OR => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const rhs_true = if (rhs.type == c.BASIC26_VALUE_TYPE_INT)
                    rhs.as.int_val != 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                const lhs_true = if (lhs.type == c.BASIC26_VALUE_TYPE_INT)
                    lhs.as.int_val != 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                state.push(Value.fromInt(if (rhs_true or lhs_true) 1 else 0)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BOOL_NOT => {
                const val = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const is_false = if (val.type == c.BASIC26_VALUE_TYPE_INT)
                    val.as.int_val == 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                state.push(Value.fromInt(if (is_false) 1 else 0)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BIT_AND => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (rhs.type != c.BASIC26_VALUE_TYPE_INT or lhs.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                state.push(Value.fromInt(rhs.as.int_val & lhs.as.int_val)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BIT_OR => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (rhs.type != c.BASIC26_VALUE_TYPE_INT or lhs.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                state.push(Value.fromInt(rhs.as.int_val | lhs.as.int_val)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BIT_XOR => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (rhs.type != c.BASIC26_VALUE_TYPE_INT or lhs.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                state.push(Value.fromInt(rhs.as.int_val ^ lhs.as.int_val)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_BIT_NOT => {
                const value = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (value.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                state.push(Value.fromInt(~value.as.int_val)) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_SHL => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (rhs.type != c.BASIC26_VALUE_TYPE_INT or lhs.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                if (rhs.as.int_val < 0 or rhs.as.int_val >= @bitSizeOf(c.basic26_IntType)) {
                    return .{ .err = ExecuteError.InvalidBitShift };
                }

                state.push(Value.fromInt(std.math.shl(c.basic26_IntType, lhs.as.int_val, rhs.as.int_val))) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_SHR => {
                const rhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const lhs = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (rhs.type != c.BASIC26_VALUE_TYPE_INT or lhs.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                if (rhs.as.int_val < 0 or rhs.as.int_val >= @bitSizeOf(c.basic26_IntType)) {
                    return .{ .err = ExecuteError.InvalidBitShift };
                }

                state.push(Value.fromInt(std.math.shr(c.basic26_IntType, lhs.as.int_val, rhs.as.int_val))) catch {
                    return .{ .err = ExecuteError.StackOverflow };
                };
            },
            c.BASIC26_OPCODE_JUMP => {
                state.ip = op.imm.as_address;
            },
            c.BASIC26_OPCODE_JUMP_IF_FALSE => {
                const val = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                const is_false = if (val.type == c.BASIC26_VALUE_TYPE_INT)
                    val.as.int_val == 0
                else
                    return .{ .err = ExecuteError.TypeMismatch };

                if (is_false) {
                    state.ip = op.imm.as_address;
                }
            },
            c.BASIC26_OPCODE_CALL => {
                const args_count = state.pop() orelse {
                    return .{ .err = ExecuteError.StackUnderflow };
                };

                if (args_count.type != c.BASIC26_VALUE_TYPE_INT) {
                    return .{ .err = ExecuteError.TypeMismatch };
                }

                if (state.sp < args_count.as.int_val or args_count.as.int_val < 0) {
                    return .{ .err = ExecuteError.StackUnderflow };
                }

                const callback = this.function_callbacks.get(op.imm.as_symbol) orelse {
                    return .{ .err = ExecuteError.UndefinedFunction };
                };

                const info: c.basic26_CallInfo = .{
                    .vm = @ptrCast(@alignCast(this)),
                    .state = @ptrCast(state),
                    .script = @ptrCast(script),
                    .userdata = userdata,
                    .function_name = op.imm.as_symbol,
                };

                const argc = @as(usize, @intCast(args_count.as.int_val));
                const args = state.stack[state.sp - argc ..];

                const res = callback.?(&info, argc, args.ptr);

                for (0..argc) |_| {
                    _ = state.pop();
                }

                return switch (res) {
                    c.BASIC26_FUNCTION_RESULT_OK => .{ .ok = {} },
                    c.BASIC26_FUNCTION_RESULT_YIELD => .{ .yield = {} },
                    else => .{ .err = ExecuteError.FunctionError },
                };
            },
            c.BASIC26_OPCODE_POP => {
                _ = state.pop();
            },
            else => unreachable,
        }

        return .{ .ok = {} };
    }
};

export fn basic26_CreateVmOptions_zeroed() callconv(.c) c.basic26_CreateVmOptions {
    return .{};
}

export fn basic26_Vm_create(
    options: ?*const c.basic26_CreateVmOptions,
    out: ?*?*c.basic26_Vm,
) callconv(.c) c.basic26_Result {
    std.debug.assert(options != null);
    std.debug.assert(out != null);

    var allocator = if (options.?.alloc == null)
        if (builtin.os.tag == .freestanding)
            Allocator.fromFailing()
        else if (builtin.mode == .Debug)
            Allocator.fromDebug()
        else
            Allocator.fromDefault()
    else
        Allocator.fromUser(options.?.alloc.*.userdata, options.?.alloc.*.alloc, options.?.alloc.*.free);

    const vm = allocator.allocator().create(Vm) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    vm.* = .init(allocator);
    out.?.* = @ptrCast(vm);

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Vm_destroy(c_vm: ?*c.basic26_Vm) callconv(.c) void {
    if (c_vm == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    var allocator = vm.allocator;

    vm.deinit();
    allocator.allocator().destroy(vm);
}

export fn basic26_Vm_alloc(
    c_vm: ?*c.basic26_Vm,
    len: usize,
    alignment: usize,
) callconv(.c) ?*anyopaque {
    std.debug.assert(c_vm != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    return vm.allocator.allocator().rawAlloc(len, .fromByteUnits(alignment), 0);
}

export fn basic26_Vm_free(
    c_vm: ?*c.basic26_Vm,
    ptr: ?*anyopaque,
    len: usize,
    alignment: usize,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    if (ptr == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const memory: []u8 = @as([*]u8, @ptrCast(@alignCast(ptr.?)))[0..len];

    vm.allocator.allocator().rawFree(memory, .fromByteUnits(alignment), 0);
}

export fn basic26_ClearVmOptions_zeroed() callconv(.c) c.basic26_ClearVmOptions {
    return .{};
}

export fn basic26_Vm_clear(
    c_vm: ?*c.basic26_Vm,
    options: ?*const c.basic26_ClearVmOptions,
) callconv(.c) void {
    std.debug.assert(c_vm != null);
    std.debug.assert(options != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    if (options.?.clear_strings) {
        vm.strings.clear();
    }

    if (options.?.clear_functions) {
        vm.function_callbacks.clearRetainingCapacity();
    }
}

export fn basic26_Vm_get_string(
    c_vm: ?*c.basic26_Vm,
    string_id: c.basic26_StringId,
    out: ?*?[*]const u8,
    out_len: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(out != null);
    std.debug.assert(out_len != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const string = vm.strings.getByid(string_id) orelse {
        return c.BASIC26_RESULT_NOT_FOUND;
    };

    out.?.* = string.ptr;
    out_len.?.* = string.len;

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Vm_get_string_id(
    c_vm: ?*c.basic26_Vm,
    c_string: ?[*]const u8,
    string_len: usize,
    create: bool,
    out: ?*c.basic26_StringId,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(c_string != null);
    std.debug.assert(out != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const string = c_string.?[0..string_len];

    if (create) {
        out.?.* = vm.strings.getOrPut(vm.allocator.allocator(), string) catch {
            return c.BASIC26_RESULT_OUT_OF_MEMORY;
        };
    } else {
        out.?.* = vm.strings.get(string) orelse {
            return c.BASIC26_RESULT_NOT_FOUND;
        };
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Vm_register_function(
    c_vm: ?*c.basic26_Vm,
    options: ?*const c.basic26_RegisterFunctionOptions,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(options != null);
    std.debug.assert(options.?.callback != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    vm.function_callbacks.put(vm.allocator.allocator(), options.?.name, options.?.callback) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Vm_unregister_function(
    c_vm: ?*c.basic26_Vm,
    symbol_id: c.basic26_SymbolId,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    _ = vm.function_callbacks.remove(symbol_id);
}

export fn basic26_RunLimits_zeroed() callconv(.c) c.basic26_RunLimits {
    return .{};
}

export fn basic26_RuntimeErrorInfo_zeroed() callconv(.c) c.basic26_RuntimeErrorInfo {
    return .{};
}

export fn basic26_RunOptions_zeroed() callconv(.c) c.basic26_RunOptions {
    return .{};
}

fn RunLoop(comptime check_ops_limit: bool, comptime check_time_limit: bool) type {
    return struct {
        fn run(
            vm: *Vm,
            state: *State,
            script: *const Script,
            limits: *const c.basic26_RunLimits,
            userdata: ?*anyopaque,
            error_out: *c.basic26_RuntimeErrorInfo,
        ) c.basic26_Result {
            if (state.ip >= script.ops.items.len) {
                return c.BASIC26_RESULT_OK;
            }

            const start_time: std.Io.Timestamp = if (comptime check_time_limit)
                std.Io.Timestamp.now(vm.io.io(), .real)
            else
                undefined;

            var ops_executed: usize = 0;
            var ops_since_time_check: usize = if (comptime check_time_limit)
                limits.time_check_interval
            else
                0;

            while (state.ip < script.ops.items.len) {
                @prefetch(script.ops.items, .{});

                if (comptime check_ops_limit) {
                    if (limits.max_ops > 0 and ops_executed >= limits.max_ops) {
                        return c.BASIC26_RESULT_OUT_OF_LIMITS;
                    }
                }

                if (comptime check_time_limit) {
                    const interval = limits.time_check_interval;

                    if (interval == 0 or ops_since_time_check >= interval) {
                        const current_time = std.Io.Timestamp.now(vm.io.io(), .real);
                        const elapsed = current_time.nanoseconds - start_time.nanoseconds;

                        if (elapsed >= limits.max_time_ns) {
                            return c.BASIC26_RESULT_OUT_OF_LIMITS;
                        }

                        ops_since_time_check = 0;
                    }
                }

                const op = script.ops.items[state.ip];
                const prev_ip = state.ip;

                state.ip += 1;
                ops_executed += 1;

                if (check_time_limit) {
                    ops_since_time_check += 1;
                }

                const is_call_op = op.code == c.BASIC26_OPCODE_CALL;

                switch (vm.execute(state, script, userdata, op)) {
                    .ok => {},
                    .yield => return c.BASIC26_RESULT_YIELDED,
                    .err => |err| {
                        error_out.ip = prev_ip;
                        error_out.code = switch (err) {
                            ExecuteError.DivisionByZero => c.BASIC26_RUNTIME_ERROR_DIVISION_BY_ZERO,
                            ExecuteError.TypeMismatch => c.BASIC26_RUNTIME_ERROR_TYPE_MISMATCH,
                            ExecuteError.StackUnderflow => c.BASIC26_RUNTIME_ERROR_STACK_UNDERFLOW,
                            ExecuteError.StackOverflow => c.BASIC26_RUNTIME_ERROR_STACK_OVERFLOW,
                            ExecuteError.UndefinedFunction => c.BASIC26_RUNTIME_ERROR_UNDEFINED_FUNCTION,
                            ExecuteError.UndefinedVariable => c.BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE,
                            ExecuteError.InvalidBitShift => c.BASIC26_RUNTIME_ERROR_INVALID_BIT_SHIFT,
                            ExecuteError.FunctionError => c.BASIC26_RUNTIME_ERROR_FUNCTION,
                            else => c.BASIC26_RUNTIME_ERROR_UNKNOWN,
                        };

                        return c.BASIC26_RESULT_RUNTIME_ERROR;
                    },
                }

                // Always check time after a CALL opcode completes, regardless of
                // time_check_interval, so that long-running native callbacks
                // cannot bypass the time limit.
                if (comptime check_time_limit) {
                    if (is_call_op) {
                        const current_time = std.Io.Timestamp.now(vm.io.io(), .real);
                        const elapsed = current_time.nanoseconds - start_time.nanoseconds;

                        if (elapsed >= limits.max_time_ns) {
                            return c.BASIC26_RESULT_OUT_OF_LIMITS;
                        }

                        ops_since_time_check = 0;
                    }
                }
            }

            return c.BASIC26_RESULT_OK;
        }
    };
}

export fn basic26_Vm_run(
    c_vm: ?*c.basic26_Vm,
    options: ?*const c.basic26_RunOptions,
    error_out: ?*c.basic26_RuntimeErrorInfo,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(options != null);
    std.debug.assert(options.?.limits != null);
    std.debug.assert(error_out != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const state: *State = @ptrCast(@alignCast(options.?.state.?));
    const script: *const Script = @ptrCast(@alignCast(options.?.script.?));
    const limits: *const c.basic26_RunLimits = @ptrCast(@alignCast(options.?.limits));

    const check_ops = limits.max_ops > 0;
    const check_time = limits.max_time_ns > 0;

    if (check_ops and check_time) {
        return RunLoop(true, true).run(vm, state, script, limits, options.?.userdata, error_out.?);
    } else if (check_ops) {
        return RunLoop(true, false).run(vm, state, script, limits, options.?.userdata, error_out.?);
    } else if (check_time) {
        return RunLoop(false, true).run(vm, state, script, limits, options.?.userdata, error_out.?);
    } else {
        return RunLoop(false, false).run(vm, state, script, limits, options.?.userdata, error_out.?);
    }
}

const State = struct {
    vm: *Vm,
    ip: usize = 0,
    sp: usize = 0,
    stack: [c.BASIC26_STACK_CAPACITY]c.basic26_Value = undefined,
    vars: std.ArrayList(c.basic26_Value) = .empty,

    pub inline fn init(vm: *Vm) State {
        return .{ .vm = vm };
    }

    pub inline fn deinit(this: *State, allocator: std.mem.Allocator) void {
        this.vars.deinit(allocator);
    }

    pub inline fn setVar(
        this: *State,
        allocator: std.mem.Allocator,
        id: c.basic26_SymbolId,
        value: c.basic26_Value,
        create: bool,
    ) error{OutOfMemory}!bool {
        if (id >= this.vars.items.len) {
            if (!create) {
                return false;
            }

            try this.vars.appendNTimes(allocator, Value.fromUndefined(), id + 1 - this.vars.items.len);
        }

        if (!create and this.vars.items[id].type == c.BASIC26_VALUE_TYPE_FORCE_32BIT) {
            return false;
        }

        this.vars.items[id] = value;

        return true;
    }

    pub inline fn getVar(this: *const State, id: c.basic26_SymbolId) ?c.basic26_Value {
        if (id >= this.vars.items.len) {
            return null;
        }

        if (this.vars.items[id].type == c.BASIC26_VALUE_TYPE_FORCE_32BIT) {
            return null;
        }

        return this.vars.items[id];
    }

    pub inline fn unsetVar(this: *State, id: c.basic26_SymbolId) void {
        if (id >= this.vars.items.len) {
            return;
        }

        this.vars.items[id].type = c.BASIC26_VALUE_TYPE_FORCE_32BIT;
    }

    pub inline fn push(this: *State, value: c.basic26_Value) error{OutOfMemory}!void {
        if (this.sp >= this.stack.len) {
            return error.OutOfMemory;
        }

        this.stack[this.sp] = value;
        this.sp += 1;
    }

    pub inline fn pop(this: *State) ?c.basic26_Value {
        if (this.sp == 0) {
            return null;
        }

        const ret = this.stack[this.sp - 1];
        this.sp -= 1;

        return ret;
    }
};

export fn basic26_CreateStateOptions_zeroed() callconv(.c) c.basic26_CreateStateOptions {
    return .{};
}

export fn basic26_State_create(
    options: ?*const c.basic26_CreateStateOptions,
    out: ?*?*c.basic26_State,
) callconv(.c) c.basic26_Result {
    std.debug.assert(options != null);
    std.debug.assert(options.?.vm != null);

    const vm: *Vm = @ptrCast(@alignCast(options.?.vm.?));
    const state = vm.allocator.allocator().create(State) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    state.* = .init(vm);
    out.?.* = @ptrCast(state);

    return c.BASIC26_RESULT_OK;
}

export fn basic26_State_destroy(
    c_state: ?*c.basic26_State,
    c_vm: ?*c.basic26_Vm,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    if (c_state == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const state: *State = @ptrCast(@alignCast(c_state.?));

    state.deinit(vm.allocator.allocator());
    vm.allocator.allocator().destroy(state);
}

export fn basic26_ClearStateOptions_zeroed() callconv(.c) c.basic26_ClearStateOptions {
    return .{};
}

export fn basic26_State_clear(
    c_state: ?*c.basic26_State,
    options: ?*const c.basic26_ClearStateOptions,
) callconv(.c) void {
    std.debug.assert(c_state != null);
    std.debug.assert(options != null);

    const state: *State = @ptrCast(@alignCast(c_state.?));

    if (options.?.clear_stack) {
        state.sp = 0;
    }

    if (options.?.clear_vars) {
        state.vars.clearRetainingCapacity();
    }
}

export fn basic26_State_get_ip(
    c_state: ?*const c.basic26_State,
    out_ip: ?*usize,
) callconv(.c) void {
    std.debug.assert(c_state != null);
    std.debug.assert(out_ip != null);

    const state: *const State = @ptrCast(@alignCast(c_state.?));
    out_ip.?.* = state.ip;
}

export fn basic26_State_set_ip(
    c_state: ?*c.basic26_State,
    ip: usize,
) callconv(.c) void {
    std.debug.assert(c_state != null);

    const state: *State = @ptrCast(@alignCast(c_state.?));
    state.ip = ip;
}

export fn basic26_State_get_stack_capacity() callconv(.c) usize {
    return c.BASIC26_STACK_CAPACITY;
}

export fn basic26_State_get_var(
    c_state: ?*const c.basic26_State,
    symbold_id: c.basic26_SymbolId,
    out_value: ?*c.basic26_Value,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_state != null);
    std.debug.assert(out_value != null);

    const state: *const State = @ptrCast(@alignCast(c_state.?));

    out_value.?.* = state.getVar(symbold_id) orelse {
        return c.BASIC26_RESULT_NOT_FOUND;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_State_set_var(
    c_state: ?*c.basic26_State,
    symbold_id: c.basic26_SymbolId,
    value: ?*const c.basic26_Value,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_state != null);
    std.debug.assert(value != null);

    const state: *State = @ptrCast(@alignCast(c_state.?));

    _ = state.setVar(state.vm.allocator.allocator(), symbold_id, value.?.*, true) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_State_unset_var(
    c_state: ?*c.basic26_State,
    symbold_id: c.basic26_SymbolId,
) callconv(.c) void {
    std.debug.assert(c_state != null);

    const state: *State = @ptrCast(@alignCast(c_state.?));

    state.unsetVar(symbold_id);
}

export fn basic26_State_var_next(
    c_state: ?*const c.basic26_State,
    cur: ?*c.basic26_SymbolId,
    next: ?*c.basic26_SymbolId,
) callconv(.c) bool {
    std.debug.assert(c_state != null);
    std.debug.assert(next != null);

    const state: *const State = @ptrCast(@alignCast(c_state.?));
    const start = if (cur == null)
        0
    else
        cur.?.* + 1;

    if (start >= state.vars.items.len) {
        return false;
    }

    for (state.vars.items[start..], start..) |val, id| {
        if (val.type == c.BASIC26_VALUE_TYPE_FORCE_32BIT) {
            continue;
        }

        next.?.* = id;

        return true;
    }

    return false;
}

export fn basic26_Script_create(
    c_vm: ?*c.basic26_Vm,
    out: ?*?*c.basic26_Script,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(out != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const script = vm.allocator.allocator().create(Script) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    script.* = .init(&vm.strings);
    out.?.* = @ptrCast(script);

    return c.BASIC26_RESULT_OK;
}

const Keyword = enum {
    @"if",
    @"else",
    elseif,
    endif,
    @"while",
    endwhile,
    goto,
    @"and",
    @"or",
    not,
    null,
};

const keyword_map = std.StaticStringMap(Keyword).initComptime(.{
    .{ "IF", .@"if" },
    .{ "ELSE", .@"else" },
    .{ "ELSEIF", .elseif },
    .{ "ENDIF", .endif },
    .{ "WHILE", .@"while" },
    .{ "ENDWHILE", .endwhile },
    .{ "GOTO", .goto },
    .{ "AND", .@"and" },
    .{ "OR", .@"or" },
    .{ "NOT", .not },
    .{ "NULL", .null },
});

const Token = struct {
    kind: TokenKind,
    pos: usize,

    pub inline fn init(kind: TokenKind, pos: usize) Token {
        return .{
            .kind = kind,
            .pos = pos,
        };
    }
};

const TokenKind = union(enum) {
    ident: []const u8,
    int: c.basic26_IntType,
    float: c.basic26_FloatType,
    string_literal: []const u8,
    symbol_literal: []const u8,
    address_literal: []const u8,
    op: []const u8,
    lparen,
    rparen,
    comma,
    colon,
    keyword: Keyword,
    eof,
};

const OpInfo = struct {
    precedence: u8,
    right_assoc: bool,
    code: c.basic26_Opcode,
};

const operator_map = std.StaticStringMap(OpInfo).initComptime(.{
    .{ "OR", @as(OpInfo, .{ .precedence = 1, .right_assoc = false, .code = c.BASIC26_OPCODE_BOOL_OR }) },
    .{ "AND", @as(OpInfo, .{ .precedence = 2, .right_assoc = false, .code = c.BASIC26_OPCODE_BOOL_AND }) },
    .{ "==", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_EQ }) },
    .{ "!=", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_NEQ }) },
    .{ "<", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_LT }) },
    .{ ">", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_GT }) },
    .{ "<=", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_LTE }) },
    .{ ">=", @as(OpInfo, .{ .precedence = 3, .right_assoc = false, .code = c.BASIC26_OPCODE_GTE }) },
    .{ "|", @as(OpInfo, .{ .precedence = 4, .right_assoc = false, .code = c.BASIC26_OPCODE_BIT_OR }) },
    .{ "^", @as(OpInfo, .{ .precedence = 4, .right_assoc = false, .code = c.BASIC26_OPCODE_BIT_XOR }) },
    .{ "&", @as(OpInfo, .{ .precedence = 4, .right_assoc = false, .code = c.BASIC26_OPCODE_BIT_AND }) },
    .{ "<<", @as(OpInfo, .{ .precedence = 5, .right_assoc = false, .code = c.BASIC26_OPCODE_SHL }) },
    .{ ">>", @as(OpInfo, .{ .precedence = 5, .right_assoc = false, .code = c.BASIC26_OPCODE_SHR }) },
    .{ "+", @as(OpInfo, .{ .precedence = 6, .right_assoc = false, .code = c.BASIC26_OPCODE_ADD }) },
    .{ "-", @as(OpInfo, .{ .precedence = 6, .right_assoc = false, .code = c.BASIC26_OPCODE_SUB }) },
    .{ "*", @as(OpInfo, .{ .precedence = 7, .right_assoc = false, .code = c.BASIC26_OPCODE_MUL }) },
    .{ "/", @as(OpInfo, .{ .precedence = 7, .right_assoc = false, .code = c.BASIC26_OPCODE_DIV }) },
    .{ "%", @as(OpInfo, .{ .precedence = 7, .right_assoc = false, .code = c.BASIC26_OPCODE_REM }) },
    .{ "NOT", @as(OpInfo, .{ .precedence = 8, .right_assoc = true, .code = c.BASIC26_OPCODE_BOOL_NOT }) },
    .{ "~", @as(OpInfo, .{ .precedence = 8, .right_assoc = true, .code = c.BASIC26_OPCODE_BIT_NOT }) },
    .{ "NEG", @as(OpInfo, .{ .precedence = 8, .right_assoc = true, .code = c.BASIC26_OPCODE_NEG }) },
});

const special_float_set = std.StaticStringMap(void).initComptime(.{
    .{ "NAN", {} },
    .{ "INF", {} },
});

const Lexer = struct {
    src: []const u8,
    pos: usize,
    expect_value: bool,

    pub inline fn init(src: []const u8) Lexer {
        return .{ .src = src, .pos = 0, .expect_value = true };
    }

    inline fn skipWhitespace(this: *Lexer) void {
        while (this.pos < this.src.len and std.ascii.isWhitespace(this.src[this.pos])) : (this.pos += 1) {}
    }

    inline fn parseFloatLiteral(this: *Lexer, word: []const u8) ?TokenKind {
        if (!special_float_set.has(word)) {
            return null;
        }

        this.expect_value = false;
        const val = std.fmt.parseFloat(c.basic26_FloatType, word) catch {
            unreachable;
        };

        return .{ .float = val };
    }

    pub fn next(this: *Lexer) Script.ParseError!Token {
        this.skipWhitespace();

        if (this.pos >= this.src.len) {
            return .init(.eof, this.pos);
        }

        // Skip line comments (// ...)
        if (this.src[this.pos] == '/' and this.pos + 1 < this.src.len and this.src[this.pos + 1] == '/') {
            while (this.pos < this.src.len and this.src[this.pos] != '\n') : (this.pos += 1) {}

            this.skipWhitespace();

            if (this.pos >= this.src.len) {
                return .init(.eof, this.pos);
            }
        }

        const ch = this.src[this.pos];

        if (ch == '"') {
            const start = this.pos;
            this.pos += 1;

            while (this.pos < this.src.len and this.src[this.pos] != '"') {
                if (this.src[this.pos] == '\\') {
                    this.pos += 1;
                }

                this.pos += 1;
            }

            if (this.pos >= this.src.len) {
                return Script.ParseError.BadStringLiteral;
            }

            this.pos += 1;

            this.expect_value = false;

            return .init(.{ .string_literal = this.src[start..this.pos] }, start);
        }

        const is_symbol_literal = ch == '$' and this.pos + 1 < this.src.len;
        const is_address_literal = ch == '@' and this.pos + 1 < this.src.len;

        // Identifier, keyword, or special float literal (NAN / INF)
        if (is_address_literal or is_symbol_literal or (std.ascii.isAlphabetic(ch) or ch == '_')) {
            if (is_symbol_literal or is_address_literal) {
                this.pos += 1;
            }

            const start = this.pos;

            while (this.pos < this.src.len and (std.ascii.isAlphanumeric(this.src[this.pos]) or this.src[this.pos] == '_')) {
                this.pos += 1;
            }

            const word = this.src[start..this.pos];

            // NAN and INF are float literals, not keywords
            if (this.parseFloatLiteral(word)) |tok| {
                return .init(tok, start);
            }

            if (keyword_map.get(word)) |kw| {
                // AND / OR / NOT are expression operators - emit as op tokens
                // so parseExpr handles them uniformly alongside symbolic operators.
                if (kw == .@"and" or kw == .@"or" or kw == .not) {
                    this.expect_value = true;

                    return .init(.{ .op = word }, start);
                }

                this.expect_value = true;

                return .init(.{ .keyword = kw }, start);
            }

            this.expect_value = false;

            if (is_address_literal) {
                return .init(.{ .address_literal = word }, start);
            } else if (is_symbol_literal) {
                return .init(.{ .symbol_literal = word }, start);
            } else {
                return .init(.{ .ident = word }, start);
            }
        }

        // Number literal (with unary minus disambiguation)
        const is_minus = ch == '-';
        const is_unary_minus = is_minus and this.expect_value;

        // -NAN / -INF
        if (is_unary_minus) {
            const peek_start = this.pos + 1;
            var peek_end = peek_start;

            while (peek_end < this.src.len and (std.ascii.isAlphanumeric(this.src[peek_end]) or this.src[peek_end] == '_')) {
                peek_end += 1;
            }

            if (peek_end > peek_start) {
                const word = this.src[peek_start..peek_end];

                if (special_float_set.has(word)) {
                    const start = this.pos;
                    this.pos = peek_end;
                    this.expect_value = false;

                    const val = std.fmt.parseFloat(c.basic26_FloatType, this.src[start..this.pos]) catch {
                        unreachable;
                    };

                    return .init(.{ .float = val }, peek_start);
                }
            }
        }

        const next_is_digit = this.pos + 1 < this.src.len and std.ascii.isDigit(this.src[this.pos + 1]);

        if (is_unary_minus and !next_is_digit) {
            this.pos += 1;
            this.expect_value = true;

            return .init(.{ .op = "NEG" }, this.pos - 1);
        }

        if (std.ascii.isDigit(ch) or (is_unary_minus and next_is_digit)) {
            const start = this.pos;
            var is_float = false;

            this.pos += 1;

            while (this.pos < this.src.len and (std.ascii.isDigit(this.src[this.pos]) or this.src[this.pos] == '.')) {
                if (this.src[this.pos] == '.') {
                    is_float = true;
                }

                this.pos += 1;
            }

            const num_str = this.src[start..this.pos];
            this.expect_value = false;

            if (is_float) {
                const val = std.fmt.parseFloat(c.basic26_FloatType, num_str) catch {
                    return Script.ParseError.BadNumberLiteral;
                };

                return .init(.{ .float = val }, start);
            } else {
                const val = std.fmt.parseInt(c.basic26_IntType, num_str, 10) catch {
                    return Script.ParseError.BadNumberLiteral;
                };

                return .init(.{ .int = val }, start);
            }
        }

        // Single-character delimiters
        if (ch == '(') {
            this.pos += 1;
            this.expect_value = true;

            return .init(.lparen, this.pos - 1);
        }

        if (ch == ')') {
            this.pos += 1;
            this.expect_value = false;

            return .init(.rparen, this.pos - 1);
        }

        if (ch == ',') {
            this.pos += 1;
            this.expect_value = true;

            return .init(.comma, this.pos - 1);
        }

        if (ch == ':') {
            this.pos += 1;
            this.expect_value = true;

            return .init(.colon, this.pos - 1);
        }

        // Operator sequence (e.g. +, -, ==, !=, <=, >=, <<, >>, etc.)
        const start = this.pos;

        while (this.pos < this.src.len) {
            const chr = this.src[this.pos];

            if (std.ascii.isAlphanumeric(chr) or
                std.ascii.isWhitespace(chr) or
                chr == '(' or
                chr == ')' or
                chr == '"' or
                chr == ',' or
                chr == ':' or
                chr == '@')
            {
                break;
            }

            this.pos += 1;
        }

        if (this.pos == start) {
            return Script.ParseError.BadSymbolLiteral;
        }

        this.expect_value = true;

        return .init(.{ .op = this.src[start..this.pos] }, start);
    }
};

inline fn isValidSymbol(bytes: []const u8) bool {
    if (bytes.len == 0) {
        return false;
    }

    for (bytes, 0..) |chr, i| {
        switch (chr) {
            '_', 'a'...'z', 'A'...'Z' => {},
            '0'...'9' => if (i == 0) return false,
            else => return false,
        }
    }

    return true;
}

const Script = struct {
    strings: *Strings,
    ops: std.ArrayList(c.basic26_Op) = .empty,
    labels: std.StringHashMapUnmanaged(usize) = .empty,
    parser_state: ParserState = .{},

    pub inline fn init(strings: *Strings) Script {
        return .{ .strings = strings };
    }

    pub inline fn deinit(this: *Script, allocator: std.mem.Allocator) void {
        this.ops.deinit(allocator);
        this.labels.deinit(allocator);
        this.parser_state.deinit(allocator);
    }

    pub const ParseError = error{
        OutOfMemory,
        BadStringLiteral,
        BadNumberLiteral,
        BadSymbolLiteral,
        TooManyStrings,
        TooBigInt,
        SyntaxError,
        UnknownOperator,
        ExpectedOp,
        TooManyArgs,
    };

    pub inline fn appendOp(
        this: *Script,
        allocator: std.mem.Allocator,
        op: c.basic26_Op,
        pos: usize,
        debug_info: ?*DebugInfo,
    ) error{OutOfMemory}!void {
        try this.ops.append(allocator, op);
        errdefer _ = this.ops.pop();

        if (debug_info != null) {
            try debug_info.?.ops_source_map.append(allocator, pos);
        }
    }

    pub inline fn parseString(
        this: *Script,
        allocator: std.mem.Allocator,
        str: []const u8,
        limits: *const c.basic26_ScriptLimits,
    ) ParseError!c.basic26_StringId {
        if (limits.max_strings != 0 and this.strings.count() >= limits.max_strings) {
            return ParseError.TooManyStrings;
        }

        const string = std.zig.string_literal.parseAlloc(allocator, str) catch {
            return ParseError.BadStringLiteral;
        };
        errdefer allocator.free(string);

        return try this.strings.getOrPutOwned(allocator, string);
    }

    pub inline fn parseSymbol(
        this: *Script,
        allocator: std.mem.Allocator,
        str: []const u8,
        limits: *const c.basic26_ScriptLimits,
    ) ParseError!c.basic26_StringId {
        if (limits.max_strings != 0 and this.strings.count() >= limits.max_strings) {
            return ParseError.TooManyStrings;
        }

        if (!isValidSymbol(str)) {
            return ParseError.BadSymbolLiteral;
        }

        return try this.strings.getOrPut(allocator, str);
    }

    pub inline fn dump(this: *const Script, allocator: std.mem.Allocator) error{ OutOfMemory, WriteFailed }![]u8 {
        var writer: std.Io.Writer.Allocating = .init(allocator);
        errdefer writer.deinit();

        var strings_iter = this.strings.id_map.iterator();

        while (strings_iter.next()) |entry| {
            try writer.writer.print("{d:0>4}: {s}\n", .{ entry.value_ptr.*, entry.key_ptr.* });
        }

        try writer.writer.print("---\n", .{});

        for (this.ops.items) |op| {
            switch (op.code) {
                c.BASIC26_OPCODE_PUSH_INT => {
                    try writer.writer.print("PUSH_INT {d}\n", .{op.imm.as_int});
                },
                c.BASIC26_OPCODE_PUSH_FLOAT => {
                    try writer.writer.print("PUSH_FLOAT {d}\n", .{op.imm.as_float});
                },
                c.BASIC26_OPCODE_PUSH_STRING => {
                    const str = this.strings.str_map.get(op.imm.as_string).?;
                    try writer.writer.print("PUSH_STRING \"{s}\"\n", .{str});
                },
                c.BASIC26_OPCODE_PUSH_SYMBOL => {
                    const str = this.strings.str_map.get(op.imm.as_symbol).?;
                    try writer.writer.print("PUSH_SYMBOL \"{s}\"\n", .{str});
                },
                c.BASIC26_OPCODE_PUSH_ADDRESS => {
                    try writer.writer.print("PUSH_ADDRESS {d}\n", .{op.imm.as_address});
                },
                c.BASIC26_OPCODE_PUSH_NULL => {
                    try writer.writer.print("PUSH_NULL\n", .{});
                },
                c.BASIC26_OPCODE_LOAD => {
                    const str = this.strings.str_map.get(op.imm.as_symbol).?;
                    try writer.writer.print("LOAD \"{s}\"\n", .{str});
                },
                c.BASIC26_OPCODE_STORE => {
                    const str = this.strings.str_map.get(op.imm.as_symbol).?;
                    try writer.writer.print("STORE \"{s}\"\n", .{str});
                },
                c.BASIC26_OPCODE_ADD => {
                    try writer.writer.print("ADD\n", .{});
                },
                c.BASIC26_OPCODE_SUB => {
                    try writer.writer.print("SUB\n", .{});
                },
                c.BASIC26_OPCODE_MUL => {
                    try writer.writer.print("MUL\n", .{});
                },
                c.BASIC26_OPCODE_DIV => {
                    try writer.writer.print("DIV\n", .{});
                },
                c.BASIC26_OPCODE_REM => {
                    try writer.writer.print("REM\n", .{});
                },
                c.BASIC26_OPCODE_NEG => {
                    try writer.writer.print("NEG\n", .{});
                },
                c.BASIC26_OPCODE_EQ => {
                    try writer.writer.print("EQ\n", .{});
                },
                c.BASIC26_OPCODE_NEQ => {
                    try writer.writer.print("NEQ\n", .{});
                },
                c.BASIC26_OPCODE_LT => {
                    try writer.writer.print("LT\n", .{});
                },
                c.BASIC26_OPCODE_GT => {
                    try writer.writer.print("GT\n", .{});
                },
                c.BASIC26_OPCODE_LTE => {
                    try writer.writer.print("LTE\n", .{});
                },
                c.BASIC26_OPCODE_GTE => {
                    try writer.writer.print("GTE\n", .{});
                },
                c.BASIC26_OPCODE_BOOL_AND => {
                    try writer.writer.print("BOOL_AND\n", .{});
                },
                c.BASIC26_OPCODE_BOOL_OR => {
                    try writer.writer.print("BOOL_OR\n", .{});
                },
                c.BASIC26_OPCODE_BOOL_NOT => {
                    try writer.writer.print("BOOL_NOT\n", .{});
                },
                c.BASIC26_OPCODE_BIT_AND => {
                    try writer.writer.print("BIT_AND\n", .{});
                },
                c.BASIC26_OPCODE_BIT_OR => {
                    try writer.writer.print("BIT_OR\n", .{});
                },
                c.BASIC26_OPCODE_BIT_XOR => {
                    try writer.writer.print("BIT_XOR\n", .{});
                },
                c.BASIC26_OPCODE_BIT_NOT => {
                    try writer.writer.print("BIT_NOT\n", .{});
                },
                c.BASIC26_OPCODE_SHL => {
                    try writer.writer.print("SHL\n", .{});
                },
                c.BASIC26_OPCODE_SHR => {
                    try writer.writer.print("SHR\n", .{});
                },
                c.BASIC26_OPCODE_JUMP => {
                    try writer.writer.print("JUMP {d}\n", .{op.imm.as_address});
                },
                c.BASIC26_OPCODE_JUMP_IF_FALSE => {
                    try writer.writer.print("JUMP_IF_FALSE {d}\n", .{op.imm.as_address});
                },
                c.BASIC26_OPCODE_CALL => {
                    const str = this.strings.str_map.get(op.imm.as_symbol).?;
                    try writer.writer.print("CALL \"{s}\"\n", .{str});
                },
                c.BASIC26_OPCODE_POP => {
                    try writer.writer.print("POP\n", .{});
                },
                else => {
                    try writer.writer.print("UNDEFINED\n", .{});
                },
            }
        }

        return try writer.toOwnedSlice();
    }
};

const ControlStmt = union(enum) {
    if_chain: struct {
        last_jump_if_false_idx: usize,
        last_jump_if_false_resolved: bool,
        end_jumps: std.ArrayList(usize),
    },
    while_stmt: struct { start_idx: usize, jump_idx: usize },
};

const PendingJump = struct {
    op_index: usize,
    label_name: []const u8,
    kind: enum { jump, push_address },
};

const ParserState = struct {
    const OpPair = struct {
        op: []const u8,
        pos: usize,

        pub inline fn init(op: []const u8, pos: usize) OpPair {
            return .{
                .op = op,
                .pos = pos,
            };
        }
    };

    tokens: std.ArrayList(Token) = .empty,
    op_stack: std.ArrayList(OpPair) = .empty,
    ctrl_stack: std.ArrayList(ControlStmt) = .empty,
    pending_jumps: std.ArrayList(PendingJump) = .empty,

    pub inline fn init() ParserState {
        return .{};
    }

    pub fn deinit(this: *ParserState, allocator: std.mem.Allocator) void {
        this.tokens.deinit(allocator);
        this.op_stack.deinit(allocator);

        for (this.ctrl_stack.items) |*item| {
            if (item.* == .if_chain) {
                item.if_chain.end_jumps.deinit(allocator);
            }
        }

        this.ctrl_stack.deinit(allocator);
        this.pending_jumps.deinit(allocator);
    }

    pub fn parseExpr(
        this: *ParserState,
        allocator: std.mem.Allocator,
        script: *Script,
        tokens: []const Token,
        idx: *usize,
        limits: *const c.basic26_ScriptLimits,
        line_offset: usize,
        debug_info: ?*DebugInfo,
    ) Script.ParseError!void {
        this.op_stack.clearRetainingCapacity();

        while (idx.* < tokens.len) {
            const tok = tokens[idx.*];

            switch (tok.kind) {
                .int => {
                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_PUSH_INT,
                        .imm = .{ .as_int = tok.kind.int },
                    }, line_offset + tok.pos, debug_info);

                    idx.* += 1;
                },
                .float => {
                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_PUSH_FLOAT,
                        .imm = .{ .as_float = tok.kind.float },
                    }, line_offset + tok.pos, debug_info);

                    idx.* += 1;
                },
                .string_literal => {
                    const id = try script.parseString(allocator, tok.kind.string_literal, limits);

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_PUSH_STRING,
                        .imm = .{ .as_string = id },
                    }, line_offset + tok.pos, debug_info);

                    idx.* += 1;
                },
                .symbol_literal => {
                    const id = try script.parseSymbol(allocator, tok.kind.symbol_literal, limits);

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_PUSH_SYMBOL,
                        .imm = .{ .as_symbol = id },
                    }, line_offset + tok.pos, debug_info);

                    idx.* += 1;
                },
                .address_literal => {
                    const jump_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_PUSH_ADDRESS,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tok.pos, debug_info);

                    try this.pending_jumps.append(allocator, .{
                        .op_index = jump_idx,
                        .label_name = tok.kind.address_literal,
                        .kind = .push_address,
                    });

                    idx.* += 1;
                },
                .ident => {
                    const id = try script.parseSymbol(allocator, tok.kind.ident, limits);

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_LOAD,
                        .imm = .{ .as_symbol = id },
                    }, line_offset + tok.pos, debug_info);

                    idx.* += 1;
                },
                .lparen => {
                    try this.op_stack.append(allocator, .init("(", line_offset + tok.pos));

                    idx.* += 1;
                },
                .rparen => {
                    while (this.op_stack.items.len > 0 and !std.mem.eql(u8, this.op_stack.items[this.op_stack.items.len - 1].op, "(")) {
                        const info = operator_map.get(this.op_stack.pop().?.op) orelse {
                            return Script.ParseError.UnknownOperator;
                        };

                        try script.appendOp(allocator, .{
                            .code = @intCast(info.code),
                        }, line_offset + tok.pos, debug_info);
                    }

                    if (this.op_stack.items.len == 0) {
                        return Script.ParseError.BadSymbolLiteral;
                    }

                    _ = this.op_stack.pop();
                    idx.* += 1;
                },
                .op => {
                    const info = operator_map.get(tok.kind.op) orelse {
                        break;
                    };

                    while (this.op_stack.items.len > 0) {
                        const top = this.op_stack.items[this.op_stack.items.len - 1];

                        if (std.mem.eql(u8, top.op, "(")) {
                            break;
                        }

                        const top_info = operator_map.get(top.op) orelse {
                            break;
                        };

                        if ((info.right_assoc and info.precedence < top_info.precedence) or
                            (!info.right_assoc and info.precedence <= top_info.precedence))
                        {
                            const popped_info = operator_map.get(this.op_stack.pop().?.op) orelse {
                                return Script.ParseError.UnknownOperator;
                            };

                            try script.appendOp(allocator, .{
                                .code = @intCast(popped_info.code),
                            }, line_offset + tok.pos, debug_info);
                        } else {
                            break;
                        }
                    }

                    try this.op_stack.append(allocator, .init(tok.kind.op, line_offset + tok.pos));
                    idx.* += 1;
                },
                .keyword => {
                    if (tok.kind.keyword == .null) {
                        try script.appendOp(allocator, .{
                            .code = c.BASIC26_OPCODE_PUSH_NULL,
                        }, line_offset + tok.pos, debug_info);

                        idx.* += 1;
                    } else {
                        break;
                    }
                },
                .comma, .eof, .colon => break,
            }
        }

        // Drain remaining operators from the stack
        while (this.op_stack.items.len > 0) {
            const op_str = this.op_stack.pop().?;

            if (std.mem.eql(u8, op_str.op, "(")) {
                return Script.ParseError.BadSymbolLiteral;
            }

            const info = operator_map.get(op_str.op) orelse {
                return Script.ParseError.UnknownOperator;
            };

            try script.appendOp(allocator, .{
                .code = @intCast(info.code),
            }, op_str.pos, debug_info);
        }
    }

    pub fn parseLine(
        this: *ParserState,
        allocator: std.mem.Allocator,
        script: *Script,
        tokens: []const Token,
        i: *usize,
        limits: *const c.basic26_ScriptLimits,
        line_offset: usize,
        debug_info: ?*DebugInfo,
    ) Script.ParseError!void {
        if (tokens.len == 0) {
            return Script.ParseError.SyntaxError;
        }

        // Label definition: `name:`
        if (tokens[0].kind == .ident and tokens.len > 1 and tokens[1].kind == .colon) {
            try script.labels.put(allocator, tokens[0].kind.ident, script.ops.items.len);

            i.* += 2;
        } else if (tokens[0].kind == .keyword) {
            switch (tokens[0].kind.keyword) {
                .@"if" => {
                    i.* += 1;

                    try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);
                    const jump_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP_IF_FALSE,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[0].pos, debug_info);

                    try this.ctrl_stack.append(allocator, .{
                        .if_chain = .{
                            .last_jump_if_false_idx = jump_idx,
                            .last_jump_if_false_resolved = false,
                            .end_jumps = .empty,
                        },
                    });
                },
                .@"else" => {
                    var ctrl = this.ctrl_stack.pop() orelse {
                        return Script.ParseError.SyntaxError;
                    };

                    if (ctrl != .if_chain) {
                        return Script.ParseError.SyntaxError;
                    }

                    const is_else_if = i.* + 1 < tokens.len and
                        tokens[i.* + 1].kind == .keyword and
                        tokens[i.* + 1].kind.keyword == .@"if";

                    const jump_end_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[0].pos, debug_info);

                    try ctrl.if_chain.end_jumps.append(allocator, jump_end_idx);

                    script.ops.items[ctrl.if_chain.last_jump_if_false_idx].imm.as_address = script.ops.items.len;

                    if (is_else_if) {
                        i.* += 2;

                        try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);
                        const jump_idx = script.ops.items.len;

                        try script.appendOp(allocator, .{
                            .code = c.BASIC26_OPCODE_JUMP_IF_FALSE,
                            .imm = .{ .as_address = 0 },
                        }, line_offset + tokens[1].pos, debug_info);

                        try this.ctrl_stack.append(allocator, .{
                            .if_chain = .{
                                .last_jump_if_false_idx = jump_idx,
                                .last_jump_if_false_resolved = false,
                                .end_jumps = ctrl.if_chain.end_jumps,
                            },
                        });
                    } else {
                        try this.ctrl_stack.append(allocator, .{
                            .if_chain = .{
                                .last_jump_if_false_idx = ctrl.if_chain.last_jump_if_false_idx,
                                .last_jump_if_false_resolved = true,
                                .end_jumps = ctrl.if_chain.end_jumps,
                            },
                        });
                    }
                },
                .elseif => {
                    var ctrl = this.ctrl_stack.pop() orelse {
                        return Script.ParseError.SyntaxError;
                    };

                    if (ctrl != .if_chain) {
                        return Script.ParseError.SyntaxError;
                    }

                    const jump_end_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[0].pos, debug_info);

                    try ctrl.if_chain.end_jumps.append(allocator, jump_end_idx);

                    script.ops.items[ctrl.if_chain.last_jump_if_false_idx].imm.as_address = script.ops.items.len;

                    i.* += 1;
                    try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);

                    const jump_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP_IF_FALSE,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[0].pos, debug_info);

                    try this.ctrl_stack.append(allocator, .{
                        .if_chain = .{
                            .last_jump_if_false_idx = jump_idx,
                            .last_jump_if_false_resolved = false,
                            .end_jumps = ctrl.if_chain.end_jumps,
                        },
                    });
                },
                .endif => {
                    var ctrl = this.ctrl_stack.pop() orelse {
                        return Script.ParseError.SyntaxError;
                    };

                    if (ctrl != .if_chain) {
                        return Script.ParseError.SyntaxError;
                    }

                    if (!ctrl.if_chain.last_jump_if_false_resolved) {
                        script.ops.items[ctrl.if_chain.last_jump_if_false_idx].imm.as_address = script.ops.items.len;
                    }

                    for (ctrl.if_chain.end_jumps.items) |jump_idx| {
                        script.ops.items[jump_idx].imm.as_address = script.ops.items.len;
                    }

                    ctrl.if_chain.end_jumps.deinit(allocator);
                },
                .@"while" => {
                    const start_idx = script.ops.items.len;
                    i.* += 1;

                    try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);

                    const jump_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP_IF_FALSE,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[0].pos, debug_info);
                    try this.ctrl_stack.append(allocator, .{ .while_stmt = .{ .start_idx = start_idx, .jump_idx = jump_idx } });
                },
                .endwhile => {
                    const ctrl = this.ctrl_stack.pop() orelse {
                        return Script.ParseError.SyntaxError;
                    };

                    if (ctrl != .while_stmt) {
                        return Script.ParseError.SyntaxError;
                    }

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP,
                        .imm = .{ .as_address = ctrl.while_stmt.start_idx },
                    }, line_offset + tokens[0].pos, debug_info);

                    script.ops.items[ctrl.while_stmt.jump_idx].imm.as_address = script.ops.items.len;
                },
                .goto => {
                    i.* += 1;

                    if (i.* >= tokens.len or tokens[i.*].kind != .address_literal) {
                        return Script.ParseError.SyntaxError;
                    }

                    const label_name = tokens[i.*].kind.address_literal;
                    const jump_idx = script.ops.items.len;

                    try script.appendOp(allocator, .{
                        .code = c.BASIC26_OPCODE_JUMP,
                        .imm = .{ .as_address = 0 },
                    }, line_offset + tokens[i.*].pos, debug_info);

                    try this.pending_jumps.append(allocator, .{
                        .op_index = jump_idx,
                        .label_name = label_name,
                        .kind = .jump,
                    });

                    i.* += 1;
                },
                else => return Script.ParseError.SyntaxError,
            }
        } else if (tokens[0].kind == .ident) {
            if (i.* + 1 < tokens.len and tokens[i.* + 1].kind == .op and std.mem.eql(u8, tokens[i.* + 1].kind.op, "=")) {
                // Assignment: `ident = expr`
                const id = try script.parseSymbol(allocator, tokens[0].kind.ident, limits);

                i.* += 2;

                try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);

                try script.appendOp(allocator, .{
                    .code = c.BASIC26_OPCODE_STORE,
                    .imm = .{ .as_symbol = id },
                }, line_offset + tokens[0].pos, debug_info);
            } else {
                // Function call: `ident expr, expr, ...`
                const id = try script.parseSymbol(allocator, tokens[0].kind.ident, limits);
                var args_count: usize = 0;

                i.* += 1;

                while (i.* < tokens.len and tokens[i.*].kind != .eof) {
                    try this.parseExpr(allocator, script, tokens, i, limits, line_offset, debug_info);
                    args_count += 1;

                    if (i.* < tokens.len and tokens[i.*].kind != .eof) {
                        if (tokens[i.*].kind == .comma) {
                            i.* += 1;
                        } else {
                            return Script.ParseError.SyntaxError;
                        }
                    }
                }

                if (args_count > std.math.maxInt(c.basic26_IntType)) {
                    return Script.ParseError.TooManyArgs;
                }

                try script.appendOp(allocator, .{
                    .code = c.BASIC26_OPCODE_PUSH_INT,
                    .imm = .{ .as_int = @intCast(args_count) },
                }, line_offset + tokens[0].pos, debug_info);

                try script.appendOp(allocator, .{
                    .code = c.BASIC26_OPCODE_CALL,
                    .imm = .{ .as_symbol = id },
                }, line_offset + tokens[0].pos, debug_info);
            }
        } else if (tokens[0].kind == .symbol_literal) {
            if (i.* + 1 < tokens.len) {
                return Script.ParseError.SyntaxError;
            }

            const id = try script.strings.getOrPut(allocator, tokens[0].kind.symbol_literal);

            try script.appendOp(allocator, .{
                .code = c.BASIC26_OPCODE_PUSH_SYMBOL,
                .imm = .{ .as_symbol = id },
            }, line_offset + tokens[0].pos, debug_info);
        }
    }
};

export fn basic26_Script_destroy(
    c_script: ?*c.basic26_Script,
    c_vm: ?*c.basic26_Vm,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    if (c_script == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const script: *Script = @ptrCast(@alignCast(c_script));

    script.deinit(vm.allocator.allocator());
    vm.allocator.allocator().destroy(script);
}

export fn basic26_ClearScriptOptions_zeroed() callconv(.c) c.basic26_ClearScriptOptions {
    return .{};
}

export fn basic26_Script_clear(
    c_script: ?*c.basic26_Script,
    options: ?*const c.basic26_ClearScriptOptions,
) callconv(.c) void {
    std.debug.assert(c_script != null);

    const script: *Script = @ptrCast(@alignCast(c_script));

    if (options.?.clear_ops) {
        script.ops.clearRetainingCapacity();
    }

    if (options.?.clear_labels) {
        script.labels.clearRetainingCapacity();
    }
}

export fn basic26_ScriptLimits_zeroed() callconv(.c) c.basic26_ScriptLimits {
    return .{};
}

export fn basic26_CompileErrorInfo_zeroed() callconv(.c) c.basic26_CompileErrorInfo {
    return .{};
}

export fn basic26_CompileOptions_zeroed() callconv(.c) c.basic26_CompileOptions {
    return .{};
}

export fn basic26_Script_compile(
    c_script: ?*c.basic26_Script,
    options: ?*const c.basic26_CompileOptions,
    error_out: ?*c.basic26_CompileErrorInfo,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(options != null);
    std.debug.assert(error_out != null);

    const vm: *Vm = @ptrCast(@alignCast(options.?.vm.?));
    const script: *Script = @ptrCast(@alignCast(c_script.?));
    const debug_info: ?*DebugInfo = @ptrCast(@alignCast(options.?.debug_info));
    const alloc = vm.allocator.allocator();

    script.ops.clearRetainingCapacity();

    if (debug_info != null) {
        debug_info.?.ops_source_map.clearRetainingCapacity();
    }

    script.labels.clearRetainingCapacity();

    var ps = &script.parser_state;
    ps.ctrl_stack.clearRetainingCapacity();
    ps.pending_jumps.clearRetainingCapacity();

    const source: []const u8 = options.?.source[0..options.?.source_len];
    var line_iter = std.mem.splitScalar(u8, source, '\n');

    while (line_iter.next()) |line| {
        const line_offset = @intFromPtr(line.ptr) - @intFromPtr(source.ptr);
        const clear_line = std.mem.trim(u8, line, "\r\t ");

        if (clear_line.len == 0) {
            continue;
        }

        // Tokenize
        var lexer = Lexer.init(clear_line);
        ps.tokens.clearRetainingCapacity();

        while (true) {
            const tok = lexer.next() catch |err| {
                error_out.?.pos = line_offset + lexer.pos;
                error_out.?.code = switch (err) {
                    error.BadStringLiteral => c.BASIC26_COMPILE_ERROR_BAD_STRING_LITERAL,
                    error.BadNumberLiteral => c.BASIC26_COMPILE_ERROR_BAD_NUMBER_LITERAL,
                    error.BadSymbolLiteral => c.BASIC26_COMPILE_ERROR_BAD_SYMBOL_LITERAL,
                    error.OutOfMemory => return c.BASIC26_RESULT_OUT_OF_MEMORY,
                    else => c.BASIC26_COMPILE_ERROR_SYNTAX,
                };

                return c.BASIC26_RESULT_COMPILE_ERROR;
            };

            ps.tokens.append(alloc, tok) catch {
                return c.BASIC26_RESULT_OUT_OF_MEMORY;
            };

            if (tok.kind == .eof) {
                break;
            }
        }

        if (ps.tokens.items.len <= 1) {
            continue;
        }

        // Parse
        var idx: usize = 0;

        const parseLineResult = ps.parseLine(
            alloc,
            script,
            ps.tokens.items,
            &idx,
            options.?.limits.?,
            line_offset,
            debug_info,
        );

        if (parseLineResult) |_| {} else |err| {
            error_out.?.pos = line_offset;
            error_out.?.code = switch (err) {
                error.ExpectedOp => c.BASIC26_COMPILE_ERROR_EXPECTED_OP,
                error.UnknownOperator => c.BASIC26_COMPILE_ERROR_UNKNOWN_OP,
                error.OutOfMemory => return c.BASIC26_RESULT_OUT_OF_MEMORY,
                else => c.BASIC26_COMPILE_ERROR_SYNTAX,
            };

            return c.BASIC26_RESULT_COMPILE_ERROR;
        }
    }

    // Resolve pending goto jumps
    for (ps.pending_jumps.items) |pj| {
        const ip = script.labels.get(pj.label_name) orelse {
            error_out.?.pos = 0;
            error_out.?.code = c.BASIC26_COMPILE_ERROR_UNKNOWN_LABEL;

            return c.BASIC26_RESULT_COMPILE_ERROR;
        };

        switch (pj.kind) {
            .jump => script.ops.items[pj.op_index].imm.as_address = ip,
            .push_address => script.ops.items[pj.op_index].imm.as_address = ip,
        }
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_get_op(
    c_script: ?*const c.basic26_Script,
    pos: usize,
    out: ?*c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(out != null);

    const script: *const Script = @ptrCast(@alignCast(c_script.?));

    if (pos >= script.ops.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    out.?.* = script.ops.items[pos];

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_set_op(
    c_script: ?*c.basic26_Script,
    pos: usize,
    op: c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));

    if (pos >= script.ops.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    script.ops.items[pos] = op;

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_push_op(
    c_script: ?*c.basic26_Script,
    c_vm: ?*c.basic26_Vm,
    op: c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(c_vm != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    script.ops.append(vm.allocator.allocator(), op) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };
    errdefer _ = script.ops.pop();

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_insert_op(
    c_script: ?*c.basic26_Script,
    c_vm: ?*c.basic26_Vm,
    pos: usize,
    op: c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(c_vm != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    if (pos > script.ops.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    script.ops.insert(vm.allocator.allocator(), pos, op) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_pop_op(
    c_script: ?*c.basic26_Script,
    out: ?*c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));

    if (script.ops.items.len == 0) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    if (out != null) {
        out.?.* = script.ops.pop().?;
    } else {
        _ = script.ops.pop().?;
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_remove_op(
    c_script: ?*c.basic26_Script,
    pos: usize,
    out: ?*c.basic26_Op,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));

    if (script.ops.items.len == 0 or pos >= script.ops.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    if (out != null) {
        out.?.* = script.ops.orderedRemove(pos);
    } else {
        _ = script.ops.orderedRemove(pos);
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_count_ops(
    c_script: ?*const c.basic26_Script,
) callconv(.c) usize {
    std.debug.assert(c_script != null);

    const script: *const Script = @ptrCast(@alignCast(c_script.?));

    return script.ops.items.len;
}

export fn basic26_Script_get_label(
    c_script: ?*const c.basic26_Script,
    name: ?[*]const u8,
    name_len: usize,
    out: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(name != null);
    std.debug.assert(out != null);

    const script: *const Script = @ptrCast(@alignCast(c_script.?));

    const ip = script.labels.get(name.?[0..name_len]) orelse {
        return c.BASIC26_RESULT_NOT_FOUND;
    };

    out.?.* = ip;

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_set_label(
    c_script: ?*c.basic26_Script,
    c_vm: ?*c.basic26_Vm,
    name: ?[*]const u8,
    name_len: usize,
    ip: usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(c_vm != null);
    std.debug.assert(name != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const label_name = name.?[0..name_len];

    script.labels.put(vm.allocator.allocator(), label_name, ip) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_remove_label(
    c_script: ?*c.basic26_Script,
    name: ?[*]const u8,
    name_len: usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(name != null);

    const script: *Script = @ptrCast(@alignCast(c_script.?));
    const label_name = name.?[0..name_len];

    if (script.labels.remove(label_name)) {
        return c.BASIC26_RESULT_OK;
    }

    return c.BASIC26_RESULT_NOT_FOUND;
}

export fn basic26_Script_count_labels(
    c_script: ?*const c.basic26_Script,
) callconv(.c) usize {
    std.debug.assert(c_script != null);

    const script: *const Script = @ptrCast(@alignCast(c_script.?));

    return script.labels.count();
}

export fn basic26_Script_dump(
    c_script: ?*const c.basic26_Script,
    c_vm: ?*c.basic26_Vm,
    out: ?*[*]const u8,
    out_len: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_script != null);
    std.debug.assert(c_vm != null);
    std.debug.assert(out != null);
    std.debug.assert(out_len != null);

    const script: *const Script = @ptrCast(@alignCast(c_script.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    const dump = script.dump(vm.allocator.allocator()) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    out.?.* = dump.ptr;
    out_len.?.* = dump.len;

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_dump_free(
    c_vm: ?*c.basic26_Vm,
    c_dump: ?[*]u8,
    dump_len: usize,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    if (c_dump == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const dump: []u8 = c_dump.?[0..dump_len];

    vm.allocator.allocator().free(dump);
}

const DebugInfo = struct {
    ops_source_map: std.ArrayList(usize) = .empty,

    pub inline fn deinit(this: *DebugInfo, allocator: std.mem.Allocator) void {
        this.ops_source_map.deinit(allocator);
    }
};

export fn basic26_DebugInfo_create(
    c_vm: ?*c.basic26_Vm,
    out: ?*?*c.basic26_DebugInfo,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_vm != null);
    std.debug.assert(out != null);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const debug_info = vm.allocator.allocator().create(DebugInfo) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    debug_info.* = .{
        .ops_source_map = .empty,
    };

    out.?.* = @ptrCast(@alignCast(debug_info));

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_destroy(
    c_debug_info: ?*c.basic26_DebugInfo,
    c_vm: ?*c.basic26_Vm,
) callconv(.c) void {
    std.debug.assert(c_vm != null);

    if (c_debug_info == null) {
        return;
    }

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));
    const allocator = vm.allocator.allocator();

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    debug_info.deinit(allocator);
    allocator.destroy(debug_info);
}

export fn basic26_DebugInfo_clear(
    c_debug_info: ?*c.basic26_DebugInfo,
) callconv(.c) void {
    std.debug.assert(c_debug_info != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    debug_info.ops_source_map.clearRetainingCapacity();
}

export fn basic26_DebugInfo_get_source_pos(
    c_debug_info: ?*const c.basic26_DebugInfo,
    ip: usize,
    out: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);
    std.debug.assert(out != null);

    const debug_info: *const DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    if (ip >= debug_info.ops_source_map.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    out.?.* = debug_info.ops_source_map.items[ip];

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_set_source_pos(
    c_debug_info: ?*c.basic26_DebugInfo,
    ip: usize,
    pos: usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    if (ip >= debug_info.ops_source_map.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    debug_info.ops_source_map.items[ip] = pos;

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_push_source_pos(
    c_debug_info: ?*c.basic26_DebugInfo,
    c_vm: ?*c.basic26_Vm,
    pos: usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);
    std.debug.assert(c_vm != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    debug_info.ops_source_map.append(vm.allocator.allocator(), pos) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_insert_source_pos(
    c_debug_info: ?*c.basic26_DebugInfo,
    c_vm: ?*c.basic26_Vm,
    ip: usize,
    pos: usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);
    std.debug.assert(c_vm != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));
    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    if (ip > debug_info.ops_source_map.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    debug_info.ops_source_map.insert(vm.allocator.allocator(), ip, pos) catch {
        return c.BASIC26_RESULT_OUT_OF_MEMORY;
    };

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_pop_source_pos(
    c_debug_info: ?*c.basic26_DebugInfo,
    out: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    if (debug_info.ops_source_map.items.len == 0) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    const val = debug_info.ops_source_map.pop().?;

    if (out != null) {
        out.?.* = val;
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_DebugInfo_remove_source_pos(
    c_debug_info: ?*c.basic26_DebugInfo,
    ip: usize,
    out: ?*usize,
) callconv(.c) c.basic26_Result {
    std.debug.assert(c_debug_info != null);

    const debug_info: *DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    if (debug_info.ops_source_map.items.len == 0 or ip >= debug_info.ops_source_map.items.len) {
        return c.BASIC26_RESULT_NOT_FOUND;
    }

    const val = debug_info.ops_source_map.orderedRemove(ip);

    if (out != null) {
        out.?.* = val;
    }

    return c.BASIC26_RESULT_OK;
}

export fn basic26_Script_count_source_pos(
    c_debug_info: ?*const c.basic26_DebugInfo,
) callconv(.c) usize {
    std.debug.assert(c_debug_info != null);

    const debug_info: *const DebugInfo = @ptrCast(@alignCast(c_debug_info.?));

    return debug_info.ops_source_map.items.len;
}

fn expectEnum(expected: c_int, actual: c_uint) !void {
    try std.testing.expectEqual(@as(c_uint, @intCast(expected)), actual);
}

fn setVar(c_vm: *c.basic26_Vm, c_state: *c.basic26_State, name: []const u8, value: c.basic26_Value) !void {
    var symbol_id: c.basic26_SymbolId = undefined;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_get_string_id(c_vm, name.ptr, name.len, true, &symbol_id),
    );

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_set_var(c_state, symbol_id, &value),
    );
}

fn getVar(c_vm: *c.basic26_Vm, c_state: *c.basic26_State, name: []const u8) !c.basic26_Value {
    var symbol_id: c.basic26_SymbolId = undefined;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_get_string_id(c_vm, name.ptr, name.len, true, &symbol_id),
    );

    var out: c.basic26_Value = undefined;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_get_var(c_state, symbol_id, &out),
    );

    return out;
}

fn printDump(c_vm: *c.basic26_Vm, c_script: *const c.basic26_Script) !void {
    var c_dump: ?[*]u8 = null;
    var dump_len: usize = 0;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_dump(c_script, c_vm, &c_dump, &dump_len),
    );
    defer basic26_Script_dump_free(c_vm, c_dump.?, dump_len);

    const dump: []const u8 = c_dump.?[0..dump_len];

    std.debug.print("\n{s}", .{dump});
}

test "Undefined variable" {
    var c_vm: ?*c.basic26_Vm = null;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_create(&.{ .alloc = null }, &c_vm),
    );
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_create(&.{ .vm = c_vm.? }, &c_state),
    );
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE = "a = 10";

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_RUNTIME_ERROR,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try expectEnum(c.BASIC26_RUNTIME_ERROR_UNDEFINED_VARIABLE, run_error.code);
}

test "Basic arithmetics" {
    var c_vm: ?*c.basic26_Vm = null;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_create(&.{ .alloc = null }, &c_vm),
    );
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_create(&.{ .vm = c_vm.? }, &c_state),
    );
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 10.0
        \\b = 5.0
        \\c = a + b
        \\d = a / b
        \\e = b * 2.0
        \\f = b - a
        \\g = (a + b) / 2.0
        \\h = b % a
        \\h = -h
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "f", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "g", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "h", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    const a_value = try getVar(c_vm.?, c_state.?, "a");
    const b_value = try getVar(c_vm.?, c_state.?, "b");
    const c_value = try getVar(c_vm.?, c_state.?, "c");
    const d_value = try getVar(c_vm.?, c_state.?, "d");
    const e_value = try getVar(c_vm.?, c_state.?, "e");
    const f_value = try getVar(c_vm.?, c_state.?, "f");
    const g_value = try getVar(c_vm.?, c_state.?, "g");
    const h_value = try getVar(c_vm.?, c_state.?, "h");

    try std.testing.expect(a_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(10.0, a_value.as.float_val);

    try std.testing.expect(b_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(5.0, b_value.as.float_val);

    try std.testing.expect(c_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(15.0, c_value.as.float_val);

    try std.testing.expect(d_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(2.0, d_value.as.float_val);

    try std.testing.expect(e_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(10.0, e_value.as.float_val);

    try std.testing.expect(f_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(-5.0, f_value.as.float_val);

    try std.testing.expect(g_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(7.5, g_value.as.float_val);

    try std.testing.expect(h_value.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectEqual(-5.0, h_value.as.float_val);
}

test "Boolean operators" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 1 AND 1
        \\b = 1 AND 0
        \\c = 1 OR 0
        \\d = NOT 1
        \\e = NOT 0
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    const a_value = try getVar(c_vm.?, c_state.?, "a");
    const b_value = try getVar(c_vm.?, c_state.?, "b");
    const c_value = try getVar(c_vm.?, c_state.?, "c");
    const d_value = try getVar(c_vm.?, c_state.?, "d");
    const e_value = try getVar(c_vm.?, c_state.?, "e");

    try std.testing.expect(a_value.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(1, a_value.as.int_val);

    try std.testing.expect(b_value.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(0, b_value.as.int_val);

    try std.testing.expect(c_value.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(1, c_value.as.int_val);

    try std.testing.expect(d_value.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(0, d_value.as.int_val);

    try std.testing.expect(e_value.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(1, e_value.as.int_val);
}

test "Bitwise operators" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 3 & 1
        \\b = 2 | 1
        \\c = 3 ^ 1
        \\d = ~0
        \\e = 1 << 2
        \\f = 4 >> 1
    ;

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "f", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(3, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
    try std.testing.expectEqual(2, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
    try std.testing.expectEqual(-1, (try getVar(c_vm.?, c_state.?, "d")).as.int_val);
    try std.testing.expectEqual(4, (try getVar(c_vm.?, c_state.?, "e")).as.int_val);
    try std.testing.expectEqual(2, (try getVar(c_vm.?, c_state.?, "f")).as.int_val);
}

test "Comparison operators" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 10 == 10
        \\b = 10 != 5
        \\c = 5 < 10
        \\d = 10 > 5
        \\e = 10 <= 10
        \\f = 10 >= 10
        \\g = "hello" == "hello"
        \\h = "hello" != "world"
    ;

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "f", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "g", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "h", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "d")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "e")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "f")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "g")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "h")).as.int_val);
}

test "If Else flow" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 0
        \\IF 1
        \\  a = 10
        \\ELSE
        \\  a = 20
        \\ENDIF
        \\
        \\b = 0
        \\IF 0
        \\  b = 10
        \\ELSE
        \\  b = 20
        \\ENDIF
        \\
        \\c = 0
        \\IF 0
        \\  c = 10
        \\ELSE IF 1
        \\  c = 20
        \\ELSE
        \\  c = 30
        \\ENDIF
        \\
        \\d = 0
        \\IF 0
        \\  d = 10
        \\ELSEIF 0
        \\  d = 20
        \\ELSEIF 1
        \\  d = 30
        \\ELSE
        \\  d = 40
        \\ENDIF
        \\
        \\e = 0
        \\IF 1
        \\  IF 0
        \\    e = 10
        \\  ELSE
        \\    e = 20
        \\  ENDIF
        \\ELSE
        \\  e = 30
        \\ENDIF
        \\
        \\f = 0
        \\IF 0
        \\  f = 10
        \\ELSEIF 1
        \\  IF 0
        \\    f = 20
        \\  ELSEIF 1
        \\    f = 30
        \\  ELSE
        \\    f = 40
        \\  ENDIF
        \\ELSE
        \\  f = 50
        \\ENDIF
    ;

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "f", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(10, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(20, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);

    try std.testing.expectEqual(20, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
    try std.testing.expectEqual(30, (try getVar(c_vm.?, c_state.?, "d")).as.int_val);

    try std.testing.expectEqual(20, (try getVar(c_vm.?, c_state.?, "e")).as.int_val);
    try std.testing.expectEqual(30, (try getVar(c_vm.?, c_state.?, "f")).as.int_val);
}

test "While If ElseIf" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\WHILE i < 3
        \\
        \\IF i == 1
        \\ELSEIF i == 2
        \\ENDIF
        \\
        \\i = i + 1
        \\ENDWHILE
    ;

    try setVar(c_vm.?, c_state.?, "i", .{
        .type = c.BASIC26_VALUE_TYPE_INT,
        .as = .{
            .int_val = 0,
        },
    });

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(3, (try getVar(c_vm.?, c_state.?, "i")).as.int_val);
}

test "While loop flow" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\i = 0
        \\WHILE i < 5
        \\  i = i + 1
        \\ENDWHILE
    ;

    try setVar(c_vm.?, c_state.?, "i", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(5, (try getVar(c_vm.?, c_state.?, "i")).as.int_val);
}

test "Goto statement" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 1
        \\GOTO @skip
        \\a = 2
        \\skip:
        \\b = a
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
}

test "Division by zero runtime error" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE = "a = 10 / 0";
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_RUNTIME_ERROR,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );
    try expectEnum(c.BASIC26_RUNTIME_ERROR_DIVISION_BY_ZERO, run_error.code);
}

test "Native negative values, NaN and Inf parsing" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_create(&.{ .alloc = null }, &c_vm),
    );
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_create(&.{ .vm = c_vm.? }, &c_state),
    );
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = -42
        \\b = -3.14
        \\c = NAN
        \\d = INF
        \\e = -INF
        \\f = 5 - 2
        \\g = f - -1
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "e", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "f", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "g", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    const a_val = try getVar(c_vm.?, c_state.?, "a");
    const b_val = try getVar(c_vm.?, c_state.?, "b");
    const c_val = try getVar(c_vm.?, c_state.?, "c");
    const d_val = try getVar(c_vm.?, c_state.?, "d");
    const e_val = try getVar(c_vm.?, c_state.?, "e");
    const f_val = try getVar(c_vm.?, c_state.?, "f");
    const g_val = try getVar(c_vm.?, c_state.?, "g");

    try std.testing.expect(a_val.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(-42, a_val.as.int_val);

    try std.testing.expect(b_val.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expectApproxEqAbs(-3.14, b_val.as.float_val, 0.0001);

    try std.testing.expect(c_val.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expect(std.math.isNan(c_val.as.float_val));

    try std.testing.expect(d_val.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expect(std.math.isInf(d_val.as.float_val));
    try std.testing.expect(d_val.as.float_val > 0);

    try std.testing.expect(e_val.type == c.BASIC26_VALUE_TYPE_FLOAT);
    try std.testing.expect(std.math.isInf(e_val.as.float_val));
    try std.testing.expect(e_val.as.float_val < 0);

    try std.testing.expect(f_val.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(3, f_val.as.int_val);

    try std.testing.expect(g_val.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(4, g_val.as.int_val);
}

test "Functions" {
    const Callback = struct {
        pub const UserData = struct {
            was_called: bool = false,
            arg_count_ok: bool = false,
            arg_values_ok: bool = false,
        };

        pub const NAME: []const u8 = "FOO";

        pub fn callback(
            info: ?*const c.basic26_CallInfo,
            argc: usize,
            argv: ?[*]const c.basic26_Value,
        ) callconv(.c) c.basic26_FunctionResult {
            const ud: *UserData = @ptrCast(@alignCast(info.?.userdata));

            ud.was_called = true;
            ud.arg_count_ok = argc == 3;
            ud.arg_values_ok = true;

            const arg_a = argv.?[0];
            const arg_b = argv.?[1];
            const arg_c = argv.?[2];

            if (arg_a.type != c.BASIC26_VALUE_TYPE_INT or arg_a.as.int_val != 5) {
                ud.arg_values_ok = false;
            } else if (arg_b.type != c.BASIC26_VALUE_TYPE_INT or arg_b.as.int_val != 10) {
                ud.arg_values_ok = false;
            } else if (arg_c.type != c.BASIC26_VALUE_TYPE_INT or arg_c.as.int_val != 0) {
                ud.arg_values_ok = false;
            }

            return c.BASIC26_FUNCTION_RESULT_OK;
        }
    };

    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_create(&.{ .alloc = null }, &c_vm),
    );
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_create(&.{ .vm = c_vm.? }, &c_state),
    );
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 10
        \\b = 5
        \\c = 0
        \\
        \\FOO (a / 2), b * 2, c
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var function_name: c.basic26_SymbolId = undefined;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_get_string_id(
        c_vm.?,
        Callback.NAME.ptr,
        Callback.NAME.len,
        true,
        &function_name,
    ));

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_register_function(c_vm.?, &.{
            .name = function_name,
            .callback = Callback.callback,
        }),
    );

    var ud: Callback.UserData = .{};

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = &ud,
        }, &run_error),
    );

    try std.testing.expectEqual(true, ud.was_called);
    try std.testing.expectEqual(true, ud.arg_count_ok);
}

test "Symbol literal" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_create(&.{ .alloc = null }, &c_vm),
    );
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_State_create(&.{ .vm = c_vm.? }, &c_state),
    );
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 10
        \\b = $a
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    const a_val = try getVar(c_vm.?, c_state.?, "a");
    const b_val = try getVar(c_vm.?, c_state.?, "b");

    try std.testing.expect(a_val.type == c.BASIC26_VALUE_TYPE_INT);
    try std.testing.expectEqual(10, a_val.as.int_val);

    var a_symbol: c.basic26_SymbolId = undefined;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_get_string_id(c_vm.?, "a", "a".len, false, &a_symbol));

    try std.testing.expect(b_val.type == c.BASIC26_VALUE_TYPE_SYMBOL);
    try std.testing.expectEqual(a_symbol, b_val.as.symbol_id);
}

test "Operator precedence: mul before add/sub" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 2 + 3 * 4
        \\b = 10 - 2 * 3
        \\c = 3 * 2 + 1
        \\d = 10 - 3 - 2
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(14, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(4, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
    try std.testing.expectEqual(7, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
    try std.testing.expectEqual(5, (try getVar(c_vm.?, c_state.?, "d")).as.int_val);
}

test "Operator precedence: arithmetic before comparison" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 5 * 5 <= 30
        \\b = 3 + 7 > 8
        \\c = 2 * 3 == 6
        \\d = 1 OR 0 AND 0
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "d", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "d")).as.int_val);
}

test "Operator precedence: modulo before equality" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 6 % 3 == 0
        \\b = 7 % 2 == 1
        \\c = 10 % 5 != 1
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "c", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "a")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "b")).as.int_val);
    try std.testing.expectEqual(1, (try getVar(c_vm.?, c_state.?, "c")).as.int_val);
}

test "Variable iteration" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    const vm: *Vm = @ptrCast(@alignCast(c_vm.?));

    _ = try vm.strings.getOrPut(vm.allocator.allocator(), "FooBar");

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    try setVar(c_vm.?, c_state.?, "a", .{
        .type = c.BASIC26_VALUE_TYPE_INT,
        .as = .{ .int_val = 5 },
    });

    _ = try vm.strings.getOrPut(vm.allocator.allocator(), "FooBar2");
    _ = try vm.strings.getOrPut(vm.allocator.allocator(), "FooBar3");

    try setVar(c_vm.?, c_state.?, "b", .{
        .type = c.BASIC26_VALUE_TYPE_INT,
        .as = .{ .int_val = 2 },
    });
    try setVar(c_vm.?, c_state.?, "c", .{
        .type = c.BASIC26_VALUE_TYPE_INT,
        .as = .{ .int_val = 7 },
    });

    var prev: ?*c.basic26_SymbolId = null;
    var it: c.basic26_SymbolId = 0;

    var a_found: bool = false;
    var b_found: bool = false;
    var c_found: bool = false;

    while (basic26_State_var_next(c_state.?, prev, &it)) {
        prev = &it;

        var str: ?[*]const u8 = null;
        var str_len: usize = 0;

        try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_get_string(c_vm.?, it, &str, &str_len));

        var value: c.basic26_Value = undefined;
        try expectEnum(c.BASIC26_RESULT_OK, basic26_State_get_var(c_state, it, &value));
        try expectEnum(c.BASIC26_VALUE_TYPE_INT, value.type);

        if (std.mem.eql(u8, str.?[0..str_len], "a")) {
            a_found = true;

            try std.testing.expectEqual(5, value.as.int_val);
        } else if (std.mem.eql(u8, str.?[0..str_len], "b")) {
            b_found = true;

            try std.testing.expectEqual(2, value.as.int_val);
        } else if (std.mem.eql(u8, str.?[0..str_len], "c")) {
            c_found = true;

            try std.testing.expectEqual(7, value.as.int_val);
        }
    }

    try std.testing.expectEqual(true, a_found);
    try std.testing.expectEqual(true, b_found);
    try std.testing.expectEqual(true, c_found);
}

test "Address literal" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\my_label:
        \\a = @my_label
        \\b = @end_label
        \\end_label:
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    var my_label_ip: usize = 0;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_get_label(c_script.?, "my_label", "my_label".len, &my_label_ip));

    var end_label_ip: usize = 0;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_get_label(c_script.?, "end_label", "end_label".len, &end_label_ip));

    const a_val = try getVar(c_vm.?, c_state.?, "a");
    try std.testing.expect(a_val.type == c.BASIC26_VALUE_TYPE_ADDRESS);
    try std.testing.expectEqual(my_label_ip, a_val.as.address_val);

    const b_val = try getVar(c_vm.?, c_state.?, "b");
    try std.testing.expect(b_val.type == c.BASIC26_VALUE_TYPE_ADDRESS);
    try std.testing.expectEqual(end_label_ip, b_val.as.address_val);
}

test "Get OP pos" {
    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    var c_debug_info: ?*c.basic26_DebugInfo = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_DebugInfo_create(c_vm.?, &c_debug_info));
    defer basic26_DebugInfo_destroy(c_debug_info.?, c_vm.?);

    var compile_error: c.basic26_CompileErrorInfo = .{};

    const SOURCE =
        \\a = 1.0
        \\b = NOT a
    ;

    try expectEnum(
        c.BASIC26_RESULT_OK,
        basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = SOURCE.ptr,
            .source_len = SOURCE.len,
            .limits = &.{},
            .debug_info = c_debug_info.?,
        }, &compile_error),
    );

    try setVar(c_vm.?, c_state.?, "a", .{ .type = c.BASIC26_VALUE_TYPE_NULL });
    try setVar(c_vm.?, c_state.?, "b", .{ .type = c.BASIC26_VALUE_TYPE_NULL });

    var run_error: c.basic26_RuntimeErrorInfo = .{};
    try expectEnum(
        c.BASIC26_RESULT_RUNTIME_ERROR,
        basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &run_error),
    );

    try expectEnum(c.BASIC26_RUNTIME_ERROR_TYPE_MISMATCH, run_error.code);

    var pos: usize = 0;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_DebugInfo_get_source_pos(c_debug_info.?, run_error.ip, &pos));

    try std.testing.expectEqual(12, pos);
}

// Seems fuzzing works only on Linux for now.

test "fuzzing" {
    try std.testing.fuzz({}, testOne, .{});
}

fn testOne(context: void, smith: *std.testing.Smith) !void {
    _ = context;

    var c_vm: ?*c.basic26_Vm = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Vm_create(&.{ .alloc = null }, &c_vm));
    defer basic26_Vm_destroy(c_vm.?);

    var c_state: ?*c.basic26_State = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_State_create(&.{ .vm = c_vm.? }, &c_state));
    defer basic26_State_destroy(c_state.?, c_vm.?);

    var c_script: ?*c.basic26_Script = null;
    try expectEnum(c.BASIC26_RESULT_OK, basic26_Script_create(c_vm.?, &c_script));
    defer basic26_Script_destroy(c_script.?, c_vm.?);

    const input = try std.testing.allocator.alloc(u8, 16384);
    defer std.testing.allocator.free(input);

    const ascii_weights: []const std.testing.Smith.Weight = &.{
        .rangeAtMost(u8, 9, 10, 1),
        .rangeAtMost(u8, 32, 126, 1),
    };

    while (!smith.eos()) {
        var compile_err: c.basic26_CompileErrorInfo = .{};

        const code_len = smith.valueRangeAtMost(u64, 1, input.len);
        const code = input[0..code_len];

        if (smith.boolWeighted(1, 99)) {
            smith.bytesWeighted(code, ascii_weights);
        } else {
            smith.bytes(code);
        }

        if (basic26_Script_compile(c_script.?, &.{
            .vm = c_vm.?,
            .source = code.ptr,
            .source_len = code.len,
            .limits = &.{},
        }, &compile_err) != c.BASIC26_RESULT_OK) {
            continue;
        }

        basic26_State_clear(c_state.?, &.{
            .clear_stack = true,
            .clear_vars = true,
        });
        basic26_State_set_ip(c_state.?, 0);

        var runtime_err: c.basic26_RuntimeErrorInfo = .{};

        _ = basic26_Vm_run(c_vm.?, &.{
            .state = c_state.?,
            .script = c_script.?,
            .limits = &.{},
            .userdata = null,
        }, &runtime_err);
    }
}
