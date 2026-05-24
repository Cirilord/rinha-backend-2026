#ifndef ENVS_H
#define ENVS_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_WORKERS 16
#define MAX_SOCKET_PATH 108

bool parse_port(int *port_out);
bool parse_worker_sockets(char worker_sockets[MAX_WORKERS][MAX_SOCKET_PATH], size_t *worker_count_out);

#endif
