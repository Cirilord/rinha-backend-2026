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
#if defined(__linux__)
#include <sys/epoll.h>
#else
#include "../../../packages/mocks/sys/epoll.h"
#include "../../../packages/mocks/sys/socket.h"
#endif
#include <sys/socket.h>
#include <unistd.h>

#include "responses.h"
#include "server.h"
#include "transaction_context.h"
#include "x-score.h"

#ifndef CMSG_LEN
#define CMSG_LEN(len) (sizeof(struct cmsghdr) + (len))
#endif
#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))
#endif
#define MAX_CTRL_CONNS 16
#define MAX_CLIENT_CONNS 1024
#define MAX_EVENTS (1 + MAX_CTRL_CONNS + MAX_CLIENT_CONNS)
#define REQUEST_BUFFER_SIZE 8192

typedef struct {
  int fd;
  size_t used;
  size_t expected_total;
  size_t request_len;
  const char *response_data;
  size_t response_len;
  size_t response_offset;
  char request[REQUEST_BUFFER_SIZE];
} ClientConn;

static volatile sig_atomic_t keep_running = 1;
static const char REQ_GET_READY[] = "GET /ready ";
static const char REQ_POST_FRAUD_SCORE[] = "POST /fraud-score ";

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

  ssize_t n = recvmsg(unix_sock, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0) {
    return -1;
  }
  if (n == 0) {
    errno = ECONNRESET;
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
    errno = EBADMSG;
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
    x_score_close(&xscore);
    return 1;
  }

  fprintf(stderr, "listening for fd passing at %s\n", socket_path);

  int *ctrl_fds = calloc(MAX_CTRL_CONNS, sizeof(*ctrl_fds));
  ClientConn *client_conns = calloc(MAX_CLIENT_CONNS, sizeof(*client_conns));
  struct epoll_event *events = calloc(MAX_EVENTS, sizeof(*events));
  if (ctrl_fds == NULL || client_conns == NULL || events == NULL) {
    free(events);
    free(client_conns);
    free(ctrl_fds);
    close(server_fd);
    unlink(socket_path);
    x_score_close(&xscore);
    return 1;
  }

  for (int i = 0; i < MAX_CTRL_CONNS; i++) {
    ctrl_fds[i] = -1;
  }
  for (int i = 0; i < MAX_CLIENT_CONNS; i++) {
    client_conns[i].fd = -1;
  }

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    free(events);
    free(client_conns);
    free(ctrl_fds);
    close(server_fd);
    unlink(socket_path);
    x_score_close(&xscore);
    return 1;
  }

  struct epoll_event server_ev;
  memset(&server_ev, 0, sizeof(server_ev));
  server_ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  server_ev.data.fd = server_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &server_ev) < 0) {
    close(epoll_fd);
    free(events);
    free(client_conns);
    free(ctrl_fds);
    close(server_fd);
    unlink(socket_path);
    x_score_close(&xscore);
    return 1;
  }

  while (keep_running) {
    int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (int ev_idx = 0; ev_idx < ready; ev_idx++) {
      int fd = events[ev_idx].data.fd;
      uint32_t ev = events[ev_idx].events;

      if (fd == server_fd) {
        while (keep_running) {
          int ctrl_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (ctrl_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            if (errno == EINTR) {
              continue;
            }
            break;
          }

          int ctrl_slot = -1;
          for (int i = 0; i < MAX_CTRL_CONNS; i++) {
            if (ctrl_fds[i] < 0) {
              ctrl_slot = i;
              break;
            }
          }
          if (ctrl_slot < 0) {
            close(ctrl_fd);
            continue;
          }

          struct epoll_event ctrl_ev;
          memset(&ctrl_ev, 0, sizeof(ctrl_ev));
          ctrl_ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
          ctrl_ev.data.fd = ctrl_fd;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ctrl_ev) < 0) {
            close(ctrl_fd);
            continue;
          }

          ctrl_fds[ctrl_slot] = ctrl_fd;
        }
        continue;
      }

      int ctrl_slot = -1;
      for (int i = 0; i < MAX_CTRL_CONNS; i++) {
        if (ctrl_fds[i] == fd) {
          ctrl_slot = i;
          break;
        }
      }
      if (ctrl_slot >= 0) {
        while (keep_running) {
          int client_fd = recv_fd(fd);
          if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            if (errno == EINTR) {
              continue;
            }
            (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            ctrl_fds[ctrl_slot] = -1;
            break;
          }

          int status_flags = fcntl(client_fd, F_GETFL, 0);
          if (status_flags < 0 || fcntl(client_fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
            close(client_fd);
            continue;
          }

          int fd_flags = fcntl(client_fd, F_GETFD, 0);
          if (fd_flags < 0 || fcntl(client_fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
            close(client_fd);
            continue;
          }

          int client_slot = -1;
          for (int i = 0; i < MAX_CLIENT_CONNS; i++) {
            if (client_conns[i].fd < 0) {
              client_slot = i;
              break;
            }
          }
          if (client_slot < 0) {
            close(client_fd);
            continue;
          }

          memset(&client_conns[client_slot], 0, sizeof(client_conns[client_slot]));
          client_conns[client_slot].fd = client_fd;

          struct epoll_event client_ev;
          memset(&client_ev, 0, sizeof(client_ev));
          client_ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
          client_ev.data.fd = client_fd;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
            close(client_fd);
            client_conns[client_slot].fd = -1;
            continue;
          }
        }
        continue;
      }

      int client_slot = -1;
      for (int i = 0; i < MAX_CLIENT_CONNS; i++) {
        if (client_conns[i].fd == fd) {
          client_slot = i;
          break;
        }
      }
      if (client_slot < 0) {
        continue;
      }

      ClientConn *client = &client_conns[client_slot];

      if (client->response_data != NULL) {
        bool close_client = false;

        while (client->response_offset < client->response_len) {
          size_t remaining = client->response_len - client->response_offset;
          ssize_t nwritten =
            send(client->fd, client->response_data + client->response_offset, remaining,
                 MSG_NOSIGNAL | MSG_DONTWAIT);
          if (nwritten > 0) {
            client->response_offset += (size_t)nwritten;
            continue;
          }
          if (nwritten == 0) {
            close_client = true;
            break;
          }
          if (errno == EINTR) {
            continue;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          close_client = true;
          break;
        }

        if (close_client || client->response_offset >= client->response_len) {
          (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
          close(client->fd);
          memset(client, 0, sizeof(*client));
          client->fd = -1;
          continue;
        }

        struct epoll_event client_ev;
        memset(&client_ev, 0, sizeof(client_ev));
        client_ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
        client_ev.data.fd = client->fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &client_ev) < 0) {
          (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
          close(client->fd);
          memset(client, 0, sizeof(*client));
          client->fd = -1;
        }
        continue;
      }

      if ((ev & EPOLLIN) == 0) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
        memset(client, 0, sizeof(*client));
        client->fd = -1;
        continue;
      }

      size_t request_len = 0;
      HttpReadStatus read_status = read_http_request(client->fd, client->request, sizeof(client->request),
                                                     &client->used, &client->expected_total, &request_len);
      if (read_status == HTTP_READ_PENDING) {
        continue;
      }
      if (read_status != HTTP_READ_COMPLETE) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
        memset(client, 0, sizeof(*client));
        client->fd = -1;
        continue;
      }

      client->request_len = request_len;
      client->request[request_len] = '\0';

      const Response *resp = &RESPONSE_NOT_FOUND;
      if (memcmp(client->request, REQ_GET_READY, sizeof(REQ_GET_READY) - 1) == 0) {
        resp = &RESPONSE_READY;
      } else if (memcmp(client->request, REQ_POST_FRAUD_SCORE, sizeof(REQ_POST_FRAUD_SCORE) - 1) == 0) {
        const char *body = NULL;
        size_t body_len = 0;
        if (!get_body(client->request, request_len, &body, &body_len)) {
          resp = &RESPONSE_NOT_FOUND;
        } else {
          TransactionContext ctx = transaction_context_from_body(body, body_len);
          if (ctx.id[0] == '\0') {
            ctx.destroy(&ctx);
            resp = &RESPONSE_BAD_REQUEST;
          } else {
            double vector[14];
            ctx.to_vector(&ctx, vector);
            ctx.destroy(&ctx);

            // uint8_t fraud_count = x_score_predict_fraud_count(&xscore, vector);
            uint8_t fraud_count = 0;
            resp = &RESPONSE_FRAUD_10;
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
          }
        }
      }

      client->response_data = resp->data;
      client->response_len = resp->len;
      client->response_offset = 0;

      bool close_client = false;
      while (client->response_offset < client->response_len) {
        size_t remaining = client->response_len - client->response_offset;
        ssize_t nwritten =
          send(client->fd, client->response_data + client->response_offset, remaining,
               MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nwritten > 0) {
          client->response_offset += (size_t)nwritten;
          continue;
        }
        if (nwritten == 0) {
          close_client = true;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        close_client = true;
        break;
      }

      if (close_client || client->response_offset >= client->response_len) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
        memset(client, 0, sizeof(*client));
        client->fd = -1;
        continue;
      }

      struct epoll_event client_ev;
      memset(&client_ev, 0, sizeof(client_ev));
      client_ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
      client_ev.data.fd = client->fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &client_ev) < 0) {
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
        memset(client, 0, sizeof(*client));
        client->fd = -1;
      }
    }
  }

  for (int i = 0; i < MAX_CTRL_CONNS; i++) {
    if (ctrl_fds[i] >= 0) {
      close(ctrl_fds[i]);
    }
  }
  for (int i = 0; i < MAX_CLIENT_CONNS; i++) {
    if (client_conns[i].fd >= 0) {
      close(client_conns[i].fd);
    }
  }
  close(epoll_fd);
  close(server_fd);
  unlink(socket_path);
  x_score_close(&xscore);
  free(events);
  free(client_conns);
  free(ctrl_fds);
  return 0;
}
