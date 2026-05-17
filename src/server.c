#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

bool create_unix_server(const char *socket_path, int *out_server_fd) {
  if (!socket_path || socket_path[0] == '\0' || !out_server_fd) {
    return false;
  }

  struct sockaddr_un addr;
  size_t path_len = strlen(socket_path);
  if (path_len >= sizeof(addr.sun_path)) {
    fprintf(stderr, "SOCKET_PATH is too long: %s\n", socket_path);
    return false;
  }

  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return false;
  }

  if (unlink(socket_path) < 0 && errno != ENOENT) {
    perror("unlink");
    close(server_fd);
    return false;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(server_fd);
    return false;
  }

  if (chmod(socket_path, 0666) < 0) {
    perror("chmod");
    close(server_fd);
    unlink(socket_path);
    return false;
  }

  if (listen(server_fd, 1024) < 0) {
    perror("listen");
    close(server_fd);
    unlink(socket_path);
    return false;
  }

  *out_server_fd = server_fd;
  return true;
}
