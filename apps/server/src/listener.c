#define _GNU_SOURCE

#ifdef __linux__
#include <sys/socket.h>
#else
#include "../../../packages/mocks/sys/socket.h"
#endif

#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "listener.h"
#include "utils.h"

int create_listener(const char *socket_path, int backlog) {
  int listener_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (listener_fd < 0) {
    fatal("socket");
  }

  int buf = 256 * 1024;
  setsockopt(listener_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  setsockopt(listener_fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  unlink(socket_path);
  if (bind(listener_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fatal("bind");
  }

  if (listen(listener_fd, backlog) < 0) {
    fatal("listen");
  }

  return listener_fd;
}
