const std = @import("std");

pub const TX2_ID_MAX_LEN: usize = 13;
pub const TX2_ID_BUF_LEN: usize = TX2_ID_MAX_LEN + 1;
pub const TX2_MERCHANT_ID_LEN: usize = 8;
pub const TX2_MERCHANT_ID_BUF_LEN: usize = TX2_MERCHANT_ID_LEN + 1;
pub const TX2_MCC_LEN: usize = 4;
pub const TX2_MCC_BUF_LEN: usize = TX2_MCC_LEN + 1;
pub const TX2_MAX_KNOWN_MERCHANTS: usize = 5;

const NORMALIZATION_MAX_AMOUNT = 10000.0;
const NORMALIZATION_MAX_INSTALLMENTS = 12.0;
const NORMALIZATION_AMOUNT_VS_AVG_RATIO = 10.0;
const NORMALIZATION_MAX_MINUTES = 1440.0;
const NORMALIZATION_MAX_KM = 1000.0;
const NORMALIZATION_MAX_TX_COUNT_24H = 20.0;
const NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT = 10000.0;

const ParentKey = enum(u8) {
    none = 0,
    transaction,
    customer,
    merchant,
    terminal,
    last_transaction,
};

const ChildKey = enum(u8) {
    none = 0,
    id,
    amount,
    installments,
    requested_at,
    avg_amount,
    tx_count_24h,
    known_merchants,
    mcc,
    is_online,
    card_present,
    km_from_home,
    timestamp,
    km_from_current,
};

const ParseState = enum(u8) {
    find_key = 0,
    after_key = 1,
    find_value = 2,
    after_value = 3,
};

