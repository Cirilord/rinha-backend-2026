#ifndef TRANSACTION_CONTEXT_H
#define TRANSACTION_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#define NORMALIZATION_MAX_AMOUNT 10000.0
#define NORMALIZATION_MAX_INSTALLMENTS 12.0
#define NORMALIZATION_AMOUNT_VS_AVG_RATIO 10.0
#define NORMALIZATION_MAX_MINUTES 1440.0
#define NORMALIZATION_MAX_KM 1000.0
#define NORMALIZATION_MAX_TX_COUNT_24H 20.0
#define NORMALIZATION_MAX_MERCHANT_AVG_AMOUNT 10000.0

typedef struct {
  double amount;
  int installments;
  long long requested_at;
} Transaction;

typedef struct {
  double avg_amount;
  int tx_count_24h;
  char **known_merchants;
  size_t known_merchants_count;
} Customer;

typedef struct {
  char *id;
  char *mcc;
  double avg_amount;
} Merchant;

typedef struct {
  bool is_online;
  bool card_present;
  double km_from_home;
} Terminal;

typedef struct {
  long long timestamp;
  double km_from_current;
} LastTransaction;

struct TransactionContext;
typedef void (*TransactionContextDestroyFn)(struct TransactionContext *self);
typedef void (*TransactionContextToVectorFn)(const struct TransactionContext *self, double out[14]);

typedef struct TransactionContext {
  char *id;
  Transaction transaction;
  Customer customer;
  Merchant merchant;
  Terminal terminal;
  LastTransaction *last_transaction;

  TransactionContextDestroyFn destroy;
  TransactionContextToVectorFn to_vector;
} TransactionContext;

TransactionContext transaction_context_new(void);

#endif
