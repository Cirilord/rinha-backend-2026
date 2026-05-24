#if defined(__linux__)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
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

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (sizeof(struct cmsghdr) + (len))
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
  CLIENT_BUFFER_SIZE = 8192,
  MAX_CTRL_CONNS = 32,
  MAX_EVENTS = 128,
  MAX_TRACKED_FDS = 131072,
  WAIT_TIMEOUT_MS = 250,
};

typedef struct {
  int fds[MAX_CTRL_CONNS];
  size_t count;
} ctrl_set_t;

typedef enum {
  CLIENT_PHASE_READING = 0,
  CLIENT_PHASE_WRITING = 1,
} client_phase_t;

typedef struct {
  int fd;
  char buffer[CLIENT_BUFFER_SIZE];
  size_t used;
  size_t scan_pos;
  size_t line_start;
  size_t content_length;
  size_t expected_total;
  size_t write_off;
  bool headers_done;
  bool content_length_seen;
  client_phase_t phase;
} client_conn_t;

static volatile sig_atomic_t keep_running = 1;
static client_conn_t *conn_by_fd[MAX_TRACKED_FDS];

static const char RESPONSE_OK[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: application/json\r\n"
  "Connection: close\r\n"
  "Content-Length: 18\r\n"
  "\r\n"
  "{\"approved\":false}";

static int accept_control_fd(int server_fd);
static int close_all_client_conns(int loop_fd);
static void close_all_ctrl_fds(ctrl_set_t *set);
static void close_client_conn(int loop_fd, client_conn_t *conn);
static client_conn_t *conn_get(int fd);
static bool conn_set(int fd, client_conn_t *conn);
static void conn_unset(int fd);
static int consume_client_read_step(client_conn_t *conn);
static bool ctrl_set_add(ctrl_set_t *set, int fd);
static ssize_t ctrl_set_find(const ctrl_set_t *set, int fd);
static void ctrl_set_remove(ctrl_set_t *set, size_t index);
static client_conn_t *create_client_conn(int fd);
static int create_unix_server(const char *path);
static void on_signal(int signo);
static bool parse_content_length(const char *line, size_t line_len, size_t *out_content_len);
static int process_control_fd(int loop_fd, int control_fd);
static int recv_fd_nonblocking(int unix_sock, int *out_client_fd);
static int register_client_read_interest(int loop_fd, int fd);
#if defined(__linux__)
static int run_loop_epoll(int server_fd);
#elif defined(__APPLE__)
static int run_loop_kqueue(int server_fd);
#endif
static int send_response_write_step(client_conn_t *conn);
static int set_nonblocking_cloexec(int fd);
static int switch_client_to_write_interest(int loop_fd, int fd);
static void unregister_fd(int loop_fd, int fd);

static int accept_control_fd(int server_fd) {
#if defined(__linux__)
  return accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  int fd = accept(server_fd, NULL, NULL);
  if (fd < 0) {
    return -1;
  }

  if (set_nonblocking_cloexec(fd) < 0) {
    close(fd);
    return -1;
  }

  return fd;
#endif
}

static int close_all_client_conns(int loop_fd) {
  for (int fd = 0; fd < MAX_TRACKED_FDS; fd++) {
    client_conn_t *conn = conn_by_fd[fd];
    if (conn != NULL) {
      close_client_conn(loop_fd, conn);
    }
  }

  return 0;
}

static void close_all_ctrl_fds(ctrl_set_t *set) {
  if (set == NULL) {
    return;
  }

  for (size_t i = 0; i < set->count; i++) {
    if (set->fds[i] >= 0) {
      close(set->fds[i]);
      set->fds[i] = -1;
    }
  }
  set->count = 0;
}

static void close_client_conn(int loop_fd, client_conn_t *conn) {
  if (conn == NULL) {
    return;
  }

  unregister_fd(loop_fd, conn->fd);
  conn_unset(conn->fd);
  close(conn->fd);
  free(conn);
}

static client_conn_t *conn_get(int fd) {
  if (fd < 0 || fd >= MAX_TRACKED_FDS) {
    return NULL;
  }
  return conn_by_fd[fd];
}

static bool conn_set(int fd, client_conn_t *conn) {
  if (fd < 0 || fd >= MAX_TRACKED_FDS || conn == NULL) {
    return false;
  }
  conn_by_fd[fd] = conn;
  return true;
}

static void conn_unset(int fd) {
  if (fd < 0 || fd >= MAX_TRACKED_FDS) {
    return;
  }
  conn_by_fd[fd] = NULL;
}

static int consume_client_read_step(client_conn_t *conn) {
  if (conn == NULL) {
    errno = EINVAL;
    return -1;
  }

  while (conn->used + 1 < sizeof(conn->buffer)) {
    ssize_t n = recv(conn->fd, conn->buffer + conn->used, sizeof(conn->buffer) - conn->used - 1, 0);
    if (n > 0) {
      conn->used += (size_t)n;
      conn->buffer[conn->used] = '\0';
    } else if (n == 0) {
      return -1;
    } else {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }

    if (!conn->headers_done) {
      while ((conn->scan_pos + 1) < conn->used) {
        if (conn->buffer[conn->scan_pos] == '\r' && conn->buffer[conn->scan_pos + 1] == '\n') {
          size_t line_len = conn->scan_pos - conn->line_start;

          if (line_len == 0) {
            conn->headers_done = true;
            size_t body_offset = conn->scan_pos + 2;
            conn->expected_total = body_offset + conn->content_length;
            if (conn->expected_total + 1 > sizeof(conn->buffer)) {
              return -1;
            }
            conn->scan_pos += 2;
            break;
          }

          if (!conn->content_length_seen) {
            size_t parsed = 0;
            if (parse_content_length(conn->buffer + conn->line_start, line_len, &parsed)) {
              conn->content_length = parsed;
              conn->content_length_seen = true;
            }
          }

          conn->scan_pos += 2;
          conn->line_start = conn->scan_pos;
          continue;
        }

        conn->scan_pos++;
      }
    }

    if (conn->headers_done && conn->used >= conn->expected_total) {
      conn->phase = CLIENT_PHASE_WRITING;
      return 1;
    }
  }

  return -1;
}

static bool ctrl_set_add(ctrl_set_t *set, int fd) {
  if (set == NULL || fd < 0 || set->count >= MAX_CTRL_CONNS) {
    return false;
  }

  set->fds[set->count] = fd;
  set->count++;
  return true;
}

static ssize_t ctrl_set_find(const ctrl_set_t *set, int fd) {
  if (set == NULL || fd < 0) {
    return -1;
  }

  for (size_t i = 0; i < set->count; i++) {
    if (set->fds[i] == fd) {
      return (ssize_t)i;
    }
  }

  return -1;
}

static void ctrl_set_remove(ctrl_set_t *set, size_t index) {
  if (set == NULL || index >= set->count) {
    return;
  }

  size_t last = set->count - 1;
  set->fds[index] = set->fds[last];
  set->count = last;
}

static client_conn_t *create_client_conn(int fd) {
  client_conn_t *conn = (client_conn_t *)calloc(1, sizeof(client_conn_t));
  if (conn == NULL) {
    return NULL;
  }

  conn->fd = fd;
  conn->phase = CLIENT_PHASE_READING;
  return conn;
}

static int create_unix_server(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (set_nonblocking_cloexec(fd) < 0) {
    close(fd);
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

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
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
    unsigned char c = (unsigned char)line[i];
    if (c < '0' || c > '9') {
      return false;
    }
    has_digits = true;
    size_t digit = (size_t)(c - '0');
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

static int process_control_fd(int loop_fd, int control_fd) {
  for (;;) {
    int client_fd = -1;
    int status = recv_fd_nonblocking(control_fd, &client_fd);
    if (status < 0) {
      return -1;
    }
    if (status == 0) {
      return 0;
    }

    if (set_nonblocking_cloexec(client_fd) < 0) {
      close(client_fd);
      continue;
    }

    client_conn_t *conn = create_client_conn(client_fd);
    if (conn == NULL) {
      close(client_fd);
      continue;
    }

    if (!conn_set(client_fd, conn)) {
      close(client_fd);
      free(conn);
      continue;
    }

    if (register_client_read_interest(loop_fd, client_fd) < 0) {
      close_client_conn(loop_fd, conn);
    }
  }
}

static int recv_fd_nonblocking(int unix_sock, int *out_client_fd) {
  if (out_client_fd == NULL) {
    errno = EINVAL;
    return -1;
  }

  for (;;) {
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

    ssize_t n = recvmsg(unix_sock, &msg, 0);
    if (n > 0) {
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

    if (n == 0) {
      return -1;
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

static int register_client_read_interest(int loop_fd, int fd) {
#if defined(__linux__)
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  event.data.fd = fd;
  return epoll_ctl(loop_fd, EPOLL_CTL_ADD, fd, &event);
#else
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  return kevent(loop_fd, &change, 1, NULL, 0, NULL);
#endif
}

#if defined(__linux__)
static int run_loop_epoll(int server_fd) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return -1;
  }

  struct epoll_event listener_event;
  memset(&listener_event, 0, sizeof(listener_event));
  listener_event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  listener_event.data.fd = server_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listener_event) < 0) {
    close(epoll_fd);
    return -1;
  }

  ctrl_set_t ctrl_set;
  memset(&ctrl_set, 0, sizeof(ctrl_set));

  struct epoll_event events[MAX_EVENTS];
  while (keep_running) {
    int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, WAIT_TIMEOUT_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }

      close_all_ctrl_fds(&ctrl_set);
      close_all_client_conns(epoll_fd);
      close(epoll_fd);
      return -1;
    }

    for (int i = 0; i < ready; i++) {
      int fd = events[i].data.fd;
      uint32_t revents = events[i].events;

      if (fd == server_fd) {
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
          close_all_ctrl_fds(&ctrl_set);
          close_all_client_conns(epoll_fd);
          close(epoll_fd);
          return -1;
        }

        while (keep_running) {
          int ctrl_fd = accept_control_fd(server_fd);
          if (ctrl_fd >= 0) {
            if (!ctrl_set_add(&ctrl_set, ctrl_fd)) {
              close(ctrl_fd);
              continue;
            }

            struct epoll_event ctrl_event;
            memset(&ctrl_event, 0, sizeof(ctrl_event));
            ctrl_event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
            ctrl_event.data.fd = ctrl_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ctrl_event) < 0) {
              close(ctrl_fd);
              ctrl_set_remove(&ctrl_set, ctrl_set.count - 1);
            }
            continue;
          }

          if (errno == EINTR) {
            continue;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }

          close_all_ctrl_fds(&ctrl_set);
          close_all_client_conns(epoll_fd);
          close(epoll_fd);
          return -1;
        }
        continue;
      }

      ssize_t ctrl_index = ctrl_set_find(&ctrl_set, fd);
      if (ctrl_index >= 0) {
        if ((revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
          (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
          ctrl_set_remove(&ctrl_set, (size_t)ctrl_index);
          continue;
        }

        if ((revents & EPOLLIN) != 0 && process_control_fd(epoll_fd, fd) < 0) {
          (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
          ctrl_set_remove(&ctrl_set, (size_t)ctrl_index);
        }
        continue;
      }

      client_conn_t *conn = conn_get(fd);
      if (conn == NULL) {
        continue;
      }

      if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
        close_client_conn(epoll_fd, conn);
        continue;
      }

      if (conn->phase == CLIENT_PHASE_READING) {
        if ((revents & EPOLLRDHUP) != 0) {
          close_client_conn(epoll_fd, conn);
          continue;
        }

        if ((revents & EPOLLIN) != 0) {
          int status = consume_client_read_step(conn);
          if (status < 0) {
            close_client_conn(epoll_fd, conn);
            continue;
          }
          if (status > 0 && switch_client_to_write_interest(epoll_fd, fd) < 0) {
            close_client_conn(epoll_fd, conn);
            continue;
          }
        }
      } else {
        if ((revents & EPOLLOUT) != 0) {
          int status = send_response_write_step(conn);
          if (status != 0) {
            close_client_conn(epoll_fd, conn);
            continue;
          }
        }
      }
    }
  }

  close_all_ctrl_fds(&ctrl_set);
  close_all_client_conns(epoll_fd);
  close(epoll_fd);
  return 0;
}
#elif defined(__APPLE__)
static int run_loop_kqueue(int server_fd) {
  int kq_fd = kqueue();
  if (kq_fd < 0) {
    return -1;
  }

  struct kevent listener_event;
  EV_SET(&listener_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
  if (kevent(kq_fd, &listener_event, 1, NULL, 0, NULL) < 0) {
    close(kq_fd);
    return -1;
  }

  ctrl_set_t ctrl_set;
  memset(&ctrl_set, 0, sizeof(ctrl_set));

  struct kevent events[MAX_EVENTS];
  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = WAIT_TIMEOUT_MS * 1000000;

  while (keep_running) {
    int ready = kevent(kq_fd, NULL, 0, events, MAX_EVENTS, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }

      close_all_ctrl_fds(&ctrl_set);
      close_all_client_conns(kq_fd);
      close(kq_fd);
      return -1;
    }

    for (int i = 0; i < ready; i++) {
      int fd = (int)events[i].ident;

      if (fd == server_fd) {
        if ((events[i].flags & EV_ERROR) != 0) {
          errno = (events[i].data == 0) ? EIO : (int)events[i].data;
          close_all_ctrl_fds(&ctrl_set);
          close_all_client_conns(kq_fd);
          close(kq_fd);
          return -1;
        }

        while (keep_running) {
          int ctrl_fd = accept_control_fd(server_fd);
          if (ctrl_fd >= 0) {
            if (!ctrl_set_add(&ctrl_set, ctrl_fd)) {
              close(ctrl_fd);
              continue;
            }

            struct kevent ctrl_event;
            EV_SET(&ctrl_event, ctrl_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
            if (kevent(kq_fd, &ctrl_event, 1, NULL, 0, NULL) < 0) {
              close(ctrl_fd);
              ctrl_set_remove(&ctrl_set, ctrl_set.count - 1);
            }
            continue;
          }

          if (errno == EINTR) {
            continue;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }

          close_all_ctrl_fds(&ctrl_set);
          close_all_client_conns(kq_fd);
          close(kq_fd);
          return -1;
        }
        continue;
      }

      ssize_t ctrl_index = ctrl_set_find(&ctrl_set, fd);
      if (ctrl_index >= 0) {
        if ((events[i].flags & (EV_ERROR | EV_EOF)) != 0) {
          close(fd);
          ctrl_set_remove(&ctrl_set, (size_t)ctrl_index);
          continue;
        }

        if (events[i].filter == EVFILT_READ && process_control_fd(kq_fd, fd) < 0) {
          close(fd);
          ctrl_set_remove(&ctrl_set, (size_t)ctrl_index);
        }
        continue;
      }

      client_conn_t *conn = conn_get(fd);
      if (conn == NULL) {
        continue;
      }

      if ((events[i].flags & (EV_ERROR | EV_EOF)) != 0) {
        close_client_conn(kq_fd, conn);
        continue;
      }

      if (conn->phase == CLIENT_PHASE_READING && events[i].filter == EVFILT_READ) {
        int status = consume_client_read_step(conn);
        if (status < 0) {
          close_client_conn(kq_fd, conn);
          continue;
        }
        if (status > 0 && switch_client_to_write_interest(kq_fd, fd) < 0) {
          close_client_conn(kq_fd, conn);
          continue;
        }
      } else if (conn->phase == CLIENT_PHASE_WRITING && events[i].filter == EVFILT_WRITE) {
        int status = send_response_write_step(conn);
        if (status != 0) {
          close_client_conn(kq_fd, conn);
          continue;
        }
      }
    }
  }

  close_all_ctrl_fds(&ctrl_set);
  close_all_client_conns(kq_fd);
  close(kq_fd);
  return 0;
}
#endif

static int send_response_write_step(client_conn_t *conn) {
  if (conn == NULL) {
    errno = EINVAL;
    return -1;
  }

  const size_t response_len = sizeof(RESPONSE_OK) - 1;
  while (conn->write_off < response_len) {
    ssize_t n = send(conn->fd, RESPONSE_OK + conn->write_off, response_len - conn->write_off, MSG_NOSIGNAL);
    if (n > 0) {
      conn->write_off += (size_t)n;
      continue;
    }

    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    return -1;
  }

  return 1;
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

static int switch_client_to_write_interest(int loop_fd, int fd) {
#if defined(__linux__)
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
  event.data.fd = fd;
  return epoll_ctl(loop_fd, EPOLL_CTL_MOD, fd, &event);
#else
  struct kevent changes[2];
  EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
  return kevent(loop_fd, changes, 2, NULL, 0, NULL);
#endif
}

static void unregister_fd(int loop_fd, int fd) {
#if defined(__linux__)
  (void)epoll_ctl(loop_fd, EPOLL_CTL_DEL, fd, NULL);
#else
  struct kevent changes[2];
  EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  (void)kevent(loop_fd, changes, 2, NULL, 0, NULL);
#endif
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

  int server_fd = create_unix_server(socket_path);
  if (server_fd < 0) {
    fprintf(stderr, "failed to bind unix socket '%s': %s\n", socket_path, strerror(errno));
    return 1;
  }

  fprintf(stderr, "server2 listening for fd passing at %s\n", socket_path);

#if defined(__linux__)
  int status = run_loop_epoll(server_fd);
#elif defined(__APPLE__)
  int status = run_loop_kqueue(server_fd);
#endif

  close(server_fd);
  return (status == 0) ? 0 : 1;
}