pub const TransactionContext = struct {
    id: [TX2_ID_BUF_LEN]u8 = [_]u8{0} ** TX2_ID_BUF_LEN,

    transaction_amount: f64 = 0.0,
    transaction_installments: u8 = 0,
    transaction_requested_at: i64 = 0,

    customer_avg_amount: f64 = 0.0,
    customer_tx_count_24h: u8 = 0,
    customer_known_merchants: [TX2_MAX_KNOWN_MERCHANTS][TX2_MERCHANT_ID_BUF_LEN]u8 = [_][TX2_MERCHANT_ID_BUF_LEN]u8{[_]u8{0} ** TX2_MERCHANT_ID_BUF_LEN} ** TX2_MAX_KNOWN_MERCHANTS,
    customer_known_merchants_len: u8 = 0,

    merchant_id: [TX2_MERCHANT_ID_BUF_LEN]u8 = [_]u8{0} ** TX2_MERCHANT_ID_BUF_LEN,
    merchant_mcc: [TX2_MCC_BUF_LEN]u8 = [_]u8{0} ** TX2_MCC_BUF_LEN,
    merchant_avg_amount: f64 = 0.0,
    merchant_known: u8 = 0,
    merchant_mcc_risk: f64 = 0.5,

    terminal_is_online: u8 = 0,
    terminal_card_present: u8 = 0,
    terminal_km_from_home: f64 = 0.0,

    has_last_transaction: u8 = 0,
    last_transaction_timestamp: i64 = 0,
    last_transaction_km_from_current: f64 = 0.0,

    pub fn fromBody(body: []const u8) ?TransactionContext {
        var ctx = TransactionContext{};
        if (!ctx.parse(body)) {
            return null;
        }

        ctx.merchant_known = merchantInKnownList(&ctx);
        ctx.merchant_mcc_risk = merchantMccRiskOrDefault(&ctx.merchant_mcc);
        return ctx;
    }

    pub fn toVector(self: *const TransactionContext, out: *[14]f64) void {
        const amount = clamp01(self.transaction_amount / NORMALIZATION_MAX_AMOUNT);
        const installments = clamp01(@as(f64, @floatFromInt(self.transaction_installments)) / NORMALIZATION_MAX_INSTALLMENTS);

        const customer_avg = if (self.customer_avg_amount < 1e-9) 1e-9 else self.customer_avg_amount;
        const amount_vs_avg = clamp01(self.transaction_amount / customer_avg / NORMALIZATION_AMOUNT_VS_AVG_RATIO);

        const hour_of_day = @as(f64, @floatFromInt(hourOfDayFromEpoch(self.transaction_requested_at))) / 23.0;
        const day_of_week = @as(f64, @floatFromInt(dayOfWeekFromEpoch(self.transaction_requested_at))) / 6.0;

        var minutes_since_last: f64 = -1.0;
        var km_from_last_tx: f64 = -1.0;
        if (self.has_last_transaction == 1) {
            const diff_minutes = @as(f64, @floatFromInt(self.transaction_requested_at - self.last_transaction_timestamp)) / 60.0;
            minutes_since_last = clamp01(diff_minutes / NORMALIZATION_MAX_MINUTES);
            km_from_last_tx = clamp01(self.last_transaction_km_from_current / NORMALIZATION_MAX_KM);
        }

        const km_from_home = clamp01(self.terminal_km_from_home / NORMALIZATION_MAX_KM);
        const tx_count_24h = clamp01(@as(f64, @floatFromInt(self.customer_tx_count_24h)) / NORMALIZATION_MAX_TX_COUNT_24H);
        const is_online: f64 = if (self.terminal_is_online == 1) 1.0 else 0.0;
        const card_present: f64 = if (self.terminal_card_present == 1) 1.0 else 0.0;
        const unknown_merchant: f64 = if (self.merchant_known == 1) 0.0 else 1.0;
        const mcc_risk = self.merchant_mcc_risk;
        const merchant_avg_amount = clamp01(self.merchant_avg_amount / NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT);

        out.* = .{
            amount,
            installments,
            amount_vs_avg,
            hour_of_day,
            day_of_week,
            minutes_since_last,
            km_from_last_tx,
            km_from_home,
            tx_count_24h,
            is_online,
            card_present,
            unknown_merchant,
            mcc_risk,
            merchant_avg_amount,
        };
    }

    fn parse(self: *TransactionContext, body: []const u8) bool {
        var i: usize = 0;
        var depth: i32 = 0;
        var state = ParseState.find_key;
        var escaped: u8 = 0;

        var current_key_start: usize = 0;
        var current_key_end: usize = 0;
        var current_child_start: usize = 0;
        var current_child_end: usize = 0;
        var value_start: usize = 0;
        var value_end: usize = 0;

        while (i < body.len) {
            const c = body[i];
            if (isAsciiSpace(c)) {
                i += 1;
                continue;
            }

            switch (state) {
                .find_key => {
                    if (c == '{') {
                        depth += 1;
                        i += 1;
                        continue;
                    }
                    if (c == '}') {
                        if (depth <= 0) return false;
                        depth -= 1;
                        i += 1;
                        if (depth == 0) break;
                        state = .after_value;
                        continue;
                    }
                    if (c == '"') {
                        const key_start = i + 1;
                        i += 1;
                        escaped = 0;
                        while (i < body.len) : (i += 1) {
                            const ch = body[i];
                            if (escaped == 1) {
                                escaped = 0;
                            } else if (ch == '\\') {
                                escaped = 1;
                            } else if (ch == '"') {
                                break;
                            }
                        }
                        if (i >= body.len) return false;

                        if (depth == 1) {
                            current_key_start = key_start;
                            current_key_end = i;
                        } else if (depth == 2) {
                            current_child_start = key_start;
                            current_child_end = i;
                        }

                        i += 1;
                        state = .after_key;
                        continue;
                    }

                    i += 1;
                    continue;
                },
                .after_key => {
                    if (c != ':') return false;
                    i += 1;
                    state = .find_value;
                    continue;
                },
                .find_value => {
                    if (c == '{') {
                        depth += 1;
                        i += 1;
                        state = .find_key;
                        continue;
                    }

                    if (c == '[') {
                        var arr_depth: i32 = 1;
                        var in_string: u8 = 0;
                        value_start = i;
                        i += 1;
                        escaped = 0;

                        while (i < body.len and arr_depth > 0) : (i += 1) {
                            const ch = body[i];
                            if (in_string == 1) {
                                if (escaped == 1) {
                                    escaped = 0;
                                } else if (ch == '\\') {
                                    escaped = 1;
                                } else if (ch == '"') {
                                    in_string = 0;
                                }
                            } else {
                                if (ch == '"') {
                                    in_string = 1;
                                } else if (ch == '[') {
                                    arr_depth += 1;
                                } else if (ch == ']') {
                                    arr_depth -= 1;
                                }
                            }
                        }

                        if (arr_depth != 0) return false;
                        value_end = i;
                    } else if (c == '"') {
                        value_start = i + 1;
                        i += 1;
                        escaped = 0;
                        while (i < body.len) : (i += 1) {
                            const ch = body[i];
                            if (escaped == 1) {
                                escaped = 0;
                            } else if (ch == '\\') {
                                escaped = 1;
                            } else if (ch == '"') {
                                break;
                            }
                        }
                        if (i >= body.len) return false;
                        value_end = i;
                        i += 1;
                    } else {
                        value_start = i;
                        while (i < body.len and body[i] != ',' and body[i] != '}' and !isAsciiSpace(body[i])) : (i += 1) {}
                        value_end = i;
                    }

                    if (depth == 1) {
                        const key = body[current_key_start..current_key_end];
                        if (std.mem.eql(u8, key, "id")) {
                            if (value_end <= value_start) return false;
                            if (!copyToBuf(body[value_start..value_end], &self.id)) return false;
                        }
                    } else if (depth == 2) {
                        const parent = parentKeyId(body[current_key_start..current_key_end]);
                        const child = childKeyId(body[current_child_start..current_child_end]);
                        if (!self.applyNestedValue(parent, child, body[value_start..value_end])) {
                            return false;
                        }
                    }

                    state = .after_value;
                    continue;
                },
                .after_value => {
                    if (c == ',') {
                        i += 1;
                        state = .find_key;
                        continue;
                    }
                    if (c == '}') {
                        if (depth <= 0) return false;
                        depth -= 1;
                        i += 1;
                        if (depth == 0) break;
                        state = .after_value;
                        continue;
                    }
                    return false;
                },
            }
        }

        while (i < body.len and isAsciiSpace(body[i])) : (i += 1) {}

        return depth == 0 and i == body.len;
    }

    fn applyNestedValue(self: *TransactionContext, parent: ParentKey, child: ChildKey, raw: []const u8) bool {
        if (raw.len == 0) return false;
        switch (parent) {
            .transaction => switch (child) {
                .amount => return toDouble(raw, &self.transaction_amount),
                .installments => return toUInt8(raw, &self.transaction_installments),
                .requested_at => return toEpoch(raw, &self.transaction_requested_at),
                else => return true,
            },
            .customer => switch (child) {
                .avg_amount => return toDouble(raw, &self.customer_avg_amount),
                .tx_count_24h => return toUInt8(raw, &self.customer_tx_count_24h),
                .known_merchants => return toKnownMerchants(raw, &self.customer_known_merchants, &self.customer_known_merchants_len),
                else => return true,
            },
            .merchant => switch (child) {
                .id => return copyToBuf(raw, &self.merchant_id),
                .mcc => return copyToBuf(raw, &self.merchant_mcc),
                .avg_amount => return toDouble(raw, &self.merchant_avg_amount),
                else => return true,
            },
            .terminal => switch (child) {
                .is_online => return toBool(raw, &self.terminal_is_online),
                .card_present => return toBool(raw, &self.terminal_card_present),
                .km_from_home => return toDouble(raw, &self.terminal_km_from_home),
                else => return true,
            },
            .last_transaction => switch (child) {
                .timestamp => {
                    if (!toEpoch(raw, &self.last_transaction_timestamp)) return false;
                    self.has_last_transaction = 1;
                    return true;
                },
                .km_from_current => {
                    if (!toDouble(raw, &self.last_transaction_km_from_current)) return false;
                    self.has_last_transaction = 1;
                    return true;
                },
                else => return true,
            },
            else => return true,
        }
    }
};

