const std = @import("std");

const c = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("stdlib.h");
    @cInclude("string.h");
    @cInclude("sys/epoll.h");
    @cInclude("sys/socket.h");
    @cInclude("sys/un.h");
    @cInclude("unistd.h");
});

const CLIENT_BUFFER_SIZE = 8192;
const MAX_CONTROL_CONNECTIONS = 32;
const MAX_EVENTS = 128;
const MAX_TRACKED_FDS = 131072;
const WAIT_TIMEOUT_MS = 250;

const ClientPhase = enum {
    reading,
    writing,
};

const ClientConn = struct {
    fd: c_int,
    buffer: [CLIENT_BUFFER_SIZE]u8 = [_]u8{0} ** CLIENT_BUFFER_SIZE,
    used: usize = 0,
    scan_pos: usize = 0,
    line_start: usize = 0,
    content_length: usize = 0,
    expected_total: usize = 0,
    write_off: usize = 0,
    headers_done: bool = false,
    content_length_seen: bool = false,
    phase: ClientPhase = .reading,
};

const ControlSet = struct {
    fds: [MAX_CONTROL_CONNECTIONS]c_int = [_]c_int{-1} ** MAX_CONTROL_CONNECTIONS,
    count: usize = 0,

    fn add(self: *ControlSet, fd: c_int) bool {
        if (fd < 0 or self.count >= MAX_CONTROL_CONNECTIONS) {
            return false;
        }
        self.fds[self.count] = fd;
        self.count += 1;
        return true;
    }

    fn closeAll(self: *ControlSet) void {
        var i: usize = 0;
        while (i < self.count) : (i += 1) {
            if (self.fds[i] >= 0) {
                _ = c.close(self.fds[i]);
                self.fds[i] = -1;
            }
        }
        self.count = 0;
    }

    fn find(self: *const ControlSet, fd: c_int) ?usize {
        if (fd < 0) return null;
        var i: usize = 0;
        while (i < self.count) : (i += 1) {
            if (self.fds[i] == fd) return i;
        }
        return null;
    }

    fn removeAt(self: *ControlSet, index: usize) void {
        if (index >= self.count) return;
        const last = self.count - 1;
        self.fds[index] = self.fds[last];
        self.count = last;
    }
};

const RunError = error{
    EpollCreateFailed,
    EpollCtlFailed,
    ListenFailed,
    SocketPathMissing,
};

const RESPONSE_OK =
    "HTTP/1.1 200 OK\r\n" ++
    "Content-Type: application/json\r\n" ++
    "Connection: close\r\n" ++
    "Content-Length: 18\r\n" ++
    "\r\n" ++
    "{\"approved\":false}";

var conn_by_fd: [MAX_TRACKED_FDS]?*ClientConn = [_]?*ClientConn{null} ** MAX_TRACKED_FDS;

fn logErr(comptime fmt: []const u8, args: anytype) void {
    std.io.getStdErr().writer().print(fmt, args) catch {};
}

fn getErrno() c_int {
    return std.c._errno().*;
}

fn connGet(fd: c_int) ?*ClientConn {
    if (fd < 0 or fd >= MAX_TRACKED_FDS) return null;
    return conn_by_fd[@intCast(fd)];
}

fn connSet(fd: c_int, conn: *ClientConn) bool {
    if (fd < 0 or fd >= MAX_TRACKED_FDS) return false;
    conn_by_fd[@intCast(fd)] = conn;
    return true;
}

fn connUnset(fd: c_int) void {
    if (fd < 0 or fd >= MAX_TRACKED_FDS) return;
    conn_by_fd[@intCast(fd)] = null;
}

fn createClientConn(fd: c_int) ?*ClientConn {
    const conn = std.heap.c_allocator.create(ClientConn) catch return null;
    conn.* = .{ .fd = fd };
    return conn;
}

fn setNonBlockingCloexec(fd: c_int) bool {
    var flags = c.fcntl(fd, c.F_GETFL, @as(c_int, 0));
    if (flags < 0) return false;
    if (c.fcntl(fd, c.F_SETFL, flags | c.O_NONBLOCK) < 0) return false;

    flags = c.fcntl(fd, c.F_GETFD, @as(c_int, 0));
    if (flags < 0) return false;
    if (c.fcntl(fd, c.F_SETFD, flags | c.FD_CLOEXEC) < 0) return false;

    return true;
}

