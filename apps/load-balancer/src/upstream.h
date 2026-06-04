#ifndef UPSTREAM_H
#define UPSTREAM_H

#ifdef __linux__
#include <sys/socket.h>
#else
#include "../../../packages/mocks/sys/socket.h"
#endif

typedef struct upstream {
  int fd;
  char dummy;
  struct iovec iov;
  union {
    struct cmsghdr cm;
    char buf[CMSG_SPACE(sizeof(int))];
  } control;
  struct msghdr msg;
  struct cmsghdr *cmsg;
} upstream;

upstream upstream__new(const char *socket_path);
int upstream__send_fd_with_flags(upstream *upstream, int client_fd, int flags);

#endif
