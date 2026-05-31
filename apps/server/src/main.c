#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/syscall.h>
#else
#error "apps/server requires Linux (epoll)"
#endif
#include <unistd.h>

#include "responses.h"
#include "server.h"
#include "transaction_context.h"
#include "x-score.h"

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (sizeof(struct cmsghdr) + (len))
#endif

static volatile sig_atomic_t keep_running = 1;
enum { REQUEST_BUFFER_SIZE = 8192, MAX_CTRL_CONNS = 32, MAX_EVENTS = 128, WAIT_TIMEOUT_MS = 250 };
static const char REQ_GET_READY[] = "GET /ready ";
static const char REQ_POST_FRAUD_SCORE[] = "POST /fraud-score ";

static int find_ctrl_fd(const int ctrl_fds[MAX_CTRL_CONNS], int ctrl_count, int fd) {
  for (int i = 0; i < ctrl_count; i++) {
    if (ctrl_fds[i] == fd) {
      return i;
    }
  }
  return -1;
}

static void remove_ctrl_fd(int ctrl_fds[MAX_CTRL_CONNS], int *ctrl_count, int index) {
  if (ctrl_count == NULL || index < 0 || index >= *ctrl_count) {
    return;
  }

  int last = *ctrl_count - 1;
  ctrl_fds[index] = ctrl_fds[last];
  *ctrl_count = last;
}

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

static int set_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }

  if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
    return -1;
  }

  return 0;
}

static int set_nonblocking_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }

  flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return -1;
  }

  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    return -1;
  }

  return 0;
}

static ssize_t recvmsg_fast(int unix_sock, struct msghdr *msg) {
#if defined(__x86_64__) && defined(__linux__)
  register long rax __asm__("rax") = SYS_recvmsg;
  register long rdi __asm__("rdi") = (long)unix_sock;
  register long rsi __asm__("rsi") = (long)msg;
  register long rdx __asm__("rdx") = 0;
  __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
  if (rax < 0) {
    errno = (int)(-rax);
    return -1;
  }
  return (ssize_t)rax;
#elif defined(__aarch64__) && defined(__linux__)
  register long x8 __asm__("x8") = SYS_recvmsg;
  register long x0 __asm__("x0") = (long)unix_sock;
  register long x1 __asm__("x1") = (long)msg;
  register long x2 __asm__("x2") = 0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "cc", "memory");
  if (x0 < 0) {
    errno = (int)(-x0);
    return -1;
  }
  return (ssize_t)x0;
#else
  return recvmsg(unix_sock, msg, 0);
#endif
}

static int recv_fd_nonblocking(int unix_sock, int *out_client_fd) {
  if (out_client_fd == NULL) {
    errno = EINVAL;
    return -1;
  }

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));

  char buf[1];
  struct iovec io = {
    .iov_base = buf,
    .iov_len = sizeof(buf),
  };
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  ssize_t got = recvmsg_fast(unix_sock, &msg);
  if (got > 0) {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
      errno = EBADMSG;
      return -1;
    }

    int client_fd = -1;
    memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(int));
    *out_client_fd = client_fd;
    return 1;
  }

  if (got == 0) {
    return -1;
  }

  if (errno == EINTR) {
    return 0;
  }

  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return 0;
  }

  return -1;
}

