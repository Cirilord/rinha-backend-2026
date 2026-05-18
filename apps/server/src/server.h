#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

int create_unix_server(const char *path);
ssize_t read_http_request(int fd, char *buf, size_t cap);
bool get_method(const char *request, char *out, size_t out_size);
bool get_pathname(const char *request, char *out, size_t out_size);
bool get_body(const char *request, size_t request_len, const char **out_body,
              size_t *out_len);

#endif
