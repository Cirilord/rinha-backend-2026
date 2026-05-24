#if defined(__linux__)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#else
#error "unsupported platform: expected Linux (epoll) or macOS (kqueue)"
#endif

#include "server.h"

#define MAX_EVENTS 256
#define WAIT_TIMEOUT_MS 250

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
  int *control_fds;
  const char *const *paths;
  size_t count;
  size_t next_index;
} worker_pool_t;

static int accept_client(int server_fd);
static int connect_worker_fd(const char *path);
static int dispatch_client_fd(worker_pool_t *pool, int client_fd);
static int drain_accept_queue(int server_fd, worker_pool_t *pool);
static int initialize_worker_pool(worker_pool_t *pool, const char *const *paths, size_t count);
#if defined(__linux__)
static int run_server_loop_epoll(int server_fd, volatile sig_atomic_t *keep_running,
                                 worker_pool_t *pool);
#elif defined(__APPLE__)
static int run_server_loop_kqueue(int server_fd, volatile sig_atomic_t *keep_running,
                                  worker_pool_t *pool);
#endif
static int send_fd(int control_fd, int fd_to_send);
static int set_nonblocking_cloexec(int fd);
static void teardown_worker_pool(worker_pool_t *pool);

static int accept_client(int server_fd) {
#if defined(__linux__)
  return accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd < 0) {
    return -1;
  }

  if (set_nonblocking_cloexec(client_fd) < 0) {
    close(client_fd);
    return -1;
  }

  return client_fd;
#endif
}

static int connect_worker_fd(const char *path) {
  if (path == NULL || *path == '\0') {
    errno = EINVAL;
    return -1;
  }

  int control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (control_fd < 0) {
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(control_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(control_fd);
    return -1;
  }

  if (set_nonblocking_cloexec(control_fd) < 0) {
    close(control_fd);
    return -1;
  }

  return control_fd;
}

static int dispatch_client_fd(worker_pool_t *pool, int client_fd) {
  if (pool == NULL || pool->count == 0) {
    errno = EINVAL;
    return -1;
  }

  size_t start = pool->next_index;
  pool->next_index = (pool->next_index + 1U) % pool->count;

  for (size_t attempt = 0; attempt < pool->count; attempt++) {
    size_t index = (start + attempt) % pool->count;

    int control_fd = pool->control_fds[index];
    if (control_fd < 0) {
      control_fd = connect_worker_fd(pool->paths[index]);
      if (control_fd < 0) {
        continue;
      }

      pool->control_fds[index] = control_fd;
    }

    if (send_fd(control_fd, client_fd) == 0) {
      return 0;
    }

    close(control_fd);
    pool->control_fds[index] = -1;
  }

  errno = EHOSTUNREACH;
  return -1;
}

static int drain_accept_queue(int server_fd, worker_pool_t *pool) {
  for (;;) {
    int client_fd = accept_client(server_fd);
    if (client_fd >= 0) {
      (void)dispatch_client_fd(pool, client_fd);
      close(client_fd);
      continue;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }

    return -1;
  }
}

static int initialize_worker_pool(worker_pool_t *pool, const char *const *paths, size_t count) {
  if (pool == NULL || paths == NULL || count == 0) {
    errno = EINVAL;
    return -1;
  }

  memset(pool, 0, sizeof(*pool));
  pool->paths = paths;
  pool->count = count;
  pool->next_index = 0;
  pool->control_fds = malloc(sizeof(int) * count);
  if (pool->control_fds == NULL) {
    return -1;
  }

  for (size_t i = 0; i < count; i++) {
    pool->control_fds[i] = -1;
  }

  return 0;
}

#if defined(__linux__)
static int run_server_loop_epoll(int server_fd, volatile sig_atomic_t *keep_running,
                                 worker_pool_t *pool) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return -1;
  }

  struct epoll_event listener_event;
  memset(&listener_event, 0, sizeof(listener_event));
  listener_event.events = EPOLLIN;
  listener_event.data.fd = server_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listener_event) < 0) {
    close(epoll_fd);
    return -1;
  }

  struct epoll_event events[MAX_EVENTS];
  while (*keep_running) {
    int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, WAIT_TIMEOUT_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }

      close(epoll_fd);
      return -1;
    }

    for (int i = 0; i < ready; i++) {
      if (events[i].data.fd != server_fd) {
        continue;
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        errno = EIO;
        close(epoll_fd);
        return -1;
      }

      if ((events[i].events & EPOLLIN) && drain_accept_queue(server_fd, pool) < 0) {
        close(epoll_fd);
        return -1;
      }
    }
  }

  close(epoll_fd);
  return 0;
}
#elif defined(__APPLE__)
static int run_server_loop_kqueue(int server_fd, volatile sig_atomic_t *keep_running,
                                  worker_pool_t *pool) {
  int kqueue_fd = kqueue();
  if (kqueue_fd < 0) {
    return -1;
  }

  struct kevent listener_event;
  EV_SET(&listener_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  if (kevent(kqueue_fd, &listener_event, 1, NULL, 0, NULL) < 0) {
    close(kqueue_fd);
    return -1;
  }

  struct kevent events[MAX_EVENTS];
  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = WAIT_TIMEOUT_MS * 1000000;

  while (*keep_running) {
    int ready = kevent(kqueue_fd, NULL, 0, events, MAX_EVENTS, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }

      close(kqueue_fd);
      return -1;
    }

    for (int i = 0; i < ready; i++) {
      if ((int)events[i].ident != server_fd) {
        continue;
      }

      if (events[i].flags & EV_ERROR) {
        errno = (events[i].data == 0) ? EIO : (int)events[i].data;
        close(kqueue_fd);
        return -1;
      }

      if (events[i].filter == EVFILT_READ && drain_accept_queue(server_fd, pool) < 0) {
        close(kqueue_fd);
        return -1;
      }
    }
  }

  close(kqueue_fd);
  return 0;
}
#endif

