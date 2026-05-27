const std = @import("std");
const builtin = @import("builtin");

pub const X_SCORE_DIMS: usize = 14;
pub const X_SCORE_LANES: usize = 8;
pub const X_SCORE_TOPK: usize = 5;
pub const X_SCORE_SCALE: i16 = 10000;
const USE_PORTABLE_SIMD = builtin.cpu.arch == .x86_64 or builtin.cpu.arch == .aarch64;

const X_SCORE_MAGIC = "RNSPCST1";
const X_SCORE_EARLY_DISTANCE_MILLI: u64 = 143;
const X_SCORE_EARLY_DISTANCE_LIMIT: u64 = blk: {
    const scaled = (X_SCORE_SCALE * X_SCORE_EARLY_DISTANCE_MILLI) / 1000;
    break :blk scaled * scaled;
};

const XScoreIndexHeader = extern struct {
    magic: [8]u8,
    scale: i32,
    dims: i32,
    count: i32,
    partition_count: i32,
    node_count: i32,
    block_count: i32,
};

const XScorePartitionEntry = extern struct {
    key: u32,
    root: i32,
    start: i32,
    len: i32,
    min: [X_SCORE_DIMS]i16,
    max: [X_SCORE_DIMS]i16,
};

const XScoreNodeEntry = extern struct {
    left: i32,
    right: i32,
    start_block: i32,
    len: i32,
    min: [X_SCORE_DIMS]i16,
    max: [X_SCORE_DIMS]i16,
};

const PartitionCandidate = struct {
    bound: u64,
    index: u32,
};

const NodeStackEntry = struct {
    node_index: u32,
    bound: u64,
};

comptime {
    if (@sizeOf(XScoreIndexHeader) != 32) @compileError("XScoreIndexHeader size must be 32");
    if (@sizeOf(XScorePartitionEntry) != 72) @compileError("XScorePartitionEntry size must be 72");
    if (@sizeOf(XScoreNodeEntry) != 72) @compileError("XScoreNodeEntry size must be 72");
}