fn createUnixServer(path: [*:0]const u8) RunError!c_int {
    const server_fd = c.socket(c.AF_UNIX, c.SOCK_STREAM, 0);
    if (server_fd < 0) return RunError.ListenFailed;

    if (!setNonBlockingCloexec(server_fd)) {
        _ = c.close(server_fd);
        return RunError.ListenFailed;
    }

    _ = c.unlink(path);

    var addr: c.struct_sockaddr_un = std.mem.zeroes(c.struct_sockaddr_un);
    addr.sun_family = c.AF_UNIX;
    _ = c.strncpy(@ptrCast(&addr.sun_path[0]), @ptrCast(path), addr.sun_path.len - 1);

    if (c.bind(server_fd, @ptrCast(&addr), @sizeOf(c.struct_sockaddr_un)) < 0) {
        _ = c.close(server_fd);
        return RunError.ListenFailed;
    }

    if (c.listen(server_fd, 128) < 0) {
        _ = c.close(server_fd);
        return RunError.ListenFailed;
    }

    return server_fd;
}

fn parseContentLength(line: []const u8) ?usize {
    const key = "Content-Length:";
    if (line.len < key.len) return null;
    if (!std.mem.eql(u8, line[0..key.len], key)) return null;

    var i = key.len;
    while (i < line.len and (line[i] == ' ' or line[i] == '\t')) : (i += 1) {}
    if (i >= line.len) return null;

    var value: usize = 0;
    var has_digits = false;

    while (i < line.len) : (i += 1) {
        const ch = line[i];
        if (ch < '0' or ch > '9') return null;
        has_digits = true;
        const digit: usize = @intCast(ch - '0');
        if (value > (std.math.maxInt(usize) - digit) / 10) return null;
        value = (value * 10) + digit;
    }

    if (!has_digits) return null;
    return value;
}

fn unregisterFd(epoll_fd: c_int, fd: c_int) void {
    _ = c.epoll_ctl(epoll_fd, c.EPOLL_CTL_DEL, fd, null);
}

fn closeClientConn(epoll_fd: c_int, conn: *ClientConn) void {
    unregisterFd(epoll_fd, conn.fd);
    connUnset(conn.fd);
    _ = c.close(conn.fd);
    std.heap.c_allocator.destroy(conn);
}

fn closeAllClientConns(epoll_fd: c_int) void {
    var fd: usize = 0;
    while (fd < MAX_TRACKED_FDS) : (fd += 1) {
        if (conn_by_fd[fd]) |conn| {
            closeClientConn(epoll_fd, conn);
        }
    }
}

fn registerClientRead(epoll_fd: c_int, fd: c_int) bool {
    var ev: c.struct_epoll_event = std.mem.zeroes(c.struct_epoll_event);
    ev.events = c.EPOLLIN | c.EPOLLRDHUP | c.EPOLLERR | c.EPOLLHUP;
    ev.data.fd = fd;
    return c.epoll_ctl(epoll_fd, c.EPOLL_CTL_ADD, fd, &ev) == 0;
}

fn switchClientToWrite(epoll_fd: c_int, fd: c_int) bool {
    var ev: c.struct_epoll_event = std.mem.zeroes(c.struct_epoll_event);
    ev.events = c.EPOLLOUT | c.EPOLLRDHUP | c.EPOLLERR | c.EPOLLHUP;
    ev.data.fd = fd;
    return c.epoll_ctl(epoll_fd, c.EPOLL_CTL_MOD, fd, &ev) == 0;
}

