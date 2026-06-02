#ifndef LOAD_BALANCER_MOCKS_NETINET_TCP_H
#define LOAD_BALANCER_MOCKS_NETINET_TCP_H

// Visual/editor-only mock header for non-Linux environments.

#ifndef TCP_NODELAY
#define TCP_NODELAY 0
#endif

#ifndef TCP_QUICKACK
#define TCP_QUICKACK 0
#endif

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 0
#endif

#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 0
#endif

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 0
#endif

#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 0
#endif

#endif
