#define _GNU_SOURCE

#ifdef __linux__
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#else
#include "../../../packages/mocks/netinet/in.h"
#include "../../../packages/mocks/netinet/tcp.h"
#include "../../../packages/mocks/sys/epoll.h"
#include "../../../packages/mocks/sys/socket.h"
#endif

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "listener.h"
#include "transaction_context.h"
#include "utils.h"
#include "x_score.h"

static const uint8_t RESPONSE_READY[] = "HTTP/1.1 200 OK\r\n"
                                        "Content-Length: 0\r\n"
                                        "\r\n";
static const uint8_t RESPONSE_404[] = "HTTP/1.1 404 Not Found\r\n"
                                      "Content-Length: 0\r\n"
                                      "\r\n";
static const char REQ_GET_READY[] = "GET /ready ";
static const char REQ_POST_FRAUD_SCORE[] = "POST /fraud-score ";

struct response {
  const uint8_t *data;
  size_t len;
};

#define RESPONSE(s) {(const uint8_t *)(s), sizeof(s) - 1}

static const struct response FRAUD_SCORE_RESPONSES[] = {
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 35\r\n"
           "\r\n"
           "{\"approved\":true,\"fraud_score\":0.0}"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 35\r\n"
           "\r\n"
           "{\"approved\":true,\"fraud_score\":0.2}"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 35\r\n"
           "\r\n"
           "{\"approved\":true,\"fraud_score\":0.4}"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 36\r\n"
           "\r\n"
           "{\"approved\":false,\"fraud_score\":0.6}"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 36\r\n"
           "\r\n"
           "{\"approved\":false,\"fraud_score\":0.8}"),
  RESPONSE("HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 36\r\n"
           "\r\n"
           "{\"approved\":false,\"fraud_score\":1.0}"),
};

#define MAX_REQ_HEAD 4096
#define MAX_BODY 4096
#define CONN_BUF_CAP 16384
#define WRITE_BUF_CAP 512
#define MAX_EVENTS 128
#define MAX_CLIENTS 2048
#define MAX_TRACKED_FDS 65536
struct epoll_params {
  uint32_t busy_poll_usecs;
  uint16_t busy_poll_budget;
  uint8_t prefer_busy_poll;
  uint8_t pad;
};

enum fd_kind {
  FD_NONE = 0,
  FD_CONTROL = 1,
  FD_CLIENT = 2,
};

struct tracked_fd {
  uint8_t kind;
  int index;
};

struct parse_result {
  uint8_t status;
  size_t consumed;
  size_t body_start;
  size_t body_len;
};

struct client_conn {
  int fd;
  uint8_t active;
  uint8_t want_write;
  size_t in_start;
  size_t in_end;
  size_t write_len;
  size_t write_pos;
  uint8_t in_buf[CONN_BUF_CAP];
  uint8_t write_buf[WRITE_BUF_CAP];
};

enum {
  PARSE_NEED = 0,
  PARSE_BAD = 1,
  PARSE_GOT = 2,
  PARSE_NOT_FOUND = 3,
  PARSE_READY = 4,
};

static struct tracked_fd fd_table[MAX_TRACKED_FDS];
static struct client_conn clients[MAX_CLIENTS];
static XScoreIndexView xscore;

static int set_nonblocking(int fd) {
#ifdef __linux__
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  (void)fd;
  return 0;
#endif
}

static void set_quickack(int fd) {
#ifdef __linux__
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#else
  (void)fd;
#endif
}

#ifdef __linux__
static int getenv_int(const char *name, int fallback) {
  const char *value = getenv(name);
  if (!value || !*value) {
    return fallback;
  }

  int parsed = atoi(value);
  return parsed >= 0 ? parsed : fallback;
}

static long getenv_long(const char *name, long fallback) {
  const char *value = getenv(name);
  if (!value || !*value) {
    return fallback;
  }

  long parsed = strtol(value, NULL, 10);
  return parsed >= 0 ? parsed : fallback;
}

