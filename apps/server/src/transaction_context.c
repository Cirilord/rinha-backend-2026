#include "transaction_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define NORMALIZATION_MAX_AMOUNT 10000.0
#define NORMALIZATION_MAX_INSTALLMENTS 12.0
#define NORMALIZATION_AMOUNT_VS_AVG_RATIO 10.0
#define NORMALIZATION_MAX_MINUTES 1440.0
#define NORMALIZATION_MAX_KM 1000.0
#define NORMALIZATION_MAX_TX_COUNT_24H 20.0
#define NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT 10000.0

typedef enum {
  PARENT_KEY_NONE = 0,
  PARENT_KEY_TRANSACTION,
  PARENT_KEY_CUSTOMER,
  PARENT_KEY_MERCHANT,
  PARENT_KEY_TERMINAL,
  PARENT_KEY_LAST_TRANSACTION,
} ParentKey;

typedef enum {
  CHILD_KEY_NONE = 0,
  CHILD_KEY_ID,
  CHILD_KEY_AMOUNT,
  CHILD_KEY_INSTALLMENTS,
  CHILD_KEY_REQUESTED_AT,
  CHILD_KEY_AVG_AMOUNT,
  CHILD_KEY_TX_COUNT_24H,
  CHILD_KEY_KNOWN_MERCHANTS,
  CHILD_KEY_MCC,
  CHILD_KEY_IS_ONLINE,
  CHILD_KEY_CARD_PRESENT,
  CHILD_KEY_KM_FROM_HOME,
  CHILD_KEY_TIMESTAMP,
  CHILD_KEY_KM_FROM_CURRENT,
} ChildKey;

static inline uint32_t pack4_ascii(const char *s);
static void transaction_context_destroy(TransactionContext *self);
static void transaction_context_init(TransactionContext *ctx);
static bool transaction_context_parse(TransactionContext *self, const char *body, size_t body_len);
static void transaction_context_to_vector(const TransactionContext *self, double out[14]);