pub const Detector = struct {
    allocator: std.mem.Allocator,
    raw: []u8,

    partitions: []align(1) const XScorePartitionEntry,
    nodes: []align(1) const XScoreNodeEntry,
    vectors: []align(1) const i16,
    labels: []const u8,

    key_partition_indices: []u32,
    key_partition_offsets: [257]u32,
    key_partition_direct: [256]i32,

    count: u32,
    partition_count: u32,
    node_count: u32,
    block_count: u32,

    pub fn open(allocator: std.mem.Allocator, path: []const u8) !Detector {
        var file = try std.fs.cwd().openFile(path, .{});
        defer file.close();

        const stat = try file.stat();
        if (stat.size < @sizeOf(XScoreIndexHeader)) {
            return error.InvalidIndexHeader;
        }
        if (stat.size > std.math.maxInt(usize)) {
            return error.InvalidIndexSize;
        }

        const raw_len: usize = @intCast(stat.size);
        const raw = try allocator.alloc(u8, raw_len);
        errdefer allocator.free(raw);

        const n = try file.readAll(raw);
        if (n != raw.len) {
            return error.InvalidIndexRead;
        }

        const header_ptr = std.mem.bytesAsValue(XScoreIndexHeader, raw[0..@sizeOf(XScoreIndexHeader)]);
        const header = header_ptr.*;

        if (!std.mem.eql(u8, header.magic[0..], X_SCORE_MAGIC)) {
            return error.InvalidIndexMagic;
        }
        if (header.scale != X_SCORE_SCALE or header.dims != X_SCORE_DIMS) {
            return error.InvalidIndexParams;
        }
        if (header.count < 0 or header.partition_count < 0 or header.node_count < 0 or header.block_count < 0) {
            return error.InvalidIndexHeader;
        }

        const count: u32 = @intCast(header.count);
        const partition_count: u32 = @intCast(header.partition_count);
        const node_count: u32 = @intCast(header.node_count);
        const block_count: u32 = @intCast(header.block_count);

        var offset: usize = @sizeOf(XScoreIndexHeader);

        const partitions_bytes = try std.math.mul(usize, partition_count, @sizeOf(XScorePartitionEntry));
        const nodes_bytes = try std.math.mul(usize, node_count, @sizeOf(XScoreNodeEntry));
        const vectors_len = try std.math.mul(usize, block_count, X_SCORE_DIMS * X_SCORE_LANES);
        const vectors_bytes = try std.math.mul(usize, vectors_len, @sizeOf(i16));
        const labels_bytes = try std.math.mul(usize, block_count, X_SCORE_LANES);

        if (offset + partitions_bytes > raw.len) return error.InvalidIndexLayout;
        const partitions = std.mem.bytesAsSlice(XScorePartitionEntry, raw[offset .. offset + partitions_bytes]);
        offset += partitions_bytes;

        if (offset + nodes_bytes > raw.len) return error.InvalidIndexLayout;
        const nodes = std.mem.bytesAsSlice(XScoreNodeEntry, raw[offset .. offset + nodes_bytes]);
        offset += nodes_bytes;

        if (offset + vectors_bytes > raw.len) return error.InvalidIndexLayout;
        const vectors = std.mem.bytesAsSlice(i16, raw[offset .. offset + vectors_bytes]);
        offset += vectors_bytes;

        if (offset + labels_bytes > raw.len) return error.InvalidIndexLayout;
        const labels = raw[offset .. offset + labels_bytes];
        if (count > labels.len) return error.InvalidIndexLayout;

        var key_partition_offsets: [257]u32 = [_]u32{0} ** 257;
        var key_partition_direct: [256]i32 = [_]i32{-1} ** 256;
        var key_counts: [256]u32 = [_]u32{0} ** 256;

        for (0..partition_count) |i| {
            const key = partitions[i].key & 255;
            key_counts[key] += 1;
        }

        var running: u32 = 0;
        for (0..256) |k| {
            key_partition_offsets[k] = running;
            running += key_counts[k];
        }
        key_partition_offsets[256] = running;

        var key_partition_indices = try allocator.alloc(u32, partition_count);
        errdefer allocator.free(key_partition_indices);

        var key_pos: [256]u32 = [_]u32{0} ** 256;
        std.mem.copyForwards(u32, key_pos[0..], key_partition_offsets[0..256]);

        for (0..partition_count) |i| {
            const key = partitions[i].key & 255;
            const pos = key_pos[key];
            key_partition_indices[pos] = @intCast(i);
            key_pos[key] = pos + 1;

            if (key_partition_direct[key] == -1) {
                key_partition_direct[key] = @intCast(i);
            } else {
                key_partition_direct[key] = -2;
            }
        }

        return Detector{
            .allocator = allocator,
            .raw = raw,
            .partitions = partitions,
            .nodes = nodes,
            .vectors = vectors,
            .labels = labels,
            .key_partition_indices = key_partition_indices,
            .key_partition_offsets = key_partition_offsets,
            .key_partition_direct = key_partition_direct,
            .count = count,
            .partition_count = partition_count,
            .node_count = node_count,
            .block_count = block_count,
        };
    }

    pub fn close(self: *Detector) void {
        self.allocator.free(self.key_partition_indices);
        self.allocator.free(self.raw);
        self.* = undefined;
    }

    pub fn predictFraudCount(self: *const Detector, query: *const [X_SCORE_DIMS]f64) u8 {
        if (self.count == 0) return 0;
        if (self.labels.len == 0 or self.vectors.len == 0) return 0;

        var q: [X_SCORE_DIMS]i16 = undefined;
        for (query, 0..) |v, d| {
            q[d] = quantizeValue(v);
        }

        var top_dist: [X_SCORE_TOPK]u64 = [_]u64{std.math.maxInt(u64)} ** X_SCORE_TOPK;
        var top_label: [X_SCORE_TOPK]u8 = [_]u8{0} ** X_SCORE_TOPK;

        if (self.partition_count == 0 or self.partition_count > 256 or self.nodes.len == 0) {
            self.searchExact(&q, &top_dist, &top_label);
        } else {
            const qkey = computePartitionKey(&q) & 255;
            const direct = self.key_partition_direct[qkey];

            if (direct >= 0) {
                const p = self.partitions[@intCast(direct)];
                if (p.root >= 0) {
                    _ = self.searchNodeIterative(@intCast(p.root), 0, &q, &top_dist, &top_label);
                }
            } else {
                const begin = self.key_partition_offsets[qkey];
                const end = self.key_partition_offsets[qkey + 1];
                var pos = begin;
                while (pos < end and !earlyDone(&top_dist)) : (pos += 1) {
                    const pidx = self.key_partition_indices[pos];
                    const p = self.partitions[pidx];
                    if (p.root < 0) continue;
                    _ = self.searchNodeIterative(@intCast(p.root), 0, &q, &top_dist, &top_label);
                }
            }

            var entries: [256]PartitionCandidate = undefined;
            var entry_len: u32 = 0;
            const cutoff = top_dist[X_SCORE_TOPK - 1];

            var i: u32 = 0;
            while (i < self.partition_count and !earlyDone(&top_dist)) : (i += 1) {
                const p = self.partitions[i];
                if ((p.key & 255) == qkey) continue;
                if (p.root < 0) continue;

                const bound = lowerBoundPartitionSqCutoff(&q, &p, cutoff);
                if (bound < cutoff and entry_len < entries.len) {
                    insertPartitionCandidateSorted(entries[0..], &entry_len, bound, i);
                }
            }

            i = 0;
            while (i < entry_len and !earlyDone(&top_dist)) : (i += 1) {
                const e = entries[i];
                if (e.bound >= top_dist[X_SCORE_TOPK - 1]) break;
                const p = self.partitions[e.index];
                if (p.root < 0) continue;
                _ = self.searchNodeIterative(@intCast(p.root), e.bound, &q, &top_dist, &top_label);
            }
        }

        var fraud_count: u8 = 0;
        for (0..X_SCORE_TOPK) |i| {
            if (top_dist[i] == std.math.maxInt(u64)) continue;
            if (top_label[i] == 1) fraud_count += 1;
        }
        return fraud_count;
    }

    pub fn predictLabel(self: *const Detector, query: *const [X_SCORE_DIMS]f64) u8 {
        return if (self.predictFraudCount(query) >= 3) 1 else 0;
    }

    fn scanBlocksLinear(
        self: *const Detector,
        start_block: u32,
        len: u32,
        q: *const [X_SCORE_DIMS]i16,
        top_dist: *[X_SCORE_TOPK]u64,
        top_label: *[X_SCORE_TOPK]u8,
    ) bool {
        if (len == 0) return false;

        const max_blocks = self.block_count;
        const blocks = (len + X_SCORE_LANES - 1) / X_SCORE_LANES;
        var remaining = len;

        var b: u32 = 0;
        while (b < blocks) : (b += 1) {
            const block_idx = start_block + b;
            if (block_idx >= max_blocks) return false;

            const lane_count: u32 = if (remaining >= X_SCORE_LANES) X_SCORE_LANES else remaining;
            remaining -= lane_count;

            const block_base = @as(usize, block_idx) * X_SCORE_DIMS * X_SCORE_LANES;
            const label_base = @as(usize, block_idx) * X_SCORE_LANES;
            var dists: [X_SCORE_LANES]u64 = undefined;
            scanBlockDistances(self.vectors, block_base, q, &dists);
            var worst = top_dist[X_SCORE_TOPK - 1];

            var lane: u32 = 0;
            while (lane < lane_count) : (lane += 1) {
                const dist = dists[lane];
                if (dist < worst) {
                    topkInsert(dist, self.labels[label_base + lane], top_dist, top_label);
                    worst = top_dist[X_SCORE_TOPK - 1];
                }
            }

            if (earlyDone(top_dist)) return true;
        }

        return false;
    }

    fn searchExact(
        self: *const Detector,
        q: *const [X_SCORE_DIMS]i16,
        top_dist: *[X_SCORE_TOPK]u64,
        top_label: *[X_SCORE_TOPK]u8,
    ) void {
        var remaining = self.count;
        var block: u32 = 0;

        while (block < self.block_count and remaining > 0) : (block += 1) {
            const lane_count: u32 = if (remaining >= X_SCORE_LANES) X_SCORE_LANES else remaining;
            const block_base = @as(usize, block) * X_SCORE_DIMS * X_SCORE_LANES;
            const label_base = @as(usize, block) * X_SCORE_LANES;
            var dists: [X_SCORE_LANES]u64 = undefined;
            scanBlockDistances(self.vectors, block_base, q, &dists);

            var lane: u32 = 0;
            while (lane < lane_count) : (lane += 1) {
                const dist = dists[lane];
                topkInsert(dist, self.labels[label_base + lane], top_dist, top_label);
            }

            remaining -= lane_count;
        }
    }

    fn searchNodeIterative(
        self: *const Detector,
        root: u32,
        root_bound: u64,
        q: *const [X_SCORE_DIMS]i16,
        top_dist: *[X_SCORE_TOPK]u64,
        top_label: *[X_SCORE_TOPK]u8,
    ) bool {
        if (root >= self.node_count) return false;

        var stack: [256]NodeStackEntry = undefined;
        var stack_len: u32 = 0;

        var current = root;
        var current_bound = root_bound;

        while (true) {
            const worst = top_dist[X_SCORE_TOPK - 1];
            if (current_bound <= worst) {
                const node = self.nodes[current];
                if (node.left < 0 or node.right < 0) {
                    if (node.start_block >= 0 and node.len > 0) {
                        if (self.scanBlocksLinear(@intCast(node.start_block), @intCast(node.len), q, top_dist, top_label)) {
                            return true;
                        }
                    }
                } else {
                    const left: u32 = @intCast(node.left);
                    const right: u32 = @intCast(node.right);
                    if (left < self.node_count and right < self.node_count) {
                        const lb = lowerBoundNodeSqCutoff(q, &self.nodes[left], worst);
                        const rb = lowerBoundNodeSqCutoff(q, &self.nodes[right], worst);

                        var near_idx = left;
                        var far_idx = right;
                        var near_bound = lb;
                        var far_bound = rb;

                        if (rb < lb) {
                            near_idx = right;
                            far_idx = left;
                            near_bound = rb;
                            far_bound = lb;
                        }

                        if (far_bound <= worst and stack_len < stack.len) {
                            stack[stack_len] = .{ .node_index = far_idx, .bound = far_bound };
                            stack_len += 1;
                        }

                        if (near_bound <= worst) {
                            current = near_idx;
                            current_bound = near_bound;
                            continue;
                        }
                    }
                }
            }

            if (stack_len == 0) break;
            stack_len -= 1;
            current = stack[stack_len].node_index;
            current_bound = stack[stack_len].bound;
        }

        return false;
    }
};

