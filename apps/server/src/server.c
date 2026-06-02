#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__linux__)
#include "../../../packages/mocks/sys/socket.h"
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int create_unix_server(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  unlink(path);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 128) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static const char *find_headers_end(const char *buf) { return strstr(buf, "\r\n\r\n"); }

static size_t parse_content_length(const char *buf) {
  const char *p = strstr(buf, "\r\nContent-Length:");
  if (p == NULL) {
    if (strncmp(buf, "Content-Length:", 15) == 0) {
      p = buf - 2;
    } else {
      return 0;
    }
  }
  p += 17;
  while (*p == ' ') {
    p++;
  }
  return (size_t)strtoul(p, NULL, 10);
}

HttpReadStatus read_http_request(int fd, char *buf, size_t cap, size_t *used,
                                 size_t *expected_total, size_t *out_request_len) {
  if (cap == 0) {
    return HTTP_READ_ERROR;
  }
  if (buf == NULL || used == NULL || expected_total == NULL || out_request_len == NULL) {
    return HTTP_READ_ERROR;
  }

  while (true) {
    if (*expected_total == 0) {
      const char *headers_end = find_headers_end(buf);
      if (headers_end != NULL) {
        size_t headers_len = (size_t)(headers_end - buf) + 4;
        size_t content_len = parse_content_length(buf);
        *expected_total = headers_len + content_len;
        if (*expected_total + 1 > cap) {
          return HTTP_READ_OVERFLOW;
        }
        if (*used >= *expected_total) {
          *out_request_len = *expected_total;
          return HTTP_READ_COMPLETE;
        }
      }
    } else if (*used >= *expected_total) {
      *out_request_len = *expected_total;
      return HTTP_READ_COMPLETE;
    }

    if (*used + 1 >= cap) {
      break;
    }

    ssize_t n = read(fd, buf + *used, cap - *used - 1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return HTTP_READ_PENDING;
      }
      return HTTP_READ_ERROR;
    }
    if (n == 0) {
      return HTTP_READ_CLOSED;
    }
    *used += (size_t)n;
    buf[*used] = '\0';
  }

  buf[cap - 1] = '\0';
  return HTTP_READ_OVERFLOW;
}

bool get_body(const char *request, size_t request_len, const char **out_body, size_t *out_len) {
  const char *body_start = NULL;
  const char *headers_end = NULL;

  if (request == NULL || out_body == NULL || out_len == NULL) {
    return false;
  }

  headers_end = strstr(request, "\r\n\r\n");
  if (headers_end == NULL) {
    return false;
  }

  body_start = headers_end + 4;
  if ((size_t)(body_start - request) > request_len) {
    return false;
  }

  *out_body = body_start;
  *out_len = request_len - (size_t)(body_start - request);
  return true;
}

bool get_method(const char *request, char *out, size_t out_size) {
  if (request == NULL || out == NULL || out_size == 0) {
    return false;
  }

  const char *method_end = strchr(request, ' ');
  if (method_end == NULL) {
    return false;
  }

  size_t method_len = (size_t)(method_end - request);
  if (method_len == 0 || method_len + 1 > out_size) {
    return false;
  }

  memcpy(out, request, method_len);
  out[method_len] = '\0';
  return true;
}

bool get_pathname(const char *request, char *out, size_t out_size) {
  if (request == NULL || out == NULL || out_size == 0) {
    return false;
  }

  const char *first_space = strchr(request, ' ');
  if (first_space == NULL) {
    return false;
  }

  const char *path_start = first_space + 1;
  if (*path_start == '\0') {
    return false;
  }

  const char *path_end = strchr(path_start, ' ');
  if (path_end == NULL) {
    return false;
  }

  size_t path_len = (size_t)(path_end - path_start);
  if (path_len == 0 || path_len + 1 > out_size) {
    return false;
  }

  memcpy(out, path_start, path_len);
  out[path_len] = '\0';
  return true;
}
