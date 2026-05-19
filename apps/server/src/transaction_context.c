#include "transaction_context.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define NORMALIZATION_MAX_AMOUNT 10000.0
#define NORMALIZATION_MAX_INSTALLMENTS 12.0
#define NORMALIZATION_AMOUNT_VS_AVG_RATIO 10.0
#define NORMALIZATION_MAX_MINUTES 1440.0
#define NORMALIZATION_MAX_KM 1000.0
#define NORMALIZATION_MAX_TX_COUNT_24H 20.0
#define NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT 10000.0

static const char *skip_ws_n(const char *p, const char *end) {
  while (p < end && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

static double clamp_01(double v) {
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 1.0) {
    return 1.0;
  }
  return v;
}

static int hour_of_day_from_epoch(long long epoch) {
  long long s = epoch % 86400LL;
  if (s < 0) {
    s += 86400LL;
  }
  return (int)(s / 3600LL);
}

static int day_of_week_from_epoch(long long epoch) {
  long long days = epoch / 86400LL;
  int dow = (int)((days + 3LL) % 7LL);
  if (dow < 0) {
    dow += 7;
  }
  return dow;
}

static double mcc_risk_or_default_view(const char *mcc, size_t len) {
  if (mcc == NULL || len == 0) {
    return 0.5;
  }
  if (len == 4 && memcmp(mcc, "5411", 4) == 0) {
    return 0.15;
  }
  if (len == 4 && memcmp(mcc, "5812", 4) == 0) {
    return 0.30;
  }
  if (len == 4 && memcmp(mcc, "5912", 4) == 0) {
    return 0.20;
  }
  if (len == 4 && memcmp(mcc, "5944", 4) == 0) {
    return 0.45;
  }
  if (len == 4 && memcmp(mcc, "7801", 4) == 0) {
    return 0.80;
  }
  if (len == 4 && memcmp(mcc, "7802", 4) == 0) {
    return 0.75;
  }
  if (len == 4 && memcmp(mcc, "7995", 4) == 0) {
    return 0.85;
  }
  if (len == 4 && memcmp(mcc, "4511", 4) == 0) {
    return 0.35;
  }
  if (len == 4 && memcmp(mcc, "5311", 4) == 0) {
    return 0.25;
  }
  if (len == 4 && memcmp(mcc, "5999", 4) == 0) {
    return 0.50;
  }
  return 0.5;
}

static bool expect_char_n(const char **p, const char *end, char c) {
  *p = skip_ws_n(*p, end);
  if (*p >= end || **p != c) {
    return false;
  }
  (*p)++;
  return true;
}

static bool expect_key_n(const char **p, const char *end, const char *key) {
  size_t len = 0;
  *p = skip_ws_n(*p, end);
  len = strlen(key);
  if ((size_t)(end - *p) < len || memcmp(*p, key, len) != 0) {
    return false;
  }
  *p += len;
  return expect_char_n(p, end, ':');
}

static bool parse_string_view_n(const char **p, const char *end, const char **out_start,
                                size_t *out_len) {
  const char *start = NULL;
  const char *q = NULL;
  int escaped = 0;

  *p = skip_ws_n(*p, end);
  if (*p >= end || **p != '"') {
    return false;
  }

  start = ++(*p);
  q = start;
  while (q < end) {
    char c = *q;
    if (escaped) {
      escaped = 0;
      q++;
      continue;
    }
    if (c == '\\') {
      escaped = 1;
      q++;
      continue;
    }
    if (c == '"') {
      break;
    }
    q++;
  }

  if (q >= end || *q != '"' || escaped) {
    return false;
  }

  *out_start = start;
  *out_len = (size_t)(q - start);
  *p = q + 1;
  return true;
}

static bool parse_double_n(const char **p, const char *end, double *out) {
  char *num_end = NULL;
  *p = skip_ws_n(*p, end);
  if (*p >= end) {
    return false;
  }
  *out = strtod(*p, &num_end);
  if (num_end == *p || num_end > end) {
    return false;
  }
  *p = num_end;
  return true;
}

static bool parse_int_n(const char **p, const char *end, int *out) {
  char *num_end = NULL;
  long v = 0;
  *p = skip_ws_n(*p, end);
  if (*p >= end) {
    return false;
  }
  v = strtol(*p, &num_end, 10);
  if (num_end == *p || num_end > end) {
    return false;
  }
  *out = (int)v;
  *p = num_end;
  return true;
}

static bool parse_bool_n(const char **p, const char *end, bool *out) {
  *p = skip_ws_n(*p, end);
  if ((size_t)(end - *p) >= 4 && memcmp(*p, "true", 4) == 0) {
    *out = true;
    *p += 4;
    return true;
  }
  if ((size_t)(end - *p) >= 5 && memcmp(*p, "false", 5) == 0) {
    *out = false;
    *p += 5;
    return true;
  }
  return false;
}

static long long days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  {
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
  }
}