static unsigned long iow(unsigned int ty, unsigned int nr, unsigned int size) {
  return (unsigned long)((1U << 30) | (size << 16) | (ty << 8) | nr);
}

static void configure_busy_poll(int epoll_fd) {
  struct epoll_params params;
  memset(&params, 0, sizeof(params));
  params.busy_poll_usecs = (uint32_t)getenv_int("EPOLL_BUSY_POLL_US", 100);
  params.busy_poll_budget = (uint16_t)getenv_int("EPOLL_BUSY_POLL_BUDGET", 8);
  params.prefer_busy_poll = (uint8_t)getenv_int("EPOLL_PREFER_BUSY_POLL", 1);
  if (params.busy_poll_usecs == 0 && params.prefer_busy_poll == 0) {
    return;
  }

  unsigned long ep_iocsparams = iow(0x8A, 0x01, (unsigned int)sizeof(params));
  ioctl(epoll_fd, ep_iocsparams, &params);
}

static int wait_events(int epoll_fd, struct epoll_event *events, int max_events) {
  long spin_us = getenv_long("EPOLL_SPIN_US", 0);
  int timeout_ms = getenv_int("EPOLL_TIMEOUT_MS", 1);

  int ready = 0;
  if (spin_us > 0) {
    ready = epoll_wait(epoll_fd, events, max_events, 0);
    if (ready == 0) {
      struct timespec start;
      clock_gettime(CLOCK_MONOTONIC, &start);
      for (;;) {
        ready = epoll_wait(epoll_fd, events, max_events, 0);
        if (ready != 0) {
          break;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_us =
          (now.tv_sec - start.tv_sec) * 1000000L + (now.tv_nsec - start.tv_nsec) / 1000L;
        if (elapsed_us >= spin_us) {
          break;
        }
      }
    }
  }

  if (ready != 0) {
    return ready;
  }

#ifdef SYS_epoll_pwait2
  long idle_us = getenv_long("EPOLL_IDLE_US", 60);
  if (idle_us > 0) {
    struct timespec timeout;
    timeout.tv_sec = idle_us / 1000000L;
    timeout.tv_nsec = (idle_us % 1000000L) * 1000L;
    ready = epoll_pwait2(epoll_fd, events, max_events, &timeout, NULL);
    if (ready >= 0 || errno != ENOSYS) {
      return ready;
    }
  }
#endif

  return epoll_wait(epoll_fd, events, max_events, timeout_ms);
}
#else
static void configure_busy_poll(int epoll_fd) { (void)epoll_fd; }

static int wait_events(int epoll_fd, struct epoll_event *events, int max_events) {
  return epoll_wait(epoll_fd, events, max_events, -1);
}
#endif

static ssize_t find_double_crlf(const uint8_t *buf, size_t len) {
  if (len < 4) {
    return -1;
  }

  for (size_t i = 3; i < len; i++) {
    if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') {
      return (ssize_t)(i - 3);
    }
  }

  return -1;
}

static size_t parse_content_length(const uint8_t *headers, size_t header_len) {
  static const uint8_t needle[] = "content-length:";
  const size_t needle_len = sizeof(needle) - 1;

  size_t i = 0;
  while (i + needle_len <= header_len) {
    size_t k = 0;
    while (k < needle_len) {
      uint8_t h = headers[i + k];
      if (h >= 'A' && h <= 'Z') {
        h = (uint8_t)(h + 32);
      }
      if (h != needle[k]) {
        break;
      }
      k++;
    }

    if (k != needle_len) {
      i++;
      continue;
    }

    size_t j = i + needle_len;
    while (j < header_len && (headers[j] == ' ' || headers[j] == '\t')) {
      j++;
    }

    size_t start = j;
    size_t value = 0;
    while (j < header_len && headers[j] >= '0' && headers[j] <= '9') {
      value = value * 10 + (size_t)(headers[j] - '0');
      j++;
    }

    if (j == start) {
      return SIZE_MAX;
    }

    return value;
  }

  return SIZE_MAX;
}

