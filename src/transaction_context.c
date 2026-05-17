#include "transaction_context.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

static inline double mcc_risk_or_default(const char *mcc) {
  if (!mcc) return 0.5;
  if (strcmp(mcc, "5411") == 0) return 0.15;
  if (strcmp(mcc, "5812") == 0) return 0.3;
  if (strcmp(mcc, "5912") == 0) return 0.2;
  if (strcmp(mcc, "5944") == 0) return 0.45;
  if (strcmp(mcc, "7801") == 0) return 0.8;
  if (strcmp(mcc, "7802") == 0) return 0.75;
  if (strcmp(mcc, "7995") == 0) return 0.85;
  if (strcmp(mcc, "4511") == 0) return 0.35;
  if (strcmp(mcc, "5311") == 0) return 0.25;
  if (strcmp(mcc, "5999") == 0) return 0.5;
  return 0.5;
}

static void transaction_context_destroy(TransactionContext *ctx) {
  if (!ctx) return;

  free(ctx->id);
  ctx->id = NULL;

  if (ctx->customer.known_merchants) {
    for (size_t i = 0; i < ctx->customer.known_merchants_count; i++) {
      free(ctx->customer.known_merchants[i]);
      ctx->customer.known_merchants[i] = NULL;
    }
    free(ctx->customer.known_merchants);
    ctx->customer.known_merchants = NULL;
  }
  ctx->customer.known_merchants_count = 0;

  free(ctx->merchant.id);
  ctx->merchant.id = NULL;

  free(ctx->merchant.mcc);
  ctx->merchant.mcc = NULL;

  free(ctx->last_transaction);
  ctx->last_transaction = NULL;
}

