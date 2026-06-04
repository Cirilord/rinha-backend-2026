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

#include <arpa/inet.h>
#include <string.h>

#include "listener.h"
#include "utils.h"

int create_listener(int port, int backlog) {
  int listener_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener_fd < 0) {
    fatal("socket");
  }

  int one = 1;
  if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    fatal("setsockopt");
  }
  if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    fatal("setsockopt");
  }
  if (setsockopt(listener_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof(one)) < 0) {
    fatal("setsockopt");
  }
  if (setsockopt(listener_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    fatal("setsockopt");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(listener_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fatal("bind");
  }

  if (listen(listener_fd, backlog) < 0) {
    fatal("listen");
  }

  return listener_fd;
}
