const std = @import("std");
const posix = std.posix;

const MAX_WORKERS = 32;
const MAX_SOCKET_PATH = 108;
const LISTEN_BACKLOG = 4096;

// Linux and POSIX define SCM_RIGHTS as 1.
const SCM_RIGHTS: i32 = 1;

const Cmsghdr = extern struct {
    cmsg_len: usize,
    cmsg_level: i32,
    cmsg_type: i32,
};

const Worker = struct {
    path: [MAX_SOCKET_PATH]u8 = [_]u8{0} ** MAX_SOCKET_PATH,
    path_len: usize = 0,
    fd: posix.socket_t = -1,
};

const LbError = error{
    InvalidPort,
    InvalidWorkers,
    ListenFailed,
};

const FD_SIZE = @sizeOf(posix.fd_t);
const CMSG_HDR_LEN = cmsgAlign(@sizeOf(Cmsghdr));
const CMSG_LEN_FD = CMSG_HDR_LEN + FD_SIZE;
const CMSG_SPACE_FD = CMSG_HDR_LEN + cmsgAlign(FD_SIZE);

var workers: [MAX_WORKERS]Worker = [_]Worker{.{}} ** MAX_WORKERS;
var worker_count: usize = 0;
var rr_next: usize = 0;

inline fn cmsgAlign(len: usize) usize {
    const alignment = @sizeOf(usize);
    const mask: usize = alignment - 1;
    return (len + mask) & ~mask;
}

fn closeWorker(index: usize) void {
    if (index >= worker_count) return;

    const worker = &workers[index];
    if (worker.fd >= 0) {
        posix.close(worker.fd);
        worker.fd = -1;
    }
}

fn connectWorker(index: usize) bool {
    if (index >= worker_count) return false;

    const worker = &workers[index];
    if (worker.fd >= 0) return true;

    const fd = posix.socket(posix.AF.UNIX, posix.SOCK.STREAM | posix.SOCK.CLOEXEC, 0) catch return false;
    errdefer posix.close(fd);

    const addr = std.net.Address.initUnix(worker.path[0..worker.path_len]) catch return false;
    posix.connect(fd, &addr.any, addr.getOsSockLen()) catch return false;

    worker.fd = fd;
    return true;
}

fn createListener(port: u16) LbError!posix.socket_t {
    const fd = posix.socket(posix.AF.INET, posix.SOCK.STREAM | posix.SOCK.CLOEXEC, 0) catch {
        return LbError.ListenFailed;
    };
    errdefer posix.close(fd);

    const one: c_int = 1;
    posix.setsockopt(fd, posix.SOL.SOCKET, posix.SO.REUSEADDR, std.mem.asBytes(&one)) catch {};

    const addr = std.net.Address.initIp4(.{ 0, 0, 0, 0 }, port);
    posix.bind(fd, &addr.any, addr.getOsSockLen()) catch return LbError.ListenFailed;
    posix.listen(fd, LISTEN_BACKLOG) catch return LbError.ListenFailed;

    return fd;
}

fn dispatchClient(client_fd: posix.socket_t) void {
    if (worker_count == 0) return;

    const start = rr_next;
    rr_next = (rr_next + 1) % worker_count;

    var attempt: usize = 0;
    while (attempt < worker_count) : (attempt += 1) {
        const idx = (start + attempt) % worker_count;
        if (!connectWorker(idx)) continue;

        if (sendFd(workers[idx].fd, client_fd)) return;

        closeWorker(idx);
        if (!connectWorker(idx)) continue;
        if (sendFd(workers[idx].fd, client_fd)) return;

        closeWorker(idx);
    }
}

fn logErr(comptime fmt: []const u8, args: anytype) void {
    std.io.getStdErr().writer().print(fmt, args) catch {};
}

fn parsePort() LbError!u16 {
    const port_env = posix.getenv("PORT") orelse return LbError.InvalidPort;
    const text = port_env;

    const parsed = std.fmt.parseInt(u32, text, 10) catch return LbError.InvalidPort;
    if (parsed == 0 or parsed > std.math.maxInt(u16)) return LbError.InvalidPort;

    return @intCast(parsed);
}

fn parseWorkerSockets() LbError!void {
    const workers_env = posix.getenv("WORKER_SOCKETS") orelse return LbError.InvalidWorkers;
    const text = workers_env;

    worker_count = 0;
    var split = std.mem.splitScalar(u8, text, ',');
    while (split.next()) |raw| {
        const token = std.mem.trim(u8, raw, " \t\r\n");
        if (token.len == 0) continue;

        if (worker_count >= MAX_WORKERS) return LbError.InvalidWorkers;
        if (token.len >= MAX_SOCKET_PATH) return LbError.InvalidWorkers;

        const worker = &workers[worker_count];
        @memset(worker.path[0..], 0);
        std.mem.copyForwards(u8, worker.path[0..token.len], token);
        worker.path_len = token.len;
        worker.fd = -1;

        worker_count += 1;
    }

    if (worker_count == 0) return LbError.InvalidWorkers;
}

fn sendFd(control_fd: posix.socket_t, client_fd: posix.fd_t) bool {
    var data: [1]u8 = .{0};
    const iov = [1]posix.iovec_const{.{ .base = data[0..].ptr, .len = data.len }};

    var control: [CMSG_SPACE_FD]u8 align(@alignOf(Cmsghdr)) = [_]u8{0} ** CMSG_SPACE_FD;
    const cmsg: *Cmsghdr = @ptrCast(@alignCast(&control[0]));
    cmsg.cmsg_len = CMSG_LEN_FD;
    cmsg.cmsg_level = posix.SOL.SOCKET;
    cmsg.cmsg_type = SCM_RIGHTS;

    const fd_bytes = std.mem.asBytes(&client_fd);
    std.mem.copyForwards(u8, control[CMSG_HDR_LEN .. CMSG_HDR_LEN + FD_SIZE], fd_bytes);

    const msg: posix.msghdr_const = .{
        .name = null,
        .namelen = 0,
        .iov = &iov,
        .iovlen = 1,
        .control = &control,
        .controllen = @intCast(CMSG_SPACE_FD),
        .flags = 0,
    };

    const written = posix.sendmsg(control_fd, &msg, posix.MSG.NOSIGNAL) catch return false;
    return written == 1;
}

fn setKeepAlive(fd: posix.socket_t) void {
    const one: c_int = 1;
    posix.setsockopt(fd, posix.SOL.SOCKET, posix.SO.KEEPALIVE, std.mem.asBytes(&one)) catch {};
}

pub fn main() u8 {
    const port = parsePort() catch {
        logErr("invalid or missing PORT environment variable\n", .{});
        return 1;
    };

    parseWorkerSockets() catch {
        logErr("invalid or missing WORKER_SOCKETS environment variable\n", .{});
        return 1;
    };

    const server_fd = createListener(port) catch {
        logErr("failed to create tcp server on port {d}\n", .{port});
        return 1;
    };
    defer posix.close(server_fd);

    logErr("load-balancer3 (zig std) listening on :{d} with {d} workers\n", .{ port, worker_count });

    while (true) {
        const client_fd = posix.accept(server_fd, null, null, posix.SOCK.CLOEXEC) catch |err| {
            switch (err) {
                error.ConnectionAborted, error.WouldBlock => continue,
                else => continue,
            }
        };

        setKeepAlive(client_fd);
        dispatchClient(client_fd);
        posix.close(client_fd);
    }
}