static int parse_2d(const char *s) {
  if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
    return -1;
  }
  return (s[0] - '0') * 10 + (s[1] - '0');
}

static int parse_4d(const char *s) {
  if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
      !isdigit((unsigned char)s[2]) || !isdigit((unsigned char)s[3])) {
    return -1;
  }
  return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

static bool parse_iso8601_utc_epoch_view(const char *s, size_t len, long long *out) {
  int year = 0;
  int mon = 0;
  int day = 0;
  int hh = 0;
  int mm = 0;
  int ss = 0;

  if (len != 20) {
    return false;
  }
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') {
    return false;
  }

  year = parse_4d(s);
  mon = parse_2d(s + 5);
  day = parse_2d(s + 8);
  hh = parse_2d(s + 11);
  mm = parse_2d(s + 14);
  ss = parse_2d(s + 17);

  if (year < 0 || mon < 1 || mon > 12 || day < 1 || day > 31 || hh < 0 || hh > 23 || mm < 0 ||
      mm > 59 || ss < 0 || ss > 59) {
    return false;
  }

  *out = days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400LL + (long long)hh * 3600LL +
         (long long)mm * 60LL + (long long)ss;
  return true;
}

static bool parse_timestamp_field_n(const char **p, const char *end, long long *out_epoch) {
  const char *s = NULL;
  size_t len = 0;
  if (!parse_string_view_n(p, end, &s, &len)) {
    return false;
  }
  return parse_iso8601_utc_epoch_view(s, len, out_epoch);
}

static bool parse_known_merchants_span_n(const char **p, const char *end, const char **out_begin,
                                         const char **out_end) {
  if (!expect_char_n(p, end, '[')) {
    return false;
  }

  *out_begin = *p;

  while (true) {
    const char *s = NULL;
    size_t len = 0;

    *p = skip_ws_n(*p, end);
    if (*p < end && **p == ']') {
      *out_end = *p;
      (*p)++;
      return true;
    }

    if (!parse_string_view_n(p, end, &s, &len)) {
      return false;
    }
    (void)s;
    (void)len;

    *p = skip_ws_n(*p, end);
    if (*p < end && **p == ',') {
      (*p)++;
      continue;
    }
    if (*p < end && **p == ']') {
      *out_end = *p;
      (*p)++;
      return true;
    }

    return false;
  }
}

static bool known_merchants_contains(const char *begin, const char *end, const char *merchant_id,
                                     size_t merchant_id_len) {
  const char *p = begin;

  if (!merchant_id || merchant_id_len == 0) {
    return false;
  }

  while (p < end) {
    const char *s = NULL;
    size_t len = 0;

    p = skip_ws_n(p, end);
    if (p >= end) {
      break;
    }

    if (*p == ',') {
      p++;
      continue;
    }

    if (!parse_string_view_n(&p, end, &s, &len)) {
      return false;
    }

    if (len == merchant_id_len && memcmp(s, merchant_id, len) == 0) {
      return true;
    }

    p = skip_ws_n(p, end);
    if (p < end && *p == ',') {
      p++;
    }
  }

  return false;
}

static void transaction_context_destroy(TransactionContext *self) {
  if (self == NULL) {
    return;
  }
  memset(self, 0, sizeof(*self));
}