fn childKeyId(key: []const u8) ChildKey {
    if (std.mem.eql(u8, key, "id")) return .id;
    if (std.mem.eql(u8, key, "amount")) return .amount;
    if (std.mem.eql(u8, key, "installments")) return .installments;
    if (std.mem.eql(u8, key, "requested_at")) return .requested_at;
    if (std.mem.eql(u8, key, "avg_amount")) return .avg_amount;
    if (std.mem.eql(u8, key, "tx_count_24h")) return .tx_count_24h;
    if (std.mem.eql(u8, key, "known_merchants")) return .known_merchants;
    if (std.mem.eql(u8, key, "mcc")) return .mcc;
    if (std.mem.eql(u8, key, "is_online")) return .is_online;
    if (std.mem.eql(u8, key, "card_present")) return .card_present;
    if (std.mem.eql(u8, key, "km_from_home")) return .km_from_home;
    if (std.mem.eql(u8, key, "timestamp")) return .timestamp;
    if (std.mem.eql(u8, key, "km_from_current")) return .km_from_current;
    return .none;
}

fn parentKeyId(key: []const u8) ParentKey {
    if (std.mem.eql(u8, key, "transaction")) return .transaction;
    if (std.mem.eql(u8, key, "customer")) return .customer;
    if (std.mem.eql(u8, key, "merchant")) return .merchant;
    if (std.mem.eql(u8, key, "terminal")) return .terminal;
    if (std.mem.eql(u8, key, "last_transaction")) return .last_transaction;
    return .none;
}

