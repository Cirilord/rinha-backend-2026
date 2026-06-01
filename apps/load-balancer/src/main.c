#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/syscall.h>
#else
#include "../../../packages/mocks/sys/epoll.h"
#include "../../../packages/mocks/sys/syscall.h"
#endif
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (sizeof(struct cmsghdr) + (len))
#endif

#define BACKLOG 2048
#define DEFAULT_PORT 9999
#define MAX_ENV_LEN 1024
#define MAX_SOCKET_PATH 108
#define MAX_WORKERS 16

static volatile sig_atomic_t keep_running = 1;

typedef struct {
  char path[MAX_SOCKET_PATH];
  int control_fd;
} worker_t;

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

static int connect_worker(const char *socket_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static int ensure_worker_connected(worker_t *worker) {
  if (worker->control_fd >= 0) {
    return 0;
  }

  int fd = connect_worker(worker->path);
  if (fd < 0) {
    return -1;
  }

  worker->control_fd = fd;
  return 0;
}

static int send_fd(int unix_sock, int fd_to_send) {
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));

  // Only a minimal control byte is sent; client payload stays on the passed FD.
  char dummy = '\0';
  struct iovec io = {
    .iov_base = &dummy,
    .iov_len = sizeof(dummy),
  };
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  char control[CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  *((int *)CMSG_DATA(cmsg)) = fd_to_send;

  ssize_t sent = -1;
#if defined(__x86_64__)
  register long rax __asm__("rax") = SYS_sendmsg;
  register long rdi __asm__("rdi") = (long)unix_sock;
  register long rsi __asm__("rsi") = (long)&msg;
  register long rdx __asm__("rdx") = 0;
  __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
  if (rax < 0) {
    errno = (int)(-rax);
    sent = -1;
  } else {
    sent = (ssize_t)rax;
  }
#elif defined(__aarch64__)
  register long x8 __asm__("x8") = SYS_sendmsg;
  register long x0 __asm__("x0") = (long)unix_sock;
  register long x1 __asm__("x1") = (long)&msg;
  register long x2 __asm__("x2") = 0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "cc", "memory");
  if (x0 < 0) {
    errno = (int)(-x0);
    sent = -1;
  } else {
    sent = (ssize_t)x0;
  }
#endif
  if (sent < 0) {
    return -1;
  }

  return 0;
}

static int create_tcp_listener(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, BACKLOG) < 0) {
    close(fd);
    return -1;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  const char *port_str = getenv("PORT");
  if (port_str == NULL || *port_str == '\0') {
    fprintf(stderr, "ERROR: PORT is required and cannot be empty\n");
    return 1;
  }
  char *end = NULL;
  int port = (int)strtol(port_str, &end, 10);
  if (*end != '\0' || port < 1 || port > 65535) {
    fprintf(stderr, "ERROR: invalid PORT='%s'\n", port_str);
    return 1;
  }

  const char *worker_sockets = getenv("WORKER_SOCKETS");
  if (worker_sockets == NULL || *worker_sockets == '\0') {
    fprintf(stderr, "ERROR: WORKER_SOCKETS is required and cannot be empty\n");
    return 1;
  }

  worker_t workers[MAX_WORKERS];
  char buffer[MAX_ENV_LEN];
  strncpy(buffer, worker_sockets, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  int worker_count = 0;
  char *token = strtok(buffer, ",");
  while (token != NULL && worker_count < MAX_WORKERS) {
    while (*token == ' ') {
      token++;
    }

    size_t len = strnlen(token, MAX_SOCKET_PATH);
    if (len == 0 || len >= MAX_SOCKET_PATH) {
      fprintf(stderr, "WARN: skipping invalid worker socket path: '%s'\n", token);
    } else {
      strncpy(workers[worker_count].path, token, sizeof(workers[worker_count].path) - 1);
      workers[worker_count].path[sizeof(workers[worker_count].path) - 1] = '\0';
      workers[worker_count].control_fd = -1;
      worker_count++;
    }

    token = strtok(NULL, ",");
  }

  int listener = create_tcp_listener(port);
  if (listener < 0) {
    fprintf(stderr, "ERROR: failed to create TCP listener on port %d: %s\n", port, strerror(errno));
    return 1;
  }

  fprintf(stderr, "INFO: listening on 0.0.0.0:%d\n", port);
  for (int i = 0; i < worker_count; i++) {
    fprintf(stderr, "INFO: worker[%d] socket path: %s\n", i, workers[i].path);
  }

  int rr = 0;
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    fprintf(stderr, "ERROR: failed to create epoll instance: %s\n", strerror(errno));
    close(listener);
    return 1;
  }

  struct epoll_event listener_ev;
  memset(&listener_ev, 0, sizeof(listener_ev));
  listener_ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  listener_ev.data.fd = listener;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &listener_ev) < 0) {
    fprintf(stderr, "ERROR: failed to register listener on epoll: %s\n", strerror(errno));
    close(epoll_fd);
    close(listener);
    return 1;
  }

  while (keep_running) {
    struct epoll_event ev;
    int nready = epoll_wait(epoll_fd, &ev, 1, -1);
    if (nready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "WARN: epoll_wait failed: %s\n", strerror(errno));
      break;
    }

    if (nready == 0 || ev.data.fd != listener ||
        (ev.events & (EPOLLIN | EPOLLERR | EPOLLHUP)) == 0) {
      continue;
    }

    while (keep_running) {
      int client_fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK);
      if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        fprintf(stderr, "WARN: accept failed: %s\n", strerror(errno));
        break;
      }

      bool forwarded = false;
      for (int tries = 0; tries < worker_count; tries++) {
        int idx = (rr + tries) % worker_count;

        if (ensure_worker_connected(&workers[idx]) < 0) {
          continue;
        }

        if (send_fd(workers[idx].control_fd, client_fd) == 0) {
          rr = (idx + 1) % worker_count;
          forwarded = true;
          break;
        }

        close(workers[idx].control_fd);
        workers[idx].control_fd = -1;
      }

      if (!forwarded) {
        const char *resp = "HTTP/1.1 503 Service Unavailable\r\n"
                           "Content-Length: 19\r\n"
                           "Connection: close\r\n\r\n"
                           "no worker available";
        (void)write(client_fd, resp, strlen(resp));
      }

      close(client_fd);
    }
  }

  close(epoll_fd);
  close(listener);
  for (int i = 0; i < worker_count; i++) {
    if (workers[i].control_fd >= 0) {
      close(workers[i].control_fd);
    }
  }

  fprintf(stderr, "INFO: shutdown complete\n");
  return 0;
}