fn earlyDone(top_dist: *const [X_SCORE_TOPK]u64) bool {
    return top_dist[X_SCORE_TOPK - 1] <= X_SCORE_EARLY_DISTANCE_LIMIT;
}

fn quantizeValue(value: f64) i16 {
    if (value <= -1.0) return -X_SCORE_SCALE;
    if (value <= 0.0) return 0;
    if (value >= 1.0) return X_SCORE_SCALE;

    var scaled = value * @as(f64, @floatFromInt(X_SCORE_SCALE));
    scaled += if (scaled >= 0.0) 0.5 else -0.5;
    return @intFromFloat(scaled);
}

fn computePartitionKey(q: *const [X_SCORE_DIMS]i16) u32 {
    var key: u32 = 0;

    if (q[5] >= 0) key |= 1 << 0;
    if (q[9] > 0) key |= 1 << 1;
    if (q[10] > 0) key |= 1 << 2;
    if (q[11] > 0) key |= 1 << 3;

    var mcc_bucket: u32 = 0;
    if (q[12] <= 2000) {
        mcc_bucket = 0;
    } else if (q[12] <= 3000) {
        mcc_bucket = 1;
    } else if (q[12] <= 7500) {
        mcc_bucket = 2;
    } else {
        mcc_bucket = 3;
    }
    key |= mcc_bucket << 4;

    if (q[2] > 1013) key |= 1 << 6;
    if (q[8] > 2500) key |= 1 << 7;

    return key;
}

