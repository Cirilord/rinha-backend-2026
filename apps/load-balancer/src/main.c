#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 9999
#define MAX_WORKERS 16
#define MAX_SOCKET_PATH 108
#define MAX_ENV_LEN 1024
#define BACKLOG 1024

static volatile sig_atomic_t keep_running = 1;

typedef struct {
  char path[MAX_SOCKET_PATH];
  int control_fd;
} worker_t;

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

static void log_msg(const char *level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

  fprintf(stderr, "[%s] [%s] ", ts, level);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");

  va_end(args);
}

static int parse_port(void) {
  const char *env = getenv("PORT");
  if (env == NULL || *env == '\0') {
    return DEFAULT_PORT;
  }

  char *end = NULL;
  long p = strtol(env, &end, 10);
  if (*end != '\0' || p < 1 || p > 65535) {
    log_msg("WARN", "invalid PORT='%s', fallback to %d", env, DEFAULT_PORT);
    return DEFAULT_PORT;
  }

  return (int)p;
}

static int parse_worker_paths(worker_t workers[], int max_workers) {
  const char *env = getenv("WORKER_SOCKETS");
  if (env == NULL || *env == '\0') {
    env = "/shared/backend-1.sock,/shared/backend-2.sock";
  }

  char buffer[MAX_ENV_LEN];
  strncpy(buffer, env, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  int count = 0;
  char *token = strtok(buffer, ",");
  while (token != NULL && count < max_workers) {
    while (*token == ' ') {
      token++;
    }

    size_t len = strnlen(token, MAX_SOCKET_PATH);
    if (len == 0 || len >= MAX_SOCKET_PATH) {
      log_msg("WARN", "skipping invalid worker socket path: '%s'", token);
    } else {
      strncpy(workers[count].path, token, sizeof(workers[count].path) - 1);
      workers[count].path[sizeof(workers[count].path) - 1] = '\0';
      workers[count].control_fd = -1;
      count++;
    }

    token = strtok(NULL, ",");
  }

  return count;
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

  char dummy = 'X';
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

  if (sendmsg(unix_sock, &msg, 0) < 0) {
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

  worker_t workers[MAX_WORKERS];
  int worker_count = parse_worker_paths(workers, MAX_WORKERS);
  if (worker_count <= 0) {
    log_msg("ERROR", "no valid workers found in WORKER_SOCKETS");
    return 1;
  }

  int port = parse_port();
  int listener = create_tcp_listener(port);
  if (listener < 0) {
    log_msg("ERROR", "failed to create TCP listener on port %d: %s", port, strerror(errno));
    return 1;
  }

  log_msg("INFO", "listening on 0.0.0.0:%d", port);
  for (int i = 0; i < worker_count; i++) {
    log_msg("INFO", "worker[%d] socket path: %s", i, workers[i].path);
  }

  int rr = 0;
  struct pollfd listener_pfd;
  listener_pfd.fd = listener;
  listener_pfd.events = POLLIN;
  listener_pfd.revents = 0;

  while (keep_running) {
    int nready = poll(&listener_pfd, 1, -1);
    if (nready < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_msg("WARN", "poll failed: %s", strerror(errno));
      break;
    }

    if ((listener_pfd.revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
      continue;
    }

    while (keep_running) {
      int client_fd = accept(listener, NULL, NULL);
      if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        log_msg("WARN", "accept failed: %s", strerror(errno));
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

  close(listener);
  for (int i = 0; i < worker_count; i++) {
    if (workers[i].control_fd >= 0) {
      close(workers[i].control_fd);
    }
  }

  log_msg("INFO", "shutdown complete");
  return 0;
}