static bool transaction_context_from_body(TransactionContext *self, const char *body) {
  if (!self || !body) return false;

  self->destroy(self);
  self->transaction.amount = 0.0;
  self->transaction.installments = 0;
  self->transaction.requested_at = 0;
  self->customer.avg_amount = 0.0;
  self->customer.tx_count_24h = 0;
  self->merchant.avg_amount = 0.0;
  self->terminal.is_online = false;
  self->terminal.card_present = false;
  self->terminal.km_from_home = 0.0;

  bool has_id = false;
  bool has_tx_amount = false;
  bool has_tx_installments = false;
  bool has_tx_requested_at = false;
  bool has_customer_avg = false;
  bool has_customer_tx_count = false;
  bool has_customer_known_merchants = false;
  bool has_merchant_id = false;
  bool has_merchant_mcc = false;
  bool has_merchant_avg = false;
  bool has_terminal_online = false;
  bool has_terminal_card_present = false;
  bool has_terminal_km = false;

  typedef enum {
    JSON_CTX_ROOT = 0,
    JSON_CTX_TRANSACTION,
    JSON_CTX_CUSTOMER,
    JSON_CTX_MERCHANT,
    JSON_CTX_TERMINAL,
    JSON_CTX_LAST_TRANSACTION,
    JSON_CTX_UNKNOWN
  } JsonContext;

  JsonContext ctx_stack[16];
  memset(ctx_stack, 0, sizeof(ctx_stack));
  ctx_stack[0] = JSON_CTX_ROOT;
  int depth = 0;

  char current_key[64];
  memset(current_key, 0, sizeof(current_key));
  bool has_key = false;
  bool in_known_merchants_array = false;
  size_t known_merchants_capacity = 0;

  const char *p = body;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;

    if (*p == ',') {
      has_key = false;
      p++;
      continue;
    }
    if (*p == ':') {
      p++;
      continue;
    }
    if (*p == '{') {
      JsonContext next_ctx = ctx_stack[depth];
      if (has_key && ctx_stack[depth] == JSON_CTX_ROOT) {
        if (strcmp(current_key, "transaction") == 0) next_ctx = JSON_CTX_TRANSACTION;
        else if (strcmp(current_key, "customer") == 0) next_ctx = JSON_CTX_CUSTOMER;
        else if (strcmp(current_key, "merchant") == 0) next_ctx = JSON_CTX_MERCHANT;
        else if (strcmp(current_key, "terminal") == 0) next_ctx = JSON_CTX_TERMINAL;
        else if (strcmp(current_key, "last_transaction") == 0) next_ctx = JSON_CTX_LAST_TRANSACTION;
        else next_ctx = JSON_CTX_UNKNOWN;
      }

      if (depth + 1 >= (int)(sizeof(ctx_stack) / sizeof(ctx_stack[0]))) {
        self->destroy(self);
        return false;
      }

      depth++;
      ctx_stack[depth] = next_ctx;
      has_key = false;
      p++;
      continue;
    }
    if (*p == '}') {
      if (depth > 0) {
        ctx_stack[depth] = JSON_CTX_ROOT;
        depth--;
      }
      in_known_merchants_array = false;
      has_key = false;
      p++;
      continue;
    }
    if (*p == '[') {
      if (has_key && ctx_stack[depth] == JSON_CTX_CUSTOMER && strcmp(current_key, "known_merchants") == 0) {
        in_known_merchants_array = true;
        has_customer_known_merchants = true;
      }
      has_key = false;
      p++;
      continue;
    }
    if (*p == ']') {
      in_known_merchants_array = false;
      has_key = false;
      p++;
      continue;
    }

    if (*p == '"') {
      char token[2048];
      size_t token_len = 0;
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p++;
        if (token_len + 1 >= sizeof(token)) {
          self->destroy(self);
          return false;
        }
        token[token_len++] = *p++;
      }
      if (*p != '"') {
        self->destroy(self);
        return false;
      }
      token[token_len] = '\0';
      p++;

      const char *look = p;
      while (*look && isspace((unsigned char)*look)) look++;
      if (!in_known_merchants_array && *look == ':') {
        if (token_len >= sizeof(current_key)) {
          self->destroy(self);
          return false;
        }
        memcpy(current_key, token, token_len + 1);
        has_key = true;
        continue;
      }

      if (in_known_merchants_array) {
        if (self->customer.known_merchants_count == known_merchants_capacity) {
          size_t new_capacity = known_merchants_capacity == 0 ? 4 : (known_merchants_capacity * 2);
          char **new_items = (char **)realloc(self->customer.known_merchants, new_capacity * sizeof(char *));
          if (!new_items) {
            self->destroy(self);
            return false;
          }
          self->customer.known_merchants = new_items;
          known_merchants_capacity = new_capacity;
        }

        self->customer.known_merchants[self->customer.known_merchants_count] = strdup(token);
        if (!self->customer.known_merchants[self->customer.known_merchants_count]) {
          self->destroy(self);
          return false;
        }
        self->customer.known_merchants_count++;
        continue;
      }

      if (!has_key) continue;

      if (ctx_stack[depth] == JSON_CTX_ROOT && strcmp(current_key, "id") == 0) {
        self->id = strdup(token);
        if (!self->id) {
          self->destroy(self);
          return false;
        }
        has_id = true;
      } else if (ctx_stack[depth] == JSON_CTX_MERCHANT && strcmp(current_key, "id") == 0) {
        self->merchant.id = strdup(token);
        if (!self->merchant.id) {
          self->destroy(self);
          return false;
        }
        has_merchant_id = true;
      } else if (ctx_stack[depth] == JSON_CTX_MERCHANT && strcmp(current_key, "mcc") == 0) {
        self->merchant.mcc = strdup(token);
        if (!self->merchant.mcc) {
          self->destroy(self);
          return false;
        }
        has_merchant_mcc = true;
      } else if (ctx_stack[depth] == JSON_CTX_TRANSACTION && strcmp(current_key, "requested_at") == 0) {
        if (!to_epoch_time(token, &self->transaction.requested_at)) {
          self->destroy(self);
          return false;
        }
        has_tx_requested_at = true;
      } else if (ctx_stack[depth] == JSON_CTX_LAST_TRANSACTION && strcmp(current_key, "timestamp") == 0) {
        if (!self->last_transaction) {
          self->last_transaction = (LastTransaction *)calloc(1, sizeof(LastTransaction));
          if (!self->last_transaction) {
            self->destroy(self);
            return false;
          }
        }
        if (!to_epoch_time(token, &self->last_transaction->timestamp)) {
          self->destroy(self);
          return false;
        }
      }

      has_key = false;
      continue;
    }

    if (has_key) {
      char token[64];
      size_t token_len = 0;
      while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) {
        if (token_len + 1 >= sizeof(token)) {
          self->destroy(self);
          return false;
        }
        token[token_len++] = *p++;
      }
      token[token_len] = '\0';

      if (ctx_stack[depth] == JSON_CTX_TRANSACTION && strcmp(current_key, "amount") == 0) {
        if (!to_double(token, &self->transaction.amount)) {
          self->destroy(self);
          return false;
        }
        has_tx_amount = true;
      } else if (ctx_stack[depth] == JSON_CTX_TRANSACTION && strcmp(current_key, "installments") == 0) {
        if (!to_int(token, &self->transaction.installments)) {
          self->destroy(self);
          return false;
        }
        has_tx_installments = true;
      } else if (ctx_stack[depth] == JSON_CTX_CUSTOMER && strcmp(current_key, "avg_amount") == 0) {
        if (!to_double(token, &self->customer.avg_amount)) {
          self->destroy(self);
          return false;
        }
        has_customer_avg = true;
      } else if (ctx_stack[depth] == JSON_CTX_CUSTOMER && strcmp(current_key, "tx_count_24h") == 0) {
        if (!to_int(token, &self->customer.tx_count_24h)) {
          self->destroy(self);
          return false;
        }
        has_customer_tx_count = true;
      } else if (ctx_stack[depth] == JSON_CTX_MERCHANT && strcmp(current_key, "avg_amount") == 0) {
        if (!to_double(token, &self->merchant.avg_amount)) {
          self->destroy(self);
          return false;
        }
        has_merchant_avg = true;
      } else if (ctx_stack[depth] == JSON_CTX_TERMINAL && strcmp(current_key, "is_online") == 0) {
        if (!to_bool(token, &self->terminal.is_online)) {
          self->destroy(self);
          return false;
        }
        has_terminal_online = true;
      } else if (ctx_stack[depth] == JSON_CTX_TERMINAL && strcmp(current_key, "card_present") == 0) {
        if (!to_bool(token, &self->terminal.card_present)) {
          self->destroy(self);
          return false;
        }
        has_terminal_card_present = true;
      } else if (ctx_stack[depth] == JSON_CTX_TERMINAL && strcmp(current_key, "km_from_home") == 0) {
        if (!to_double(token, &self->terminal.km_from_home)) {
          self->destroy(self);
          return false;
        }
        has_terminal_km = true;
      } else if (ctx_stack[depth] == JSON_CTX_LAST_TRANSACTION && strcmp(current_key, "km_from_current") == 0) {
        if (!self->last_transaction) {
          self->last_transaction = (LastTransaction *)calloc(1, sizeof(LastTransaction));
          if (!self->last_transaction) {
            self->destroy(self);
            return false;
          }
        }
        if (!to_double(token, &self->last_transaction->km_from_current)) {
          self->destroy(self);
          return false;
        }
      }

      has_key = false;
      continue;
    }

    p++;
  }

  if (!has_id || !has_tx_amount || !has_tx_installments || !has_tx_requested_at || !has_customer_avg
      || !has_customer_tx_count || !has_customer_known_merchants || !has_merchant_id || !has_merchant_mcc
      || !has_merchant_avg || !has_terminal_online || !has_terminal_card_present || !has_terminal_km) {
    self->destroy(self);
    return false;
  }

  return true;
}