static void update_interest(int epoll_fd, int fd, int want_write) {
  int index = (fd >= 0 && fd < MAX_TRACKED_FDS) ? fd_table[fd].index : -1;
  if (index >= 0 && index < MAX_CLIENTS && clients[index].active) {
    if (clients[index].want_write == (uint8_t)want_write) {
      return;
    }
    clients[index].want_write = (uint8_t)want_write;
  }

  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  if (want_write) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
    fatal("epoll_ctl mod");
  }
}

static void reset_client(struct client_conn *client) {
  client->in_start = 0;
  client->in_end = 0;
  client->write_len = 0;
  client->write_pos = 0;
  client->want_write = 0;
}

static void consume_input(struct client_conn *client, size_t consumed) {
  size_t buffered = client->in_end - client->in_start;
  if (consumed >= buffered) {
    client->in_start = 0;
    client->in_end = 0;
    return;
  }

  client->in_start += consumed;
}

static void ensure_input_space(struct client_conn *client) {
  if (client->in_end < sizeof(client->in_buf)) {
    return;
  }

  if (client->in_start == 0) {
    return;
  }

  size_t buffered = client->in_end - client->in_start;
  memmove(client->in_buf, client->in_buf + client->in_start, buffered);
  client->in_start = 0;
  client->in_end = buffered;
}

static void close_client(int epoll_fd, int index) {
  struct client_conn *client = &clients[index];
  int fd = client->fd;
  if (fd >= 0) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    if (fd < MAX_TRACKED_FDS) {
      fd_table[fd].kind = FD_NONE;
      fd_table[fd].index = -1;
    }
    close(fd);
  }

  memset(client, 0, sizeof(*client));
  client->fd = -1;
}

static int alloc_client(void) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) {
      memset(&clients[i], 0, sizeof(clients[i]));
      clients[i].active = 1;
      clients[i].fd = -1;
      return i;
    }
  }

  return -1;
}

static int register_client(int epoll_fd, int client_fd) {
  int index = alloc_client();
  if (index < 0) {
    close(client_fd);
    return -1;
  }

  clients[index].fd = client_fd;
  reset_client(&clients[index]);
  set_quickack(client_fd);

  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  event.data.fd = client_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0) {
    close_client(epoll_fd, index);
    return -1;
  }

  if (client_fd < MAX_TRACKED_FDS) {
    fd_table[client_fd].kind = FD_CLIENT;
    fd_table[client_fd].index = index;
  }

  return index;
}

static int recv_fd(int socket_fd) {
  char payload = 0;
  struct iovec iov;
  iov.iov_base = &payload;
  iov.iov_len = sizeof(payload);

  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  ssize_t received = recvmsg(socket_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (received <= 0) {
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return -2;
    }
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
    return -1;
  }

  int client_fd = -1;
  memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(client_fd));
  return client_fd;
}

static struct parse_result parse_request(const uint8_t *buf, size_t len) {
  struct parse_result result;
  result.status = PARSE_NEED;
  result.consumed = 0;
  result.body_start = 0;
  result.body_len = 0;

  int is_ready = 0;
  int is_fraud_score = 0;

  if (len >= sizeof(REQ_GET_READY) - 1) {
    if (memcmp(buf, REQ_GET_READY, sizeof(REQ_GET_READY) - 1) == 0) {
      is_ready = 1;
    }
  }

  if (len >= sizeof(REQ_POST_FRAUD_SCORE) - 1) {
    if (memcmp(buf, REQ_POST_FRAUD_SCORE, sizeof(REQ_POST_FRAUD_SCORE) - 1) == 0) {
      is_fraud_score = 1;
    }
  }

  ssize_t head_end = find_double_crlf(buf, len);
  if (head_end < 0) {
    if (len > MAX_REQ_HEAD) {
      result.status = PARSE_BAD;
      result.consumed = len;
    }
    return result;
  }

  size_t header_end = (size_t)head_end;
  size_t body_start = header_end + 4;

