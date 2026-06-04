#include "upstream.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "utils.h"

static int upstream__connect(const char *socket_path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  int sndbuf = 256 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (errno != ENOENT && errno != ECONNREFUSED) {
      close(fd);
      return -1;
    }
    poll(NULL, 0, 50);
  }

  return fd;
}

upstream upstream__new(const char *socket_path) {
  upstream upstream;
  int fd = upstream__connect(socket_path);
  if (fd < 0) {
    fatal("upstream__connect");
  }

  // Start from a clean state and bind the connected UDS fd.
  memset(&upstream, 0, sizeof(upstream));
  upstream.fd = fd;

  // `sendmsg()` still needs one byte of payload alongside the control message.
  upstream.dummy = 'F';
  upstream.iov.iov_base = &upstream.dummy;
  upstream.iov.iov_len = sizeof(upstream.dummy);

  // Point the message envelope at the payload byte and ancillary buffer.
  upstream.msg.msg_iov = &upstream.iov;
  upstream.msg.msg_iovlen = 1;
  upstream.msg.msg_control = upstream.control.buf;
  upstream.msg.msg_controllen = sizeof(upstream.control.buf);

  // Prebuild the SCM_RIGHTS header so each handoff only needs the client fd.
  upstream.cmsg = CMSG_FIRSTHDR(&upstream.msg);
  upstream.cmsg->cmsg_level = SOL_SOCKET;
  upstream.cmsg->cmsg_type = SCM_RIGHTS;
  upstream.cmsg->cmsg_len = CMSG_LEN(sizeof(int));

  return upstream;
}

int upstream__send_fd_with_flags(upstream *upstream, int client_fd, int flags) {
  upstream->msg.msg_controllen = sizeof(upstream->control.buf);
  memcpy(CMSG_DATA(upstream->cmsg), &client_fd, sizeof(client_fd));

  for (;;) {
    ssize_t sent = sendmsg(upstream->fd, &upstream->msg, MSG_NOSIGNAL | flags);
    if (sent > 0) {
      return 0;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
}
