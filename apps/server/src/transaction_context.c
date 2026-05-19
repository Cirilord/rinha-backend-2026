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

static const char *skip_ws(const char *p) {
  while (*p != '\0' && isspace((unsigned char)*p)) {
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

static bool merchant_is_known(const TransactionContext *self) {
  int i = 0;
  if (self->merchant_id == NULL || self->customer_known_merchants == NULL) {
    return false;
  }
  for (i = 0; i < self->customer_known_merchants_len; i++) {
    if (self->customer_known_merchants[i] != NULL &&
        strcmp(self->customer_known_merchants[i], self->merchant_id) == 0) {
      return true;
    }
  }
  return false;
}

static double mcc_risk_or_default(const char *mcc) {
  if (mcc == NULL) {
    return 0.5;
  }
  if (strcmp(mcc, "5411") == 0) {
    return 0.15;
  }
  if (strcmp(mcc, "5812") == 0) {
    return 0.30;
  }
  if (strcmp(mcc, "5912") == 0) {
    return 0.20;
  }
  if (strcmp(mcc, "5944") == 0) {
    return 0.45;
  }
  if (strcmp(mcc, "7801") == 0) {
    return 0.80;
  }
  if (strcmp(mcc, "7802") == 0) {
    return 0.75;
  }
  if (strcmp(mcc, "7995") == 0) {
    return 0.85;
  }
  if (strcmp(mcc, "4511") == 0) {
    return 0.35;
  }
  if (strcmp(mcc, "5311") == 0) {
    return 0.25;
  }
  if (strcmp(mcc, "5999") == 0) {
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

static bool parse_string_dup(const char **p, char **out) {
  const char *start = NULL;
  const char *end = NULL;
  size_t len = 0;
  char *copy = NULL;

  *p = skip_ws(*p);
  if (**p != '"') {
    return false;
  }
  start = ++(*p);
  end = strchr(start, '"');
  if (end == NULL) {
    return false;
  }

  len = (size_t)(end - start);
  copy = (char *)malloc(len + 1);
  if (copy == NULL) {
    return false;
  }
  memcpy(copy, start, len);
  copy[len] = '\0';

  *out = copy;
  *p = end + 1;
  return true;
}

static bool parse_string_dup_n(const char **p, const char *end, char **out) {
  const char *start = NULL;
  const char *q = NULL;
  size_t len = 0;
  char *copy = NULL;

  *p = skip_ws_n(*p, end);
  if (*p >= end || **p != '"') {
    return false;
  }
  start = ++(*p);

  q = start;
  while (q < end && *q != '"') {
    q++;
  }
  if (q >= end || *q != '"') {
    return false;
  }

  len = (size_t)(q - start);
  copy = (char *)malloc(len + 1);
  if (copy == NULL) {
    return false;
  }
  memcpy(copy, start, len);
  copy[len] = '\0';

  *out = copy;
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

static bool parse_iso8601_utc_epoch(const char *s, long long *out) {
  int year = 0;
  int mon = 0;
  int day = 0;
  int hh = 0;
  int mm = 0;
  int ss = 0;

  if (strlen(s) != 20) {
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

static bool parse_timestamp_field(const char **p, long long *out_epoch) {
  char *tmp = NULL;
  if (!parse_string_dup(p, &tmp)) {
    return false;
  }
  if (!parse_iso8601_utc_epoch(tmp, out_epoch)) {
    free(tmp);
    return false;
  }
  free(tmp);
  return true;
}

static bool parse_known_merchants_n(const char **p, const char *end, char ***out_items,
                                    int *out_len) {
  int cap = 4;
  int len = 0;
  char **items = NULL;

  if (!expect_char_n(p, end, '[')) {
    return false;
  }

  items = (char **)malloc((size_t)cap * sizeof(char *));
  if (items == NULL) {
    return false;
  }

  while (true) {
    char *value = NULL;
    char **new_items = NULL;

    *p = skip_ws_n(*p, end);
    if (*p < end && **p == ']') {
      (*p)++;
      break;
    }

    if (!parse_string_dup_n(p, end, &value)) {
      int i = 0;
      for (i = 0; i < len; i++) {
        free(items[i]);
      }
      free(items);
      return false;
    }

    if (len == cap) {
      cap *= 2;
      new_items = (char **)realloc(items, (size_t)cap * sizeof(char *));
      if (new_items == NULL) {
        int i = 0;
        free(value);
        for (i = 0; i < len; i++) {
          free(items[i]);
        }
        free(items);
        return false;
      }
      items = new_items;
    }
    items[len++] = value;

    *p = skip_ws_n(*p, end);
    if (*p < end && **p == ',') {
      (*p)++;
      continue;
    }
    if (*p < end && **p == ']') {
      (*p)++;
      break;
    }

    {
      int i = 0;
      for (i = 0; i < len; i++) {
        free(items[i]);
      }
      free(items);
      return false;
    }
  }

  *out_items = items;
  *out_len = len;
  return true;
}

static void transaction_context_destroy(TransactionContext *self) {
  int i = 0;
  if (self == NULL) {
    return;
  }
  if (self->id != NULL) {
    free(self->id);
  }
  if (self->merchant_id != NULL) {
    free(self->merchant_id);
  }
  if (self->merchant_mcc != NULL) {
    free(self->merchant_mcc);
  }
  if (self->customer_known_merchants != NULL) {
    for (i = 0; i < self->customer_known_merchants_len; i++) {
      free(self->customer_known_merchants[i]);
    }
    free(self->customer_known_merchants);
  }
  memset(self, 0, sizeof(*self));
}

static bool transaction_context_parse_n(TransactionContext *self, const char *body,
                                        size_t body_len) {
  const char *p = NULL;
  const char *end = NULL;
  bool b = false;

  if (self == NULL || body == NULL) {
    return false;
  }

  p = body;
  end = body + body_len;

  if (!expect_char_n(&p, end, '{')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"id\"")) {
    goto fail;
  }
  if (!parse_string_dup_n(&p, end, &self->id)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"transaction\"")) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '{')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"amount\"")) {
    goto fail;
  }
  if (!parse_double_n(&p, end, &self->transaction_amount)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"installments\"")) {
    goto fail;
  }
  if (!parse_int_n(&p, end, &self->transaction_installments)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"requested_at\"")) {
    goto fail;
  }
  if (!parse_timestamp_field(&p, &self->transaction_requested_at)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '}')) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"customer\"")) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '{')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"avg_amount\"")) {
    goto fail;
  }
  if (!parse_double_n(&p, end, &self->customer_avg_amount)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"tx_count_24h\"")) {
    goto fail;
  }
  if (!parse_int_n(&p, end, &self->customer_tx_count_24h)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"known_merchants\"")) {
    goto fail;
  }
  if (!parse_known_merchants_n(&p, end, &self->customer_known_merchants,
                               &self->customer_known_merchants_len)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '}')) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"merchant\"")) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '{')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"id\"")) {
    goto fail;
  }
  if (!parse_string_dup_n(&p, end, &self->merchant_id)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"mcc\"")) {
    goto fail;
  }
  if (!parse_string_dup_n(&p, end, &self->merchant_mcc)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"avg_amount\"")) {
    goto fail;
  }
  if (!parse_double_n(&p, end, &self->merchant_avg_amount)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '}')) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"terminal\"")) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '{')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"is_online\"")) {
    goto fail;
  }
  if (!parse_bool_n(&p, end, &b)) {
    goto fail;
  }
  self->terminal_is_online = b ? 1 : 0;
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"card_present\"")) {
    goto fail;
  }
  if (!parse_bool_n(&p, end, &b)) {
    goto fail;
  }
  self->terminal_card_present = b ? 1 : 0;
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }
  if (!expect_key_n(&p, end, "\"km_from_home\"")) {
    goto fail;
  }
  if (!parse_double_n(&p, end, &self->terminal_km_from_home)) {
    goto fail;
  }
  if (!expect_char_n(&p, end, '}')) {
    goto fail;
  }
  if (!expect_char_n(&p, end, ',')) {
    goto fail;
  }

  if (!expect_key_n(&p, end, "\"last_transaction\"")) {
    goto fail;
  }
  p = skip_ws_n(p, end);
  if ((size_t)(end - p) >= 4 && memcmp(p, "null", 4) == 0) {
    self->has_last_transaction = false;
    p += 4;
  } else {
    self->has_last_transaction = true;
    if (!expect_char_n(&p, end, '{')) {
      goto fail;
    }
    if (!expect_key_n(&p, end, "\"timestamp\"")) {
      goto fail;
    }
    if (!parse_timestamp_field(&p, &self->last_transaction_timestamp)) {
      goto fail;
    }
    if (!expect_char_n(&p, end, ',')) {
      goto fail;
    }
    if (!expect_key_n(&p, end, "\"km_from_current\"")) {
      goto fail;
    }
    if (!parse_double_n(&p, end, &self->last_transaction_km_from_current)) {
      goto fail;
    }
    if (!expect_char_n(&p, end, '}')) {
      goto fail;
    }
  }

  if (!expect_char_n(&p, end, '}')) {
    goto fail;
  }
  p = skip_ws_n(p, end);
  return p == end;

fail:
  transaction_context_destroy(self);
  return false;
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
  unknown_merchant = merchant_is_known(self) ? 0.0 : 1.0;
  mcc_risk = mcc_risk_or_default(self->merchant_mcc);
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
