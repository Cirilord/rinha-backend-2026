#ifndef RESPONSES_H
#define RESPONSES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *data;
  size_t len;
} Response;

#define FRAUD_RESPONSES_LEN 6

extern const Response RESPONSE_READY;
extern const Response RESPONSE_NOT_FOUND;
extern const Response RESPONSE_BAD_REQUEST;
extern const Response RESPONSE_FRAUD_00;
extern const Response RESPONSE_FRAUD_02;
extern const Response RESPONSE_FRAUD_04;
extern const Response RESPONSE_FRAUD_06;
extern const Response RESPONSE_FRAUD_08;
extern const Response RESPONSE_FRAUD_10;

#endif
