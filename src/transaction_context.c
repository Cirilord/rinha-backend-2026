#include "transaction_context.h"

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
  ctx->to_vector = transaction_context_to_vector;
}

TransactionContext transaction_context_new(void) {
  TransactionContext ctx;
  transaction_context_init(&ctx);
  return ctx;
}