fn consumeClientReadStep(conn: *ClientConn) i32 {
    while (conn.used + 1 < conn.buffer.len) {
        const n = c.recv(conn.fd, @ptrCast(&conn.buffer[conn.used]), conn.buffer.len - conn.used - 1, 0);
        if (n > 0) {
            conn.used += @intCast(n);
            conn.buffer[conn.used] = 0;
        } else if (n == 0) {
            return -1;
        } else {
            const err = getErrno();
            if (err == c.EINTR) continue;
            if (err == c.EAGAIN or err == c.EWOULDBLOCK) return 0;
            return -1;
        }

        if (!conn.headers_done) {
            while ((conn.scan_pos + 1) < conn.used) {
                if (conn.buffer[conn.scan_pos] == '\r' and conn.buffer[conn.scan_pos + 1] == '\n') {
                    const line_len = conn.scan_pos - conn.line_start;
                    if (line_len == 0) {
                        conn.headers_done = true;
                        const body_offset = conn.scan_pos + 2;
                        conn.expected_total = body_offset + conn.content_length;
                        if (conn.expected_total + 1 > conn.buffer.len) return -1;
                        conn.scan_pos += 2;
                        break;
                    }

                    if (!conn.content_length_seen) {
                        if (parseContentLength(conn.buffer[conn.line_start .. conn.line_start + line_len])) |parsed| {
                            conn.content_length = parsed;
                            conn.content_length_seen = true;
                        }
                    }

                    conn.scan_pos += 2;
                    conn.line_start = conn.scan_pos;
                    continue;
                }

                conn.scan_pos += 1;
            }
        }

        if (conn.headers_done and conn.used >= conn.expected_total) {
            conn.phase = .writing;
            return 1;
        }
    }

    return -1;
}

fn sendResponseWriteStep(conn: *ClientConn) i32 {
    const response = RESPONSE_OK;
    while (conn.write_off < response.len) {
        const n = c.send(conn.fd, @ptrCast(response.ptr + conn.write_off), response.len - conn.write_off, c.MSG_NOSIGNAL);
        if (n > 0) {
            conn.write_off += @intCast(n);
            continue;
        }

        if (n < 0) {
            const err = getErrno();
            if (err == c.EINTR) continue;
            if (err == c.EAGAIN or err == c.EWOULDBLOCK) return 0;
        }

        return -1;
    }

    return 1;
}

