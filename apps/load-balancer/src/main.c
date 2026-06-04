#define _GNU_SOURCE

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#else
#include "../../../packages/mocks/netinet/in.h"
#include "../../../packages/mocks/netinet/tcp.h"
#include "../../../packages/mocks/sys/socket.h"
#endif

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "listener.h"
#include "utils.h"

static const char *BACKEND_PATHS[] = {
  "/tmp/server-1.sock",
  "/tmp/server-2.sock",
};

#define BACKEND_COUNT 2
#define ACCEPT_BATCH 64

struct backend {
  int fd;
  char dummy;
  struct iovec iov;
  union {
    struct cmsghdr cm;
    char buf[CMSG_SPACE(sizeof(int))];
  } control;
  struct msghdr msg;
  struct cmsghdr *cmsg;
};

static int connect_backend(const char *socket_path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    fatal("socket");
  }

  int sndbuf = 256 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (errno != ENOENT && errno != ECONNREFUSED) {
      fatal("connect");
    }
    usleep(50000);
  }

  return fd;
}

static void init_backend(struct backend *backend, int fd) {
  memset(backend, 0, sizeof(*backend));
  backend->fd = fd;
  backend->dummy = 'F';
  backend->iov.iov_base = &backend->dummy;
  backend->iov.iov_len = sizeof(backend->dummy);
  backend->msg.msg_iov = &backend->iov;
  backend->msg.msg_iovlen = 1;
  backend->msg.msg_control = backend->control.buf;
  backend->msg.msg_controllen = sizeof(backend->control.buf);
  backend->cmsg = CMSG_FIRSTHDR(&backend->msg);
  backend->cmsg->cmsg_level = SOL_SOCKET;
  backend->cmsg->cmsg_type = SCM_RIGHTS;
  backend->cmsg->cmsg_len = CMSG_LEN(sizeof(int));
}

static int send_fd_with_flags(struct backend *backend, int client_fd, int flags) {
  backend->msg.msg_controllen = sizeof(backend->control.buf);
  memcpy(CMSG_DATA(backend->cmsg), &client_fd, sizeof(client_fd));

  for (;;) {
    ssize_t sent = sendmsg(backend->fd, &backend->msg, MSG_NOSIGNAL | flags);
    if (sent > 0) {
      return 0;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
}

static int handoff_client(struct backend backends[BACKEND_COUNT], int first_backend,
                          int client_fd) {
  for (int offset = 0; offset < BACKEND_COUNT; offset++) {
    int target = (first_backend + offset) % BACKEND_COUNT;
    if (send_fd_with_flags(&backends[target], client_fd, MSG_DONTWAIT) == 0) {
      return 0;
    }
  }

  return send_fd_with_flags(&backends[first_backend], client_fd, 0);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  int listener_fd = create_listener(9999, 65535);
  struct backend backends[BACKEND_COUNT];
  for (int i = 0; i < BACKEND_COUNT; i++) {
    init_backend(&backends[i], connect_backend(BACKEND_PATHS[i]));
  }

  int next_backend = 0;
  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = listener_fd;
  pfd.events = POLLIN;

  for (;;) {
    int accepted = 0;
    while (accepted < ACCEPT_BATCH) {
      int client_fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client_fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        fatal("accept4");
      }

      accepted++;

      int one = 1;
      if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        fatal("setsockopt");
      }
      if (setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
        fatal("setsockopt");
      }

      int first_backend = next_backend;
      next_backend = (next_backend + 1) % BACKEND_COUNT;
      if (handoff_client(backends, first_backend, client_fd) < 0) {
        close(client_fd);
        continue;
      }

      close(client_fd);
    }

    if (accepted == 0) {
      poll(&pfd, 1, -1);
    }
  }
}