static ChildKey child_key_id(const char *body, size_t start, size_t end) {
  size_t len = end - start;
  if (len == 2 && memcmp(body + start, "id", 2) == 0) {
    return CHILD_KEY_ID;
  }
  if (len == 6 && memcmp(body + start, "amount", 6) == 0) {
    return CHILD_KEY_AMOUNT;
  }
  if (len == 12 && memcmp(body + start, "installments", 12) == 0) {
    return CHILD_KEY_INSTALLMENTS;
  }
  if (len == 12 && memcmp(body + start, "requested_at", 12) == 0) {
    return CHILD_KEY_REQUESTED_AT;
  }
  if (len == 10 && memcmp(body + start, "avg_amount", 10) == 0) {
    return CHILD_KEY_AVG_AMOUNT;
  }
  if (len == 12 && memcmp(body + start, "tx_count_24h", 12) == 0) {
    return CHILD_KEY_TX_COUNT_24H;
  }
  if (len == 15 && memcmp(body + start, "known_merchants", 15) == 0) {
    return CHILD_KEY_KNOWN_MERCHANTS;
  }
  if (len == 3 && memcmp(body + start, "mcc", 3) == 0) {
    return CHILD_KEY_MCC;
  }
  if (len == 9 && memcmp(body + start, "is_online", 9) == 0) {
    return CHILD_KEY_IS_ONLINE;
  }
  if (len == 12 && memcmp(body + start, "card_present", 12) == 0) {
    return CHILD_KEY_CARD_PRESENT;
  }
  if (len == 12 && memcmp(body + start, "km_from_home", 12) == 0) {
    return CHILD_KEY_KM_FROM_HOME;
  }
  if (len == 9 && memcmp(body + start, "timestamp", 9) == 0) {
    return CHILD_KEY_TIMESTAMP;
  }
  if (len == 15 && memcmp(body + start, "km_from_current", 15) == 0) {
    return CHILD_KEY_KM_FROM_CURRENT;
  }
  return CHILD_KEY_NONE;
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

static int day_of_week_from_epoch(int64_t epoch) {
  int64_t days = epoch / 86400LL;
  int dow = (int)((days + 3LL) % 7LL);
  if (dow < 0) {
    dow += 7;
  }
  return dow;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned mp = m + (m > 2 ? -3 : 9);
  const unsigned doy = (153 * mp + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int hour_of_day_from_epoch(int64_t epoch) {
  int64_t s = epoch % 86400LL;
  if (s < 0) {
    s += 86400LL;
  }
  return (int)(s / 3600LL);
}

static inline bool is_ascii_space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static inline bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

static double merchant_mcc_risk_or_default(const char *mcc) {
  if (mcc == NULL || mcc[0] == '\0') {
    return 0.5;
  }

  switch (pack4_ascii(mcc)) {
  case ('5' << 24) | ('4' << 16) | ('1' << 8) | '1':
    return 0.15;
  case ('5' << 24) | ('8' << 16) | ('1' << 8) | '2':
    return 0.30;
  case ('5' << 24) | ('9' << 16) | ('1' << 8) | '2':
    return 0.20;
  case ('5' << 24) | ('9' << 16) | ('4' << 8) | '4':
    return 0.45;
  case ('7' << 24) | ('8' << 16) | ('0' << 8) | '1':
    return 0.80;
  case ('7' << 24) | ('8' << 16) | ('0' << 8) | '2':
    return 0.75;
  case ('7' << 24) | ('9' << 16) | ('9' << 8) | '5':
    return 0.85;
  case ('4' << 24) | ('5' << 16) | ('1' << 8) | '1':
    return 0.35;
  case ('5' << 24) | ('3' << 16) | ('1' << 8) | '1':
    return 0.25;
  case ('5' << 24) | ('9' << 16) | ('9' << 8) | '9':
    return 0.50;
  default:
    return 0.5;
  }
}

static uint8_t merchant_in_known_list(const TransactionContext *self) {
  if (self == NULL || self->merchant_id[0] == '\0') {
    return 0;
  }

  for (uint8_t i = 0; i < self->customer_known_merchants_len; i++) {
    if (memcmp(self->customer_known_merchants[i], self->merchant_id, TX2_MERCHANT_ID_LEN) == 0) {
      return 1;
    }
  }
  return 0;
}

static inline uint32_t pack4_ascii(const char *s) {
  return ((uint32_t)(unsigned char)s[0] << 24) | ((uint32_t)(unsigned char)s[1] << 16) |
         ((uint32_t)(unsigned char)s[2] << 8) | (uint32_t)(unsigned char)s[3];
}

static ParentKey parent_key_id(const char *body, size_t start, size_t end) {
  size_t len = end - start;
  if (len == 11 && memcmp(body + start, "transaction", 11) == 0) {
    return PARENT_KEY_TRANSACTION;
  }
  if (len == 8 && memcmp(body + start, "customer", 8) == 0) {
    return PARENT_KEY_CUSTOMER;
  }
  if (len == 8 && memcmp(body + start, "merchant", 8) == 0) {
    return PARENT_KEY_MERCHANT;
  }
  if (len == 8 && memcmp(body + start, "terminal", 8) == 0) {
    return PARENT_KEY_TERMINAL;
  }
  if (len == 16 && memcmp(body + start, "last_transaction", 16) == 0) {
    return PARENT_KEY_LAST_TRANSACTION;
  }
  return PARENT_KEY_NONE;
}

static bool to_bool(const char *str, size_t start, size_t end, uint8_t *out) {
  if (str == NULL || out == NULL || start >= end) {
    return false;
  }

  size_t len = end - start;
  if (len == 4 && str[start] == 't' && str[start + 1] == 'r' && str[start + 2] == 'u' &&
      str[start + 3] == 'e') {
    *out = 1;
    return true;
  }
  if (len == 5 && str[start] == 'f' && str[start + 1] == 'a' && str[start + 2] == 'l' &&
      str[start + 3] == 's' && str[start + 4] == 'e') {
    *out = 0;
    return true;
  }
  return false;
}

static bool to_double(const char *str, size_t start, size_t end, double *out) {
  if (str == NULL || out == NULL || start >= end) {
    return false;
  }

  static const double INV_POW10[] = {
    1.0,   1e-1,  1e-2,  1e-3,  1e-4,  1e-5,  1e-6,  1e-7,  1e-8,  1e-9,
    1e-10, 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18,
  };

  uint64_t int_part = 0;
  uint64_t frac_part = 0;
  uint8_t frac_digits = 0;
  bool has_dot = false;
  bool has_digit = false;

  for (size_t i = start; i < end; i++) {
    char c = str[i];
    if (c == '.') {
      if (has_dot) {
        return false;
      }
      has_dot = true;
      continue;
    }

    if (!is_ascii_digit(c)) {
      return false;
    }

    has_digit = true;
    uint64_t digit = (uint64_t)(c - '0');
    if (!has_dot) {
      int_part = int_part * 10U + digit;
    } else {
      if (frac_digits < 18) {
        frac_part = frac_part * 10U + digit;
        frac_digits++;
      }
    }
  }

  if (!has_digit) {
    return false;
  }

  *out = (double)int_part + (double)frac_part * INV_POW10[frac_digits];
  return true;
}

static bool to_epoch(const char *str, size_t start, size_t end, int64_t *out_epoch) {
  if (str == NULL || out_epoch == NULL || start >= end) {
    return false;
  }
  if (end - start != 20) {
    return false;
  }

  const size_t y0 = start;
  const size_t m0 = start + 5;
  const size_t d0 = start + 8;
  const size_t h0 = start + 11;
  const size_t n0 = start + 14;
  const size_t s0 = start + 17;
  const size_t z = start + 19;

  if (str[start + 4] != '-' || str[start + 7] != '-' || str[start + 10] != 'T' ||
      str[start + 13] != ':' || str[start + 16] != ':' || str[z] != 'Z') {
    return false;
  }

  for (size_t i = start; i < end; i++) {
    if (i == start + 4 || i == start + 7 || i == start + 10 || i == start + 13 || i == start + 16 ||
        i == z) {
      continue;
    }
    if (!is_ascii_digit(str[i])) {
      return false;
    }
  }

  const int year = (str[y0] - '0') * 1000 + (str[y0 + 1] - '0') * 100 + (str[y0 + 2] - '0') * 10 +
                   (str[y0 + 3] - '0');
  const int month = (str[m0] - '0') * 10 + (str[m0 + 1] - '0');
  const int day = (str[d0] - '0') * 10 + (str[d0 + 1] - '0');
  const int hour = (str[h0] - '0') * 10 + (str[h0 + 1] - '0');
  const int minute = (str[n0] - '0') * 10 + (str[n0 + 1] - '0');
  const int second = (str[s0] - '0') * 10 + (str[s0 + 1] - '0');

  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
    return false;
  }

  *out_epoch = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 +
               minute * 60 + second;
  return true;
}

static bool to_known_merchants(const char *str, size_t start, size_t end,
                               char out[TX2_MAX_KNOWN_MERCHANTS][TX2_MERCHANT_ID_BUF_LEN],
                               uint8_t *out_len) {
  if (str == NULL || out == NULL || out_len == NULL || start >= end) {
    return false;
  }
  if (str[start] != '[' || str[end - 1] != ']') {
    return false;
  }

  uint8_t count = 0;
  size_t item_start = 0;
  enum {
    ARRAY_EXPECT_ITEM_OR_END = 0,
    ARRAY_IN_ITEM = 1,
    ARRAY_EXPECT_COMMA_OR_END = 2,
  } state = ARRAY_EXPECT_ITEM_OR_END;

  for (size_t i = start + 1; i < end - 1; i++) {
    char c = str[i];

    if (state == ARRAY_EXPECT_ITEM_OR_END) {
      if (is_ascii_space(c)) {
        continue;
      }
      if (c != '"') {
        return false;
      }
      item_start = i + 1;
      state = ARRAY_IN_ITEM;
      continue;
    }

    if (state == ARRAY_IN_ITEM) {
      if (c == '\\') {
        return false;
      }
      if (c != '"') {
        continue;
      }

      size_t item_len = i - item_start;
      if (item_len == 0 || item_len > TX2_MERCHANT_ID_LEN) {
        return false;
      }
      if (count >= TX2_MAX_KNOWN_MERCHANTS) {
        return false;
      }

      memcpy(out[count], str + item_start, item_len);
      out[count][item_len] = '\0';
      count++;
      state = ARRAY_EXPECT_COMMA_OR_END;
      continue;
    }

    if (is_ascii_space(c)) {
      continue;
    }
    if (c != ',') {
      return false;
    }
    state = ARRAY_EXPECT_ITEM_OR_END;
  }

  if (state == ARRAY_IN_ITEM) {
    return false;
  }
  if (state == ARRAY_EXPECT_ITEM_OR_END && count > 0) {
    // Disallow trailing comma: ["MERC-001",]
    return false;
  }

  *out_len = count;
  return true;
}

static bool to_string(const char *str, size_t start, size_t end, char *out, size_t out_cap) {
  if (str == NULL || out == NULL || out_cap == 0 || start >= end) {
    return false;
  }

  size_t len = end - start;
  if (len >= out_cap) {
    return false;
  }

  memcpy(out, str + start, len);
  out[len] = '\0';
  return true;
}

static bool to_uint8(const char *str, size_t start, size_t end, uint8_t *out) {
  if (str == NULL || out == NULL || start >= end) {
    return false;
  }

  unsigned value = 0;
  for (size_t i = start; i < end; i++) {
    if (!is_ascii_digit(str[i])) {
      return false;
    }
    value = value * 10U + (unsigned)(str[i] - '0');
    if (value > UINT8_MAX) {
      return false;
    }
  }

  *out = (uint8_t)value;
  return true;
}

static void transaction_context_destroy(TransactionContext *self) {
  if (self == NULL) {
    return;
  }
  memset(self, 0, sizeof(*self));
}

static void transaction_context_init(TransactionContext *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->merchant_mcc_risk = 0.5;
  ctx->destroy = transaction_context_destroy;
  ctx->to_vector = transaction_context_to_vector;
}

static bool transaction_context_parse(TransactionContext *self, const char *body, size_t body_len) {
  if (self == NULL || body == NULL) {
    return false;
  }

  enum {
    STATE_FIND_KEY = 0,
    STATE_AFTER_KEY = 1,
    STATE_FIND_VALUE = 2,
    STATE_AFTER_VALUE = 3,
  };
  size_t i = 0;
  int depth = 0;
  int state = STATE_FIND_KEY;
  int escaped = 0;
  size_t current_key_start = 0;
  size_t current_key_end = 0;
  size_t current_key_child_start = 0;
  size_t current_key_child_end = 0;
  size_t value_start = 0;
  size_t value_end = 0;

  while (i < body_len) {
    char c = body[i];

    if (is_ascii_space(c)) {
      i++;
      continue;
    }

    if (state == STATE_FIND_KEY) {
      if (c == '{') {
        depth++;
        i++;
        continue;
      }
      if (c == '}') {
        if (depth <= 0) {
          return false;
        }
        depth--;
        i++;
        if (depth == 0) {
          break;
        }
        state = STATE_AFTER_VALUE;
        continue;
      }
      if (c == '"') {
        size_t key_start = i + 1;
        i++;
        escaped = 0;
        while (i < body_len) {
          c = body[i];
          if (escaped) {
            escaped = 0;
          } else if (c == '\\') {
            escaped = 1;
          } else if (c == '"') {
            break;
          }
          i++;
        }
        if (i >= body_len) {
          return false;
        }
        if (depth == 1) {
          current_key_start = key_start;
          current_key_end = i;
        } else if (depth == 2) {
          current_key_child_start = key_start;
          current_key_child_end = i;
        }
        i++;
        state = STATE_AFTER_KEY;
        continue;
      }

      i++;
      continue;
    }

    if (state == STATE_AFTER_KEY) {
      if (c != ':') {
        return false;
      }
      i++;
      state = STATE_FIND_VALUE;
      continue;
    }

    if (state == STATE_FIND_VALUE) {
      if (c == '{') {
        depth++;
        i++;
        state = STATE_FIND_KEY;
        continue;
      }

      if (c == '[') {
        int arr_depth = 1;
        int in_string = 0;
        value_start = i;
        i++;
        escaped = 0;
        while (i < body_len && arr_depth > 0) {
          c = body[i];
          if (in_string) {
            if (escaped) {
              escaped = 0;
            } else if (c == '\\') {
              escaped = 1;
            } else if (c == '"') {
              in_string = 0;
            }
          } else {
            if (c == '"') {
              in_string = 1;
            } else if (c == '[') {
              arr_depth++;
            } else if (c == ']') {
              arr_depth--;
            }
          }
          i++;
        }
        if (arr_depth != 0) {
          return false;
        }
        value_end = i;
      } else if (c == '"') {
        value_start = i + 1;
        i++;
        escaped = 0;
        while (i < body_len) {
          c = body[i];
          if (escaped) {
            escaped = 0;
          } else if (c == '\\') {
            escaped = 1;
          } else if (c == '"') {
            break;
          }
          i++;
        }
        if (i >= body_len) {
          return false;
        }
        value_end = i;
        i++;
      } else {
        value_start = i;
        while (i < body_len && body[i] != ',' && body[i] != '}' && !is_ascii_space(body[i])) {
          i++;
        }
        value_end = i;
      }

      if (depth == 1) {
        if ((current_key_end - current_key_start) == 2 &&
            strncmp(body + current_key_start, "id", 2) == 0) {
          if (value_end <= value_start ||
              !to_string(body, value_start, value_end, self->id, TX2_ID_BUF_LEN)) {
            return false;
          }
        }
      } else if (depth == 2) {
        ParentKey parent_key = parent_key_id(body, current_key_start, current_key_end);
        ChildKey child_key = child_key_id(body, current_key_child_start, current_key_child_end);

        switch (parent_key) {
        case PARENT_KEY_TRANSACTION:
          switch (child_key) {
          case CHILD_KEY_AMOUNT:
            if (value_end <= value_start ||
                !to_double(body, value_start, value_end, &self->transaction_amount)) {
              return false;
            }
            break;
          case CHILD_KEY_INSTALLMENTS:
            if (value_end <= value_start ||
                !to_uint8(body, value_start, value_end, &self->transaction_installments)) {
              return false;
            }
            break;
          case CHILD_KEY_REQUESTED_AT:
            if (value_end <= value_start ||
                !to_epoch(body, value_start, value_end, &self->transaction_requested_at)) {
              return false;
            }
            break;
          default:
            break;
          }
          break;

        case PARENT_KEY_CUSTOMER:
          switch (child_key) {
          case CHILD_KEY_AVG_AMOUNT:
            if (value_end <= value_start ||
                !to_double(body, value_start, value_end, &self->customer_avg_amount)) {
              return false;
            }
            break;
          case CHILD_KEY_TX_COUNT_24H:
            if (value_end <= value_start ||
                !to_uint8(body, value_start, value_end, &self->customer_tx_count_24h)) {
              return false;
            }
            break;
          case CHILD_KEY_KNOWN_MERCHANTS:
            if (value_end <= value_start ||
                !to_known_merchants(body, value_start, value_end, self->customer_known_merchants,
                                    &self->customer_known_merchants_len)) {
              return false;
            }
            break;
          default:
            break;
          }
          break;

        case PARENT_KEY_MERCHANT:
          switch (child_key) {
          case CHILD_KEY_ID:
            if (value_end <= value_start ||
                !to_string(body, value_start, value_end, self->merchant_id,
                           TX2_MERCHANT_ID_BUF_LEN)) {
              return false;
            }
            break;
          case CHILD_KEY_MCC:
            if (value_end <= value_start ||
                !to_string(body, value_start, value_end, self->merchant_mcc, TX2_MCC_BUF_LEN)) {
              return false;
            }
            break;
          case CHILD_KEY_AVG_AMOUNT:
            if (value_end <= value_start ||
                !to_double(body, value_start, value_end, &self->merchant_avg_amount)) {
              return false;
            }
            break;
          default:
            break;
          }
          break;

        case PARENT_KEY_TERMINAL:
          switch (child_key) {
          case CHILD_KEY_IS_ONLINE:
            if (value_end <= value_start ||
                !to_bool(body, value_start, value_end, &self->terminal_is_online)) {
              return false;
            }
            break;
          case CHILD_KEY_CARD_PRESENT:
            if (value_end <= value_start ||
                !to_bool(body, value_start, value_end, &self->terminal_card_present)) {
              return false;
            }
            break;
          case CHILD_KEY_KM_FROM_HOME:
            if (value_end <= value_start ||
                !to_double(body, value_start, value_end, &self->terminal_km_from_home)) {
              return false;
            }
            break;
          default:
            break;
          }
          break;

        case PARENT_KEY_LAST_TRANSACTION:
          switch (child_key) {
          case CHILD_KEY_TIMESTAMP:
            if (value_end <= value_start ||
                !to_epoch(body, value_start, value_end, &self->last_transaction_timestamp)) {
              return false;
            }
            self->has_last_transaction = 1;
            break;
          case CHILD_KEY_KM_FROM_CURRENT:
            if (value_end <= value_start ||
                !to_double(body, value_start, value_end, &self->last_transaction_km_from_current)) {
              return false;
            }
            self->has_last_transaction = 1;
            break;
          default:
            break;
          }
          break;

        default:
          break;
        }
      }

      state = STATE_AFTER_VALUE;
      continue;
    }

    if (state == STATE_AFTER_VALUE) {
      if (c == ',') {
        i++;
        state = STATE_FIND_KEY;
        continue;
      }
      if (c == '}') {
        if (depth <= 0) {
          return false;
        }
        depth--;
        i++;
        if (depth == 0) {
          break;
        }
        state = STATE_AFTER_VALUE;
        continue;
      }
      return false;
    }
  }

  while (i < body_len && is_ascii_space(body[i])) {
    i++;
  }

  bool ok = depth == 0 && i == body_len;
  if (ok) {
    self->merchant_known = merchant_in_known_list(self);
    self->merchant_mcc_risk = merchant_mcc_risk_or_default(self->merchant_mcc);
  }

  return ok;
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

TransactionContext transaction_context_new(void) {
  TransactionContext ctx;
  transaction_context_init(&ctx);
  return ctx;
}

TransactionContext transaction_context_from_body(const char *body, size_t body_len) {
  TransactionContext ctx = transaction_context_new();
  if (!transaction_context_parse(&ctx, body, body_len)) {
    transaction_context_init(&ctx);
  } else {
    ctx.destroy = transaction_context_destroy;
    ctx.to_vector = transaction_context_to_vector;
  }
  return ctx;
}
