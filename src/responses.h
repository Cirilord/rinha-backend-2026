#ifndef RESPONSES_H
#define RESPONSES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *data;
  size_t len;
} Response;

#define FRAUD_RESPONSES_LEN 6

extern const Response RESPONSE_OK;
extern const Response FRAUD_RESPONSES[FRAUD_RESPONSES_LEN];
extern const Response RESPONSE_READY;
extern const Response RESPONSE_NOT_FOUND;
extern const Response RESPONSE_BAD_REQUEST;

#endif
