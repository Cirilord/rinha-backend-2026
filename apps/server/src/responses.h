#ifndef RESPONSES_H
#define RESPONSES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *data;
  size_t len;
} Response;

#define RESPONSE_CLOSE_INDEX 0
#define RESPONSE_KEEP_ALIVE_INDEX 1
#define FRAUD_RESPONSES_LEN 6

extern const Response RESPONSE_READY_VARIANTS[2];
extern const Response RESPONSE_NOT_FOUND_VARIANTS[2];
extern const Response RESPONSE_BAD_REQUEST_VARIANTS[2];
extern const Response RESPONSE_FRAUD_VARIANTS[2][FRAUD_RESPONSES_LEN];

#endif
