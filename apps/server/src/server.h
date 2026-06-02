#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
  HTTP_READ_PENDING = 0,
  HTTP_READ_COMPLETE = 1,
  HTTP_READ_CLOSED = -1,
  HTTP_READ_ERROR = -2,
  HTTP_READ_OVERFLOW = -3,
} HttpReadStatus;

int create_unix_server(const char *path);
HttpReadStatus read_http_request(int fd, char *buf, size_t cap, size_t *used,
                                 size_t *expected_total, size_t *out_request_len);
bool get_method(const char *request, char *out, size_t out_size);
bool get_pathname(const char *request, char *out, size_t out_size);
bool get_body(const char *request, size_t request_len, const char **out_body, size_t *out_len);

#endif
