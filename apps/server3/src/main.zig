const std = @import("std");
const linux = std.os.linux;
const posix = std.posix;
const detector_mod = @import("x-score.zig");
const responses = @import("responses.zig");
const tx = @import("transaction_context.zig");

const MAX_REQUEST_SIZE = 64 * 1024;
const SCM_RIGHTS: i32 = 1;
const REQ_GET_READY = "GET /ready ";
const REQ_POST_FRAUD_SCORE = "POST /fraud-score ";

const Cmsghdr = extern struct {
    cmsg_len: usize,
    cmsg_level: i32,
    cmsg_type: i32,
};

const FD_SIZE = @sizeOf(posix.fd_t);
const CMSG_HDR_LEN = cmsgAlign(@sizeOf(Cmsghdr));
const CMSG_LEN_FD = CMSG_HDR_LEN + FD_SIZE;
const CMSG_SPACE_FD = CMSG_HDR_LEN + cmsgAlign(FD_SIZE);

const HttpRequestView = struct {
    body_offset: usize = 0,
    body_len: usize = 0,
};

const ReadState = struct {
    used: usize = 0,
    scan_pos: usize = 0,
    line_start: usize = 0,
    content_length: usize = 0,
    expected_total: usize = 0,
    headers_done: bool = false,
    content_length_seen: bool = false,
};

const ConsumeResult = enum(i8) {
    fail = -1,
    need_more = 0,
    complete = 1,
};

fn logErr(comptime fmt: []const u8, args: anytype) void {
    std.io.getStdErr().writer().print(fmt, args) catch {};
}

