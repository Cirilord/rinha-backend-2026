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
#include <pthread.h>
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
#define MAX_CLIENTS 512
#define WORKER_COUNT 2
#define MAX_CLIENTS_PER_WORKER (MAX_CLIENTS / WORKER_COUNT)
#define PENDING_QUEUE_CAP 4096
#define WORKER_STACK_SIZE (1024 * 1024)
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
  FD_NOTIFY = 3,
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

struct worker_ctx {
  int epoll_fd;
  int notify_rfd;
  int notify_wfd;
  pthread_mutex_t queue_mu;
  int pending_fds[PENDING_QUEUE_CAP];
  size_t pending_head;
  size_t pending_len;
  struct tracked_fd fd_table[MAX_TRACKED_FDS];
  struct client_conn clients[MAX_CLIENTS_PER_WORKER];
};

enum {
  PARSE_NEED = 0,
  PARSE_BAD = 1,
  PARSE_GOT = 2,
  PARSE_NOT_FOUND = 3,
  PARSE_READY = 4,
};

static XScoreIndexView xscore;
static int socket_busy_poll_cached = -1;

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

static void set_busy_poll(int fd) {
#ifdef __linux__
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
  if (socket_busy_poll_cached < 0) {
    socket_busy_poll_cached = getenv_int("SO_BUSY_POLL_US", 64);
  }
  if (socket_busy_poll_cached > 0) {
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &socket_busy_poll_cached,
               sizeof(socket_busy_poll_cached));
  }
#else
  (void)fd;
#endif
}

static unsigned long iow(unsigned int ty, unsigned int nr, unsigned int size) {
  return (unsigned long)((1U << 30) | (size << 16) | (ty << 8) | nr);
}

static void configure_busy_poll(int epoll_fd) {
#ifdef __linux__
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
#else
  (void)epoll_fd;
#endif
}

static int wait_events(int epoll_fd, struct epoll_event *events, int max_events) {
#ifdef __linux__
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
#else
  return epoll_wait(epoll_fd, events, max_events, -1);
#endif
}

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

