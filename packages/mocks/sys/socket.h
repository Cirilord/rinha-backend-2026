#pragma once

#ifdef __APPLE__
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef __SA_FAMILY_T_DEFINED
#define __SA_FAMILY_T_DEFINED
typedef uint8_t sa_family_t;
#endif

#ifndef __SOCKLEN_T_DEFINED
#define __SOCKLEN_T_DEFINED
typedef uint32_t socklen_t;
#endif

struct sockaddr {
  uint8_t sa_len;
  sa_family_t sa_family;
  char sa_data[14];
};

struct iovec {
  void *iov_base;
  size_t iov_len;
};

struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  int msg_iovlen;
  void *msg_control;
  socklen_t msg_controllen;
  int msg_flags;
};

struct cmsghdr {
  socklen_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef SOL_SOCKET
#define SOL_SOCKET 0xffff
#endif

#ifndef SO_REUSEADDR
#define SO_REUSEADDR 0x0004
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0x0200
#endif

#ifndef SO_SNDBUF
#define SO_SNDBUF 0x1001
#endif

#ifndef SO_RCVBUF
#define SO_RCVBUF 0x1002
#endif

#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif

#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif

#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x0004
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x10000000
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x80000
#endif

#ifndef SCM_RIGHTS
#define SCM_RIGHTS 0x01
#endif

#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#endif

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#endif

#ifndef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR(mhdr)                                                                        \
  ((mhdr) != NULL && (mhdr)->msg_controllen >= sizeof(struct cmsghdr)                              \
     ? (struct cmsghdr *)((mhdr)->msg_control)                                                     \
     : (struct cmsghdr *)0)
#endif

#ifndef CMSG_DATA
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#endif

int socket(int domain, int type, int protocol);
int setsockopt(int socket, int level, int option_name, const void *option_value,
               socklen_t option_len);
int connect(int socket, const struct sockaddr *address, socklen_t address_len);
int bind(int socket, const struct sockaddr *address, socklen_t address_len);
int listen(int socket, int backlog);
ssize_t recv(int socket, void *buffer, size_t length, int flags);
ssize_t send(int socket, const void *buffer, size_t length, int flags);
ssize_t recvmsg(int socket, struct msghdr *message, int flags);
ssize_t sendmsg(int socket, const struct msghdr *message, int flags);

int accept4(int socket, struct sockaddr *address, socklen_t *address_len, int flags);
#endif