fn recvFdNonBlocking(control_fd: c_int, out_client_fd: *c_int) i32 {
    while (true) {
        var one: [1]u8 = [_]u8{0};
        var iov = c.struct_iovec{
            .iov_base = &one,
            .iov_len = one.len,
        };

        var control: [64]u8 align(@alignOf(c.struct_cmsghdr)) = [_]u8{0} ** 64;
        var msg: c.struct_msghdr = std.mem.zeroes(c.struct_msghdr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = @ptrCast(&control[0]);
        const fd_size: c_uint = @intCast(@sizeOf(c_int));
        msg.msg_controllen = @as(c_uint, @intCast(c.CMSG_SPACE(fd_size)));

        const n = c.recvmsg(control_fd, &msg, 0);
        if (n > 0) {
            const cmsg = c.CMSG_FIRSTHDR(&msg);
            if (cmsg == null) {
                return -1;
            }
            if (cmsg.?.*.cmsg_level != c.SOL_SOCKET or cmsg.?.*.cmsg_type != c.SCM_RIGHTS) {
                return -1;
            }

            const payload = @as([*]const u8, @ptrCast(c.CMSG_DATA(cmsg)));
            const bytes = payload[0..@sizeOf(c_int)];
            out_client_fd.* = std.mem.bytesToValue(c_int, bytes);
            return 1;
        }

        if (n == 0) return -1;

        const err = getErrno();
        if (err == c.EINTR) continue;
        if (err == c.EAGAIN or err == c.EWOULDBLOCK) return 0;
        return -1;
    }
}

fn processControlFd(epoll_fd: c_int, control_fd: c_int) i32 {
    while (true) {
        var client_fd: c_int = -1;
        const status = recvFdNonBlocking(control_fd, &client_fd);
        if (status < 0) return -1;
        if (status == 0) return 0;

        if (!setNonBlockingCloexec(client_fd)) {
            _ = c.close(client_fd);
            continue;
        }

        const conn = createClientConn(client_fd) orelse {
            _ = c.close(client_fd);
            continue;
        };

        if (!connSet(client_fd, conn)) {
            _ = c.close(client_fd);
            std.heap.c_allocator.destroy(conn);
            continue;
        }

        if (!registerClientRead(epoll_fd, client_fd)) {
            closeClientConn(epoll_fd, conn);
        }
    }
}

fn runLoopEpoll(server_fd: c_int) RunError!void {
    const epoll_fd = c.epoll_create1(c.EPOLL_CLOEXEC);
    if (epoll_fd < 0) return RunError.EpollCreateFailed;
    defer _ = c.close(epoll_fd);

    var listener_event: c.struct_epoll_event = std.mem.zeroes(c.struct_epoll_event);
    listener_event.events = c.EPOLLIN | c.EPOLLERR | c.EPOLLHUP;
    listener_event.data.fd = server_fd;
    if (c.epoll_ctl(epoll_fd, c.EPOLL_CTL_ADD, server_fd, &listener_event) < 0) {
        return RunError.EpollCtlFailed;
    }

    var controls: ControlSet = .{};
    defer controls.closeAll();
    defer closeAllClientConns(epoll_fd);

    var events: [MAX_EVENTS]c.struct_epoll_event = undefined;
    while (true) {
        const ready = c.epoll_wait(epoll_fd, &events, MAX_EVENTS, WAIT_TIMEOUT_MS);
        if (ready < 0) {
            const err = getErrno();
            if (err == c.EINTR) continue;
            return RunError.EpollCtlFailed;
        }

        var i: usize = 0;
        while (i < @as(usize, @intCast(ready))) : (i += 1) {
            const fd = events[i].data.fd;
            const revents = events[i].events;

            if (fd == server_fd) {
                if ((revents & (c.EPOLLERR | c.EPOLLHUP)) != 0) {
                    return RunError.EpollCtlFailed;
                }

                while (true) {
                    const control_fd = c.accept4(server_fd, null, null, c.SOCK_NONBLOCK | c.SOCK_CLOEXEC);
                    if (control_fd >= 0) {
                        if (!controls.add(control_fd)) {
                            _ = c.close(control_fd);
                            continue;
                        }

                        var ev: c.struct_epoll_event = std.mem.zeroes(c.struct_epoll_event);
                        ev.events = c.EPOLLIN | c.EPOLLRDHUP | c.EPOLLERR | c.EPOLLHUP;
                        ev.data.fd = control_fd;
                        if (c.epoll_ctl(epoll_fd, c.EPOLL_CTL_ADD, control_fd, &ev) < 0) {
                            _ = c.close(control_fd);
                            controls.removeAt(controls.count - 1);
                        }
                        continue;
                    }

                    const err = getErrno();
                    if (err == c.EINTR) continue;
                    if (err == c.EAGAIN or err == c.EWOULDBLOCK) break;
                    return RunError.EpollCtlFailed;
                }
                continue;
            }

            if (controls.find(fd)) |control_index| {
                if ((revents & (c.EPOLLERR | c.EPOLLHUP | c.EPOLLRDHUP)) != 0) {
                    unregisterFd(epoll_fd, fd);
                    _ = c.close(fd);
                    controls.removeAt(control_index);
                    continue;
                }

                if ((revents & c.EPOLLIN) != 0 and processControlFd(epoll_fd, fd) < 0) {
                    unregisterFd(epoll_fd, fd);
                    _ = c.close(fd);
                    controls.removeAt(control_index);
                }
                continue;
            }

            const conn = connGet(fd) orelse continue;
            if ((revents & (c.EPOLLERR | c.EPOLLHUP)) != 0) {
                closeClientConn(epoll_fd, conn);
                continue;
            }

            if (conn.phase == .reading) {
                if ((revents & c.EPOLLRDHUP) != 0) {
                    closeClientConn(epoll_fd, conn);
                    continue;
                }

                if ((revents & c.EPOLLIN) != 0) {
                    const status = consumeClientReadStep(conn);
                    if (status < 0) {
                        closeClientConn(epoll_fd, conn);
                        continue;
                    }
                    if (status > 0 and !switchClientToWrite(epoll_fd, fd)) {
                        closeClientConn(epoll_fd, conn);
                        continue;
                    }
                }
            } else {
                if ((revents & c.EPOLLOUT) != 0) {
                    const status = sendResponseWriteStep(conn);
                    if (status != 0) {
                        closeClientConn(epoll_fd, conn);
                        continue;
                    }
                }
            }
        }
    }
}

pub fn main() u8 {
    const socket_path = c.getenv("UNIX_SOCKET_PATH");
    if (socket_path == null or socket_path.?[0] == 0) {
        logErr("UNIX_SOCKET_PATH is required and cannot be empty\n", .{});
        return 1;
    }

    const server_fd = createUnixServer(socket_path.?) catch {
        logErr("failed to bind unix socket '{s}'\n", .{std.mem.span(socket_path.?)});
        return 1;
    };
    defer _ = c.close(server_fd);

    logErr("server3 (zig) listening for fd passing at {s}\n", .{std.mem.span(socket_path.?)});

    runLoopEpoll(server_fd) catch {
        return 1;
    };

    return 0;
}
