#ifndef TRANSACTION_CONTEXT_H
#define TRANSACTION_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

enum {
  TX2_ID_MAX_LEN = 13,
  TX2_ID_BUF_LEN = TX2_ID_MAX_LEN + 1,
  TX2_MERCHANT_ID_LEN = 8,
  TX2_MERCHANT_ID_BUF_LEN = TX2_MERCHANT_ID_LEN + 1,
  TX2_MCC_LEN = 4,
  TX2_MCC_BUF_LEN = TX2_MCC_LEN + 1,
  TX2_MAX_KNOWN_MERCHANTS = 5,
};

typedef struct TransactionContext TransactionContext;
typedef void (*TransactionContextDestroyFn)(TransactionContext *self);
typedef void (*TransactionContextToVectorFn)(const TransactionContext *self, double out[14]);

typedef struct TransactionContext {
  // "tx-##########" => max 13 chars in test-data + '\0'
  char id[TX2_ID_BUF_LEN];

  // test-data range: 10.01 .. 9999.51
  double transaction_amount;
  // test-data range: 1 .. 12
  uint8_t transaction_installments;
  // unix epoch (seconds, UTC)
  int64_t transaction_requested_at;

  // test-data range: 20.02 .. 999.96
  double customer_avg_amount;
  // test-data range: 1 .. 20
  uint8_t customer_tx_count_24h;
  char customer_known_merchants[TX2_MAX_KNOWN_MERCHANTS][TX2_MERCHANT_ID_BUF_LEN];
  uint8_t customer_known_merchants_len;

  char merchant_id[TX2_MERCHANT_ID_BUF_LEN];
  char merchant_mcc[TX2_MCC_BUF_LEN];
  // test-data range: 20.00 .. 499.98
  double merchant_avg_amount;
  uint8_t merchant_known;
  // original risk value (ex: 0.5)
  double merchant_mcc_risk;

  uint8_t terminal_is_online;
  uint8_t terminal_card_present;
  // test-data range: 0.0017452869 .. 999.9759074300
  double terminal_km_from_home;

  uint8_t has_last_transaction;
  // unix epoch (seconds, UTC)
  int64_t last_transaction_timestamp;
  // test-data range: 0.0001938548 .. 999.9832609668
  double last_transaction_km_from_current;

  TransactionContextDestroyFn destroy;
  TransactionContextToVectorFn to_vector;
} TransactionContext;

TransactionContext transaction_context_new(void);
TransactionContext transaction_context_from_body(const char *body, size_t body_len);

#endif
