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
#include <unistd.h>

#include "env.h"
#include "listener.h"
#include "upstream.h"
#include "utils.h"

#define ACCEPT_BATCH 64

static int dispatch_client(upstream upstreams[MAX_UPSTREAM_COUNT], int upstream_count,
                           int first_upstream, int client_fd) {
  for (int offset = 0; offset < upstream_count; offset++) {
    int target = (first_upstream + offset) % upstream_count;
    if (upstream__send_fd_with_flags(&upstreams[target], client_fd, MSG_DONTWAIT) == 0) {
      return 0;
    }
  }

  return upstream__send_fd_with_flags(&upstreams[first_upstream], client_fd, 0);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  const char *upstream_paths[MAX_UPSTREAM_COUNT];
  int upstream_count = get_upstream_paths(upstream_paths);
  if (upstream_count <= 0) {
    fatal("get_upstream_paths");
  }

  int listener_fd = create_listener(9999, 65535);

  upstream upstreams[MAX_UPSTREAM_COUNT];
  for (int i = 0; i < upstream_count; i++) {
    upstreams[i] = upstream__new(upstream_paths[i]);
  }

  int next_upstream = 0;

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

      int first_upstream = next_upstream;
      next_upstream = (next_upstream + 1) % upstream_count;
      if (dispatch_client(upstreams, upstream_count, first_upstream, client_fd) < 0) {
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