  if (is_ready) {
    result.status = PARSE_READY;
    result.consumed = body_start;
    return result;
  }

  size_t content_length = parse_content_length(buf, header_end);

  if (!is_fraud_score) {
    if (content_length == SIZE_MAX) {
      result.status = PARSE_NOT_FOUND;
      result.consumed = body_start;
      return result;
    }

    if (content_length > MAX_BODY) {
      result.status = PARSE_BAD;
      result.consumed = len;
      return result;
    }

    if (len < body_start + content_length) {
      return result;
    }

    result.status = PARSE_NOT_FOUND;
    result.consumed = body_start + content_length;
    result.body_start = body_start;
    result.body_len = content_length;
    return result;
  }

  if (content_length == SIZE_MAX || content_length > MAX_BODY) {
    result.status = PARSE_BAD;
    result.consumed = len;
    return result;
  }

  if (len < body_start + content_length) {
    return result;
  }

  result.status = PARSE_GOT;
  result.consumed = body_start + content_length;
  result.body_start = body_start;
  result.body_len = content_length;
  return result;
}

static int write_all_nonblock(int fd, const uint8_t *buf, size_t len, size_t *written_total) {
  while (*written_total < len) {
    ssize_t written = send(fd, buf + *written_total, len - *written_total, MSG_NOSIGNAL);
    if (written > 0) {
      *written_total += (size_t)written;
      continue;
    }

    if (written == 0) {
      return -1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    }

    if (errno == EINTR) {
      continue;
    }

    return -1;
  }

  return 0;
}

static int flush_write(struct client_conn *client) {
  while (client->write_pos < client->write_len) {
    ssize_t written = send(client->fd, client->write_buf + client->write_pos,
                           client->write_len - client->write_pos, MSG_NOSIGNAL);
    if (written > 0) {
      client->write_pos += (size_t)written;
      continue;
    }

    if (written == 0) {
      return -1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }

    if (errno == EINTR) {
      continue;
    }

    return -1;
  }

  client->write_len = 0;
  client->write_pos = 0;
  return 1;
}

static int process_requests(int epoll_fd, int index) {
  struct client_conn *client = &clients[index];

  for (;;) {
    if (client->write_pos < client->write_len) {
      int flushed = flush_write(client);
      if (flushed < 0) {
        return -1;
      }
      if (flushed == 0) {
        update_interest(epoll_fd, client->fd, 1);
        return 0;
      }
      update_interest(epoll_fd, client->fd, 0);
    }

    size_t buffered = client->in_end - client->in_start;
    struct parse_result parsed = parse_request(client->in_buf + client->in_start, buffered);
    if (parsed.status == PARSE_NEED) {
      return 0;
    }

    if (parsed.status == PARSE_BAD) {
      return -1;
    }

    const uint8_t *response = FRAUD_SCORE_RESPONSES[0].data;
    size_t response_len = FRAUD_SCORE_RESPONSES[0].len;
    if (parsed.status == PARSE_GOT) {
      transaction_context ctx = transaction_context__from_body(
        (const char *)(client->in_buf + client->in_start + parsed.body_start), parsed.body_len);
      double vector[14];
      transaction_context__to_vector(&ctx, vector);
      transaction_context__destroy(&ctx);
      int fraud_count = x_score_predict_fraud_count(&xscore, vector);
      if (fraud_count < 0) {
        fraud_count = 0;
      } else if (fraud_count > 5) {
        fraud_count = 5;
      }
      response = FRAUD_SCORE_RESPONSES[fraud_count].data;
      response_len = FRAUD_SCORE_RESPONSES[fraud_count].len;
    } else if (parsed.status == PARSE_READY) {
      response = RESPONSE_READY;
      response_len = sizeof(RESPONSE_READY) - 1;
    } else if (parsed.status == PARSE_NOT_FOUND) {
      response = RESPONSE_404;
      response_len = sizeof(RESPONSE_404) - 1;
    }

    consume_input(client, parsed.consumed);

    size_t written_total = 0;
    int write_result = write_all_nonblock(client->fd, response, response_len, &written_total);
    if (write_result < 0) {
      return -1;
    }

    if (write_result > 0) {
      size_t remaining = response_len - written_total;
      if (remaining > sizeof(client->write_buf)) {
        return -1;
      }

      memcpy(client->write_buf, response + written_total, remaining);
      client->write_len = remaining;
      client->write_pos = 0;
      update_interest(epoll_fd, client->fd, 1);
      return 0;
    }
  }
}

