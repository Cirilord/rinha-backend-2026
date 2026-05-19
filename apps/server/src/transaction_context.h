#ifndef TRANSACTION_CONTEXT_H
#define TRANSACTION_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

struct TransactionContext;
typedef void (*TransactionContextDestroyFn)(struct TransactionContext *self);
typedef void (*TransactionContextToVectorFn)(const struct TransactionContext *self, double out[14]);

typedef struct TransactionContext {
  char *id;

  double transaction_amount;
  int transaction_installments;
  long long transaction_requested_at;

  double customer_avg_amount;
  int customer_tx_count_24h;
  char **customer_known_merchants;
  int customer_known_merchants_len;

  char *merchant_id;
  char *merchant_mcc;
  double merchant_avg_amount;
  int merchant_known;
  double merchant_mcc_risk;

  int terminal_is_online;
  int terminal_card_present;
  double terminal_km_from_home;

  bool has_last_transaction;
  long long last_transaction_timestamp;
  double last_transaction_km_from_current;

  TransactionContextDestroyFn destroy;
  TransactionContextToVectorFn to_vector;
} TransactionContext;

TransactionContext transaction_context_from_body(const char *body);
TransactionContext transaction_context_from_body_n(const char *body, size_t body_len);
TransactionContext transaction_context_new(void);

#endif