static void handle_client_request(int client_fd, const XScoreIndexView *xscore) {
  if (set_blocking(client_fd) < 0) {
    close(client_fd);
    return;
  }

  char request[REQUEST_BUFFER_SIZE];
  HttpRequestView req_view;
  ssize_t nread = read_http_request(client_fd, request, sizeof(request), &req_view);
  if (nread <= 0) {
    close(client_fd);
    return;
  }

  request[nread] = '\0';

  if (memcmp(request, REQ_GET_READY, sizeof(REQ_GET_READY) - 1) == 0) {
    (void)write(client_fd, RESPONSE_READY.data, RESPONSE_READY.len);
    close(client_fd);
    return;
  }

  if (memcmp(request, REQ_POST_FRAUD_SCORE, sizeof(REQ_POST_FRAUD_SCORE) - 1) == 0) {
    if (req_view.body_offset > (size_t)nread ||
        req_view.body_len > ((size_t)nread - req_view.body_offset)) {
      (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
      close(client_fd);
      return;
    }

    const char *body = request + req_view.body_offset;
    const size_t body_len = req_view.body_len;

    TransactionContext ctx = transaction_context_from_body(body, body_len);
    if (ctx.id[0] == '\0') {
      ctx.destroy(&ctx);
      (void)write(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
      close(client_fd);
      return;
    }

    double vector[14];
    ctx.to_vector(&ctx, vector);
    ctx.destroy(&ctx);

    uint8_t fraud_count = x_score_predict_fraud_count(xscore, vector);
    // uint8_t fraud_count = 0;
    const Response *resp = &RESPONSE_FRAUD_10;
    switch (fraud_count) {
    case 0:
      resp = &RESPONSE_FRAUD_00;
      break;
    case 1:
      resp = &RESPONSE_FRAUD_02;
      break;
    case 2:
      resp = &RESPONSE_FRAUD_04;
      break;
    case 3:
      resp = &RESPONSE_FRAUD_06;
      break;
    case 4:
      resp = &RESPONSE_FRAUD_08;
      break;
    default:
      resp = &RESPONSE_FRAUD_10;
      break;
    }

    (void)write(client_fd, resp->data, resp->len);
    close(client_fd);
    return;
  }

  (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
  close(client_fd);
}

static void warm_up(const XScoreIndexView *xscore) {
  static const char warmup_body[] =
    "{\"id\":\"tx-warmup\","
    "\"transaction\":{\"amount\":384.88,\"installments\":3,\"requested_at\":"
    "\"2026-03-11T20:23:35Z\"},"
    "\"customer\":{\"avg_amount\":769.76,\"tx_count_24h\":3,\"known_"
    "merchants\":[\"MERC-009\",\"MERC-001\",\"MERC-001\"]},"
    "\"merchant\":{\"id\":\"MERC-001\",\"mcc\":\"5912\",\"avg_amount\":298."
    "95},"
    "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_"
    "home\":13.7090520965},"
    "\"last_transaction\":{\"timestamp\":\"2026-03-11T14:58:35Z\",\"km_from_"
    "current\":18.8626479774}}";

  TransactionContext ctx = transaction_context_from_body(warmup_body, sizeof(warmup_body) - 1);
  if (ctx.id[0] != '\0') {
    double vector[14];
    volatile uint8_t warmup_sink = 0;
    ctx.to_vector(&ctx, vector);
    ctx.destroy(&ctx);

    // Heat parser/vector path and touch x-score pages ahead of first real
    // requests.
    for (int i = 0; i < 32; i++) {
      warmup_sink ^= x_score_predict_fraud_count(xscore, vector);
    }
    (void)warmup_sink;
  }
}

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  const char *socket_path = getenv("UNIX_SOCKET_PATH");
  if (socket_path == NULL || *socket_path == '\0') {
    fprintf(stderr, "UNIX_SOCKET_PATH is required and cannot be empty\n");
    return 1;
  }

  const char *xscore_path = getenv("X_SCORE_INDEX_PATH");
  if (xscore_path == NULL || *xscore_path == '\0') {
    fprintf(stderr, "X_SCORE_INDEX_PATH is required and cannot be empty\n");
    return 1;
  }

  XScoreIndexView xscore;
  if (!x_score_open(xscore_path, &xscore)) {
    fprintf(stderr, "failed to load x-score index from resources/references.idx\n");
    return 1;
  }
  warm_up(&xscore);

  int server_fd = create_unix_server(socket_path);
  if (server_fd < 0) {
    fprintf(stderr, "failed to bind unix socket '%s': %s\n", socket_path, strerror(errno));
    return 1;
  }
  if (set_nonblocking_cloexec(server_fd) < 0) {
    fprintf(stderr, "failed to configure unix socket '%s' as non-blocking: %s\n", socket_path,
            strerror(errno));
    close(server_fd);
    x_score_close(&xscore);
    return 1;
  }

  fprintf(stderr, "listening for fd passing at %s\n", socket_path);

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    fprintf(stderr, "failed to create epoll: %s\n", strerror(errno));
    close(server_fd);
    x_score_close(&xscore);
    return 1;
  }

  struct epoll_event listener_event;
  memset(&listener_event, 0, sizeof(listener_event));
  listener_event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  listener_event.data.fd = server_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listener_event) < 0) {
    fprintf(stderr, "failed to register unix listener on epoll: %s\n", strerror(errno));
    close(epoll_fd);
    close(server_fd);
    x_score_close(&xscore);
    return 1;
  }

  struct epoll_event events[MAX_EVENTS];
  int ctrl_fds[MAX_CTRL_CONNS];
  int ctrl_count = 0;
  memset(ctrl_fds, 0, sizeof(ctrl_fds));

  while (keep_running) {
    int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, WAIT_TIMEOUT_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (int i = 0; i < ready; i++) {
      int fd = events[i].data.fd;
      uint32_t ev = events[i].events;

      if (fd == server_fd) {
        if ((ev & (EPOLLERR | EPOLLHUP)) != 0) {
          keep_running = 0;
          break;
        }

        while (keep_running) {
          int ctrl_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (ctrl_fd >= 0) {
            if (ctrl_count >= MAX_CTRL_CONNS) {
              close(ctrl_fd);
              continue;
            }

            struct epoll_event ctrl_event;
            memset(&ctrl_event, 0, sizeof(ctrl_event));
            ctrl_event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
            ctrl_event.data.fd = ctrl_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ctrl_event) < 0) {
              close(ctrl_fd);
              continue;
            }

            ctrl_fds[ctrl_count] = ctrl_fd;
            ctrl_count++;
            continue;
          }

          if (errno == EINTR) {
            continue;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          keep_running = 0;
          break;
        }
        continue;
      }

      int ctrl_index = find_ctrl_fd(ctrl_fds, ctrl_count, fd);
      if (ctrl_index < 0) {
        continue;
      }

      if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        remove_ctrl_fd(ctrl_fds, &ctrl_count, ctrl_index);
        continue;
      }

      if ((ev & EPOLLIN) != 0) {
        for (;;) {
          int client_fd = -1;
          int recv_status = recv_fd_nonblocking(fd, &client_fd);
          if (recv_status < 0) {
            (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            remove_ctrl_fd(ctrl_fds, &ctrl_count, ctrl_index);
            break;
          }
          if (recv_status == 0) {
            break;
          }

          handle_client_request(client_fd, &xscore);
          if (!keep_running) {
            break;
          }
        }
        if (!keep_running) {
          continue;
        }
      }
    }
  }

  for (int i = 0; i < ctrl_count; i++) {
    if (ctrl_fds[i] >= 0) {
      close(ctrl_fds[i]);
    }
  }
  close(epoll_fd);
  close(server_fd);
  unlink(socket_path);
  x_score_close(&xscore);
  return 0;
}