fn lowerBoundPartitionSqCutoff(
    q: *const [X_SCORE_DIMS]i16,
    partition: *align(1) const XScorePartitionEntry,
    cutoff: u64,
) u64 {
    var sum: u64 = 0;
    var d: usize = 0;
    while (d < X_SCORE_DIMS) : (d += 1) {
        const qq: i64 = q[d];
        const lo: i64 = partition.min[d];
        const hi: i64 = partition.max[d];

        var diff: i64 = 0;
        if (qq < lo) {
            diff = lo - qq;
        } else if (qq > hi) {
            diff = qq - hi;
        }

        sum += @as(u64, @intCast(diff * diff));
        if (sum > cutoff) return sum;
    }
    return sum;
}

fn lowerBoundNodeSqCutoff(
    q: *const [X_SCORE_DIMS]i16,
    node: *align(1) const XScoreNodeEntry,
    cutoff: u64,
) u64 {
    var sum: u64 = 0;
    var d: usize = 0;
    while (d < X_SCORE_DIMS) : (d += 1) {
        const qq: i64 = q[d];
        const lo: i64 = node.min[d];
        const hi: i64 = node.max[d];

        var diff: i64 = 0;
        if (qq < lo) {
            diff = lo - qq;
        } else if (qq > hi) {
            diff = qq - hi;
        }

        sum += @as(u64, @intCast(diff * diff));
        if (sum > cutoff) return sum;
    }
    return sum;
}