fn clamp01(v: f64) f64 {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

fn dayOfWeekFromEpoch(epoch: i64) i32 {
    const days = @divTrunc(epoch, 86400);
    var dow = @mod(days + 3, 7);
    if (dow < 0) {
        dow += 7;
    }
    return @intCast(dow);
}

fn daysFromCivil(y_input: i32, m: u32, d: u32) i64 {
    var y = y_input;
    y -= if (m <= 2) 1 else 0;
    const era = @divTrunc(if (y >= 0) y else y - 399, 400);
    const yoe: u32 = @intCast(y - era * 400);
    const mp: u32 = m + (if (m > 2) @as(u32, 0) else 9) - 3;
    const doy: u32 = @divTrunc(153 * mp + 2, 5) + d - 1;
    const doe: u32 = yoe * 365 + @divTrunc(yoe, 4) - @divTrunc(yoe, 100) + doy;
    return @as(i64, era) * 146097 + @as(i64, doe) - 719468;
}

fn hourOfDayFromEpoch(epoch: i64) i32 {
    var s = @mod(epoch, 86400);
    if (s < 0) s += 86400;
    return @intCast(@divTrunc(s, 3600));
}

fn isAsciiSpace(c: u8) bool {
    return c == ' ' or c == '\n' or c == '\r' or c == '\t';
}

fn isAsciiDigit(c: u8) bool {
    return c >= '0' and c <= '9';
}

fn merchantMccRiskOrDefault(mcc: *const [TX2_MCC_BUF_LEN]u8) f64 {
    if (mcc[0] == 0) return 0.5;
    if (mcc[4] != 0) return 0.5;

    switch (pack4Ascii(mcc[0..TX2_MCC_LEN])) {
        pack4Literal("5411") => return 0.15,
        pack4Literal("5812") => return 0.30,
        pack4Literal("5912") => return 0.20,
        pack4Literal("5944") => return 0.45,
        pack4Literal("7801") => return 0.80,
        pack4Literal("7802") => return 0.75,
        pack4Literal("7995") => return 0.85,
        pack4Literal("4511") => return 0.35,
        pack4Literal("5311") => return 0.25,
        pack4Literal("5999") => return 0.50,
        else => return 0.5,
    }
}

fn merchantInKnownList(self: *const TransactionContext) u8 {
    if (self.merchant_id[0] == 0) return 0;

    const merchant = self.merchant_id[0..TX2_MERCHANT_ID_LEN];
    var i: usize = 0;
    while (i < self.customer_known_merchants_len) : (i += 1) {
        if (std.mem.eql(u8, self.customer_known_merchants[i][0..TX2_MERCHANT_ID_LEN], merchant)) {
            return 1;
        }
    }
    return 0;
}

fn pack4Ascii(s: []const u8) u32 {
    return (@as(u32, s[0]) << 24) | (@as(u32, s[1]) << 16) | (@as(u32, s[2]) << 8) | @as(u32, s[3]);
}

fn pack4Literal(comptime s: []const u8) u32 {
    return (@as(u32, s[0]) << 24) | (@as(u32, s[1]) << 16) | (@as(u32, s[2]) << 8) | @as(u32, s[3]);
}

fn copyToBuf(src: []const u8, dst: anytype) bool {
    const buf = dst.*;
    if (src.len == 0 or src.len >= buf.len) return false;
    @memset(dst, 0);
    std.mem.copyForwards(u8, dst[0..src.len], src);
    dst[src.len] = 0;
    return true;
}

fn toBool(raw: []const u8, out: *u8) bool {
    if (std.mem.eql(u8, raw, "true")) {
        out.* = 1;
        return true;
    }
    if (std.mem.eql(u8, raw, "false")) {
        out.* = 0;
        return true;
    }
    return false;
}

fn toDouble(raw: []const u8, out: *f64) bool {
    if (raw.len == 0) return false;

    var int_part: u64 = 0;
    var frac_part: u64 = 0;
    var frac_digits: u8 = 0;
    var has_dot = false;
    var has_digit = false;

    for (raw) |c| {
        if (c == '.') {
            if (has_dot) return false;
            has_dot = true;
            continue;
        }
        if (!isAsciiDigit(c)) return false;

        has_digit = true;
        const digit: u64 = @intCast(c - '0');
        if (!has_dot) {
            int_part = int_part * 10 + digit;
        } else if (frac_digits < 18) {
            frac_part = frac_part * 10 + digit;
            frac_digits += 1;
        }
    }

    if (!has_digit) return false;

    const inv_pow10 = [_]f64{
        1.0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,  1e-7,  1e-8,  1e-9,
        1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18,
    };

    out.* = @as(f64, @floatFromInt(int_part)) + @as(f64, @floatFromInt(frac_part)) * inv_pow10[frac_digits];
    return true;
}

fn toEpoch(raw: []const u8, out_epoch: *i64) bool {
    if (raw.len != 20) return false;
    if (raw[4] != '-' or raw[7] != '-' or raw[10] != 'T' or raw[13] != ':' or raw[16] != ':' or raw[19] != 'Z') {
        return false;
    }

    var i: usize = 0;
    while (i < raw.len) : (i += 1) {
        if (i == 4 or i == 7 or i == 10 or i == 13 or i == 16 or i == 19) continue;
        if (!isAsciiDigit(raw[i])) return false;
    }

    const year: i32 = @as(i32, raw[0] - '0') * 1000 +
        @as(i32, raw[1] - '0') * 100 +
        @as(i32, raw[2] - '0') * 10 +
        @as(i32, raw[3] - '0');
    const month: i32 = @as(i32, raw[5] - '0') * 10 + @as(i32, raw[6] - '0');
    const day: i32 = @as(i32, raw[8] - '0') * 10 + @as(i32, raw[9] - '0');
    const hour: i32 = @as(i32, raw[11] - '0') * 10 + @as(i32, raw[12] - '0');
    const minute: i32 = @as(i32, raw[14] - '0') * 10 + @as(i32, raw[15] - '0');
    const second: i32 = @as(i32, raw[17] - '0') * 10 + @as(i32, raw[18] - '0');

    if (month < 1 or month > 12 or day < 1 or day > 31 or hour > 23 or minute > 59 or second > 59) {
        return false;
    }

    out_epoch.* = daysFromCivil(year, @intCast(month), @intCast(day)) * 86400 +
        @as(i64, hour) * 3600 +
        @as(i64, minute) * 60 +
        @as(i64, second);
    return true;
}

fn toKnownMerchants(
    raw: []const u8,
    out: *[TX2_MAX_KNOWN_MERCHANTS][TX2_MERCHANT_ID_BUF_LEN]u8,
    out_len: *u8,
) bool {
    if (raw.len < 2 or raw[0] != '[' or raw[raw.len - 1] != ']') return false;

    out.* = [_][TX2_MERCHANT_ID_BUF_LEN]u8{[_]u8{0} ** TX2_MERCHANT_ID_BUF_LEN} ** TX2_MAX_KNOWN_MERCHANTS;
    out_len.* = 0;

    const State = enum(u8) {
        expect_item_or_end,
        in_item,
        expect_comma_or_end,
    };

    var state = State.expect_item_or_end;
    var count: u8 = 0;
    var item_start: usize = 0;

    var i: usize = 1;
    while (i < raw.len - 1) : (i += 1) {
        const c = raw[i];

        switch (state) {
            .expect_item_or_end => {
                if (isAsciiSpace(c)) continue;
                if (c != '"') return false;
                item_start = i + 1;
                state = .in_item;
            },
            .in_item => {
                if (c == '\\') return false;
                if (c != '"') continue;

                const item_len = i - item_start;
                if (item_len == 0 or item_len > TX2_MERCHANT_ID_LEN) return false;
                if (count >= TX2_MAX_KNOWN_MERCHANTS) return false;

                std.mem.copyForwards(u8, out[count][0..item_len], raw[item_start..i]);
                out[count][item_len] = 0;
                count += 1;
                state = .expect_comma_or_end;
            },
            .expect_comma_or_end => {
                if (isAsciiSpace(c)) continue;
                if (c != ',') return false;
                state = .expect_item_or_end;
            },
        }
    }

    if (state == .in_item) return false;
    if (state == .expect_item_or_end and count > 0) return false;

    out_len.* = count;
    return true;
}

fn toUInt8(raw: []const u8, out: *u8) bool {
    if (raw.len == 0) return false;

    var value: u16 = 0;
    for (raw) |c| {
        if (!isAsciiDigit(c)) return false;
        value = value * 10 + @as(u16, c - '0');
        if (value > std.math.maxInt(u8)) return false;
    }

    out.* = @intCast(value);
    return true;
}