static int send_fd(int control_fd, int fd_to_send) {
  char data = 0;
  struct iovec io;
  io.iov_base = &data;
  io.iov_len = 1;

  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  memset(cmsgbuf, 0, sizeof(cmsgbuf));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

  for (;;) {
    ssize_t sent = sendmsg(control_fd, &msg, MSG_NOSIGNAL);
    if (sent >= 0) {
      return 0;
    }

    if (errno == EINTR) {
      continue;
    }

    return -1;
  }
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

static void teardown_worker_pool(worker_pool_t *pool) {
  if (pool == NULL) {
    return;
  }

  if (pool->control_fds != NULL) {
    for (size_t i = 0; i < pool->count; i++) {
      if (pool->control_fds[i] >= 0) {
        close(pool->control_fds[i]);
      }
    }

    free(pool->control_fds);
  }

  pool->control_fds = NULL;
  pool->paths = NULL;
  pool->count = 0;
  pool->next_index = 0;
}

int create_tcp_server(int port) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return -1;
  }

  if (set_nonblocking_cloexec(server_fd) < 0) {
    close(server_fd);
    return -1;
  }

  int reuse = 1;
  (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(server_fd);
    return -1;
  }

  if (listen(server_fd, 4096) < 0) {
    close(server_fd);
    return -1;
  }

  return server_fd;
}

int run_server_loop(int server_fd, volatile sig_atomic_t *keep_running,
                    const char *const *worker_sockets, size_t worker_count) {
  if (server_fd < 0 || keep_running == NULL || worker_sockets == NULL || worker_count == 0) {
    errno = EINVAL;
    return -1;
  }

  worker_pool_t pool;
  if (initialize_worker_pool(&pool, worker_sockets, worker_count) < 0) {
    return -1;
  }

  int result = 0;
#if defined(__linux__)
  result = run_server_loop_epoll(server_fd, keep_running, &pool);
#elif defined(__APPLE__)
  result = run_server_loop_kqueue(server_fd, keep_running, &pool);
#endif

  teardown_worker_pool(&pool);
  return result;
}
