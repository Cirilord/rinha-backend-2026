#ifndef LOAD_BALANCER_MOCKS_SYS_SOCKET_H
#define LOAD_BALANCER_MOCKS_SYS_SOCKET_H

// Visual/editor-only mock header for non-Linux environments.

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0
#endif

#endif