static bool transaction_context_parse_n(TransactionContext *self, const char *body,
                                        size_t body_len) {
  const char *p = NULL;
  const char *end = NULL;
  const char *id_view = NULL;
  size_t id_len = 0;
  const char *known_begin = NULL;
  const char *known_end = NULL;
  const char *merchant_id_view = NULL;
  size_t merchant_id_len = 0;
  const char *mcc_view = NULL;
  size_t mcc_len = 0;
  bool b = false;

  if (self == NULL || body == NULL) {
    return false;
  }

  p = body;
  end = body + body_len;

  if (!expect_char_n(&p, end, '{')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"id\"")) {
    return false;
  }
  if (!parse_string_view_n(&p, end, &id_view, &id_len)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"transaction\"")) {
    return false;
  }
  if (!expect_char_n(&p, end, '{')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"amount\"")) {
    return false;
  }
  if (!parse_double_n(&p, end, &self->transaction_amount)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"installments\"")) {
    return false;
  }
  if (!parse_int_n(&p, end, &self->transaction_installments)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"requested_at\"")) {
    return false;
  }
  if (!parse_timestamp_field_n(&p, end, &self->transaction_requested_at)) {
    return false;
  }
  if (!expect_char_n(&p, end, '}')) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"customer\"")) {
    return false;
  }
  if (!expect_char_n(&p, end, '{')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"avg_amount\"")) {
    return false;
  }
  if (!parse_double_n(&p, end, &self->customer_avg_amount)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"tx_count_24h\"")) {
    return false;
  }
  if (!parse_int_n(&p, end, &self->customer_tx_count_24h)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"known_merchants\"")) {
    return false;
  }
  if (!parse_known_merchants_span_n(&p, end, &known_begin, &known_end)) {
    return false;
  }
  if (!expect_char_n(&p, end, '}')) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"merchant\"")) {
    return false;
  }
  if (!expect_char_n(&p, end, '{')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"id\"")) {
    return false;
  }
  if (!parse_string_view_n(&p, end, &merchant_id_view, &merchant_id_len)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"mcc\"")) {
    return false;
  }
  if (!parse_string_view_n(&p, end, &mcc_view, &mcc_len)) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"avg_amount\"")) {
    return false;
  }
  if (!parse_double_n(&p, end, &self->merchant_avg_amount)) {
    return false;
  }
  if (!expect_char_n(&p, end, '}')) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"terminal\"")) {
    return false;
  }
  if (!expect_char_n(&p, end, '{')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"is_online\"")) {
    return false;
  }
  if (!parse_bool_n(&p, end, &b)) {
    return false;
  }
  self->terminal_is_online = b ? 1 : 0;
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"card_present\"")) {
    return false;
  }
  if (!parse_bool_n(&p, end, &b)) {
    return false;
  }
  self->terminal_card_present = b ? 1 : 0;
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }
  if (!expect_key_n(&p, end, "\"km_from_home\"")) {
    return false;
  }
  if (!parse_double_n(&p, end, &self->terminal_km_from_home)) {
    return false;
  }
  if (!expect_char_n(&p, end, '}')) {
    return false;
  }
  if (!expect_char_n(&p, end, ',')) {
    return false;
  }

  if (!expect_key_n(&p, end, "\"last_transaction\"")) {
    return false;
  }
  p = skip_ws_n(p, end);
  if ((size_t)(end - p) >= 4 && memcmp(p, "null", 4) == 0) {
    self->has_last_transaction = false;
    p += 4;
  } else {
    self->has_last_transaction = true;
    if (!expect_char_n(&p, end, '{')) {
      return false;
    }
    if (!expect_key_n(&p, end, "\"timestamp\"")) {
      return false;
    }
    if (!parse_timestamp_field_n(&p, end, &self->last_transaction_timestamp)) {
      return false;
    }
    if (!expect_char_n(&p, end, ',')) {
      return false;
    }
    if (!expect_key_n(&p, end, "\"km_from_current\"")) {
      return false;
    }
    if (!parse_double_n(&p, end, &self->last_transaction_km_from_current)) {
      return false;
    }
    if (!expect_char_n(&p, end, '}')) {
      return false;
    }
  }

  if (!expect_char_n(&p, end, '}')) {
    return false;
  }

  p = skip_ws_n(p, end);
  if (p != end) {
    return false;
  }

  self->id = (char *)id_view;
  (void)id_len;
  self->merchant_known =
    known_merchants_contains(known_begin, known_end, merchant_id_view, merchant_id_len) ? 1 : 0;
  self->merchant_mcc_risk = mcc_risk_or_default_view(mcc_view, mcc_len);
  return true;
}

