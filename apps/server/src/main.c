#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
enum { REQUEST_BUFFER_SIZE = 8192, MAX_CTRL_CONNS = 32 };

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

static int recv_fd(int unix_sock) {
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

  if (recvmsg(unix_sock, &msg, 0) <= 0) {
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
    return -1;
  }

  int client_fd = -1;
  memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(int));
  return client_fd;
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

  TransactionContext ctx = transaction_context_from_body_n(warmup_body, sizeof(warmup_body) - 1);
  if (ctx.id != NULL) {
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

  fprintf(stderr, "listening for fd passing at %s\n", socket_path);

  struct pollfd pfds[1 + MAX_CTRL_CONNS];
  int ctrl_count = 0;
  memset(pfds, 0, sizeof(pfds));
  pfds[0].fd = server_fd;
  pfds[0].events = POLLIN;

  while (keep_running) {
    int ready = poll(pfds, 1 + ctrl_count, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if ((pfds[0].revents & POLLIN) != 0 && ctrl_count < MAX_CTRL_CONNS) {
      int ctrl_fd = accept(server_fd, NULL, NULL);
      if (ctrl_fd >= 0) {
        pfds[1 + ctrl_count].fd = ctrl_fd;
        pfds[1 + ctrl_count].events = POLLIN;
        pfds[1 + ctrl_count].revents = 0;
        ctrl_count++;
      }
    }

    for (int i = 0; i < ctrl_count; i++) {
      struct pollfd *p = &pfds[1 + i];
      if ((p->revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }

      int client_fd = recv_fd(p->fd);
      if (client_fd < 0) {
        close(p->fd);
        pfds[1 + i] = pfds[ctrl_count];
        ctrl_count--;
        i--;
        continue;
      }

      char request[REQUEST_BUFFER_SIZE];
      ssize_t nread = read_http_request(client_fd, request, sizeof(request));
      if (nread <= 0) {
        close(client_fd);
        continue;
      }

      request[nread] = '\0';

      char method[16];
      if (!get_method(request, method, sizeof(method))) {
        (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
        close(client_fd);
        continue;
      }

      char pathname[256];
      if (!get_pathname(request, pathname, sizeof(pathname))) {
        (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
        close(client_fd);
        continue;
      }

      if (strcmp(method, "GET") == 0 && strcmp(pathname, "/ready") == 0) {
        (void)write(client_fd, RESPONSE_READY.data, RESPONSE_READY.len);
        close(client_fd);
        continue;
      }

      if (strcmp(method, "POST") == 0 && strcmp(pathname, "/fraud-score") == 0) {
        const char *body = NULL;
        size_t body_len = 0;
        if (!get_body(request, (size_t)nread, &body, &body_len)) {
          (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
          close(client_fd);
          continue;
        }

        TransactionContext ctx = transaction_context_from_body_n(body, body_len);
        if (ctx.id == NULL) {
          ctx.destroy(&ctx);
          (void)write(client_fd, RESPONSE_BAD_REQUEST.data, RESPONSE_BAD_REQUEST.len);
          close(client_fd);
          continue;
        }

        double vector[14];
        ctx.to_vector(&ctx, vector);
        ctx.destroy(&ctx);

        uint8_t fraud_count = x_score_predict_fraud_count(&xscore, vector);
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
        continue;
      }

      (void)write(client_fd, RESPONSE_NOT_FOUND.data, RESPONSE_NOT_FOUND.len);
      close(client_fd);
    }
  }

  for (int i = 0; i < ctrl_count; i++) {
    close(pfds[1 + i].fd);
  }
  close(server_fd);
  unlink(socket_path);
  x_score_close(&xscore);
  return 0;
}
