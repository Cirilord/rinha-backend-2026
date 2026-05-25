const std = @import("std");

const c = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("arpa/inet.h");
    @cInclude("netinet/in.h");
    @cInclude("stdlib.h");
    @cInclude("string.h");
    @cInclude("sys/socket.h");
    @cInclude("sys/un.h");
    @cInclude("unistd.h");
});

const MAX_WORKERS = 32;
const MAX_SOCKET_PATH = 108;
const LISTEN_BACKLOG = 4096;

const Worker = struct {
    path: [MAX_SOCKET_PATH]u8 = [_]u8{0} ** MAX_SOCKET_PATH,
    fd: c_int = -1,
};

var workers: [MAX_WORKERS]Worker = [_]Worker{.{}} ** MAX_WORKERS;
var worker_count: usize = 0;
var rr_next: usize = 0;

const LbError = error{
    InvalidPort,
    InvalidWorkers,
    ListenFailed,
};

fn logErr(comptime fmt: []const u8, args: anytype) void {
    std.io.getStdErr().writer().print(fmt, args) catch {};
}

fn parsePort() LbError!u16 {
    const port_env = c.getenv("PORT") orelse return LbError.InvalidPort;
    const text = std.mem.span(port_env);
    const parsed = std.fmt.parseInt(u32, text, 10) catch return LbError.InvalidPort;
    if (parsed == 0 or parsed > 65_535) return LbError.InvalidPort;
    return @intCast(parsed);
}

fn parseWorkerSockets() LbError!void {
    const workers_env = c.getenv("WORKER_SOCKETS") orelse return LbError.InvalidWorkers;
    const text = std.mem.span(workers_env);
    var split = std.mem.splitScalar(u8, text, ',');
    worker_count = 0;

    while (split.next()) |raw| {
        const token = std.mem.trim(u8, raw, " \t\r\n");
        if (token.len == 0) continue;
        if (token.len >= MAX_SOCKET_PATH) return LbError.InvalidWorkers;
        if (worker_count >= MAX_WORKERS) return LbError.InvalidWorkers;

        @memset(workers[worker_count].path[0..], 0);
        std.mem.copyForwards(u8, workers[worker_count].path[0..token.len], token);
        workers[worker_count].fd = -1;
        worker_count += 1;
    }

    if (worker_count == 0) return LbError.InvalidWorkers;
}

fn setKeepAlive(fd: c_int) void {
    var one: c_int = 1;
    _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_KEEPALIVE, &one, @sizeOf(c_int));
}

fn connectWorker(index: usize) bool {
    if (index >= worker_count) return false;
    const worker = &workers[index];

    if (worker.fd >= 0) return true;

    const fd = c.socket(c.AF_UNIX, c.SOCK_STREAM | c.SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    var addr: c.struct_sockaddr_un = std.mem.zeroes(c.struct_sockaddr_un);
    addr.sun_family = c.AF_UNIX;
    _ = c.strncpy(@ptrCast(&addr.sun_path[0]), @ptrCast(&worker.path[0]), MAX_SOCKET_PATH - 1);

    if (c.connect(fd, @ptrCast(&addr), @sizeOf(c.struct_sockaddr_un)) != 0) {
        _ = c.close(fd);
        return false;
    }

    worker.fd = fd;
    return true;
}

fn closeWorker(index: usize) void {
    if (index >= worker_count) return;
    const worker = &workers[index];
    if (worker.fd >= 0) {
        _ = c.close(worker.fd);
        worker.fd = -1;
    }
}

fn sendFd(control_fd: c_int, client_fd: c_int) bool {
    var data: u8 = 0;
    var iov = c.struct_iovec{
        .iov_base = &data,
        .iov_len = 1,
    };

    var control: [64]u8 align(@alignOf(c.struct_cmsghdr)) = [_]u8{0} ** 64;

    var msg: c.struct_msghdr = std.mem.zeroes(c.struct_msghdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = @ptrCast(&control[0]);
    const fd_size: c_uint = @intCast(@sizeOf(c_int));
    msg.msg_controllen = @as(c_uint, @intCast(c.CMSG_SPACE(fd_size)));

    const cmsg = c.CMSG_FIRSTHDR(&msg);
    if (cmsg == null) return false;
    cmsg.*.cmsg_level = c.SOL_SOCKET;
    cmsg.*.cmsg_type = c.SCM_RIGHTS;
    cmsg.*.cmsg_len = @as(c_uint, @intCast(c.CMSG_LEN(fd_size)));

    const payload_ptr = @as([*]u8, @ptrCast(c.CMSG_DATA(cmsg)));
    const fd_bytes = std.mem.asBytes(&client_fd);
    std.mem.copyForwards(u8, payload_ptr[0..@sizeOf(c_int)], fd_bytes);
    msg.msg_controllen = cmsg.*.cmsg_len;

    const sent = c.sendmsg(control_fd, &msg, c.MSG_NOSIGNAL);
    return sent == 1;
}

fn dispatchClient(client_fd: c_int) void {
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

fn createListener(port: u16) LbError!c_int {
    const fd = c.socket(c.AF_INET, c.SOCK_STREAM | c.SOCK_CLOEXEC, 0);
    if (fd < 0) return LbError.ListenFailed;

    var one: c_int = 1;
    _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_REUSEADDR, &one, @sizeOf(c_int));

    var addr: c.struct_sockaddr_in = std.mem.zeroes(c.struct_sockaddr_in);
    addr.sin_family = c.AF_INET;
    addr.sin_port = c.htons(port);
    addr.sin_addr.s_addr = c.htonl(c.INADDR_ANY);

    if (c.bind(fd, @ptrCast(&addr), @sizeOf(c.struct_sockaddr_in)) != 0) {
        _ = c.close(fd);
        return LbError.ListenFailed;
    }

    if (c.listen(fd, LISTEN_BACKLOG) != 0) {
        _ = c.close(fd);
        return LbError.ListenFailed;
    }

    return fd;
}

fn acceptLoop(server_fd: c_int) noreturn {
    while (true) {
        const client_fd = c.accept4(server_fd, null, null, c.SOCK_CLOEXEC);
        if (client_fd < 0) {
            continue;
        }

        setKeepAlive(client_fd);
        dispatchClient(client_fd);
        _ = c.close(client_fd);
    }
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

    logErr("load-balancer3 (zig) listening on :{d} with {d} workers\n", .{ port, worker_count });
    acceptLoop(server_fd);
}
