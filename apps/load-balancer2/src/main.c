#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "envs.h"
#include "server.h"

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  int port = 0;
  if (!parse_port(&port)) {
    fprintf(stderr, "invalid or missing PORT environment variable\n");
    return 1;
  }

  char worker_sockets[MAX_WORKERS][MAX_SOCKET_PATH];
  size_t worker_count = 0;
  if (!parse_worker_sockets(worker_sockets, &worker_count)) {
    fprintf(stderr, "invalid or missing WORKER_SOCKETS environment variable\n");
    return 1;
  }

  const char *worker_socket_ptrs[MAX_WORKERS];
  for (size_t i = 0; i < worker_count; i++) {
    worker_socket_ptrs[i] = worker_sockets[i];
  }

  int server_fd = create_tcp_server(port);
  if (server_fd < 0) {
    fprintf(stderr, "failed to create tcp server on port %d\n", port);
    return 1;
  }

  if (run_server_loop(server_fd, &keep_running, worker_socket_ptrs, worker_count) < 0) {
    fprintf(stderr, "server loop failed\n");
    close(server_fd);
    return 1;
  }

  close(server_fd);
  return 0;
}