static void update_interest(struct worker_ctx *worker, int fd, int want_write) {
  int index = (fd >= 0 && fd < MAX_TRACKED_FDS) ? worker->fd_table[fd].index : -1;
  if (index >= 0 && index < MAX_CLIENTS_PER_WORKER && worker->clients[index].active) {
    if (worker->clients[index].want_write == (uint8_t)want_write) {
      return;
    }
    worker->clients[index].want_write = (uint8_t)want_write;
  }

  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  if (want_write) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fd;

  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
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

static void close_client(struct worker_ctx *worker, int index) {
  struct client_conn *client = &worker->clients[index];
  int fd = client->fd;
  if (fd >= 0) {
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    if (fd < MAX_TRACKED_FDS) {
      worker->fd_table[fd].kind = FD_NONE;
      worker->fd_table[fd].index = -1;
    }
    close(fd);
  }

  memset(client, 0, sizeof(*client));
  client->fd = -1;
}

static int alloc_client_from_worker(struct worker_ctx *worker) {
  for (int i = 0; i < MAX_CLIENTS_PER_WORKER; i++) {
    if (!worker->clients[i].active) {
      memset(&worker->clients[i], 0, sizeof(worker->clients[i]));
      worker->clients[i].active = 1;
      worker->clients[i].fd = -1;
      return i;
    }
  }

  return -1;
}

static int register_client(struct worker_ctx *worker, int client_fd) {
  int index = alloc_client_from_worker(worker);
  if (index < 0) {
    close(client_fd);
    return -1;
  }

  worker->clients[index].fd = client_fd;
  reset_client(&worker->clients[index]);
  set_quickack(client_fd);
  set_busy_poll(client_fd);

  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  event.data.fd = client_fd;

  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0) {
    close_client(worker, index);
    return -1;
  }

  if (client_fd < MAX_TRACKED_FDS) {
    worker->fd_table[client_fd].kind = FD_CLIENT;
    worker->fd_table[client_fd].index = index;
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

static int process_requests(struct worker_ctx *worker, int index) {
  struct client_conn *client = &worker->clients[index];

  for (;;) {
    if (client->write_pos < client->write_len) {
      int flushed = flush_write(client);
      if (flushed < 0) {
        return -1;
      }
      if (flushed == 0) {
        update_interest(worker, client->fd, 1);
        return 0;
      }
      update_interest(worker, client->fd, 0);
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
      update_interest(worker, client->fd, 1);
      return 0;
    }
  }
}

static int handle_client_read(struct worker_ctx *worker, int index) {
  struct client_conn *client = &worker->clients[index];

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

  return process_requests(worker, index);
}

static int handle_client_write(struct worker_ctx *worker, int index) {
  return process_requests(worker, index);
}

static int enqueue_client_fd(struct worker_ctx *worker, int client_fd) {
  int notify = 0;

  pthread_mutex_lock(&worker->queue_mu);
  if (worker->pending_len >= PENDING_QUEUE_CAP) {
    pthread_mutex_unlock(&worker->queue_mu);
    return -1;
  }

  notify = (worker->pending_len == 0);
  worker->pending_fds[(worker->pending_head + worker->pending_len) % PENDING_QUEUE_CAP] = client_fd;
  worker->pending_len++;
  pthread_mutex_unlock(&worker->queue_mu);

  if (notify) {
    uint8_t token = 1;
    ssize_t written;
    do {
      written = write(worker->notify_wfd, &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
  }

  return 0;
}

static int dequeue_client_fd(struct worker_ctx *worker) {
  int client_fd = -1;

  pthread_mutex_lock(&worker->queue_mu);
  if (worker->pending_len > 0) {
    client_fd = worker->pending_fds[worker->pending_head];
    worker->pending_head = (worker->pending_head + 1) % PENDING_QUEUE_CAP;
    worker->pending_len--;
  }
  pthread_mutex_unlock(&worker->queue_mu);

  return client_fd;
}

static void handle_notify_fd(struct worker_ctx *worker) {
  uint8_t buf[64];

  for (;;) {
    ssize_t n = read(worker->notify_rfd, buf, sizeof(buf));
    if (n > 0) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }

  for (;;) {
    int client_fd = dequeue_client_fd(worker);
    if (client_fd < 0) {
      break;
    }

    int index = register_client(worker, client_fd);
    if (index >= 0) {
      if (process_requests(worker, index) != 0) {
        close_client(worker, index);
      }
    }
  }
}

static void *worker_main(void *arg) {
  struct worker_ctx *worker = (struct worker_ctx *)arg;
  struct epoll_event events[MAX_EVENTS];

  for (;;) {
    int ready = wait_events(worker->epoll_fd, events, MAX_EVENTS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fatal("epoll_wait");
    }

    for (int i = 0; i < ready; i++) {
      int fd = events[i].data.fd;
      if (fd == worker->notify_rfd) {
        handle_notify_fd(worker);
        continue;
      }

      if (fd < 0 || fd >= MAX_TRACKED_FDS) {
        continue;
      }
      if (worker->fd_table[fd].kind != FD_CLIENT) {
        continue;
      }

      int index = worker->fd_table[fd].index;
      if (index < 0 || index >= MAX_CLIENTS_PER_WORKER || !worker->clients[index].active) {
        continue;
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        close_client(worker, index);
        continue;
      }

      if ((events[i].events & EPOLLIN) && handle_client_read(worker, index) != 0) {
        close_client(worker, index);
        continue;
      }

      if ((events[i].events & EPOLLOUT) && handle_client_write(worker, index) != 0) {
        close_client(worker, index);
      }
    }
  }
}

static void init_worker(struct worker_ctx *worker) {
  int pipefd[2];
  struct epoll_event event;

  memset(worker, 0, sizeof(*worker));
  for (int i = 0; i < MAX_CLIENTS_PER_WORKER; i++) {
    worker->clients[i].fd = -1;
  }
  for (int i = 0; i < MAX_TRACKED_FDS; i++) {
    worker->fd_table[i].index = -1;
  }

  if (pthread_mutex_init(&worker->queue_mu, NULL) != 0) {
    fatal("pthread_mutex_init");
  }

  if (pipe(pipefd) != 0) {
    fatal("pipe");
  }
  worker->notify_rfd = pipefd[0];
  worker->notify_wfd = pipefd[1];
  if (set_nonblocking(worker->notify_rfd) < 0 || set_nonblocking(worker->notify_wfd) < 0) {
    fatal("set_nonblocking");
  }

  worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (worker->epoll_fd < 0) {
    fatal("epoll_create1");
  }
  configure_busy_poll(worker->epoll_fd);

  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  event.data.fd = worker->notify_rfd;
  if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->notify_rfd, &event) < 0) {
    fatal("epoll_ctl add notify");
  }

  if (worker->notify_rfd < MAX_TRACKED_FDS) {
    worker->fd_table[worker->notify_rfd].kind = FD_NOTIFY;
    worker->fd_table[worker->notify_rfd].index = -1;
  }
}

int main(int argc, char **argv) {
  const char *socket_path = "/tmp/server.sock";
  if (argc > 1 && argv[1][0] != '\0') {
    socket_path = argv[1];
  }

  if (!x_score_open("/resources/kdtree.bin", &xscore)) {
    fatal("x_score_open");
  }

  struct worker_ctx *workers =
    (struct worker_ctx *)calloc((size_t)WORKER_COUNT, sizeof(struct worker_ctx));
  pthread_t worker_threads[WORKER_COUNT];
  pthread_attr_t attr;

  if (workers == NULL) {
    fatal("calloc workers");
  }

  for (int i = 0; i < WORKER_COUNT; i++) {
    init_worker(&workers[i]);
  }
  if (pthread_attr_init(&attr) != 0) {
    fatal("pthread_attr_init");
  }
  if (pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE) != 0) {
    fatal("pthread_attr_setstacksize");
  }
  for (int i = 0; i < WORKER_COUNT; i++) {
    if (pthread_create(&worker_threads[i], &attr, worker_main, &workers[i]) != 0) {
      fatal("pthread_create");
    }
  }
  pthread_attr_destroy(&attr);

  int listener_fd = create_listener(socket_path, 64);
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
  set_busy_poll(control_fd);
  {
    int buf = 256 * 1024;
    setsockopt(control_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(control_fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  }

  int next_worker = 0;
  for (;;) {
    int client_fd = recv_fd(control_fd);
    if (client_fd == -2) {
      struct pollfd pfd;
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = control_fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
        fatal("poll");
      }
      continue;
    }
    if (client_fd < 0) {
      if (client_fd == -1 && errno == EINTR) {
        continue;
      }
      fatal("recv_fd");
    }

    if (enqueue_client_fd(&workers[next_worker], client_fd) != 0) {
      close(client_fd);
    }
    next_worker = (next_worker + 1) % WORKER_COUNT;
  }
}
