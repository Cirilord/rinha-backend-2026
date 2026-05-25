#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::size_t kClientBufferSize = 8192;

constexpr char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "Content-Length: 18\r\n"
    "\r\n"
    "{\"approved\":false}";

static void close_fd(int fd) {
  if (fd >= 0) {
    (void)close(fd);
  }
}

static int create_unix_server(const char* socket_path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  (void)unlink(socket_path);

  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(fd);
    return -1;
  }

  if (listen(fd, 256) != 0) {
    close_fd(fd);
    return -1;
  }

  return fd;
}

static int recv_client_fd(int control_fd) {
  for (;;) {
    char data = 0;
    iovec io;
    io.iov_base = &data;
    io.iov_len = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    std::memset(cmsgbuf, 0, sizeof(cmsgbuf));

    msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    const ssize_t n = recvmsg(control_fd, &msg, 0);
    if (n > 0) {
      cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
      if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
      }

      int client_fd = -1;
      std::memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(client_fd));
      return client_fd;
    }

    if (n == 0) {
      return 0;
    }

    if (errno == EINTR) {
      continue;
    }

    return -1;
  }
}

static bool parse_content_length_line(const char* line, std::size_t line_len,
                                      std::size_t* out_content_len) {
  const char key[] = "Content-Length:";
  const std::size_t key_len = sizeof(key) - 1;
  if (line_len < key_len || std::memcmp(line, key, key_len) != 0) {
    return false;
  }

  std::size_t i = key_len;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t')) {
    i++;
  }
  if (i >= line_len) {
    return false;
  }

  std::size_t value = 0;
  bool has_digits = false;
  for (; i < line_len; i++) {
    const unsigned char ch = static_cast<unsigned char>(line[i]);
    if (ch < '0' || ch > '9') {
      return false;
    }

    has_digits = true;
    const std::size_t digit = static_cast<std::size_t>(ch - '0');
    if (value > ((static_cast<std::size_t>(-1) - digit) / 10)) {
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

static bool read_full_request(int client_fd) {
  char buffer[kClientBufferSize];
  std::size_t used = 0;
  std::size_t scan_pos = 0;
  std::size_t line_start = 0;
  std::size_t content_length = 0;
  std::size_t expected_total = 0;
  bool headers_done = false;
  bool content_length_seen = false;

  while (used + 1 < sizeof(buffer)) {
    const ssize_t n = recv(client_fd, buffer + used, sizeof(buffer) - used - 1, 0);
    if (n > 0) {
      used += static_cast<std::size_t>(n);
      buffer[used] = '\0';
    } else if (n == 0) {
      return false;
    } else {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    if (!headers_done) {
      while ((scan_pos + 1) < used) {
        if (buffer[scan_pos] == '\r' && buffer[scan_pos + 1] == '\n') {
          const std::size_t line_len = scan_pos - line_start;
          if (line_len == 0) {
            headers_done = true;
            const std::size_t body_offset = scan_pos + 2;
            expected_total = body_offset + content_length;
            if (expected_total + 1 > sizeof(buffer)) {
              return false;
            }
            scan_pos += 2;
            break;
          }

          if (!content_length_seen) {
            std::size_t parsed = 0;
            if (parse_content_length_line(buffer + line_start, line_len, &parsed)) {
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
      return true;
    }
  }

  return false;
}

static bool send_response(int client_fd) {
  const std::size_t response_len = sizeof(kResponse) - 1;
  std::size_t offset = 0;
  while (offset < response_len) {
    const ssize_t n = send(client_fd, kResponse + offset, response_len - offset, MSG_NOSIGNAL);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }

  return true;
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);

  const char* socket_path = std::getenv("UNIX_SOCKET_PATH");
  if (socket_path == nullptr || socket_path[0] == '\0') {
    std::fprintf(stderr, "UNIX_SOCKET_PATH is required and cannot be empty\n");
    return 1;
  }

  const int server_fd = create_unix_server(socket_path);
  if (server_fd < 0) {
    std::fprintf(stderr, "failed to bind unix socket '%s': %s\n", socket_path, std::strerror(errno));
    return 1;
  }

  std::fprintf(stderr, "server5 (c++) listening for fd passing at %s\n", socket_path);

  for (;;) {
    const int control_fd = accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (control_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      continue;
    }

    for (;;) {
      const int client_fd = recv_client_fd(control_fd);
      if (client_fd <= 0) {
        break;
      }

      if (read_full_request(client_fd)) {
        (void)send_response(client_fd);
      }
      close_fd(client_fd);
    }

    close_fd(control_fd);
  }
}
