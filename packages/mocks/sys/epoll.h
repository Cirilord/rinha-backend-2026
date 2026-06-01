#ifndef LOAD_BALANCER_MOCKS_SYS_EPOLL_H
#define LOAD_BALANCER_MOCKS_SYS_EPOLL_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef union {
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

struct epoll_event {
  uint32_t events;
  epoll_data_t data;
};

#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 0
#endif

#ifndef EPOLL_CTL_ADD
#define EPOLL_CTL_ADD 1
#endif

#ifndef EPOLL_CTL_DEL
#define EPOLL_CTL_DEL 2
#endif

#ifndef EPOLLIN
#define EPOLLIN 0x001
#endif

#ifndef EPOLLERR
#define EPOLLERR 0x008
#endif

#ifndef EPOLLHUP
#define EPOLLHUP 0x010
#endif

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif

static inline int epoll_create1(int flags) {
  (void)flags;
  errno = ENOSYS;
  return -1;
}

static inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
  (void)epfd;
  (void)op;
  (void)fd;
  (void)event;
  errno = ENOSYS;
  return -1;
}

static inline int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
  (void)epfd;
  (void)events;
  (void)maxevents;
  (void)timeout;
  errno = ENOSYS;
  return -1;
}

static inline int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
  (void)sockfd;
  (void)addr;
  (void)addrlen;
  (void)flags;
  errno = ENOSYS;
  return -1;
}

#endif