static void transaction_context_to_vector(const TransactionContext *self, double out[14]) {
  double amount = clamp_01(self->transaction.amount / NORMALIZATION_MAX_AMOUNT);
  double installments = clamp_01((double)self->transaction.installments / NORMALIZATION_MAX_INSTALLMENTS);
  
  double customer_avg = self->customer.avg_amount < 1e-9 ? 1e-9 : self->customer.avg_amount;
  double amount_vs_avg = clamp_01(self->transaction.amount / customer_avg / NORMALIZATION_AMOUNT_VS_AVG_RATIO);
  
  double hour_of_day = (double)hour_of_day_from_epoch(self->transaction.requested_at) / 23.0;
  double day_of_week = (double)day_of_week_from_epoch(self->transaction.requested_at) / 6;
  double minutes_since_last =
      self->last_transaction
          ? clamp_01(((double)(self->transaction.requested_at - self->last_transaction->timestamp) / 60.0) /
                     NORMALIZATION_MAX_MINUTES)
          : -1.0;
  double km_from_last_tx =
      self->last_transaction ? clamp_01(self->last_transaction->km_from_current / NORMALIZATION_MAX_KM) : -1.0;
  double km_from_home = clamp_01(self->terminal.km_from_home / NORMALIZATION_MAX_KM);
  double tx_count_24h = clamp_01((double)self->customer.tx_count_24h / NORMALIZATION_MAX_TX_COUNT_24H);
  double is_online = self->terminal.is_online ? 1.0 : 0.0;
  double card_present = self->terminal.card_present ? 1.0 : 0.0;
  
  double unknown_merchant = 1.0;
  if (self->merchant.id && self->customer.known_merchants && self->customer.known_merchants_count > 0) {
    for (size_t i = 0; i < self->customer.known_merchants_count; i++) {
      if (self->customer.known_merchants[i] && strcmp(self->customer.known_merchants[i], self->merchant.id) == 0) {
        unknown_merchant = 0.0;
        break;
      }
    }
  }

  double mcc_risk = mcc_risk_or_default(self->merchant.mcc);
  double merchant_avg_amount =
      clamp_01(self->merchant.avg_amount / NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT);

  memset(out, 0, 14 * sizeof(double));

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
  ctx->from_body = transaction_context_from_body;
}

TransactionContext transaction_context_new(void) {
  TransactionContext ctx;
  transaction_context_init(&ctx);
  return ctx;
}