inline fn cmsgAlign(len: usize) usize {
    const alignment = @sizeOf(usize);
    const mask: usize = alignment - 1;
    return (len + mask) & ~mask;
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

fn consumeRead(state: *ReadState, view: *HttpRequestView, buf: []const u8) ConsumeResult {
    if (!state.headers_done) {
        while ((state.scan_pos + 1) < state.used) {
            if (buf[state.scan_pos] == '\r' and buf[state.scan_pos + 1] == '\n') {
                const line_len = state.scan_pos - state.line_start;
                if (line_len == 0) {
                    state.headers_done = true;
                    view.body_offset = state.scan_pos + 2;
                    view.body_len = state.content_length;
                    state.expected_total = view.body_offset + state.content_length;
                    if (state.expected_total > buf.len) return .fail;
                    state.scan_pos += 2;
                    break;
                }

                if (!state.content_length_seen) {
                    if (parseContentLength(buf[state.line_start .. state.line_start + line_len])) |parsed| {
                        state.content_length = parsed;
                        state.content_length_seen = true;
                    }
                }

                state.scan_pos += 2;
                state.line_start = state.scan_pos;
                continue;
            }

            state.scan_pos += 1;
        }
    }

    if (state.headers_done and state.used >= state.expected_total) {
        return .complete;
    }

    if (state.used + 1 >= buf.len) {
        return .fail;
    }

    return .need_more;
}

fn readFullRequest(client_fd: posix.socket_t, buffer: *[MAX_REQUEST_SIZE]u8, out_view: *HttpRequestView) ?[]const u8 {
    var state: ReadState = .{};
    out_view.* = .{};

    while (true) {
        const n = posix.recv(client_fd, buffer[state.used .. buffer.len - 1], 0) catch return null;

        if (n == 0) return null;
        state.used += n;
        buffer[state.used] = 0;

        const step = consumeRead(&state, out_view, buffer[0..]);
        if (step == .fail) return null;
        if (step == .complete) {
            return buffer[0..state.expected_total];
        }
    }
}

fn sendAll(client_fd: posix.socket_t, bytes: []const u8) bool {
    var offset: usize = 0;

    while (offset < bytes.len) {
        const n = posix.send(client_fd, bytes[offset..], posix.MSG.NOSIGNAL) catch return false;
        if (n == 0) return false;
        offset += n;
    }

    return true;
}

fn createUnixServer(socket_path: []const u8) !posix.socket_t {
    const server_fd = try posix.socket(posix.AF.UNIX, posix.SOCK.STREAM | posix.SOCK.CLOEXEC, 0);
    errdefer posix.close(server_fd);

    const addr = try std.net.Address.initUnix(socket_path);
    try posix.bind(server_fd, &addr.any, addr.getOsSockLen());
    try posix.listen(server_fd, 128);

    return server_fd;
}

fn recvPassedFd(control_fd: posix.socket_t) ?posix.fd_t {
    var payload: [1]u8 = .{0};
    var iov = [1]posix.iovec{.{
        .base = payload[0..].ptr,
        .len = payload.len,
    }};

    var control: [CMSG_SPACE_FD]u8 align(@alignOf(Cmsghdr)) = [_]u8{0} ** CMSG_SPACE_FD;
    var msg: linux.msghdr = .{
        .name = null,
        .namelen = 0,
        .iov = @ptrCast(&iov),
        .iovlen = 1,
        .control = &control,
        .controllen = @intCast(control.len),
        .flags = 0,
    };

    const received = while (true) {
        const rc = linux.recvmsg(control_fd, &msg, 0);
        switch (posix.errno(rc)) {
            .SUCCESS => break @as(usize, @intCast(rc)),
            .INTR => continue,
            else => return null,
        }
    };
    if (received == 0) return null;

    if (@as(usize, msg.controllen) < CMSG_LEN_FD) return null;

    const control_ptr = msg.control orelse return null;
    const cmsg: *const Cmsghdr = @ptrCast(@alignCast(control_ptr));

    if (cmsg.cmsg_level != posix.SOL.SOCKET or cmsg.cmsg_type != SCM_RIGHTS) return null;
    if (cmsg.cmsg_len < CMSG_LEN_FD) return null;

    var client_fd: posix.fd_t = -1;
    const fd_bytes = std.mem.asBytes(&client_fd);
    std.mem.copyForwards(u8, fd_bytes, control[CMSG_HDR_LEN .. CMSG_HDR_LEN + FD_SIZE]);
    if (client_fd < 0) return null;

    return client_fd;
}

fn warmUp(detector: *const detector_mod.Detector) void {
    const warmup_body =
        "{\"id\":\"tx-warmup\"," ++
        "\"transaction\":{\"amount\":384.88,\"installments\":3,\"requested_at\":\"2026-03-11T20:23:35Z\"}," ++
        "\"customer\":{\"avg_amount\":769.76,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-009\",\"MERC-001\",\"MERC-001\"]}," ++
        "\"merchant\":{\"id\":\"MERC-001\",\"mcc\":\"5912\",\"avg_amount\":298.95}," ++
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":13.7090520965}," ++
        "\"last_transaction\":{\"timestamp\":\"2026-03-11T14:58:35Z\",\"km_from_current\":18.8626479774}}";

    const ctx = tx.TransactionContext.fromBody(warmup_body) orelse return;
    var vector: [14]f64 = undefined;
    ctx.toVector(&vector);

    var warmup_sink: u8 = 0;
    var i: usize = 0;
    while (i < 32) : (i += 1) {
        warmup_sink ^= detector.predictFraudCount(&vector);
    }
    std.mem.doNotOptimizeAway(warmup_sink);
}

fn serveControlConnection(control_fd: posix.socket_t, detector: *const detector_mod.Detector) void {
    defer posix.close(control_fd);

    while (true) {
        const client_fd = recvPassedFd(control_fd) orelse break;
        handleClient(client_fd, detector);
    }
}

fn handleClient(client_fd: posix.socket_t, detector: *const detector_mod.Detector) void {
    defer posix.close(client_fd);

    var request_buf: [MAX_REQUEST_SIZE]u8 = undefined;
    var req_view: HttpRequestView = .{};
    const request = readFullRequest(client_fd, &request_buf, &req_view) orelse return;

    if (std.mem.startsWith(u8, request, REQ_GET_READY)) {
        _ = sendAll(client_fd, responses.response_ready.data);
        return;
    }

    if (std.mem.startsWith(u8, request, REQ_POST_FRAUD_SCORE)) {
        if (req_view.body_offset > request.len or req_view.body_len > (request.len - req_view.body_offset)) {
            _ = sendAll(client_fd, responses.response_not_found.data);
            return;
        }

        const body = request[req_view.body_offset .. req_view.body_offset + req_view.body_len];
        const ctx = tx.TransactionContext.fromBody(body) orelse {
            _ = sendAll(client_fd, responses.response_bad_request.data);
            return;
        };

        var vector: [14]f64 = undefined;
        ctx.toVector(&vector);

        const fraud_count = detector.predictFraudCount(&vector);
        const resp = responses.fraudResponseByCount(fraud_count);
        _ = sendAll(client_fd, resp.data);
        return;
    }

    _ = sendAll(client_fd, responses.response_not_found.data);
}

pub fn main() u8 {
    const path_z = posix.getenv("UNIX_SOCKET_PATH") orelse {
        logErr("UNIX_SOCKET_PATH is required and cannot be empty\n", .{});
        return 1;
    };
    const socket_path = path_z;
    if (socket_path.len == 0) {
        logErr("UNIX_SOCKET_PATH is required and cannot be empty\n", .{});
        return 1;
    }

    const xscore_path_z = posix.getenv("X_SCORE_INDEX_PATH") orelse {
        logErr("X_SCORE_INDEX_PATH is required and cannot be empty\n", .{});
        return 1;
    };
    if (xscore_path_z.len == 0) {
        logErr("X_SCORE_INDEX_PATH is required and cannot be empty\n", .{});
        return 1;
    }

    var detector = detector_mod.Detector.open(std.heap.c_allocator, xscore_path_z) catch {
        logErr("failed to load x-score index from {s}\n", .{xscore_path_z});
        return 1;
    };
    defer detector.close();
    warmUp(&detector);

    posix.unlink(path_z) catch |err| switch (err) {
        error.FileNotFound => {},
        else => {},
    };

    const server_fd = createUnixServer(socket_path) catch {
        logErr("failed to bind unix socket '{s}'\n", .{socket_path});
        return 1;
    };
    defer posix.close(server_fd);
    defer posix.unlink(path_z) catch {};

    logErr("server3 (zig fdpass) listening at {s}\n", .{socket_path});

    while (true) {
        const control_fd = posix.accept(server_fd, null, null, posix.SOCK.CLOEXEC) catch |err| {
            switch (err) {
                error.ConnectionAborted, error.WouldBlock => continue,
                else => continue,
            }
        };

        serveControlConnection(control_fd, &detector);
    }
}
