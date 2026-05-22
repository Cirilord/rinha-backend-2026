#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int create_unix_server(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

static bool parse_content_length(const char *line, size_t line_len, size_t *out_content_len) {
  static const char key[] = "Content-Length:";
  const size_t key_len = sizeof(key) - 1;

  if (line_len < key_len) {
    return false;
  }

  for (size_t i = 0; i < key_len; i++) {
    if (line[i] != key[i]) {
      return false;
    }
  }

  size_t i = key_len;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t')) {
    i++;
  }
  if (i == line_len) {
    return false;
  }

  size_t value = 0;
  bool has_digits = false;
  for (; i < line_len; i++) {
    const unsigned char c = (unsigned char)line[i];
    if (c < '0' || c > '9') {
      return false;
    }
    has_digits = true;
    const size_t digit = (size_t)(c - '0');
    if (value > ((size_t)-1 - digit) / 10) {
      return false;
    }
    value = (value * 10) + digit;
  }

  if (!has_digits) {
    return false;
  }

  *out_content_len = value;
  return true;
}

ssize_t read_http_request(int fd, char *buf, size_t cap, HttpRequestView *out_view) {
  if (cap == 0 || out_view == NULL) {
    return -1;
  }

  out_view->body_offset = 0;
  out_view->body_len = 0;

  size_t used = 0;
  size_t scan_pos = 0;
  size_t line_start = 0;
  size_t content_length = 0;
  size_t expected_total = 0;
  bool headers_done = false;
  bool content_length_seen = false;

  while (used + 1 < cap) {
    ssize_t n = read(fd, buf + used, cap - used - 1);
    if (n <= 0) {
      return (used > 0) ? (ssize_t)used : -1;
    }

    used += (size_t)n;
    buf[used] = '\0';

    if (!headers_done) {
      while ((scan_pos + 1) < used) {
        if (buf[scan_pos] == '\r' && buf[scan_pos + 1] == '\n') {
          const size_t line_len = scan_pos - line_start;

          if (line_len == 0) {
            headers_done = true;
            out_view->body_offset = scan_pos + 2;
            out_view->body_len = content_length;
            expected_total = out_view->body_offset + content_length;
            if (expected_total + 1 > cap) {
              return -1;
            }
            scan_pos += 2;
            break;
          }

          if (!content_length_seen) {
            size_t parsed = 0;
            if (parse_content_length(buf + line_start, line_len, &parsed)) {
              content_length = parsed;
              content_length_seen = true;
            }
          }

          scan_pos += 2;
          line_start = scan_pos;
          continue;
        }

        scan_pos++;
      }
    }

    if (headers_done && used >= expected_total) {
      return (ssize_t)used;
    }
  }

  buf[cap - 1] = '\0';
  return (ssize_t)(cap - 1);
}