static void transaction_context_to_vector(const TransactionContext *self, double out[14]) {
  double amount = 0.0;
  double installments = 0.0;
  double customer_avg = 0.0;
  double amount_vs_avg = 0.0;
  double hour_of_day = 0.0;
  double day_of_week = 0.0;
  double minutes_since_last = -1.0;
  double km_from_last_tx = -1.0;
  double km_from_home = 0.0;
  double tx_count_24h = 0.0;
  double is_online = 0.0;
  double card_present = 0.0;
  double unknown_merchant = 0.0;
  double mcc_risk = 0.5;
  double merchant_avg_amount = 0.0;

  memset(out, 0, 14 * sizeof(double));
  if (self == NULL) {
    return;
  }

  amount = clamp_01(self->transaction_amount / NORMALIZATION_MAX_AMOUNT);
  installments = clamp_01((double)self->transaction_installments / NORMALIZATION_MAX_INSTALLMENTS);

  customer_avg = self->customer_avg_amount < 1e-9 ? 1e-9 : self->customer_avg_amount;
  amount_vs_avg =
    clamp_01(self->transaction_amount / customer_avg / NORMALIZATION_AMOUNT_VS_AVG_RATIO);

  hour_of_day = (double)hour_of_day_from_epoch(self->transaction_requested_at) / 23.0;
  day_of_week = (double)day_of_week_from_epoch(self->transaction_requested_at) / 6.0;

  if (self->has_last_transaction) {
    minutes_since_last = clamp_01(
      ((double)(self->transaction_requested_at - self->last_transaction_timestamp) / 60.0) /
      NORMALIZATION_MAX_MINUTES);
    km_from_last_tx = clamp_01(self->last_transaction_km_from_current / NORMALIZATION_MAX_KM);
  }

  km_from_home = clamp_01(self->terminal_km_from_home / NORMALIZATION_MAX_KM);
  tx_count_24h = clamp_01((double)self->customer_tx_count_24h / NORMALIZATION_MAX_TX_COUNT_24H);
  is_online = self->terminal_is_online ? 1.0 : 0.0;
  card_present = self->terminal_card_present ? 1.0 : 0.0;
  unknown_merchant = self->merchant_known ? 0.0 : 1.0;
  mcc_risk = self->merchant_mcc_risk;
  merchant_avg_amount = clamp_01(self->merchant_avg_amount / NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT);

  out[0] = amount;
  out[1] = installments;
  out[2] = amount_vs_avg;
  out[3] = hour_of_day;
  out[4] = day_of_week;
  out[5] = minutes_since_last;
  out[6] = km_from_last_tx;
  out[7] = km_from_home;
  out[8] = tx_count_24h;
  out[9] = is_online;
  out[10] = card_present;
  out[11] = unknown_merchant;
  out[12] = mcc_risk;
  out[13] = merchant_avg_amount;
}

static void transaction_context_init(TransactionContext *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->merchant_mcc_risk = 0.5;
  ctx->destroy = transaction_context_destroy;
  ctx->to_vector = transaction_context_to_vector;
}

TransactionContext transaction_context_from_body(const char *body) {
  if (body == NULL) {
    return transaction_context_new();
  }
  return transaction_context_from_body_n(body, strlen(body));
}

TransactionContext transaction_context_from_body_n(const char *body, size_t body_len) {
  TransactionContext ctx = transaction_context_new();
  if (!transaction_context_parse_n(&ctx, body, body_len)) {
    transaction_context_init(&ctx);
  } else {
    ctx.destroy = transaction_context_destroy;
    ctx.to_vector = transaction_context_to_vector;
  }
  return ctx;
}

TransactionContext transaction_context_new(void) {
  TransactionContext ctx;
  transaction_context_init(&ctx);
  return ctx;
}
