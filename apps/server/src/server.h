#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
  size_t body_offset;
  size_t body_len;
} HttpRequestView;

int create_unix_server(const char *path);
ssize_t read_http_request(int fd, char *buf, size_t cap, HttpRequestView *out_view);

#endif