static void handle_control_fd(int epoll_fd, int control_fd) {
  for (;;) {
    int client_fd = recv_fd(control_fd);
    if (client_fd == -2) {
      return;
    }

    if (client_fd < 0) {
      return;
    }

    int index = register_client(epoll_fd, client_fd);
    if (index >= 0) {
      if (process_requests(epoll_fd, index) != 0) {
        close_client(epoll_fd, index);
      }
    }
  }
}

static int handle_client_read(int epoll_fd, int index) {
  struct client_conn *client = &clients[index];

  for (;;) {
    ensure_input_space(client);

    if (client->in_end == sizeof(client->in_buf)) {
      return -1;
    }

    ssize_t received =
      recv(client->fd, client->in_buf + client->in_end, sizeof(client->in_buf) - client->in_end, 0);
    if (received > 0) {
      client->in_end += (size_t)received;
      break;
    }

    if (received == 0) {
      return -1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    if (errno == EINTR) {
      continue;
    }

    return -1;
  }

  return process_requests(epoll_fd, index);
}

static int handle_client_write(int epoll_fd, int index) {
  return process_requests(epoll_fd, index);
}

int main(int argc, char **argv) {
  const char *socket_path = "/tmp/server.sock";
  if (argc > 1 && argv[1][0] != '\0') {
    socket_path = argv[1];
  }

  if (!x_score_open("/resources/kdtree.bin", &xscore)) {
    fatal("x_score_open");
  }

  for (int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].fd = -1;
  }
  for (int i = 0; i < MAX_TRACKED_FDS; i++) {
    fd_table[i].index = -1;
  }

  int listener_fd = create_listener(socket_path, 4);
  int control_fd;
  for (;;) {
    control_fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
    if (control_fd >= 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      poll(NULL, 0, 1);
      continue;
    }
    fatal("accept4");
  }
  close(listener_fd);
  if (set_nonblocking(control_fd) < 0) {
    fatal("set_nonblocking");
  }

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    fatal("epoll_create1");
  }
  configure_busy_poll(epoll_fd);

  struct epoll_event control_event;
  memset(&control_event, 0, sizeof(control_event));
  control_event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  control_event.data.fd = control_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, control_fd, &control_event) < 0) {
    fatal("epoll_ctl add control");
  }

  if (control_fd < MAX_TRACKED_FDS) {
    fd_table[control_fd].kind = FD_CONTROL;
    fd_table[control_fd].index = -1;
  }

  struct epoll_event events[MAX_EVENTS];
  for (;;) {
    int ready = wait_events(epoll_fd, events, MAX_EVENTS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fatal("epoll_wait");
    }

    for (int i = 0; i < ready; i++) {
      int fd = events[i].data.fd;
      if (fd < 0 || fd >= MAX_TRACKED_FDS) {
        continue;
      }

      if (fd_table[fd].kind == FD_CONTROL) {
        handle_control_fd(epoll_fd, fd);
        continue;
      }

      if (fd_table[fd].kind != FD_CLIENT) {
        continue;
      }

      int index = fd_table[fd].index;
      if (index < 0 || index >= MAX_CLIENTS || !clients[index].active) {
        continue;
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        close_client(epoll_fd, index);
        continue;
      }

      if ((events[i].events & EPOLLIN) && handle_client_read(epoll_fd, index) != 0) {
        close_client(epoll_fd, index);
        continue;
      }

      if ((events[i].events & EPOLLOUT) && handle_client_write(epoll_fd, index) != 0) {
        close_client(epoll_fd, index);
      }
    }
  }
}