fn insertPartitionCandidateSorted(
    entries: []PartitionCandidate,
    entry_len: *u32,
    bound: u64,
    index: u32,
) void {
    var pos = entry_len.*;
    while (pos > 0 and entries[pos - 1].bound > bound) : (pos -= 1) {
        entries[pos] = entries[pos - 1];
    }
    entries[pos] = .{
        .bound = bound,
        .index = index,
    };
    entry_len.* += 1;
}

fn topkInsert(dist: u64, label: u8, top_dist: *[X_SCORE_TOPK]u64, top_label: *[X_SCORE_TOPK]u8) void {
    if (dist >= top_dist[4]) return;

    if (dist < top_dist[3]) {
        top_dist[4] = top_dist[3];
        top_label[4] = top_label[3];

        if (dist < top_dist[2]) {
            top_dist[3] = top_dist[2];
            top_label[3] = top_label[2];

            if (dist < top_dist[1]) {
                top_dist[2] = top_dist[1];
                top_label[2] = top_label[1];

                if (dist < top_dist[0]) {
                    top_dist[1] = top_dist[0];
                    top_label[1] = top_label[0];
                    top_dist[0] = dist;
                    top_label[0] = label;
                    return;
                }

                top_dist[1] = dist;
                top_label[1] = label;
                return;
            }

            top_dist[2] = dist;
            top_label[2] = label;
            return;
        }

        top_dist[3] = dist;
        top_label[3] = label;
        return;
    }

    top_dist[4] = dist;
    top_label[4] = label;
}

fn scanBlockDistances(
    vectors: []align(1) const i16,
    block_base: usize,
    q: *const [X_SCORE_DIMS]i16,
    out_dist: *[X_SCORE_LANES]u64,
) void {
    if (comptime USE_PORTABLE_SIMD) {
        const Vec8i16 = @Vector(X_SCORE_LANES, i16);
        const Vec8i32 = @Vector(X_SCORE_LANES, i32);
        const Vec8i64 = @Vector(X_SCORE_LANES, i64);

        var acc32: Vec8i32 = @splat(0);
        var acc64: Vec8i64 = @splat(0);
        var pending: u8 = 0;

        var d: usize = 0;
        while (d < X_SCORE_DIMS) : (d += 1) {
            const dim_base = block_base + d * X_SCORE_LANES;
            const vals16: Vec8i16 = .{
                vectors[dim_base + 0],
                vectors[dim_base + 1],
                vectors[dim_base + 2],
                vectors[dim_base + 3],
                vectors[dim_base + 4],
                vectors[dim_base + 5],
                vectors[dim_base + 6],
                vectors[dim_base + 7],
            };
            const vals32: Vec8i32 = @intCast(vals16);
            const qq: Vec8i32 = @splat(@as(i32, q[d]));
            const diff: Vec8i32 = vals32 - qq;
            const sq: Vec8i32 = diff * diff;
            acc32 += sq;

            pending += 1;
            if (pending == 4) {
                acc64 += widenI32ToI64(acc32);
                acc32 = @splat(0);
                pending = 0;
            }
        }

        if (pending != 0) {
            acc64 += widenI32ToI64(acc32);
        }

        inline for (0..X_SCORE_LANES) |lane| {
            out_dist[lane] = @intCast(acc64[lane]);
        }
        return;
    }

    inline for (0..X_SCORE_LANES) |lane| {
        out_dist[lane] = 0;
    }

    var d: usize = 0;
    while (d < X_SCORE_DIMS) : (d += 1) {
        const qq: i64 = q[d];
        const dim_base = block_base + d * X_SCORE_LANES;
        inline for (0..X_SCORE_LANES) |lane| {
            const diff: i64 = qq - @as(i64, vectors[dim_base + lane]);
            out_dist[lane] += @as(u64, @intCast(diff * diff));
        }
    }
}

fn widenI32ToI64(v: @Vector(X_SCORE_LANES, i32)) @Vector(X_SCORE_LANES, i64) {
    return .{
        @as(i64, v[0]),
        @as(i64, v[1]),
        @as(i64, v[2]),
        @as(i64, v[3]),
        @as(i64, v[4]),
        @as(i64, v[5]),
        @as(i64, v[6]),
        @as(i64, v[7]),
    };
}
