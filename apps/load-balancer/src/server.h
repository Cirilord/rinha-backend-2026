#ifndef SERVER_H
#define SERVER_H

#include <signal.h>
#include <stddef.h>

int create_tcp_server(int port);
int run_server_loop(int server_fd, volatile sig_atomic_t *keep_running,
                    const char *const *worker_sockets, size_t worker_count);

#endif
